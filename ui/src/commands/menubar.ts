/**
 * macOS: the app's commands in the OS menu bar (api.window.setMenuBar) instead of the title bar's app menu, kept in
 * step with the key bindings and the state (sidebar shown, a pane to act on). A chosen item comes back through
 * api.window.onMenuCommand. Quit, Edit and Window are the host's own menus.
 */
import { api } from '@/bridge/api';
import { useSettings } from '@/store/data';
import { useUi } from '@/store/ui';
import { getActivePane, useWorkspace } from '@/store/workspace';
import type { MenuBarItem, MenuBarMenu } from '@shared/ipc';
import { isMac } from '@shared/platform';
import type { CommandId } from '@shared/types';
import { commandInfo, runCommand } from './commands';
import { keybindingFor } from './keybindings';

/** About is not a palette command: the menu opens the dialog itself. */
const ABOUT = 'app.about';

type Entry = readonly [command: CommandId | typeof ABOUT, label: string] | '-';

const MENUS: readonly { title: string; entries: readonly Entry[] }[] = [
  { title: 'app', entries: [[ABOUT, 'About SSHTermaq'], '-', ['app.settings', 'Settings…']] },
  {
    title: 'File',
    entries: [
      ['app.newSession', 'New Session…'],
      ['app.quickConnect', 'Quick Connect…'],
      ['tab.new', 'New Tab…'],
      ['tab.duplicate', 'Duplicate Tab'],
      '-',
      ['app.importPutty', 'Import from PuTTY…'],
      ['app.keygen', 'Key Generator…'],
      '-',
      ['pane.close', 'Close Pane'],
      ['tab.close', 'Close Tab']
    ]
  },
  {
    title: 'View',
    entries: [
      ['app.toggleSidebar', 'Show Sidebar'],
      ['app.commandPalette', 'Command Palette…'],
      '-',
      ['term.zoomIn', 'Zoom In'],
      ['term.zoomOut', 'Zoom Out'],
      ['term.zoomReset', 'Actual Size'],
      '-',
      ['tab.prev', 'Show Previous Tab'],
      ['tab.next', 'Show Next Tab']
    ]
  },
  {
    title: 'Terminal',
    entries: [
      ['term.find', 'Find…'],
      ['term.copyAll', 'Copy All'],
      ['term.clearScrollback', 'Clear Scrollback'],
      ['term.reset', 'Reset Terminal'],
      '-',
      ['pane.splitRight', 'Split Right'],
      ['pane.splitDown', 'Split Down'],
      ['pane.focusPrev', 'Previous Pane'],
      ['pane.focusNext', 'Next Pane'],
      ['pane.toggleBroadcast', 'Broadcast Input to All Panes'],
      '-',
      ['pane.sftp', 'SFTP Panel'],
      ['pane.fileManager', 'File Manager'],
      ['pane.eventLog', 'Event Log'],
      '-',
      ['pane.restart', 'Restart Session'],
      ['pane.changeSettings', 'Session Settings…']
    ]
  }
];

export interface MenuBarState {
  /** The chord bound to a command ('' for none). */
  shortcut: (id: CommandId) => string;
  sidebarShown: boolean;
  /** A tab / pane is there for the commands that need one. */
  hasPane: boolean;
}

/** The menus for a state (pure, for tests). */
export function buildMenuBar(state: MenuBarState): MenuBarMenu[] {
  return MENUS.map(({ title, entries }) => ({
    title,
    items: entries.map((e): MenuBarItem => {
      if (e === '-') return { separator: true };
      const [command, label] = e;
      if (command === ABOUT) return { command, label };
      return {
        command,
        label: command === 'app.toggleSidebar' && state.sidebarShown ? 'Hide Sidebar' : label,
        shortcut: state.shortcut(command),
        enabled: !commandInfo(command)?.needsPane || state.hasPane
      };
    })
  }));
}

function currentState(): MenuBarState {
  return {
    shortcut: keybindingFor,
    sidebarShown: useSettings.getState().settings.appearance.showSidebar,
    hasPane: !!getActivePane()
  };
}

/** macOS: keeps the menu bar up to date and runs what is chosen in it. Returns an uninstall function. */
export function installMenuBar(): () => void {
  if (!isMac) return () => undefined;
  let sent = '';
  const update = () => {
    const menus = buildMenuBar(currentState());
    const json = JSON.stringify(menus);
    if (json === sent) return;
    sent = json;
    api.window.setMenuBar(menus);
  };
  update();
  const offs = [
    useSettings.subscribe(update),
    useWorkspace.subscribe(update),
    api.window.onMenuCommand((command) => {
      if (command === ABOUT) useUi.getState().openDialog({ type: 'about' });
      else if (commandInfo(command as CommandId)) runCommand(command as CommandId);
    })
  ];
  return () => offs.forEach((off) => off());
}
