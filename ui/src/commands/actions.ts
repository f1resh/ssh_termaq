/**
 * Shell actions shared by the sidebar, tab strip, palette, welcome screen and commands: opening sessions,
 * session/folder CRUD with confirmations, and tab/pane closing with "connected" confirmations.
 */
import { api } from '@/bridge/api';
import { toast } from '@/components/ui';
import { choiceDialog } from '@/components/layout/ChoiceDialog';
import { useSessions, useSettings } from '@/store/data';
import { confirmDialog, inputDialog, useUi } from '@/store/ui';
import { paneIdsOf, useWorkspace, type OpenTarget, type PaneState } from '@/store/workspace';
import { newId, normalizeFolder, sessionLabel } from '@shared/defaults';
import type { SessionConfig } from '@shared/types';

const errorText = (e: unknown) => (e instanceof Error ? e.message : String(e));

/** Reports a failed async action as an error toast. */
export function reportError(title: string, e: unknown): void {
  console.error(title, e);
  toast.error(title, errorText(e));
}

/* -------------------------------------------------------------------------- */
/* Opening sessions                                                            */
/* -------------------------------------------------------------------------- */

/** A fresh, unsaved session built from the "Default Settings" template (new id, no name). */
export function createAdHocSession(patch: Partial<SessionConfig> = {}): SessionConfig {
  const tpl = structuredClone(useSettings.getState().settings.defaultSession);
  const now = Date.now();
  return {
    ...tpl,
    id: newId(),
    name: '',
    folder: '',
    notes: '',
    createdAt: now,
    updatedAt: now,
    lastUsedAt: 0,
    ...patch
  };
}

/** Opens a session (saved or ad-hoc) in a new tab or split. Saved sessions get their lastUsedAt bumped. */
export function connectSession(session: SessionConfig, target: OpenTarget = 'tab', password?: string): string {
  const saved = useSessions.getState().byId(session.id);
  const paneId = useWorkspace.getState().openSession(session, { target, sessionId: saved ? saved.id : null, password });
  if (saved) {
    api.sessions.save({ ...saved, lastUsedAt: Date.now() }).catch((e) => console.warn('lastUsedAt update failed', e));
  }
  return paneId;
}

export function connectSavedSession(id: string, target: OpenTarget = 'tab'): void {
  const s = useSessions.getState().byId(id);
  if (!s) {
    toast.warning('Session not found', 'It may have been deleted.');
    return;
  }
  connectSession(s, target);
}

/* -------------------------------------------------------------------------- */
/* Session CRUD                                                                */
/* -------------------------------------------------------------------------- */

/** Opens the session editor with a new draft from the default-session template. */
export function newSessionDraft(folder = ''): void {
  const draft = createAdHocSession({ folder: normalizeFolder(folder) });
  useUi.getState().openDialog({ type: 'sessionEditor', draft, folder: draft.folder });
}

export function editSession(id: string, section?: string): void {
  useUi.getState().openDialog({ type: 'sessionEditor', sessionId: id, section });
}

/** Unique-ish copy name: "web-01 (copy)", "web-01 (copy 2)", ... */
export function copyName(name: string, existing: string[]): string {
  const base = name.replace(/ \(copy(?: \d+)?\)$/, '') || 'Session';
  const taken = new Set(existing);
  let candidate = `${base} (copy)`;
  for (let i = 2; taken.has(candidate); i++) candidate = `${base} (copy ${i})`;
  return candidate;
}

export async function duplicateSession(s: SessionConfig): Promise<void> {
  const names = useSessions.getState().sessions.filter((x) => x.folder === s.folder).map((x) => x.name);
  const now = Date.now();
  try {
    await api.sessions.save({
      ...structuredClone(s),
      id: newId(),
      name: copyName(sessionLabel(s), names),
      createdAt: now,
      updatedAt: now,
      lastUsedAt: 0
    });
  } catch (e) {
    reportError('Could not duplicate session', e);
  }
}

export async function renameSession(s: SessionConfig): Promise<void> {
  const name = await inputDialog({
    title: 'Rename session',
    label: 'Name',
    initial: s.name || sessionLabel(s),
    confirmLabel: 'Rename',
    validate: (v) => (v.trim() ? null : 'Name cannot be empty')
  });
  if (name === null || name.trim() === s.name) return;
  try {
    await api.sessions.save({ ...s, name: name.trim() });
  } catch (e) {
    reportError('Could not rename session', e);
  }
}

