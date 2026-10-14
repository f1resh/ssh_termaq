/**
 * Workspace = open tabs, each holding a split-pane layout tree of panes.
 * A terminal pane owns one connection. A pane's connection id is `${paneId}#${generation}`; restarting a pane
 * bumps the generation so late events from the previous connection are ignored.
 * A files pane (the file manager, always alone in its tab) either borrows the connection of a terminal pane
 * (`linkedPaneId`) or owns a shell-less SSH connection of its own.
 */
import { create } from 'zustand';
import { newId, sessionLabel } from '@shared/defaults';
import type { ConnStatus, SessionConfig, SessionView } from '@shared/types';

/** 'row' = children side by side (split right), 'column' = stacked (split down). */
export type SplitDirection = 'row' | 'column';

export type LayoutNode =
  | { type: 'pane'; paneId: string }
  | { type: 'split'; id: string; direction: SplitDirection; children: LayoutNode[]; sizes: number[] };

export type PaneStatus = ConnStatus | 'idle';

export type PaneKind = 'terminal' | 'files';

/** File manager folders (kept with the workspace so a restored tab reopens where it was). */
export interface FilesPaneState {
  localPath?: string;
  remotePath?: string;
}

export interface PaneState {
  id: string;
  tabId: string;
  kind: PaneKind;
  /** Files panes opened from a terminal: the terminal pane whose connection they use. */
  linkedPaneId?: string;
  files?: FilesPaneState;
  /** Saved session id; null for ad-hoc (quick connect / CLI). */
  sessionId: string | null;
  /** Effective configuration used for the connection (snapshot at open/restart time). */
  session: SessionConfig;
  generation: number;
  connId: string;
  status: PaneStatus;
  statusMessage?: string;
  serverInfo?: string;
  /** Title set by the remote via OSC 0/2. */
  remoteTitle?: string;
  /** Output arrived while the pane was not visible. */
  hasActivity: boolean;
  /** Bell rang while the pane was not visible. */
  bell: boolean;
  /** One-time password (CLI -pw); consumed by the first connection attempt. */
  oneTimePassword?: string;
}

export interface TabState {
  id: string;
  layout: LayoutNode;
  activePaneId: string;
  customTitle?: string;
  color?: string;
  /** Typing in the active pane is sent to all panes of this tab. */
  broadcast: boolean;
}

export type OpenTarget = 'tab' | 'split-right' | 'split-down';

export interface OpenOptions {
  target?: OpenTarget;
  /** Saved session id (defaults to session.id when the session exists in the sessions store). */
  sessionId?: string | null;
  password?: string;
  /** Pane to split beside (defaults to the active pane). */
  besidePaneId?: string;
  /** 'files' opens the file manager (always in a new tab). */
  view?: SessionView;
  /** Files view: use this terminal pane's connection. */
  linkedPaneId?: string;
  remotePath?: string;
  localPath?: string;
}

export interface WorkspaceState {
  tabs: TabState[];
  activeTabId: string | null;
  panes: Record<string, PaneState>;

  openSession(session: SessionConfig, opts?: OpenOptions): string;
  closeTab(tabId: string): void;
  closePane(paneId: string): void;
  closeAll(): void;
  activateTab(tabId: string): void;
  activatePane(paneId: string): void;
  moveTab(fromIndex: number, toIndex: number): void;
  renameTab(tabId: string, title?: string): void;
  setTabColor(tabId: string, color?: string): void;
  toggleBroadcast(tabId: string): void;
  splitPane(paneId: string, direction: SplitDirection, session?: SessionConfig, sessionId?: string | null): string;
  duplicatePane(paneId: string, target: OpenTarget): string | undefined;
  restartPane(paneId: string): void;
  resizeSplit(splitId: string, sizes: number[]): void;
  updatePane(paneId: string, patch: Partial<PaneState>): void;
  /** Applies only if connId is the pane's current connection. */
  updatePaneByConnId(connId: string, patch: Partial<PaneState>): void;
  focusAdjacentPane(delta: 1 | -1): void;
  nextTab(delta: 1 | -1): void;
  gotoTab(index: number): void;
}

