/**
 * TermSession — the long-lived, non-React half of a terminal pane.
 *
 * One instance per workspace pane, kept in a module-level map and independent of React's mount lifecycle:
 * React StrictMode double mounts, tab hiding and layout changes (a pane moving into a split re-parents its
 * React subtree) only detach/re-attach `element`; the xterm.js instance, its scrollback and the connection
 * survive. The session is disposed (connection closed, xterm disposed) when its pane disappears from the
 * workspace store.
 *
 * Responsibilities: xterm.js + addons (fit, webgl with DOM fallback, search, web-links, unicode11), options from
 * settings/session/theme (live), the connection lifecycle (open on 'idle', resize, close, restart), output
 * processing (answerback, implicit LF, activity), input (PuTTY keymap, broadcast, paste, clipboard), bell,
 * per-pane zoom, auto-reconnect / close-on-exit and the TerminalHandle registered in the registry.
 */
import { Terminal, type ITerminalOptions, type ITheme } from '@xterm/xterm';
import { FitAddon } from '@xterm/addon-fit';
import { SearchAddon, type ISearchOptions } from '@xterm/addon-search';
import { Unicode11Addon } from '@xterm/addon-unicode11';
import { WebLinksAddon } from '@xterm/addon-web-links';
import { WebglAddon } from '@xterm/addon-webgl';
import { api } from '@/bridge/api';
import { toast } from '@/components/ui';
import { usePrompts, useSettings } from '@/store/data';
import { confirmDialog, useUi } from '@/store/ui';
import { paneIdsOf, useWorkspace, type PaneState } from '@/store/workspace';
import { resolveTerminalScheme } from '@/theme/resolve';
import { useSystemThemeStore } from '@/theme/ThemeController';
import type { ColorScheme, ConnStatusEvent, SessionConfig } from '@shared/types';
import { forgetConn, lastStatusOf, PANE_PROMPT_KINDS, subscribeConn } from './connEvents';
import { translateKey } from './keymap';
import { terminalRegistry, type TerminalHandle } from './registry';
import {
  BellLimiter,
  bufferToText,
  countEnq,
  decodeCaretNotation,
  implicitLfInCr,
  isMultiline,
  isTerminalReport,
  mixHex,
  pastePreview,
  reconnectDelayMs,
  toHex6
} from './termUtil';

export type SessionEvent = 'find' | 'contextmenu' | 'reconnect' | 'renderer';

export interface FindOptions {
  caseSensitive: boolean;
  wholeWord: boolean;
  regex: boolean;
}

const MIN_FONT = 6;
const MAX_FONT = 72;
/** Browsers cap live WebGL contexts (~16); keep a margin for other canvases. */
const MAX_WEBGL_CONTEXTS = 10;
const RESIZE_DEBOUNCE_MS = 50;

/** connIds for which conn:open was issued (guards double opens from StrictMode / re-renders). */
const openedConns = new Set<string>();
const sessions = new Map<string, TermSession>();
/** Sessions currently owning a WebGL renderer, least recently visible first. */
const webglOwners: TermSession[] = [];

const isEnded = (s: PaneState['status'] | undefined) => s === 'disconnected' || s === 'error';

export function getSession(paneId: string): TermSession | undefined {
  return sessions.get(paneId);
}

/** Returns the pane's session, creating it on first use. */
export function acquireSession(pane: PaneState): TermSession {
  let s = sessions.get(pane.id);
  if (!s) {
    ensureWorkspaceWatch();
    s = new TermSession(pane);
    sessions.set(pane.id, s);
  }
  return s;
}

let workspaceWatched = false;
function ensureWorkspaceWatch(): void {
  if (workspaceWatched) return;
  workspaceWatched = true;
  useWorkspace.subscribe((st, prev) => {
    if (st.panes === prev.panes && st.tabs === prev.tabs) return;
    for (const [id, s] of [...sessions]) {
      const p = st.panes[id];
      if (!p) s.dispose();
      else s.syncPane(p);
    }
  });
}

/* -------------------------------------------------------------------------- */
/* Bell sounds (data URLs cached per path)                                     */
/* -------------------------------------------------------------------------- */

const bellSounds = new Map<string, Promise<HTMLAudioElement | null>>();

function bellAudio(path: string): Promise<HTMLAudioElement | null> {
  let p = bellSounds.get(path);
  if (!p) {
    p = api.app
      .readFileDataUrl(path)
      .then((url) => (url ? new Audio(url) : null))
      .catch(() => null);
    bellSounds.set(path, p);
  }
  return p;
}