export async function moveSessions(sessions: SessionConfig[], folder: string): Promise<void> {
  const target = normalizeFolder(folder);
  const changed = sessions.filter((s) => s.folder !== target).map((s) => ({ ...s, folder: target }));
  if (!changed.length) return;
  try {
    if (changed.length === 1) await api.sessions.save(changed[0]);
    else await api.sessions.saveMany(changed);
  } catch (e) {
    reportError('Could not move sessions', e);
  }
}

export async function deleteSessions(sessions: SessionConfig[]): Promise<void> {
  if (!sessions.length) return;
  const ok = await confirmDialog({
    title: sessions.length === 1 ? 'Delete session' : `Delete ${sessions.length} sessions`,
    message:
      sessions.length === 1
        ? `Delete "${sessionLabel(sessions[0])}"? This cannot be undone.`
        : `Delete ${sessions.length} sessions? This cannot be undone.`,
    confirmLabel: 'Delete',
    danger: true
  });
  if (!ok) return;
  try {
    await api.sessions.delete(sessions.map((s) => s.id));
  } catch (e) {
    reportError('Could not delete', e);
  }
}

export async function exportSessions(ids: string[], format: 'reg' | 'json'): Promise<void> {
  try {
    const path = format === 'reg' ? await api.importer.exportPuttyRegFile(ids) : await api.importer.exportJsonFile(ids);
    if (path) toast.success('Exported', path);
  } catch (e) {
    reportError('Export failed', e);
  }
}

/* -------------------------------------------------------------------------- */
/* Folders                                                                     */
/* -------------------------------------------------------------------------- */

const folderNameError = (v: string) => {
  const t = v.trim();
  if (!t) return 'Name cannot be empty';
  if (t.includes('/')) return 'Use a name without "/"';
  return null;
};

export async function createFolder(parent = ''): Promise<string | null> {
  const name = await inputDialog({
    title: parent ? `New folder in "${parent}"` : 'New folder',
    label: 'Folder name',
    placeholder: 'e.g. Production',
    confirmLabel: 'Create',
    validate: folderNameError
  });
  if (name === null) return null;
  const path = normalizeFolder(parent ? `${parent}/${name}` : name);
  try {
    await api.sessions.createFolder(path);
    return path;
  } catch (e) {
    reportError('Could not create folder', e);
    return null;
  }
}

export async function renameFolder(path: string): Promise<void> {
  const parts = path.split('/');
  const leaf = parts.pop() ?? '';
  const name = await inputDialog({
    title: 'Rename folder',
    label: 'Folder name',
    initial: leaf,
    confirmLabel: 'Rename',
    validate: folderNameError
  });
  if (name === null || name.trim() === leaf) return;
  try {
    await api.sessions.renameFolder(path, normalizeFolder([...parts, name.trim()].join('/')));
  } catch (e) {
    reportError('Could not rename folder', e);
  }
}

/** Moves a folder (and everything beneath it) under a new parent ('' = root). */
export async function moveFolder(path: string, newParent: string): Promise<void> {
  const leaf = path.split('/').pop() ?? path;
  const target = normalizeFolder(newParent ? `${newParent}/${leaf}` : leaf);
  if (target === path || normalizeFolder(newParent) === path || normalizeFolder(newParent).startsWith(path + '/')) return;
  if (useSessions.getState().folders.includes(target)) {
    toast.warning('Folder exists', `"${target}" already exists.`);
    return;
  }
  try {
    await api.sessions.renameFolder(path, target);
  } catch (e) {
    reportError('Could not move folder', e);
  }
}

export async function deleteFolder(path: string): Promise<void> {
  const inside = useSessions.getState().sessions.filter((s) => s.folder === path || s.folder.startsWith(path + '/'));
  let deleteContents = false;
  if (inside.length) {
    const choice = await choiceDialog({
      title: 'Delete folder',
      message: `"${path}" contains ${inside.length} session${inside.length === 1 ? '' : 's'}. Keep them (moved to the parent folder) or delete them too?`,
      choices: [
        { value: 'keep', label: 'Keep sessions', variant: 'primary' },
        { value: 'delete', label: 'Delete all', variant: 'danger' }
      ]
    });
    if (!choice) return;
    deleteContents = choice === 'delete';
  } else {
    const ok = await confirmDialog({ title: 'Delete folder', message: `Delete the empty folder "${path}"?`, confirmLabel: 'Delete', danger: true });
    if (!ok) return;
  }
  try {
    await api.sessions.deleteFolder(path, deleteContents);
  } catch (e) {
    reportError('Could not delete folder', e);
  }
}

