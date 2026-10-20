/**
 * Connections owned by file manager panes. A pane with its own connection (not borrowing a terminal's) gets a
 * shell-less copy of its session — no tunnels, X11 or agent forwarding, so it never competes with a terminal of the
 * same session for local ports — opened while the pane is 'idle', and closed when the pane goes away or restarts.
 */
import { api } from '@/bridge/api';
import { useWorkspace, type PaneState } from '@/store/workspace';
import type { SessionConfig } from '@shared/types';

/** The session as used for a files-only connection. */
export function filesSession(s: SessionConfig): SessionConfig {
  const c = structuredClone(s);
  c.ssh.noShell = true;
  c.ssh.remoteCommand = '';
  c.ssh.tunnels = [];
  c.ssh.x11.enabled = false;
  c.ssh.auth.agentForwarding = false;
  if (c.logging.mode !== 'ssh-debug') c.logging.mode = 'off';
  return c;
}

/** paneId -> connId this module opened. */
const opened = new Map<string, string>();
let started = false;

function open(p: PaneState): void {
  opened.set(p.id, p.connId);
  useWorkspace.getState().updatePaneByConnId(p.connId, { status: 'connecting', statusMessage: undefined, oneTimePassword: undefined });
  api.conn
    .open({
      connId: p.connId,
      session: filesSession(p.session),
      cols: 80,
      rows: 24,
      widthPx: 0,
      heightPx: 0,
      password: p.oneTimePassword || undefined
    })
    .catch((err: unknown) => {
      useWorkspace.getState().updatePaneByConnId(p.connId, { status: 'error', statusMessage: err instanceof Error ? err.message : String(err) });
    });
}

function sync(panes: Record<string, PaneState>): void {
  for (const [paneId, connId] of [...opened]) {
    const p = panes[paneId];
    if (!p || p.linkedPaneId || p.connId !== connId) {
      api.conn.close(connId);
      opened.delete(paneId);
    }
  }
  for (const p of Object.values(panes)) {
    if (p.kind !== 'files' || p.linkedPaneId) continue;
    if (p.status === 'idle' && opened.get(p.id) !== p.connId) open(p);
  }
}

/** Idempotent: starts watching the workspace for file manager panes. */
export function initFilesConnections(): void {
  if (started) return;
  started = true;
  useWorkspace.subscribe((s, prev) => {
    if (s.panes !== prev.panes) sync(s.panes);
  });
  sync(useWorkspace.getState().panes);
}

/** Connection a files pane works on: its own, or that of the terminal it was opened from. */
export function connOfFilesPane(pane: PaneState, panes: Record<string, PaneState>): { connId: string; status: PaneState['status']; message?: string } {
  if (!pane.linkedPaneId) return { connId: pane.connId, status: pane.status, message: pane.statusMessage };
  const term = panes[pane.linkedPaneId];
  if (!term) return { connId: '', status: 'disconnected', message: 'The terminal this file manager was opened from has been closed.' };
  return { connId: term.connId, status: term.status, message: term.statusMessage };
}