/* -------------------------------------------------------------------------- */

function toXtermTheme(s: ColorScheme): ITheme {
  return {
    foreground: s.foreground,
    background: s.background,
    cursor: s.cursor,
    cursorAccent: s.cursorAccent,
    selectionBackground: s.selectionBackground,
    selectionForeground: s.selectionForeground,
    selectionInactiveBackground: s.selectionBackground,
    black: s.black,
    red: s.red,
    green: s.green,
    yellow: s.yellow,
    blue: s.blue,
    magenta: s.magenta,
    cyan: s.cyan,
    white: s.white,
    brightBlack: s.brightBlack,
    brightRed: s.brightRed,
    brightGreen: s.brightGreen,
    brightYellow: s.brightYellow,
    brightBlue: s.brightBlue,
    brightMagenta: s.brightMagenta,
    brightCyan: s.brightCyan,
    brightWhite: s.brightWhite
  };
}

export class TermSession {
  readonly paneId: string;
  /** Outer element moved between React containers; holds the padding, the xterm screen and the bell flash. */
  readonly element: HTMLDivElement;
  readonly term: Terminal;

  private readonly screen: HTMLDivElement;
  private readonly bellFlash: HTMLDivElement;
  private readonly fitAddon = new FitAddon();
  private readonly searchAddon = new SearchAddon({ highlightLimit: 2000 });
  private webgl: WebglAddon | null = null;
  private webglFailed = false;

  private opened = false;
  private disposed = false;
  private visible = false;
  private container: HTMLElement | null = null;
  private resizeObserver: ResizeObserver | null = null;
  private fitFrame = 0;
  private resizeTimer: ReturnType<typeof setTimeout> | undefined;
  private lastSentSize = '';

  private connId = '';
  private connUnsub: (() => void) | null = null;
  private session: SessionConfig;
  private status: PaneState['status'];
  private zoomDelta = 0;
  private scheme: ColorScheme;
  private appliedOptions = new Map<string, string>();

  private readonly bell = new BellLimiter();
  private copyTimer: ReturnType<typeof setTimeout> | undefined;
  private wheelAccum = 0;

  /** Auto-reconnect: armed once the pane has been connected; cleared by explicit user close / cancel. */
  private reconnectArmed = false;
  private reconnectAttempt = 0;
  private reconnectTimer: ReturnType<typeof setTimeout> | undefined;
  /** Epoch ms of the scheduled reconnect (for the banner countdown), null when none is pending. */
  reconnectAt: number | null = null;
  /** The user closed this connection (Cancel / Close): no close-on-exit or reconnect handling. */
  private userClosed = false;

  private readonly listeners = new Map<SessionEvent, Set<(arg?: unknown) => void>>();
  private readonly unsubs: (() => void)[] = [];
  private readonly unregister: () => void;

  constructor(pane: PaneState) {
    this.paneId = pane.id;
    this.session = pane.session;
    this.status = pane.status;
    this.scheme = this.resolveScheme();

    this.element = document.createElement('div');
    this.element.className = 'term-host';
    this.element.dataset.pane = pane.id;
    this.screen = document.createElement('div');
    this.screen.className = 'term-screen';
    this.bellFlash = document.createElement('div');
    this.bellFlash.className = 'term-bell';
    this.bellFlash.addEventListener('animationend', () => this.bellFlash.classList.remove('term-bell--on'));
    this.element.append(this.screen, this.bellFlash);

    this.term = new Terminal({
      ...this.computeOptions(),
      allowProposedApi: true,
      rightClickSelectsWord: false,
      altClickMovesCursor: false,
      macOptionIsMeta: true
    });
    this.rememberOptions(this.computeOptions());
    this.term.loadAddon(this.fitAddon);
    this.term.loadAddon(this.searchAddon);
    this.term.loadAddon(new Unicode11Addon());
    this.term.unicode.activeVersion = '11';
    this.term.loadAddon(
      new WebLinksAddon((event, uri) => {
        if (event.ctrlKey || event.metaKey) void api.app.openExternal(uri);
      })
    );

    this.term.attachCustomKeyEventHandler((ev) => this.onKeyEvent(ev));
    this.term.onData((d) => this.onTermData(d));
    this.term.onBinary((d) => this.writeOwn(d, true));
    this.term.onBell(() => this.ringBell());
    this.term.onTitleChange((title) => {
      const p = useWorkspace.getState().panes[this.paneId];
      if (p && p.remoteTitle !== title) useWorkspace.getState().updatePane(this.paneId, { remoteTitle: title });
    });
    this.term.onSelectionChange(() => this.onSelectionChange());

    this.installDomHandlers();

    this.unsubs.push(
      useSettings.subscribe((s, prev) => {
        if (s.settings !== prev.settings) this.applyOptions();
      }),
      useSystemThemeStore.subscribe((s, prev) => {
        if (s.dark !== prev.dark) this.applyOptions();
      })
    );

    this.unregister = terminalRegistry.register(this.createHandle());
    this.bindConnection(pane);
  }

