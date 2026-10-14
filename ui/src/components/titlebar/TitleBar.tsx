/**
 * Custom title bar: app menu button, tab strip, drag space and HTML caption buttons.
 * The whole bar is a drag region (WebView2 non-client region); interactive children opt out with no-drag.
 */
import type { ReactNode } from 'react';
import { Command, Download, Info, KeyRound, LogOut, Maximize, PanelLeft, Settings, SquarePlus, Zap } from 'lucide-react';
import { api } from '@/bridge/api';
import { cx, showContextMenu, type MenuItem } from '@/components/ui';
import { runCommand } from '@/commands/commands';
import { shortcutLabel } from '@/commands/keybindings';
import { useSettings } from '@/store/data';
import { useUi } from '@/store/ui';
import { Logo } from '@/components/welcome/Logo';
import { CaptionButtons } from './CaptionButtons';
import { TabStrip } from './TabStrip';
import './titlebar.css';

function appMenu(): MenuItem[] {
  const sidebar = useSettings.getState().settings.appearance.showSidebar;
  const item = (label: string, id: Parameters<typeof runCommand>[0], icon: ReactNode, extra: Partial<MenuItem> = {}): MenuItem =>
    ({ label, icon, shortcut: shortcutLabel(id), onClick: () => runCommand(id), ...extra }) as MenuItem;
  return [
    item('New session…', 'app.newSession', <SquarePlus size={14} />),
    item('Quick connect…', 'app.quickConnect', <Zap size={14} />),
    { type: 'separator' },
    item('Import from PuTTY…', 'app.importPutty', <Download size={14} />),
    item('Key generator…', 'app.keygen', <KeyRound size={14} />),
    { type: 'separator' },
    item('Command palette', 'app.commandPalette', <Command size={14} />),
    item('Show sidebar', 'app.toggleSidebar', <PanelLeft size={14} />, { checked: sidebar }),
    item('Full screen', 'app.fullscreen', <Maximize size={14} />, { checked: useUi.getState().fullscreen }),
    item('Settings', 'app.settings', <Settings size={14} />),
    { type: 'separator' },
    { label: 'About SSHTerm', icon: <Info size={14} />, onClick: () => useUi.getState().openDialog({ type: 'about' }) },
    { label: 'Exit', icon: <LogOut size={14} />, onClick: () => api.window.close() }
  ];
}

export function TitleBar() {
  const focused = useUi((s) => s.windowFocused);
  return (
    <header className={cx('titlebar drag-region', !focused && 'titlebar--inactive')}>
      <button
        className="titlebar__app no-drag"
        title="Menu"
        aria-label="Application menu"
        onClick={(e) => {
          const r = (e.currentTarget as HTMLElement).getBoundingClientRect();
          showContextMenu(r.left + 4, r.bottom + 2, appMenu());
        }}
      >
        <Logo size={18} />
      </button>
      <TabStrip />
      <div className="titlebar__drag" />
      <CaptionButtons />
    </header>
  );
}
