/**
 * Single router for connection events. Subscribes ONCE to conn:onData / conn:onStatus / conn:onEventLog and
 * dispatches by connId, so N panes do not mean N bridge listeners and no React component re-renders per data
 * chunk (terminals write directly into xterm.js).
 *
 *  - status events are mirrored into the workspace store (useWorkspace.updatePaneByConnId), whether or not a
 *    terminal is mounted for the pane;
 *  - data that arrives before a listener is attached is buffered (bounded) and flushed on subscribe;
 *  - the latest status / event log line per connection is kept for overlays (connecting card, banner);
 *  - auth prompts (username/password/kbd-int/passphrase) for a pane in a background tab activate that pane.
 */
import { api } from '@/bridge/api';
import { usePrompts } from '@/store/data';
import { paneIdOfConn, useWorkspace, type PaneState } from '@/store/workspace';
import type { ConnStatusEvent, EventLogEntry, PromptRequest } from '@shared/types';

export interface ConnListener {
  onData?(data: string): void;
  onStatus?(ev: ConnStatusEvent): void;
  onEventLog?(entry: EventLogEntry): void;
}

/** Kinds rendered inside the pane (hostkey / confirm prompts are rendered globally by the dialogs module). */
export const PANE_PROMPT_KINDS: ReadonlySet<PromptRequest['kind']> = new Set(['username', 'password', 'kbdint', 'passphrase']);

const MAX_PENDING_CHARS = 1 << 20;

const listeners = new Map<string, Set<ConnListener>>();
const lastStatus = new Map<string, ConnStatusEvent>();
const lastLog = new Map<string, EventLogEntry>();
const pending = new Map<string, { chunks: string[]; size: number }>();
const logWatchers = new Set<() => void>();
let initialized = false;

function dispatch<K extends keyof ConnListener>(connId: string, kind: K, arg: Parameters<NonNullable<ConnListener[K]>>[0]) {
  const set = listeners.get(connId);
  if (!set) return false;
  let delivered = false;
  for (const l of [...set]) {
    const fn = l[kind] as ((a: typeof arg) => void) | undefined;
    if (fn) {
      delivered = true;
      try {
        fn.call(l, arg);
      } catch (e) {
        console.error('[terminal] listener failed', e);
      }
    }
  }
  return delivered;
}

/** Idempotent: wires the bridge subscriptions on first use. */
export function initConnEvents(): void {
  if (initialized) return;
  initialized = true;

  api.conn.onData((connId, data) => {
    if (typeof data !== 'string' || !data) return;
    if (dispatch(connId, 'onData', data)) return;
    // No terminal attached yet: keep the output until one subscribes (bounded, oldest dropped).
    let buf = pending.get(connId);
    if (!buf) pending.set(connId, (buf = { chunks: [], size: 0 }));
    buf.chunks.push(data);
    buf.size += data.length;
    while (buf.size > MAX_PENDING_CHARS && buf.chunks.length > 1) buf.size -= buf.chunks.shift()!.length;
  });

  api.conn.onStatus((ev) => {
    if (!ev || typeof ev.connId !== 'string') return;
    lastStatus.set(ev.connId, ev);
    const patch: Partial<PaneState> = {
      status: ev.status,
      statusMessage: ev.message
    };
    if (ev.serverInfo) patch.serverInfo = ev.serverInfo;
    useWorkspace.getState().updatePaneByConnId(ev.connId, patch);
    dispatch(ev.connId, 'onStatus', ev);
  });

  api.conn.onEventLog((connId, entry) => {
    if (!entry) return;
    lastLog.set(connId, entry);
    dispatch(connId, 'onEventLog', entry);
    logWatchers.forEach((w) => w());
  });

  // Bring panes with a pending authentication prompt to the front.
  const seen = new Set<string>();
  const activateForPrompts = (list: PromptRequest[]) => {
    const live = new Set(list.map((p) => p.id));
    for (const id of [...seen]) if (!live.has(id)) seen.delete(id);
    for (const p of list) {
      if (seen.has(p.id)) continue;
      seen.add(p.id);
      if (!PANE_PROMPT_KINDS.has(p.kind)) continue;
      const ws = useWorkspace.getState();
      const pane = ws.panes[paneIdOfConn(p.connId)];
      if (!pane || pane.connId !== p.connId) continue;
      const tab = ws.tabs.find((t) => t.id === pane.tabId);
      if (ws.activeTabId !== pane.tabId || tab?.activePaneId !== pane.id) ws.activatePane(pane.id);
    }
  };
  activateForPrompts(usePrompts.getState().pending);
  usePrompts.subscribe((s, prev) => {
    if (s.pending !== prev.pending) activateForPrompts(s.pending);
  });
}

/** Attach a listener for one connection. Buffered output is flushed to the first data listener. */
export function subscribeConn(connId: string, l: ConnListener): () => void {
  initConnEvents();
  let set = listeners.get(connId);
  if (!set) listeners.set(connId, (set = new Set()));
  set.add(l);
  if (l.onData) {
    const buf = pending.get(connId);
    if (buf) {
      pending.delete(connId);
      l.onData(buf.chunks.join(''));
    }
  }
  return () => {
    const s = listeners.get(connId);
    if (!s) return;
    s.delete(l);
    if (!s.size) listeners.delete(connId);
  };
}

export function lastStatusOf(connId: string): ConnStatusEvent | undefined {
  return lastStatus.get(connId);
}

export function lastEventLogOf(connId: string): EventLogEntry | undefined {
  return lastLog.get(connId);
}

/** Notified on every event log entry (any connection); used by overlays showing the latest line. */
export function watchEventLog(cb: () => void): () => void {
  logWatchers.add(cb);
  return () => void logWatchers.delete(cb);
}

/** Drop cached state of a finished connection (after the pane restarted or closed). */
export function forgetConn(connId: string): void {
  listeners.delete(connId);
  lastStatus.delete(connId);
  lastLog.delete(connId);
  pending.delete(connId);
}

/**
 * Event log of a connection: the host's history plus live entries, without duplicates even when entries
 * arrive while the history request is in flight. `onChange` receives the full list each time.
 */
export function followEventLog(connId: string, onChange: (entries: EventLogEntry[]) => void): () => void {
  let entries: EventLogEntry[] | null = null;
  const early: EventLogEntry[] = [];
  let disposed = false;
  const unsub = subscribeConn(connId, {
    onEventLog(e) {
      if (entries) {
        entries = [...entries, e];
        onChange(entries);
      } else early.push(e);
    }
  });
  api.conn
    .eventLog(connId)
    .then((list) => {
      if (disposed) return;
      const base = Array.isArray(list) ? list : [];
      const key = (e: EventLogEntry) => `${e.time}\u0000${e.level}\u0000${e.message}`;
      const known = new Set(base.map(key));
      entries = [...base, ...early.filter((e) => !known.has(key(e)))];
      onChange(entries);
    })
    .catch(() => {
      if (disposed) return;
      entries = [...early];
      onChange(entries);
    });
  return () => {
    disposed = true;
    unsub();
  };
}
