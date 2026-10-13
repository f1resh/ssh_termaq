/**
 * Workspace persistence (settings.behavior.restoreTabs): tabs, split layouts, titles/colours and the session
 * behind every pane are saved to host state ('workspace') and reopened on the next start.
 *
 * Saved sessions are referenced by id (their config is re-read from the sessions store on restore); ad-hoc
 * sessions (quick connect / CLI) are stored as a config snapshot with every secret field blanked.
 */
import { api } from '@/bridge/api';
import { newId, normalizeSession } from '@shared/defaults';
import type { SessionConfig } from '@shared/types';
import { connIdOf, paneIdsOf, useWorkspace, type LayoutNode, type PaneState, type SplitDirection, type TabState } from './workspace';

export const WORKSPACE_STATE_KEY = 'workspace';

export interface SnapshotPane {
  sessionId: string | null;
  /** Ad-hoc sessions only (secrets removed). */
  session?: SessionConfig;
}

export type SnapshotNode =
  | { type: 'pane'; pane: SnapshotPane }
  | { type: 'split'; direction: SplitDirection; sizes: number[]; children: SnapshotNode[] };

export interface SnapshotTab {
  layout: SnapshotNode;
  /** Index of the active pane in layout order. */
  activePane: number;
  customTitle?: string;
  color?: string;
  broadcast?: boolean;
}

export interface WorkspaceSnapshot {
  version: 1;
  activeTab: number;
  tabs: SnapshotTab[];
}

/** Session config safe to write to disk unencrypted. */
export function stripSecrets(s: SessionConfig): SessionConfig {
  const c = structuredClone(s);
  if (c.proxy) c.proxy.password = '';
  return c;
}

/** Pure: workspace state -> snapshot. `isSaved(id)` tells whether a session id refers to a saved session. */
export function serializeWorkspace(
  state: { tabs: TabState[]; panes: Record<string, PaneState>; activeTabId: string | null },
  isSaved: (id: string) => boolean
): WorkspaceSnapshot {
  const nodeOf = (n: LayoutNode): SnapshotNode | null => {
    if (n.type === 'pane') {
      const p = state.panes[n.paneId];
      if (!p) return null;
      const saved = p.sessionId && isSaved(p.sessionId);
      return { type: 'pane', pane: saved ? { sessionId: p.sessionId } : { sessionId: null, session: stripSecrets(p.session) } };
    }
    const kids: SnapshotNode[] = [];
    const sizes: number[] = [];
    n.children.forEach((c, i) => {
      const k = nodeOf(c);
      if (k) {
        kids.push(k);
        sizes.push(n.sizes[i] ?? 1 / n.children.length);
      }
    });
    if (!kids.length) return null;
    if (kids.length === 1) return kids[0];
    return { type: 'split', direction: n.direction, sizes, children: kids };
  };
  const tabs: SnapshotTab[] = [];
  let activeTab = 0;
  for (const t of state.tabs) {
    const layout = nodeOf(t.layout);
    if (!layout) continue;
    if (t.id === state.activeTabId) activeTab = tabs.length;
    const ids = paneIdsOf(t.layout).filter((id) => state.panes[id]);
    tabs.push({
      layout,
      activePane: Math.max(0, ids.indexOf(t.activePaneId)),
      customTitle: t.customTitle,
      color: t.color,
      broadcast: t.broadcast || undefined
    });
  }
  return { version: 1, activeTab, tabs };
}

/**
 * Pure: snapshot -> workspace state. Panes whose saved session no longer exists are dropped.
 * `template` fills fields missing from older ad-hoc snapshots.
 */