  /* ------------------------------------------------------------------------ */
  /* Events for the React overlay layer                                        */
  /* ------------------------------------------------------------------------ */

  on(ev: SessionEvent, cb: (arg?: unknown) => void): () => void {
    let set = this.listeners.get(ev);
    if (!set) this.listeners.set(ev, (set = new Set()));
    set.add(cb);
    return () => void set!.delete(cb);
  }

  private emit(ev: SessionEvent, arg?: unknown): void {
    this.listeners.get(ev)?.forEach((cb) => cb(arg));
  }

  /* ------------------------------------------------------------------------ */
  /* DOM attachment                                                            */
  /* ------------------------------------------------------------------------ */

  /** Moves the terminal into `container` (opening xterm on first attach). */
  attach(container: HTMLElement): void {
    if (this.disposed) return;
    this.container = container;
    if (this.element.parentElement !== container) container.appendChild(this.element);
    if (!this.opened) {
      this.term.open(this.screen);
      this.opened = true;
      this.applyOptions();  // padding / background / renderer for the first layout
    }
    this.resizeObserver?.disconnect();
    this.resizeObserver = new ResizeObserver(() => this.scheduleFit());
    this.resizeObserver.observe(this.element);
    this.scheduleFit();
    // A connection waiting for the first layout (pane created while no terminal was mounted).
    const p = useWorkspace.getState().panes[this.paneId];
    if (p) this.syncPane(p);
  }

  /** Detaches from `container` (if still attached there); the instance stays alive. */
  detach(container: HTMLElement | null): void {
    if (container && this.container !== container) return;
    this.resizeObserver?.disconnect();
    this.resizeObserver = null;
    if (container && this.element.parentElement === container) container.removeChild(this.element);
    this.container = null;
    this.setVisible(false);
  }

  setVisible(visible: boolean): void {
    if (this.visible === visible) return;
    this.visible = visible;
    if (visible) {
      const i = webglOwners.indexOf(this);
      if (i >= 0) webglOwners.push(...webglOwners.splice(i, 1));
      this.updateRenderer();
      this.scheduleFit();
      this.term.refresh(0, this.term.rows - 1);
    }
  }

  get isVisible(): boolean {
    return this.visible;
  }

  /* ------------------------------------------------------------------------ */
  /* Renderer (WebGL with DOM fallback)                                         */
  /* ------------------------------------------------------------------------ */

  get rendererKind(): 'webgl' | 'dom' {
    return this.webgl ? 'webgl' : 'dom';
  }

  private updateRenderer(): void {
    if (!this.opened || this.disposed) return;
    const want = useSettings.getState().settings.terminal.renderer !== 'dom' && !this.webglFailed;
    if (!want) {
      this.disableWebgl();
      return;
    }
    if (this.webgl || !this.visible) return;
    if (webglOwners.length >= MAX_WEBGL_CONTEXTS) {
      // Reclaim a context from a background pane; if every owner is visible, stay on the DOM renderer.
      const victim = webglOwners.find((s) => !s.visible);
      if (!victim) return;
      victim.disableWebgl();
    }
    try {
      const addon = new WebglAddon();
      addon.onContextLoss(() => {
        this.webglFailed = true;
        this.disableWebgl();
      });
      this.term.loadAddon(addon);
      this.webgl = addon;
      webglOwners.push(this);
      this.emit('renderer');
    } catch (e) {
      console.warn('[terminal] WebGL renderer unavailable, using DOM renderer', e);
      this.webglFailed = true;
    }
  }

  private disableWebgl(): void {
    if (!this.webgl) return;
    const addon = this.webgl;
    this.webgl = null;
    const i = webglOwners.indexOf(this);
    if (i >= 0) webglOwners.splice(i, 1);
    try {
      addon.dispose();
    } catch {
      /* already gone with its context */
    }
    if (!this.disposed) {
      this.term.refresh(0, this.term.rows - 1);
      this.emit('renderer');
    }
  }

  /* ------------------------------------------------------------------------ */
  /* Options                                                                   */
  /* ------------------------------------------------------------------------ */