export const connIdOf = (paneId: string, generation: number) => `${paneId}#${generation}`;
export const paneIdOfConn = (connId: string) => connId.split('#')[0];

export function paneIdsOf(node: LayoutNode): string[] {
  return node.type === 'pane' ? [node.paneId] : node.children.flatMap(paneIdsOf);
}

/** Removes a pane; collapses splits with a single child. Returns null when the tree becomes empty. */
function removePane(node: LayoutNode, paneId: string): LayoutNode | null {
  if (node.type === 'pane') return node.paneId === paneId ? null : node;
  const kept: LayoutNode[] = [];
  const sizes: number[] = [];
  node.children.forEach((c, i) => {
    const r = removePane(c, paneId);
    if (r) {
      kept.push(r);
      sizes.push(node.sizes[i] ?? 1);
    }
  });
  if (kept.length === 0) return null;
  if (kept.length === 1) return kept[0];
  const total = sizes.reduce((a, b) => a + b, 0) || 1;
  return { ...node, children: kept, sizes: sizes.map((s) => s / total) };
}

/** Insert `newPaneId` next to `paneId` in `direction`, flattening into an existing split of the same direction. */
function insertBeside(node: LayoutNode, paneId: string, newPaneId: string, direction: SplitDirection): LayoutNode {
  if (node.type === 'pane') {
    if (node.paneId !== paneId) return node;
    return {
      type: 'split',
      id: newId(),
      direction,
      children: [node, { type: 'pane', paneId: newPaneId }],
      sizes: [0.5, 0.5]
    };
  }
  const idx = node.children.findIndex((c) => c.type === 'pane' && c.paneId === paneId);
  if (idx >= 0 && node.direction === direction) {
    const children = [...node.children];
    children.splice(idx + 1, 0, { type: 'pane', paneId: newPaneId });
    const sizes = [...node.sizes];
    const half = (sizes[idx] ?? 1 / node.children.length) / 2;
    sizes.splice(idx, 1, half, half);
    return { ...node, children, sizes };
  }
  return { ...node, children: node.children.map((c) => insertBeside(c, paneId, newPaneId, direction)) };
}

function mapSplit(node: LayoutNode, splitId: string, fn: (n: Extract<LayoutNode, { type: 'split' }>) => LayoutNode): LayoutNode {
  if (node.type === 'pane') return node;
  if (node.id === splitId) return fn(node);
  return { ...node, children: node.children.map((c) => mapSplit(c, splitId, fn)) };
}

function makePane(tabId: string, session: SessionConfig, sessionId: string | null, password?: string, extra: Partial<PaneState> = {}): PaneState {
  const id = newId();
  return {
    kind: 'terminal',
    ...extra,
    id,
    tabId,
    sessionId,
    session: structuredClone(session),
    generation: 1,
    connId: connIdOf(id, 1),
    status: 'idle',
    hasActivity: false,
    bell: false,
    oneTimePassword: password
  };
}

/** Title shown on a tab: custom > fixed window title > remote title > session label. */
export function tabTitle(tab: TabState, panes: Record<string, PaneState>): string {
  if (tab.customTitle) return tab.customTitle;
  const p = panes[tab.activePaneId];
  if (!p) return 'Tab';
  return paneTitle(p);
}

export function paneTitle(p: PaneState): string {
  if (p.kind === 'files') return sessionLabel(p.session) || p.session.host || 'Files';
  if (p.session.terminal.windowTitle) return p.session.terminal.windowTitle;
  if (p.remoteTitle && p.session.terminal.allowRemoteTitle) return p.remoteTitle;
  return sessionLabel(p.session) || p.session.host || 'Session';
}

