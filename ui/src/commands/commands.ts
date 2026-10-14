/**
 * Command registry: every user-invokable action (palette, keybindings, menus).
 * COMMANDS holds the metadata (label, category, icon); runCommand executes a command against the current
 * focus (active tab / pane) or an explicit target (context menus pass the tab / pane they were opened on).
 */
import type { LucideIcon } from 'lucide-react';
import {
  ArrowLeft,
  ArrowRight,
  ChevronLeft,
  ChevronRight,
  ClipboardCopy,
  ClipboardPaste,
  Columns2,
  Command,
  Copy,
  Eraser,
  FolderOpen,
  Hash,
  Import,
  KeyRound,
  Maximize,
  PanelLeft,
  Pencil,
  Plus,
  Radio,
  RefreshCcw,
  RotateCw,
  Rows2,
  ScrollText,
  Search,
  Settings,
  SlidersHorizontal,
  SquarePlus,
  SquareX,
  TextSelect,
  X,
  Zap,
  ZoomIn,
  ZoomOut
} from 'lucide-react';
import { api } from '@/bridge/api';
import { terminalRegistry } from '@/terminal/registry';
import { useUi } from '@/store/ui';
import { getActivePane, useWorkspace } from '@/store/workspace';
import type { CommandId } from '@shared/types';
import {
  changePaneSettings,
  closePane,
  closeTab,
  newSessionDraft,
  reportError,
  openFileManagerFor,
  restartTab,
  toggleSidebar
} from './actions';

export type CommandCategory = 'App' | 'Tab' | 'Pane' | 'Terminal';

export interface CommandInfo {
  id: CommandId;
  label: string;
  category: CommandCategory;
  icon?: LucideIcon;
  /** Extra search terms for the palette. */
  keywords?: string;
  /** Needs an open tab / pane to make sense (palette hides it otherwise). */
  needsPane?: boolean;
}

const c = (
  id: CommandId,
  label: string,
  category: CommandCategory,
  icon?: LucideIcon,
  extra: Partial<Pick<CommandInfo, 'keywords' | 'needsPane'>> = {}
): CommandInfo => ({ id, label, category, icon, ...extra });