  private resolveScheme(): ColorScheme {
    return resolveTerminalScheme(useSettings.getState().settings, this.session, useSystemThemeStore.getState().dark);
  }

  get colorScheme(): ColorScheme {
    return this.scheme;
  }

  private baseFontSize(): number {
    const t = useSettings.getState().settings.terminal;
    return this.session.appearance?.fontSize || t.fontSize || 14;
  }

  private computeOptions(): ITerminalOptions {
    const st = useSettings.getState().settings;
    const t = st.terminal;
    const ap = this.session.appearance ?? {};
    return {
      fontFamily: ap.fontFamily || t.fontFamily,
      fontSize: Math.min(MAX_FONT, Math.max(MIN_FONT, this.baseFontSize() + this.zoomDelta)),
      lineHeight: t.lineHeight > 0 ? t.lineHeight : 1,
      letterSpacing: t.letterSpacing,
      fontWeight: t.fontWeight,
      fontWeightBold: t.fontWeightBold,
      cursorStyle: ap.cursorStyle || t.cursorStyle,
      cursorBlink: ap.cursorBlink ?? t.cursorBlink,
      scrollback: Math.max(0, this.session.terminal.scrollbackLines || t.scrollback),
      theme: toXtermTheme(this.scheme),
      drawBoldTextInBrightColors: ap.boldIsBright ?? t.drawBoldTextInBrightColors,
      minimumContrastRatio: t.minimumContrastRatio || 1,
      wordSeparator: st.behavior.wordSeparators,
      convertEol: this.session.terminal.implicitCrInLf,
      scrollOnUserInput: st.behavior.scrollOnKeystroke
    };
  }

  private rememberOptions(o: ITerminalOptions): void {
    for (const [k, v] of Object.entries(o)) this.appliedOptions.set(k, JSON.stringify(v));
  }

  /** Re-applies settings/session/theme to xterm, touching only options that changed. */
  applyOptions(): void {
    if (this.disposed) return;
    this.scheme = this.resolveScheme();
    const next = this.computeOptions();
    let metricsChanged = false;
    for (const [k, v] of Object.entries(next) as [keyof ITerminalOptions, unknown][]) {
      const json = JSON.stringify(v);
      if (this.appliedOptions.get(k) === json) continue;
      this.appliedOptions.set(k, json);
      (this.term.options as Record<string, unknown>)[k] = v;
      if (k === 'fontFamily' || k === 'fontSize' || k === 'lineHeight' || k === 'letterSpacing') metricsChanged = true;
    }
    this.element.style.padding = `${Math.max(0, useSettings.getState().settings.terminal.padding)}px`;
    this.element.style.background = this.scheme.background;
    this.updateRenderer();
    if (metricsChanged) this.scheduleFit();
  }

  /** Per-pane font zoom: +1 / -1 px, 0 resets to the configured size. */
  zoom(delta: number): void {
    if (delta === 0) this.zoomDelta = 0;
    else {
      const base = this.baseFontSize();
      const size = Math.min(MAX_FONT, Math.max(MIN_FONT, base + this.zoomDelta + Math.sign(delta)));
      this.zoomDelta = size - base;
    }
    this.applyOptions();
  }

  /* ------------------------------------------------------------------------ */
  /* Fit / resize                                                              */
  /* ------------------------------------------------------------------------ */

  scheduleFit(): void {
    if (this.fitFrame || this.disposed) return;
    this.fitFrame = requestAnimationFrame(() => {
      this.fitFrame = 0;
      this.fit();
    });
  }

  fit(): void {
    if (!this.opened || this.disposed || !this.element.isConnected) return;
    if (this.element.clientWidth < 4 || this.element.clientHeight < 4) return; // hidden tab
    const dims = this.fitAddon.proposeDimensions();
    if (!dims || !Number.isFinite(dims.cols) || !Number.isFinite(dims.rows)) return;
    const cols = Math.max(2, dims.cols);
    const rows = Math.max(1, dims.rows);
    if (cols !== this.term.cols || rows !== this.term.rows) this.term.resize(cols, rows);
    this.scheduleRemoteResize();
  }

  private pixelSize(): { widthPx: number; heightPx: number } {
    return { widthPx: Math.round(this.screen.clientWidth), heightPx: Math.round(this.screen.clientHeight) };
  }

