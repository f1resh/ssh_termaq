/**
 * Bottom status bar for the active pane: connection state, endpoint, server info, charset, broadcast, SFTP panel.
 */
import { FolderOpen, Radio } from 'lucide-react';
import { useShallow } from 'zustand/react/shallow';
import { runCommand } from '@/commands/commands';
import { shortcutLabel } from '@/commands/keybindings';
import { cx } from '@/components/ui';
import { connOfFilesPane } from '@/components/files/filesConn';
import { useUi } from '@/store/ui';
import { useWorkspace } from '@/store/workspace';
import './statusbar.css';

const STATUS_LABEL: Record<string, string> = {
  idle: 'Starting',
  connecting: 'Connecting',
  authenticating: 'Authenticating',
  connected: 'Connected',
  disconnected: 'Disconnected',
  error: 'Error'
};

export function StatusBar() {
  const { pane, broadcast, tabCount } = useWorkspace(
    useShallow((s) => {
      const tab = s.tabs.find((t) => t.id === s.activeTabId);
      return { pane: tab ? s.panes[tab.activePaneId] : undefined, broadcast: !!tab?.broadcast, tabCount: s.tabs.length };
    })
  );
  const panelOpen = useUi((s) => !!pane && s.sftpPanels[pane.tabId] === pane.id);
  const filesConn = useWorkspace(useShallow((s) => (pane?.kind === 'files' ? connOfFilesPane(pane, s.panes) : null)));

  if (!pane) {
    return (
      <footer className="statusbar">
        <span className="statusbar__item statusbar__muted">Ready</span>
        <span className="statusbar__spacer" />
        <span className="statusbar__item statusbar__muted">{tabCount} tabs</span>
      </footer>
    );
  }
  const s = pane.session;
  const endpoint =
    s.protocol === 'serial'
      ? `${s.serial.line} · ${s.serial.baudRate} baud`
      : `${s.username ? s.username + '@' : ''}${s.host}${s.port && s.port !== 22 ? ':' + s.port : ''}`;
  const serverInfo = pane.serverInfo?.split('|').map((x) => x.trim()).filter(Boolean).slice(1).join(' · ');
  const status = filesConn?.status ?? pane.status;

  return (
    <footer className="statusbar">
      <span className="statusbar__item" title={filesConn?.message ?? pane.statusMessage}>
        <span className={cx('statusbar__dot', `statusbar__dot--${status}`)} />
        {STATUS_LABEL[status] ?? status}
      </span>
      <span className="statusbar__item">
        <span className="statusbar__proto">{pane.kind === 'files' ? 'files' : s.protocol}</span>
        {endpoint}
      </span>
      {serverInfo && (
        <span className="statusbar__item statusbar__muted" title={pane.serverInfo}>
          {serverInfo}
        </span>
      )}
      <span className="statusbar__spacer" />
      {broadcast && (
        <button className="statusbar__btn statusbar__btn--warn" onClick={() => runCommand('pane.toggleBroadcast')} title="Broadcast input is on — click to turn off">
          <Radio size={12} /> Broadcast
        </button>
      )}
      {pane.kind !== 'files' && <span className="statusbar__item statusbar__muted">{s.translation.charset}</span>}
      {s.protocol === 'ssh' && pane.kind !== 'files' && (
        <button
          className={cx('statusbar__btn', panelOpen && 'statusbar__btn--on')}
          disabled={pane.status !== 'connected' && !panelOpen}
          aria-pressed={panelOpen}
          onClick={() => runCommand('pane.sftp')}
          title={`${panelOpen ? 'Hide' : 'Show'} this server's files in the SFTP panel${shortcutLabel('pane.sftp') ? ` (${shortcutLabel('pane.sftp')})` : ''}`}
        >
          <FolderOpen size={12} /> SFTP
        </button>
      )}
    </footer>
  );
}
