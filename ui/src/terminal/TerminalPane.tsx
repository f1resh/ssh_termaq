/**
 * One terminal pane bound to a workspace pane (connection). The xterm instance and the connection live in a
 * TermSession (termSession.ts) that survives remounts; this component only hosts its element and renders the
 * overlays: connecting card, disconnected banner, authentication prompts, find bar and the context menu.
 */
import { useEffect, useRef, useState, useSyncExternalStore } from 'react';
import { Loader2, Plug, RotateCcw, ScrollText, X } from 'lucide-react';
import { api } from '@/bridge/api';
import { runCommand } from '@/commands/commands';
import { Button, showContextMenu, type MenuItem } from '@/components/ui';
import { shortcutLabel } from '@/commands/keybindings';
import { usePrompts } from '@/store/data';
import { useUi } from '@/store/ui';
import { useWorkspace } from '@/store/workspace';
import type { SpecialCommandDef } from '@shared/types';
import { lastEventLogOf, PANE_PROMPT_KINDS, watchEventLog } from './connEvents';
import { FindBar } from './FindBar';
import { PromptOverlay } from './PromptOverlay';
import { acquireSession, type TermSession } from './termSession';
import './terminal.css';

export interface TerminalPaneProps {
  paneId: string;
  /** Pane has keyboard focus within its tab (the tab's activePaneId). */
  active: boolean;
  /** Pane's tab is the visible tab. */
  visible: boolean;
}

function useLastLogLine(connId: string): string {
  return useSyncExternalStore(watchEventLog, () => lastEventLogOf(connId)?.message ?? '');
}

async function specialsMenu(connId: string): Promise<MenuItem[]> {
  let defs: SpecialCommandDef[] = [];
  try {
    defs = await api.conn.specials(connId);
  } catch {
    defs = [];
  }
  if (!defs.length) return [{ label: 'No special commands', disabled: true }];
  const items: MenuItem[] = [];
  let group: string | undefined;
  for (const d of defs) {
    if ((d.group || undefined) !== group) {
      group = d.group || undefined;
      if (items.length) items.push({ type: 'separator' });
      if (group) items.push({ type: 'header', label: group });
    }
    items.push({ label: d.label, onClick: () => api.conn.special(connId, d.id) });
  }
  return items;
}

async function openContextMenu(session: TermSession, paneId: string, x: number, y: number) {
  const ws = useWorkspace.getState();
  const pane = ws.panes[paneId];
  if (!pane) return;
  const tab = ws.tabs.find((t) => t.id === pane.tabId);
  const connected = pane.status === 'connected';
  const ended = pane.status === 'disconnected' || pane.status === 'error';
  const ssh = pane.session.protocol === 'ssh';
  const run = (id: Parameters<typeof runCommand>[0]) => () => runCommand(id, { paneId });
  const specials = connected ? await specialsMenu(pane.connId) : [];
  const items: MenuItem[] = [
    { label: 'Copy', shortcut: shortcutLabel('term.copy'), disabled: !session.hasSelection(), onClick: run('term.copy') },
    { label: 'Paste', shortcut: shortcutLabel('term.paste'), disabled: !connected, onClick: run('term.paste') },
    { label: 'Copy All to Clipboard', onClick: run('term.copyAll') },
    { label: 'Select All', shortcut: shortcutLabel('term.selectAll'), onClick: run('term.selectAll') },
    { label: 'Find…', shortcut: shortcutLabel('term.find'), onClick: run('term.find') },
    { type: 'separator' },
    { label: 'Special Commands', disabled: !connected, submenu: specials },
    { label: 'Event Log', onClick: run('pane.eventLog') },
    { label: 'Clear Scrollback', onClick: run('term.clearScrollback') },
    { label: 'Reset Terminal', onClick: run('term.reset') },
    { type: 'separator' },
    { label: 'Duplicate Session', shortcut: shortcutLabel('tab.duplicate'), onClick: run('tab.duplicate') },
    { label: 'Restart Session', disabled: !ended, onClick: () => session.restart() },
    { label: 'Change Settings…', onClick: run('pane.changeSettings') },
    { label: 'Open File Manager', disabled: !ssh, shortcut: shortcutLabel('pane.sftp'), onClick: run('pane.sftp') },
    { type: 'separator' },
    { label: 'Split Right', shortcut: shortcutLabel('pane.splitRight'), onClick: run('pane.splitRight') },
    { label: 'Split Down', shortcut: shortcutLabel('pane.splitDown'), onClick: run('pane.splitDown') },
    { label: 'Broadcast Input to Tab', checked: !!tab?.broadcast, onClick: run('pane.toggleBroadcast') },
    { label: 'Full Screen', shortcut: shortcutLabel('app.fullscreen'), checked: useUi.getState().fullscreen, onClick: run('app.fullscreen') },
    { type: 'separator' },
    { label: 'Close Pane', shortcut: shortcutLabel('pane.close'), danger: true, onClick: run('pane.close') }
  ];
  showContextMenu(x, y, items);
}