  private scheduleRemoteResize(): void {
    clearTimeout(this.resizeTimer);
    this.resizeTimer = setTimeout(() => {
      if (this.disposed || !this.connId || !openedConns.has(this.connId) || isEnded(this.status)) return;
      const key = `${this.term.cols}x${this.term.rows}`;
      if (key === this.lastSentSize) return;
      this.lastSentSize = key;
      const { widthPx, heightPx } = this.pixelSize();
      api.conn.resize(this.connId, this.term.cols, this.term.rows, widthPx, heightPx);
    }, RESIZE_DEBOUNCE_MS);
  }

  /* ------------------------------------------------------------------------ */
  /* Connection lifecycle                                                       */
  /* ------------------------------------------------------------------------ */

  private bindConnection(pane: PaneState): void {
    if (pane.connId === this.connId) return;
    const old = this.connId;
    if (old) {
      this.connUnsub?.();
      this.connUnsub = null;
      if (openedConns.has(old) && !isEnded(lastStatusOf(old)?.status)) api.conn.close(old);
      openedConns.delete(old);
      forgetConn(old);
      // Restarting in place: leave the old screen contents, reset modes the previous session may have set.
      if (this.opened) this.term.write('\x1b[0m\x1b[?1049l\x1b[?1000l\x1b[?1002l\x1b[?1003l\x1b[?1006l\x1b[?2004l\x1b[?7h\r\n');
    }
    this.connId = pane.connId;
    this.lastSentSize = '';
    this.userClosed = false;
    this.connUnsub = subscribeConn(this.connId, {
      onData: (d) => this.onConnData(d),
      onStatus: (ev) => this.onConnStatus(ev)
    });
  }

  /** Called on every workspace change for this pane (and on attach). */
  syncPane(pane: PaneState): void {
    if (this.disposed) return;
    if (pane.session !== this.session) {
      this.session = pane.session;
      this.applyOptions();
    }
    this.bindConnection(pane);
    this.status = pane.status;
    if (pane.status === 'idle' && this.opened && this.element.isConnected && !openedConns.has(pane.connId)) {
      this.openConnection(pane);
    }
  }

  private openConnection(pane: PaneState): void {
    const connId = pane.connId;
    openedConns.add(connId);
    this.cancelReconnect();
    this.fit();
    const { widthPx, heightPx } = this.pixelSize();
    this.lastSentSize = `${this.term.cols}x${this.term.rows}`;
    useWorkspace.getState().updatePaneByConnId(connId, { status: 'connecting', statusMessage: undefined, oneTimePassword: undefined });
    api.conn
      .open({
        connId,
        session: pane.session,
        cols: this.term.cols,
        rows: this.term.rows,
        widthPx,
        heightPx,
        password: pane.oneTimePassword || undefined
      })
      .catch((err: unknown) => {
        const message = err instanceof Error ? err.message : String(err);
        useWorkspace.getState().updatePaneByConnId(connId, { status: 'error', statusMessage: message });
      });
  }

  private onConnStatus(ev: ConnStatusEvent): void {
    if (this.disposed || ev.connId !== this.connId) return;
    this.status = ev.status;
    if (ev.status === 'connected') {
      this.reconnectArmed = true;
      this.reconnectAttempt = 0;
      this.cancelReconnect();
      if (!this.session.terminal.autoWrap) this.term.write('\x1b[?7l');
      // The size may have changed while connecting.
      this.lastSentSize = '';
      this.scheduleRemoteResize();
      return;
    }
    if (!isEnded(ev.status) || this.userClosed) return;

    const clean = ev.status === 'disconnected' && ev.clean === true;
    const closeOnExit = this.session.terminal.closeOnExit;
    if (closeOnExit === 'always' || (closeOnExit === 'clean' && clean)) {
      if (ev.status === 'error' && ev.message) toast.error('Connection failed', ev.message);
      setTimeout(() => useWorkspace.getState().closePane(this.paneId), 0);
      return;
    }
    if (!clean && this.reconnectArmed && useSettings.getState().settings.behavior.reconnect === 'auto') {
      this.scheduleReconnect();
    }
  }

  private scheduleReconnect(): void {
    this.cancelReconnect();
    const delay = reconnectDelayMs(this.reconnectAttempt++);
    this.reconnectAt = Date.now() + delay;
    this.reconnectTimer = setTimeout(() => {
      this.reconnectAt = null;
      this.reconnectTimer = undefined;
      this.emit('reconnect');
      if (!this.disposed && isEnded(useWorkspace.getState().panes[this.paneId]?.status)) this.restart(true);
    }, delay);
    this.emit('reconnect');
  }

  cancelReconnect(): void {
    if (this.reconnectTimer === undefined && this.reconnectAt === null) return;
    clearTimeout(this.reconnectTimer);
    this.reconnectTimer = undefined;
    this.reconnectAt = null;
    this.emit('reconnect');
  }