export const useWorkspace = create<WorkspaceState>((set, get) => ({
  tabs: [],
  activeTabId: null,
  panes: {},

  openSession(session, opts = {}) {
    const files = opts.view === 'files';
    const target = files ? 'tab' : (opts.target ?? 'tab');
    const sessionId = opts.sessionId === undefined ? session.id ?? null : opts.sessionId;
    const st = get();
    if (target !== 'tab' && st.panes[opts.besidePaneId ?? '']?.kind !== 'files') {
      const tab = st.tabs.find((t) => t.id === st.activeTabId);
      const beside = opts.besidePaneId ?? tab?.activePaneId;
      if (tab && beside && st.panes[beside]) {
        const p = makePane(st.panes[beside].tabId, session, sessionId, opts.password);
        const direction: SplitDirection = target === 'split-right' ? 'row' : 'column';
        set((s) => ({
          panes: { ...s.panes, [p.id]: p },
          tabs: s.tabs.map((t) =>
            t.id === p.tabId ? { ...t, layout: insertBeside(t.layout, beside, p.id, direction), activePaneId: p.id } : t
          ),
          activeTabId: p.tabId
        }));
        return p.id;
      }
    }
    const tabId = newId();
    const p = makePane(
      tabId,
      session,
      sessionId,
      opts.password,
      files ? { kind: 'files', linkedPaneId: opts.linkedPaneId, files: { remotePath: opts.remotePath, localPath: opts.localPath } } : {}
    );
    const tab: TabState = {
      id: tabId,
      layout: { type: 'pane', paneId: p.id },
      activePaneId: p.id,
      color: session.color || undefined,
      broadcast: false
    };
    set((s) => ({ tabs: [...s.tabs, tab], panes: { ...s.panes, [p.id]: p }, activeTabId: tabId }));
    return p.id;
  },

  closeTab(tabId) {
    set((s) => {
      const tab = s.tabs.find((t) => t.id === tabId);
      if (!tab) return s;
      const ids = new Set(paneIdsOf(tab.layout));
      const panes = Object.fromEntries(Object.entries(s.panes).filter(([id]) => !ids.has(id)));
      const idx = s.tabs.findIndex((t) => t.id === tabId);
      const tabs = s.tabs.filter((t) => t.id !== tabId);
      let activeTabId = s.activeTabId;
      if (activeTabId === tabId) activeTabId = tabs[Math.min(idx, tabs.length - 1)]?.id ?? null;
      return { tabs, panes, activeTabId };
    });
  },

  closePane(paneId) {
    const s = get();
    const pane = s.panes[paneId];
    if (!pane) return;
    const tab = s.tabs.find((t) => t.id === pane.tabId);
    if (!tab) return;
    const layout = removePane(tab.layout, paneId);
    if (!layout) {
      get().closeTab(tab.id);
      return;
    }
    const remaining = paneIdsOf(layout);
    set((st) => {
      const panes = { ...st.panes };
      delete panes[paneId];
      return {
        panes,
        tabs: st.tabs.map((t) =>
          t.id === tab.id
            ? { ...t, layout, activePaneId: t.activePaneId === paneId ? remaining[0] : t.activePaneId }
            : t
        )
      };
    });
  },

  closeAll() {
    set({ tabs: [], panes: {}, activeTabId: null });
  },

  activateTab(tabId) {
    set((s) => {
      const tab = s.tabs.find((t) => t.id === tabId);
      if (!tab) return s;
      const panes = { ...s.panes };
      for (const id of paneIdsOf(tab.layout)) {
        if (panes[id] && (panes[id].hasActivity || panes[id].bell)) panes[id] = { ...panes[id], hasActivity: false, bell: false };
      }
      return { activeTabId: tabId, panes };
    });
  },

  activatePane(paneId) {
    const pane = get().panes[paneId];
    if (!pane) return;
    set((s) => ({
      activeTabId: pane.tabId,
      tabs: s.tabs.map((t) => (t.id === pane.tabId ? { ...t, activePaneId: paneId } : t))
    }));
    get().activateTab(pane.tabId);
  },

  moveTab(from, to) {
    set((s) => {
      if (from === to || from < 0 || from >= s.tabs.length) return s;
      const tabs = [...s.tabs];
      const [t] = tabs.splice(from, 1);
      tabs.splice(Math.max(0, Math.min(to, tabs.length)), 0, t);
      return { tabs };
    });
  },

  renameTab(tabId, title) {
    set((s) => ({ tabs: s.tabs.map((t) => (t.id === tabId ? { ...t, customTitle: title || undefined } : t)) }));
  },

  setTabColor(tabId, color) {
    set((s) => ({ tabs: s.tabs.map((t) => (t.id === tabId ? { ...t, color: color || undefined } : t)) }));
  },

  toggleBroadcast(tabId) {
    set((s) => ({ tabs: s.tabs.map((t) => (t.id === tabId ? { ...t, broadcast: !t.broadcast } : t)) }));
  },

  splitPane(paneId, direction, session, sessionId) {
    const src = get().panes[paneId];
    if (!src || src.kind === 'files') return '';
    return get().openSession(session ?? src.session, {
      target: direction === 'row' ? 'split-right' : 'split-down',
      besidePaneId: paneId,
      sessionId: session ? (sessionId ?? null) : src.sessionId
    });
  },

  duplicatePane(paneId, target) {
    const src = get().panes[paneId];
    if (!src) return undefined;
    if (src.kind === 'files') {
      // A file manager duplicates into a new file manager tab with its own connection.
      return get().openSession(src.session, { view: 'files', sessionId: src.sessionId, remotePath: src.files?.remotePath, localPath: src.files?.localPath });
    }
    return get().openSession(src.session, { target, besidePaneId: paneId, sessionId: src.sessionId });
  },

  restartPane(paneId) {
    set((s) => {
      const p = s.panes[paneId];
      if (!p) return s;
      const generation = p.generation + 1;
      return {
        panes: {
          ...s.panes,
          [paneId]: {
            ...p,
            generation,
            connId: connIdOf(paneId, generation),
            status: 'idle',
            statusMessage: undefined,
            serverInfo: undefined,
            oneTimePassword: undefined,
            // A file manager whose terminal is gone reconnects on its own.
            linkedPaneId: p.linkedPaneId && s.panes[p.linkedPaneId] ? p.linkedPaneId : undefined
          }
        }
      };
    });
  },

  resizeSplit(splitId, sizes) {
    set((s) => ({
      tabs: s.tabs.map((t) => ({ ...t, layout: mapSplit(t.layout, splitId, (n) => ({ ...n, sizes })) }))
    }));
  },

  updatePane(paneId, patch) {
    set((s) => (s.panes[paneId] ? { panes: { ...s.panes, [paneId]: { ...s.panes[paneId], ...patch } } } : s));
  },

  updatePaneByConnId(connId, patch) {
    const paneId = paneIdOfConn(connId);
    const p = get().panes[paneId];
    if (!p || p.connId !== connId) return;
    get().updatePane(paneId, patch);
  },

  focusAdjacentPane(delta) {
    const s = get();
    const tab = s.tabs.find((t) => t.id === s.activeTabId);
    if (!tab) return;
    const ids = paneIdsOf(tab.layout);
    if (ids.length < 2) return;
    const i = ids.indexOf(tab.activePaneId);
    get().activatePane(ids[(i + delta + ids.length) % ids.length]);
  },

  nextTab(delta) {
    const s = get();
    if (!s.tabs.length) return;
    const i = s.tabs.findIndex((t) => t.id === s.activeTabId);
    get().activateTab(s.tabs[(i + delta + s.tabs.length) % s.tabs.length].id);
  },

  gotoTab(index) {
    const s = get();
    const tab = index === 8 ? s.tabs[s.tabs.length - 1] : s.tabs[index];
    if (tab) get().activateTab(tab.id);
  }
}));

/** Non-reactive helpers. */
export function getActivePane(): PaneState | undefined {
  const s = useWorkspace.getState();
  const tab = s.tabs.find((t) => t.id === s.activeTabId);
  return tab ? s.panes[tab.activePaneId] : undefined;
}