export function TerminalPane({ paneId, active, visible }: TerminalPaneProps) {
  const hostRef = useRef<HTMLDivElement>(null);
  const pane = useWorkspace((s) => s.panes[paneId]);
  const [session, setSession] = useState<TermSession | null>(null);
  const [findOpen, setFindOpen] = useState(false);
  const [, forceTick] = useState(0);
  const prompt = usePrompts((s) => s.pending.find((p) => pane && p.connId === pane.connId && PANE_PROMPT_KINDS.has(p.kind)));
  const lastLine = useLastLogLine(pane?.connId ?? '');

  // Attach the long-lived session element to this container.
  useEffect(() => {
    const p = useWorkspace.getState().panes[paneId];
    const host = hostRef.current;
    if (!p || !host) return;
    const s = acquireSession(p);
    s.attach(host);
    setSession(s);
    const offFind = s.on('find', () => setFindOpen(true));
    const offMenu = s.on('contextmenu', (arg) => {
      const pt = arg as { x: number; y: number };
      void openContextMenu(s, paneId, pt.x, pt.y);
    });
    const offReconnect = s.on('reconnect', () => forceTick((n) => n + 1));
    return () => {
      offFind();
      offMenu();
      offReconnect();
      s.detach(host);
    };
  }, [paneId]);

  useEffect(() => {
    session?.setVisible(visible);
  }, [session, visible]);

  useEffect(() => {
    if (session && visible && active && !prompt) session.focus();
  }, [session, visible, active, prompt]);

  // Reconnect countdown refresh.
  useEffect(() => {
    if (!session?.reconnectAt) return;
    const t = setInterval(() => forceTick((n) => n + 1), 500);
    return () => clearInterval(t);
  }, [session, session?.reconnectAt]);

  const status = pane?.status ?? 'idle';
  const connecting = status === 'idle' || status === 'connecting' || status === 'authenticating';
  const ended = status === 'disconnected' || status === 'error';
  const host = pane ? (pane.session.protocol === 'serial' ? pane.session.serial.line : pane.session.host) : '';

  // Enter / R restarts an ended session (PuTTY-like), Esc closes the find bar.
  useEffect(() => {
    if (!ended || !visible || !active || !session) return;
    const onKey = (e: KeyboardEvent) => {
      if (e.ctrlKey || e.altKey || e.metaKey) return;
      if (useUi.getState().dialog || prompt) return;
      if (e.key === 'Enter' || e.key === 'r' || e.key === 'R') {
        e.preventDefault();
        e.stopPropagation();
        session.restart();
      }
    };
    window.addEventListener('keydown', onKey, true);
    return () => window.removeEventListener('keydown', onKey, true);
  }, [ended, visible, active, session, prompt]);

  const reconnectIn = session?.reconnectAt ? Math.max(0, Math.ceil((session.reconnectAt - Date.now()) / 1000)) : null;

  return (
    <div className="term-pane" data-status={status}>
      <div ref={hostRef} className="term-pane__host" />

      {findOpen && session && <FindBar session={session} onClose={() => setFindOpen(false)} />}

      {connecting && !prompt && (
        <div className="term-overlay term-overlay--center">
          <div className="term-card">
            <Loader2 size={18} className="term-spin" />
            <div className="term-card__body">
              <div className="term-card__title">
                {status === 'authenticating' ? 'Authenticating' : 'Connecting to'} <b>{host}</b>
              </div>
              {lastLine && <div className="term-card__detail">{lastLine}</div>}
            </div>
            <Button size="sm" variant="ghost" onClick={() => session?.cancelConnection()}>
              Cancel
            </Button>
          </div>
        </div>
      )}

      {ended && !prompt && (
        <div className={`term-banner term-banner--${status}`}>
          <Plug size={15} />
          <div className="term-banner__text">
            <b>{status === 'error' ? 'Connection failed' : 'Session ended'}</b>
            <span>
              {pane?.statusMessage || (status === 'error' ? 'The connection was lost.' : 'The remote side closed the connection.')}
              {reconnectIn !== null && ` — reconnecting in ${reconnectIn}s`}
            </span>
          </div>
          {reconnectIn !== null && (
            <Button size="sm" variant="ghost" onClick={() => session?.cancelReconnect()}>
              Stop
            </Button>
          )}
          <Button size="sm" variant="ghost" icon={<ScrollText size={14} />} onClick={() => runCommand('pane.eventLog', { paneId })}>
            Event log
          </Button>
          <Button size="sm" variant="primary" icon={<RotateCcw size={14} />} onClick={() => session?.restart()} title="Restart (Enter)">
            Restart
          </Button>
          <Button size="sm" icon={<X size={14} />} onClick={() => session?.closePane()}>
            Close
          </Button>
        </div>
      )}

      {prompt && <PromptOverlay prompt={prompt} host={host} />}
    </div>
  );
}