  /** Restart the session in this pane (new generation). `auto` keeps the reconnect backoff going. */
  restart(auto = false): void {
    if (!auto) {
      this.reconnectAttempt = 0;
      this.cancelReconnect();
    }
    useWorkspace.getState().restartPane(this.paneId);
  }

  /** User-initiated disconnect (Cancel on the connecting card). */
  cancelConnection(): void {
    this.userClosed = true;
    this.reconnectArmed = false;
    this.cancelReconnect();
    for (const p of usePrompts.getState().pending) {
      if (p.connId === this.connId) usePrompts.getState().respond(p.id, { cancelled: true });
    }
    if (this.connId) api.conn.close(this.connId);
  }

  /** Close the pane (Close button on the banner). */
  closePane(): void {
    this.userClosed = true;
    this.reconnectArmed = false;
    this.cancelReconnect();
    useWorkspace.getState().closePane(this.paneId);
  }

  get currentConnId(): string {
    return this.connId;
  }

  /* ------------------------------------------------------------------------ */
  /* Output                                                                    */
  /* ------------------------------------------------------------------------ */

  private onConnData(data: string): void {
    if (this.disposed) return;
    const term = this.session.terminal;
    const enq = countEnq(data);
    if (enq && term.answerback) {
      const answer = decodeCaretNotation(term.answerback);
      for (let i = 0; i < enq; i++) this.writeOwn(answer);
    }
    const out = term.implicitLfInCr ? implicitLfInCr(data) : data;
    if (useSettings.getState().settings.behavior.scrollOnOutput) this.term.write(out, () => this.term.scrollToBottom());
    else this.term.write(out);
    if (!this.visible) {
      const p = useWorkspace.getState().panes[this.paneId];
      if (p && !p.hasActivity) useWorkspace.getState().updatePane(this.paneId, { hasActivity: true });
    }
  }

  private ringBell(): void {
    const mode = this.session.terminal.bell;
    if (mode === 'none' || !this.bell.ring(Date.now())) return;
    if (mode === 'visual') {
      this.bellFlash.classList.remove('term-bell--on');
      void this.bellFlash.offsetWidth; // restart the animation
      this.bellFlash.classList.add('term-bell--on');
    } else if (mode === 'system') {
      api.app.beep();
    } else if (mode === 'sound') {
      const path = this.session.terminal.bellSoundPath;
      if (!path) api.app.beep();
      else
        void bellAudio(path).then((audio) => {
          if (!audio) return api.app.beep();
          audio.currentTime = 0;
          audio.play().catch(() => api.app.beep());
        });
    }
    if (!this.visible) {
      const p = useWorkspace.getState().panes[this.paneId];
      if (p && !p.bell) useWorkspace.getState().updatePane(this.paneId, { bell: true });
    }
    if (!useUi.getState().windowFocused) api.window.flash();
  }

  /* ------------------------------------------------------------------------ */
  /* Input                                                                     */
  /* ------------------------------------------------------------------------ */

  private onKeyEvent(ev: KeyboardEvent): boolean {
    if (ev.type !== 'keydown') return true;
    if (ev.isComposing || ev.keyCode === 229) return true;
    this.bell.reset();
    if ((ev.key === 'ContextMenu' || (ev.key === 'F10' && ev.shiftKey)) && !ev.ctrlKey && !ev.altKey) {
      ev.preventDefault();
      this.emit('contextmenu');
      return false;
    }
    const action = translateKey(
      {
        key: ev.key,
        code: ev.code,
        shiftKey: ev.shiftKey,
        ctrlKey: ev.ctrlKey,
        altKey: ev.altKey,
        metaKey: ev.metaKey,
        altGraph: ev.getModifierState?.('AltGraph') ?? false
      },
      this.session.keyboard,
      { applicationCursorKeys: this.term.modes.applicationCursorKeysMode, applicationKeypad: this.term.modes.applicationKeypadMode }
    );
    switch (action.kind) {
      case 'default':
        return true;
      case 'send':
        if (useSettings.getState().settings.behavior.scrollOnKeystroke) this.term.scrollToBottom();
        this.sendInput(action.data);
        break;
      case 'copy':
        void this.copySelection();
        break;
      case 'paste':
        void this.paste();
        break;
      case 'scroll':
        if (action.pages) this.term.scrollPages(action.pages);
        if (action.lines) this.term.scrollLines(action.lines);
        break;
      case 'special':
        if (this.status === 'connected') api.conn.special(this.connId, action.id);
        break;
    }
    ev.preventDefault();
    ev.stopPropagation();
    return false;
  }