export const COMMANDS: CommandInfo[] = [
  c('app.commandPalette', 'Command palette', 'App', Command, { keywords: 'commands search' }),
  c('app.quickConnect', 'Quick connect…', 'App', Zap, { keywords: 'connect host ssh telnet open' }),
  c('app.newSession', 'New session…', 'App', SquarePlus, { keywords: 'create add saved session' }),
  c('app.settings', 'Settings', 'App', Settings, { keywords: 'preferences options configuration' }),
  c('app.toggleSidebar', 'Toggle sidebar', 'App', PanelLeft, { keywords: 'sessions panel hide show' }),
  c('app.importPutty', 'Import sessions from PuTTY…', 'App', Import, { keywords: 'registry migrate putty reg' }),
  c('app.fullscreen', 'Toggle full screen', 'App', Maximize, { keywords: 'fullscreen' }),
  c('app.keygen', 'Key generator…', 'App', KeyRound, { keywords: 'puttygen ssh-keygen keys ed25519 rsa' }),
  c('tab.new', 'New tab…', 'Tab', Plus, { keywords: 'open session' }),
  c('tab.close', 'Close tab', 'Tab', X, { needsPane: true }),
  c('tab.next', 'Next tab', 'Tab', ChevronRight, { needsPane: true }),
  c('tab.prev', 'Previous tab', 'Tab', ChevronLeft, { needsPane: true }),
  c('tab.duplicate', 'Duplicate tab', 'Tab', Copy, { needsPane: true, keywords: 'clone' }),
  c('tab.rename', 'Rename tab…', 'Tab', Pencil, { needsPane: true, keywords: 'title' }),
  ...([1, 2, 3, 4, 5, 6, 7, 8, 9] as const).map((n) =>
    c(`tab.goto${n}` as CommandId, n === 9 ? 'Go to last tab' : `Go to tab ${n}`, 'Tab', Hash, { needsPane: true })
  ),
  c('pane.splitRight', 'Split pane right', 'Pane', Columns2, { needsPane: true, keywords: 'vertical duplicate' }),
  c('pane.splitDown', 'Split pane down', 'Pane', Rows2, { needsPane: true, keywords: 'horizontal duplicate' }),
  c('pane.close', 'Close pane', 'Pane', SquareX, { needsPane: true }),
  c('pane.focusNext', 'Focus next pane', 'Pane', ArrowRight, { needsPane: true }),
  c('pane.focusPrev', 'Focus previous pane', 'Pane', ArrowLeft, { needsPane: true }),
  c('pane.restart', 'Restart session', 'Pane', RotateCw, { needsPane: true, keywords: 'reconnect' }),
  c('pane.toggleBroadcast', 'Toggle broadcast input', 'Pane', Radio, { needsPane: true, keywords: 'send all panes multi' }),
  c('pane.sftp', 'Open file manager', 'Pane', FolderOpen, { needsPane: true, keywords: 'files upload download sftp scp winscp psftp transfer' }),
  c('pane.eventLog', 'Event log', 'Pane', ScrollText, { needsPane: true, keywords: 'log diagnostics' }),
  c('pane.changeSettings', 'Change session settings…', 'Pane', SlidersHorizontal, { needsPane: true, keywords: 'edit configure' }),
  c('term.copy', 'Copy', 'Terminal', ClipboardCopy, { needsPane: true }),
  c('term.paste', 'Paste', 'Terminal', ClipboardPaste, { needsPane: true }),
  c('term.copyAll', 'Copy all to clipboard', 'Terminal', ClipboardCopy, { needsPane: true, keywords: 'scrollback' }),
  c('term.selectAll', 'Select all', 'Terminal', TextSelect, { needsPane: true }),
  c('term.find', 'Find…', 'Terminal', Search, { needsPane: true, keywords: 'search' }),
  c('term.clearScrollback', 'Clear scrollback', 'Terminal', Eraser, { needsPane: true }),
  c('term.reset', 'Reset terminal', 'Terminal', RefreshCcw, { needsPane: true }),
  c('term.zoomIn', 'Zoom in', 'Terminal', ZoomIn, { needsPane: true, keywords: 'font bigger' }),
  c('term.zoomOut', 'Zoom out', 'Terminal', ZoomOut, { needsPane: true, keywords: 'font smaller' }),
  c('term.zoomReset', 'Reset zoom', 'Terminal', ZoomIn, { needsPane: true, keywords: 'font size' })
];

const BY_ID = new Map(COMMANDS.map((x) => [x.id, x]));

export function commandInfo(id: CommandId): CommandInfo | undefined {
  return BY_ID.get(id);
}

/** Explicit target for a command (context menus); defaults to the active tab / pane. */
export interface CommandTarget {
  tabId?: string;
  paneId?: string;
}

function resolveTarget(target?: CommandTarget): { tabId?: string; paneId?: string } {
  const ws = useWorkspace.getState();
  if (target?.paneId && ws.panes[target.paneId]) return { paneId: target.paneId, tabId: ws.panes[target.paneId].tabId };
  if (target?.tabId) {
    const tab = ws.tabs.find((t) => t.id === target.tabId);
    if (tab) return { tabId: tab.id, paneId: tab.activePaneId };
  }
  const pane = getActivePane();
  return { tabId: ws.activeTabId ?? undefined, paneId: pane?.id };
}

function term(paneId: string | undefined) {
  return terminalRegistry.get(paneId);
}

const recentKey = 'palette.recentCommands';
let recent: CommandId[] = [];

/** Most recently run commands first (used by the palette). */
export function recentCommands(): CommandId[] {
  return recent;
}

export function loadRecentCommands(): void {
  api.app
    .getState<CommandId[]>(recentKey)
    .then((v) => {
      if (Array.isArray(v)) recent = v.filter((id) => BY_ID.has(id)).slice(0, 20);
    })
    .catch(() => undefined);
}

function remember(id: CommandId): void {
  if (id === 'app.commandPalette') return;
  recent = [id, ...recent.filter((x) => x !== id)].slice(0, 20);
  api.app.setState(recentKey, recent).catch(() => undefined);
}

