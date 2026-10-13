/**
 * Bottom status bar for the active pane: connection state, endpoint, server info, charset, broadcast, SFTP.
 */
import { FolderOpen, Radio } from 'lucide-react';
import { useShallow } from 'zustand/react/shallow';
import { runCommand } from '@/commands/commands';
import { cx } from '@/components/ui';
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
  const sftpOpen = useUi((s) => !!pane && s.sftpPaneId === pane.id);

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

  return (
    <footer className="statusbar">
      <span className="statusbar__item" title={pane.statusMessage}>
        <span className={cx('statusbar__dot', `statusbar__dot--${pane.status}`)} />
        {STATUS_LABEL[pane.status] ?? pane.status}
      </span>
      <span className="statusbar__item">
        <span className="statusbar__proto">{s.protocol}</span>
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
      <span className="statusbar__item statusbar__muted">{s.translation.charset}</span>
      {s.protocol === 'ssh' && (
        <button
          className={cx('statusbar__btn', sftpOpen && 'statusbar__btn--on')}
          disabled={pane.status !== 'connected'}
          onClick={() => runCommand('pane.sftp')}
          title="SFTP browser"
        >
          <FolderOpen size={12} /> SFTP
        </button>
      )}
    </footer>
  );
}