  private onTermData(data: string): void {
    if (isTerminalReport(data)) this.writeOwn(data);
    else this.sendInput(data);
  }

  /** Writes to this pane's connection only (terminal reports, answerback, binary mouse reports). */
  private writeOwn(data: string, binary = false): void {
    if (this.status !== 'connected' || !this.connId) return;
    api.conn.write(this.connId, data, binary || undefined);
  }

  /** User input: goes to every connected pane of the tab when broadcasting, else to this pane. */
  sendInput(data: string): void {
    if (this.disposed || !data) return;
    this.bell.reset();
    if (isEnded(this.status)) {
      // Banner shortcut: Enter / R restarts a finished session.
      if (data === '\r' || data === 'r' || data === 'R') this.restart();
      return;
    }
    const ws = useWorkspace.getState();
    const pane = ws.panes[this.paneId];
    const tab = pane && ws.tabs.find((t) => t.id === pane.tabId);
    if (tab?.broadcast) {
      for (const id of paneIdsOf(tab.layout)) {
        const p = ws.panes[id];
        if (p && p.status === 'connected') api.conn.write(p.connId, data);
      }
      return;
    }
    this.writeOwn(data);
  }

  /* ------------------------------------------------------------------------ */
  /* Clipboard / selection                                                     */
  /* ------------------------------------------------------------------------ */

  private onSelectionChange(): void {
    clearTimeout(this.copyTimer);
    if (!useSettings.getState().settings.behavior.copyOnSelect) return;
    this.copyTimer = setTimeout(() => {
      if (!this.disposed && this.term.hasSelection()) {
        const text = this.term.getSelection();
        if (text) api.app.writeClipboard(text);
      }
    }, 120);
  }

  async copySelection(): Promise<boolean> {
    const text = this.term.getSelection();
    if (!text) return false;
    api.app.writeClipboard(text);
    return true;
  }

  allText(): string {
    const buf = this.term.buffer.active;
    return bufferToText(buf.length, (i) => buf.getLine(i));
  }

  async copyAll(): Promise<void> {
    api.app.writeClipboard(this.allText());
    toast.success('Copied', 'Terminal contents copied to the clipboard.');
  }

  /** Paste clipboard text (or `text`), with the multi-line warning; xterm applies bracketed paste. */
  async paste(text?: string): Promise<void> {
    if (this.disposed) return;
    let data = text;
    if (data === undefined) {
      try {
        data = await api.app.readClipboard();
      } catch {
        data = '';
      }
    }
    if (!data) return;
    if (useSettings.getState().settings.behavior.warnOnMultilinePaste && isMultiline(data) && !this.term.modes.bracketedPasteMode) {
      const { preview, lines } = pastePreview(data);
      const ok = await confirmDialog({
        title: 'Paste multiple lines?',
        message: `You are about to paste ${lines} lines into the terminal. Each line may run as a command.\n\n${preview}`,
        confirmLabel: 'Paste'
      });
      if (!ok || this.disposed) return;
    }
    this.term.paste(data);
    this.focus();
  }

  /* ------------------------------------------------------------------------ */
  /* Search                                                                    */
  /* ------------------------------------------------------------------------ */

  private searchOptions(opts: FindOptions, incremental: boolean): ISearchOptions {
    const s = this.scheme;
    const bg = toHex6(s.background) ?? '#000000';
    const match = mixHex(toHex6(s.yellow) ?? '#c0a000', bg, 0.55);
    const active = toHex6(s.brightYellow) ?? '#ffd700';
    return {
      caseSensitive: opts.caseSensitive,
      wholeWord: opts.wholeWord,
      regex: opts.regex,
      incremental,
      decorations: {
        matchBackground: match,
        matchBorder: match,
        matchOverviewRuler: match,
        activeMatchBackground: active,
        activeMatchBorder: toHex6(s.foreground) ?? '#ffffff',
        activeMatchColorOverviewRuler: active
      }
    };
  }

  /** Runs a search; returns false when nothing matched. Throws on an invalid regular expression. */
  find(query: string, opts: FindOptions, direction: 'next' | 'prev', incremental = false): boolean {
    if (!query) {
      this.searchAddon.clearDecorations();
      this.term.clearSelection();
      return false;
    }
    if (opts.regex) new RegExp(query); // validate (throws SyntaxError)
    const o = this.searchOptions(opts, incremental);
    return direction === 'next' ? this.searchAddon.findNext(query, o) : this.searchAddon.findPrevious(query, o);
  }

