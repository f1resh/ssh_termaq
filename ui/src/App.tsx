/**
 * Application root: title bar (tabs), sidebar, workspace (split panes), status bar, plus global hosts for
 * dialogs, palette, context menus and toasts. Wires host events (window state, CLI open requests, close
 * confirmation) and workspace persistence.
 */
import { useEffect, useState } from 'react';
import { api } from '@/bridge/api';
import { connectSession, isPaneLive } from '@/commands/actions';
import { loadRecentCommands, runCommand } from '@/commands/commands';
import { installKeybindings } from '@/commands/keybindings';
import { DialogHost } from '@/components/dialogs/DialogHost';
import { Workspace } from '@/components/layout/Workspace';
import { CommandPalette } from '@/components/palette/CommandPalette';
import { Sidebar } from '@/components/sidebar/Sidebar';
import { StatusBar } from '@/components/statusbar/StatusBar';
import { TitleBar } from '@/components/titlebar/TitleBar';
import { initFilesConnections } from '@/components/files/filesConn';
import { initFilesEvents } from '@/components/files/filesStore';
import { ContextMenuHost, ToastHost } from '@/components/ui';
import { initDataStores, useSessions, useSettings } from '@/store/data';
import { confirmDialog, useUi } from '@/store/ui';
import { tabTitle, useWorkspace } from '@/store/workspace';
import { restoreWorkspace, startWorkspaceAutosave } from '@/store/workspacePersist';
import { initConnEvents } from '@/terminal/connEvents';
import { ThemeController } from '@/theme/ThemeController';
import type { OpenRequest } from '@shared/types';
import './app.css';

function openRequest(req: OpenRequest) {
  const saved = req.savedSessionId ? useSessions.getState().byId(req.savedSessionId) : undefined;
  connectSession(saved ? { ...saved, ...req.session, id: saved.id } : req.session, 'tab', req.password, {
    view: req.view,
    remotePath: req.remotePath
  });
}

async function handleCloseRequest() {
  const live = Object.values(useWorkspace.getState().panes).filter(isPaneLive);
  if (!live.length || !useSettings.getState().settings.behavior.confirmCloseConnected) {
    api.app.confirmClose(true);
    return;
  }
  const ok = await confirmDialog({
    title: 'Exit SSHTerm?',
    message: `${live.length} session${live.length === 1 ? ' is' : 's are'} still connected. Close ${live.length === 1 ? 'it' : 'them'} and exit?`,
    confirmLabel: 'Exit',
    danger: true
  });
  api.app.confirmClose(ok);
}

function useHostWiring(ready: boolean) {
  useEffect(() => {
    const ui = useUi.getState();
    void api.window.isMaximized().then((m) => ui.set({ maximized: !!m })).catch(() => undefined);
    const offs = [
      api.window.onMaximizedChanged((maximized) => useUi.getState().set({ maximized })),
      api.window.onFocusChanged((windowFocused) => useUi.getState().set({ windowFocused })),
      api.app.onFullscreenChanged((fullscreen) => useUi.getState().set({ fullscreen })),
      api.app.onCloseRequested(() => void handleCloseRequest())
    ];
    return () => offs.forEach((off) => off());
  }, []);

  // CLI / second-instance open requests (after data stores are ready so saved sessions resolve).
  useEffect(() => {
    if (!ready) return;
    const off = api.app.onOpenRequest(openRequest);
    void api.app
      .takeOpenRequests()
      .then((list) => (list ?? []).forEach(openRequest))
      .catch(() => undefined);
    return off;
  }, [ready]);

  // Workspace restore + autosave.
  useEffect(() => {
    if (!ready) return;
    const settings = useSettings.getState().settings;
    if (settings.behavior.restoreTabs && useWorkspace.getState().tabs.length === 0) {
      void restoreWorkspace((id) => useSessions.getState().byId(id), settings.defaultSession).catch(() => 0);
    }
    return startWorkspaceAutosave(
      () => useSettings.getState().settings.behavior.restoreTabs,
      (id) => !!useSessions.getState().byId(id)
    );
  }, [ready]);

  // Window title follows the active tab.
  useEffect(() => {
    let last = '';
    const update = () => {
      const ws = useWorkspace.getState();
      const tab = ws.tabs.find((t) => t.id === ws.activeTabId);
      const mode = useSettings.getState().settings.appearance.tabTitle;
      const title = tab ? `${tabTitle(tab, ws.panes, mode)} - SSHTerm` : 'SSHTerm';
      if (title !== last) {
        last = title;
        api.window.setTitle(title);
      }
    };
    update();
    const offs = [useWorkspace.subscribe(update), useSettings.subscribe(update)];
    return () => offs.forEach((off) => off());
  }, []);
}

export function App() {
  const [ready, setReady] = useState(false);
  const showSidebar = useSettings((s) => s.settings.appearance.showSidebar);
  const fullscreen = useUi((s) => s.fullscreen);

  useEffect(() => {
    initConnEvents();
    initFilesEvents();
    initFilesConnections();
    loadRecentCommands();
    const uninstall = installKeybindings((id) => runCommand(id));
    initDataStores()
      .then(() => setReady(true))
      .catch((e) => {
        console.error('Failed to load data', e);
        setReady(true);
      });
    return uninstall;
  }, []);

  useHostWiring(ready);

  return (
    <div className="app" data-fullscreen={fullscreen || undefined}>
      <ThemeController />
      {!fullscreen && <TitleBar />}
      <div className="app__body">
        {showSidebar && !fullscreen && <Sidebar />}
        <main className="app__main">{ready ? <Workspace /> : null}</main>
      </div>
      {!fullscreen && <StatusBar />}
      <DialogHost />
      <CommandPalette />
      <ContextMenuHost />
      <ToastHost />
    </div>
  );
}