/** Run a command against the current focus (active tab / pane) or an explicit target. */
export function runCommand(id: CommandId, target?: CommandTarget): void {
  try {
    remember(id);
    execute(id, target);
  } catch (e) {
    reportError('Command failed', e);
  }
}

function execute(id: CommandId, target?: CommandTarget): void {
  const ui = useUi.getState();
  const ws = useWorkspace.getState();
  const { tabId, paneId } = resolveTarget(target);

  if (id.startsWith('tab.goto')) {
    ws.gotoTab(Number(id.slice('tab.goto'.length)) - 1);
    return;
  }

  switch (id) {
    /* ---- app ---- */
    case 'app.commandPalette':
      if (ui.paletteOpen) ui.setPalette(false);
      else ui.openPalette('');
      return;
    case 'app.quickConnect':
      ui.openDialog({ type: 'quickConnect' });
      return;
    case 'app.newSession':
      newSessionDraft();
      return;
    case 'app.settings':
      ui.openDialog({ type: 'settings' });
      return;
    case 'app.toggleSidebar':
      void toggleSidebar();
      return;
    case 'app.importPutty':
      ui.openDialog({ type: 'import', source: 'putty-registry' });
      return;
    case 'app.fullscreen':
      api.app.toggleFullscreen();
      return;
    case 'app.keygen':
      ui.openDialog({ type: 'keygen' });
      return;

    /* ---- tabs ---- */
    case 'tab.new':
      // Session picker with a "quick connect to <typed host>" fallback.
      ui.openPalette('@');
      return;
    case 'tab.close':
      if (tabId) void closeTab(tabId);
      return;
    case 'tab.next':
      ws.nextTab(1);
      return;
    case 'tab.prev':
      ws.nextTab(-1);
      return;
    case 'tab.duplicate':
      if (paneId) ws.duplicatePane(paneId, 'tab');
      return;
    case 'tab.rename':
      if (tabId) {
        ws.activateTab(tabId);
        ui.setRenamingTab(tabId);
      }
      return;

    /* ---- panes ---- */
    case 'pane.splitRight':
      if (paneId) ws.duplicatePane(paneId, 'split-right');
      return;
    case 'pane.splitDown':
      if (paneId) ws.duplicatePane(paneId, 'split-down');
      return;
    case 'pane.close':
      if (paneId) void closePane(paneId);
      return;
    case 'pane.focusNext':
      ws.focusAdjacentPane(1);
      return;
    case 'pane.focusPrev':
      ws.focusAdjacentPane(-1);
      return;
    case 'pane.restart':
      if (target?.tabId && !target.paneId) restartTab(target.tabId);
      else if (paneId) ws.restartPane(paneId);
      return;
    case 'pane.toggleBroadcast':
      if (tabId) ws.toggleBroadcast(tabId);
      return;
    case 'pane.sftp':
      openFileManagerFor(paneId);
      return;
    case 'pane.eventLog':
      if (paneId) ui.openDialog({ type: 'eventLog', paneId });
      return;
    case 'pane.changeSettings':
      if (paneId) changePaneSettings(paneId);
      return;

    /* ---- terminal ---- */
    case 'term.copy':
      void term(paneId)?.copySelection();
      return;
    case 'term.paste':
      void term(paneId)?.paste();
      return;
    case 'term.copyAll':
      void term(paneId)?.copyAll();
      return;
    case 'term.selectAll':
      term(paneId)?.selectAll();
      return;
    case 'term.find':
      term(paneId)?.openFind();
      return;
    case 'term.clearScrollback':
      term(paneId)?.clearScrollback();
      return;
    case 'term.reset':
      term(paneId)?.reset();
      return;
    case 'term.zoomIn':
      term(paneId)?.zoom(1);
      return;
    case 'term.zoomOut':
      term(paneId)?.zoom(-1);
      return;
    case 'term.zoomReset':
      term(paneId)?.zoom(0);
      return;
    default:
      // tab.goto1..9 handled above; anything else is a newer CommandId this build does not know.
      console.warn('Unknown command', id);
  }
}

/** Focuses the active pane's terminal (after closing overlays). */
export function focusActiveTerminal(): void {
  const p = getActivePane();
  if (p) requestAnimationFrame(() => terminalRegistry.get(p.id)?.focus());
}