/* -------------------------------------------------------------------------- */
/* Tabs / panes                                                                */
/* -------------------------------------------------------------------------- */

const LIVE: PaneState['status'][] = ['connected', 'connecting', 'authenticating'];

export function isPaneLive(p: PaneState | undefined): boolean {
  return !!p && LIVE.includes(p.status);
}

function livePanesOfTabs(tabIds: string[]): PaneState[] {
  const ws = useWorkspace.getState();
  return ws.tabs
    .filter((t) => tabIds.includes(t.id))
    .flatMap((t) => paneIdsOf(t.layout))
    .map((id) => ws.panes[id])
    .filter(isPaneLive);
}

/** Asks for confirmation when closing would drop live connections (settings.behavior.confirmCloseConnected). */
export async function confirmDropConnections(live: PaneState[], what: string): Promise<boolean> {
  if (!live.length || !useSettings.getState().settings.behavior.confirmCloseConnected) return true;
  const names = [...new Set(live.map((p) => sessionLabel(p.session) || p.session.host))];
  return confirmDialog({
    title: `Close ${what}?`,
    message:
      live.length === 1
        ? `"${names[0]}" is still connected. Closing will disconnect it.`
        : `${live.length} sessions are still connected (${names.slice(0, 3).join(', ')}${names.length > 3 ? ', …' : ''}). Closing will disconnect them.`,
    confirmLabel: 'Close',
    danger: true
  });
}

export async function closeTabs(tabIds: string[], what = tabIds.length === 1 ? 'tab' : `${tabIds.length} tabs`): Promise<void> {
  if (!tabIds.length) return;
  if (!(await confirmDropConnections(livePanesOfTabs(tabIds), what))) return;
  const ws = useWorkspace.getState();
  const sftp = useUi.getState().sftpPaneId;
  for (const id of tabIds) {
    const tab = ws.tabs.find((t) => t.id === id);
    if (tab && sftp && paneIdsOf(tab.layout).includes(sftp)) useUi.getState().setSftpPane(null);
    ws.closeTab(id);
  }
}

export function closeTab(tabId: string): Promise<void> {
  return closeTabs([tabId]);
}

export function closeOtherTabs(tabId: string): Promise<void> {
  const ids = useWorkspace.getState().tabs.filter((t) => t.id !== tabId).map((t) => t.id);
  return closeTabs(ids, 'other tabs');
}

export function closeTabsToRight(tabId: string): Promise<void> {
  const tabs = useWorkspace.getState().tabs;
  const i = tabs.findIndex((t) => t.id === tabId);
  return closeTabs(tabs.slice(i + 1).map((t) => t.id), 'tabs to the right');
}

export async function closePane(paneId: string): Promise<void> {
  const p = useWorkspace.getState().panes[paneId];
  if (!p) return;
  if (!(await confirmDropConnections(isPaneLive(p) ? [p] : [], 'pane'))) return;
  if (useUi.getState().sftpPaneId === paneId) useUi.getState().setSftpPane(null);
  useWorkspace.getState().closePane(paneId);
}

export function restartTab(tabId: string): void {
  const ws = useWorkspace.getState();
  const tab = ws.tabs.find((t) => t.id === tabId);
  if (!tab) return;
  for (const id of paneIdsOf(tab.layout)) ws.restartPane(id);
}

/** Opens the session editor for a pane: its saved session, or a draft of its effective config. */
export function changePaneSettings(paneId: string): void {
  const p = useWorkspace.getState().panes[paneId];
  if (!p) return;
  const saved = useSessions.getState().byId(p.sessionId);
  if (saved) editSession(saved.id);
  else useUi.getState().openDialog({ type: 'sessionEditor', draft: structuredClone(p.session) });
}

export function toggleSftp(paneId: string | undefined): void {
  const ui = useUi.getState();
  if (!paneId) return;
  const p = useWorkspace.getState().panes[paneId];
  if (!p || p.session.protocol !== 'ssh') {
    toast.info('SFTP is available for SSH sessions only');
    return;
  }
  ui.setSftpPane(ui.sftpPaneId === paneId ? null : paneId);
}

export async function toggleSidebar(): Promise<void> {
  const cur = useSettings.getState().settings.appearance.showSidebar;
  try {
    await useSettings.getState().update({ appearance: { showSidebar: !cur } });
  } catch (e) {
    reportError('Could not update settings', e);
  }
}