  onSearchResults(cb: (r: { resultIndex: number; resultCount: number }) => void): () => void {
    const d = this.searchAddon.onDidChangeResults(cb);
    return () => d.dispose();
  }

  clearSearch(): void {
    this.searchAddon.clearDecorations();
  }

  openFind(): void {
    this.emit('find');
  }

  /* ------------------------------------------------------------------------ */
  /* Misc                                                                      */
  /* ------------------------------------------------------------------------ */

  focus(): void {
    if (!this.disposed && this.opened) this.term.focus();
  }

  hasSelection(): boolean {
    return this.term.hasSelection();
  }

  clearScrollback(): void {
    this.term.clear();
  }

  resetTerminal(): void {
    this.term.reset();
    if (!this.session.terminal.autoWrap) this.term.write('\x1b[?7l');
  }

  private installDomHandlers(): void {
    const el = this.element;
    el.addEventListener(
      'contextmenu',
      (e) => {
        e.preventDefault();
        if (this.term.modes.mouseTrackingMode !== 'none' && !e.shiftKey) return; // the application owns the mouse
        const b = useSettings.getState().settings.behavior;
        if (e.shiftKey || b.rightClick === 'menu') this.emit('contextmenu', { x: e.clientX, y: e.clientY });
        else void this.paste();
      },
      true
    );
    el.addEventListener(
      'mousedown',
      (e) => {
        this.activatePane();
        if (e.button === 1 && useSettings.getState().settings.behavior.middleClickPaste) {
          if (this.term.modes.mouseTrackingMode !== 'none' && !e.shiftKey) return;
          e.preventDefault();
          e.stopPropagation();
          void this.paste();
        }
      },
      true
    );
    el.addEventListener('auxclick', (e) => {
      if (e.button === 1) e.preventDefault();
    });
    el.addEventListener('focusin', () => this.activatePane());
    el.addEventListener(
      'wheel',
      (e) => {
        if (!e.ctrlKey) return;
        e.preventDefault();
        e.stopPropagation();
        // One notch is ~100 (pixel mode) or 1-3 (line mode); touchpads send small deltas that accumulate.
        const dy = e.deltaMode === 1 ? e.deltaY * 33 : e.deltaY;
        this.wheelAccum += dy;
        if (Math.abs(this.wheelAccum) >= 50) {
          this.zoom(this.wheelAccum < 0 ? 1 : -1);
          this.wheelAccum = 0;
        }
      },
      { capture: true, passive: false }
    );
  }

  private activatePane(): void {
    const ws = useWorkspace.getState();
    const pane = ws.panes[this.paneId];
    if (!pane) return;
    const tab = ws.tabs.find((t) => t.id === pane.tabId);
    if (tab && (tab.activePaneId !== this.paneId || ws.activeTabId !== tab.id)) ws.activatePane(this.paneId);
  }

  private createHandle(): TerminalHandle {
    return {
      paneId: this.paneId,
      focus: () => this.focus(),
      copySelection: () => this.copySelection(),
      paste: (text?: string) => this.paste(text),
      copyAll: () => this.copyAll(),
      selectAll: () => this.term.selectAll(),
      clearScrollback: () => this.clearScrollback(),
      reset: () => this.resetTerminal(),
      openFind: () => this.openFind(),
      zoom: (delta: number) => this.zoom(delta),
      scrollToBottom: () => this.term.scrollToBottom(),
      sendText: (text: string) => this.sendInput(text),
      getSelection: () => this.term.getSelection(),
      fit: () => this.fit()
    };
  }

  dispose(): void {
    if (this.disposed) return;
    this.disposed = true;
    sessions.delete(this.paneId);
    this.unregister();
    this.unsubs.forEach((u) => u());
    clearTimeout(this.resizeTimer);
    clearTimeout(this.copyTimer);
    clearTimeout(this.reconnectTimer);
    if (this.fitFrame) cancelAnimationFrame(this.fitFrame);
    this.resizeObserver?.disconnect();
    for (const p of usePrompts.getState().pending) {
      if (p.connId === this.connId && PANE_PROMPT_KINDS.has(p.kind)) usePrompts.getState().respond(p.id, { cancelled: true });
    }
    if (this.connId) {
      if (openedConns.has(this.connId) && !isEnded(lastStatusOf(this.connId)?.status)) api.conn.close(this.connId);
      openedConns.delete(this.connId);
      this.connUnsub?.();
      forgetConn(this.connId);
    }
    this.disableWebgl();
    this.listeners.clear();
    this.term.dispose();
    this.element.remove();
  }
}