export function deserializeWorkspace(
  snap: WorkspaceSnapshot,
  lookup: (id: string) => SessionConfig | undefined,
  template?: SessionConfig
): { tabs: TabState[]; panes: Record<string, PaneState>; activeTabId: string | null } {
  const tabs: TabState[] = [];
  const panes: Record<string, PaneState> = {};
  let activeTabId: string | null = null;
  if (!snap || snap.version !== 1 || !Array.isArray(snap.tabs)) return { tabs, panes, activeTabId };

  snap.tabs.forEach((st, index) => {
    const tabId = newId();
    const build = (n: SnapshotNode): LayoutNode | null => {
      if (!n || typeof n !== 'object') return null;
      if (n.type === 'pane') {
        const saved = n.pane?.sessionId ? lookup(n.pane.sessionId) : undefined;
        const session = saved ?? (n.pane?.session ? normalizeSession(n.pane.session, template) : undefined);
        if (!session) return null;
        const id = newId();
        panes[id] = {
          id,
          tabId,
          sessionId: saved ? saved.id : null,
          session: structuredClone(session),
          generation: 1,
          connId: connIdOf(id, 1),
          status: 'idle',
          hasActivity: false,
          bell: false
        };
        return { type: 'pane', paneId: id };
      }
      if (n.type !== 'split' || !Array.isArray(n.children)) return null;
      const kids: LayoutNode[] = [];
      const sizes: number[] = [];
      n.children.forEach((c, i) => {
        const k = build(c);
        if (k) {
          kids.push(k);
          sizes.push(Number.isFinite(n.sizes?.[i]) && n.sizes[i] > 0 ? n.sizes[i] : 1);
        }
      });
      if (!kids.length) return null;
      if (kids.length === 1) return kids[0];
      const total = sizes.reduce((a, b) => a + b, 0) || 1;
      return { type: 'split', id: newId(), direction: n.direction === 'column' ? 'column' : 'row', children: kids, sizes: sizes.map((s) => s / total) };
    };
    const layout = build(st.layout);
    if (!layout) return;
    const ids = paneIdsOf(layout);
    tabs.push({
      id: tabId,
      layout,
      activePaneId: ids[Math.min(Math.max(0, st.activePane | 0), ids.length - 1)],
      customTitle: st.customTitle || undefined,
      color: st.color || undefined,
      broadcast: !!st.broadcast
    });
    if (index === snap.activeTab) activeTabId = tabId;
  });
  if (!activeTabId && tabs.length) activeTabId = tabs[0].id;
  return { tabs, panes, activeTabId };
}

/** Restores the saved workspace (appending to anything already open). Returns the number of tabs restored. */
export async function restoreWorkspace(lookup: (id: string) => SessionConfig | undefined, template?: SessionConfig): Promise<number> {
  const snap = await api.app.getState<WorkspaceSnapshot>(WORKSPACE_STATE_KEY);
  if (!snap) return 0;
  const restored = deserializeWorkspace(snap, lookup, template);
  if (!restored.tabs.length) return 0;
  useWorkspace.setState((s) => ({
    tabs: [...s.tabs, ...restored.tabs],
    panes: { ...s.panes, ...restored.panes },
    activeTabId: restored.activeTabId ?? s.activeTabId
  }));
  return restored.tabs.length;
}

/**
 * Saves the workspace (debounced) whenever its structure changes while `enabled()` is true.
 * Returns an unsubscribe function.
 */
export function startWorkspaceAutosave(enabled: () => boolean, isSaved: (id: string) => boolean, delayMs = 600): () => void {
  let last = '';
  let timer: ReturnType<typeof setTimeout> | undefined;
  const flush = () => {
    timer = undefined;
    if (!enabled()) return;
    const snap = serializeWorkspace(useWorkspace.getState(), isSaved);
    const json = JSON.stringify(snap);
    if (json === last) return;
    last = json;
    api.app.setState(WORKSPACE_STATE_KEY, snap).catch((e) => console.warn('workspace save failed', e));
  };
  const unsub = useWorkspace.subscribe((s, prev) => {
    // Only structural changes matter (status / activity updates do not).
    if (s.tabs === prev.tabs && s.activeTabId === prev.activeTabId && Object.keys(s.panes).length === Object.keys(prev.panes).length) return;
    if (timer) clearTimeout(timer);
    timer = setTimeout(flush, delayMs);
  });
  const onUnload = () => {
    if (timer) {
      clearTimeout(timer);
      flush();
    }
  };
  window.addEventListener('beforeunload', onUnload);
  return () => {
    unsub();
    window.removeEventListener('beforeunload', onUnload);
    if (timer) clearTimeout(timer);
  };
}
