import { createDefaultSession, createDefaultSettings, deepMerge, normalizeFolder, normalizeSession } from '@shared/defaults';
import type { AppSettings, KnownHost, SessionConfig } from '@shared/types';
import type { MockContext } from './index';

const LS_KEY = 'sshtermaq.mock.v1';

interface MockDb {
  sessions: SessionConfig[];
  folders: string[];
  settings: AppSettings;
  knownHosts: KnownHost[];
  state: Record<string, unknown>;
  passwords: Record<string, string>;
}

function seed(): MockDb {
  const mk = (o: Partial<SessionConfig>) => createDefaultSession(o);
  return {
    sessions: [
      mk({ name: 'web-01', folder: 'Production/Web', host: 'web-01.example.com', username: 'deploy', color: '#e5534b' }),
      mk({ name: 'web-02', folder: 'Production/Web', host: 'web-02.example.com', username: 'deploy', color: '#e5534b' }),
      mk({ name: 'db-primary', folder: 'Production', host: '10.0.0.12', username: 'postgres' }),
      mk({ name: 'staging', folder: 'Staging', host: 'staging.example.com', username: 'ubuntu', color: '#3fb950' }),
      mk({ name: 'router', protocol: 'telnet', port: 23, host: '192.168.1.1' }),
      mk({ name: 'Console COM3', protocol: 'serial', port: 0, host: '', serial: { line: 'COM3', baudRate: 115200, dataBits: 8, stopBits: 1, parity: 'none', flowControl: 'none' } }),
      mk({ name: 'raspberrypi', host: 'raspberrypi.local', username: 'pi' })
    ],
    folders: ['Production', 'Production/Web', 'Staging'],
    settings: createDefaultSettings(),
    knownHosts: [],
    state: {},
    passwords: {}
  };
}

function load(): MockDb {
  try {
    const raw = globalThis.localStorage?.getItem(LS_KEY);
    if (raw) return JSON.parse(raw) as MockDb;
  } catch {
    /* ignore */
  }
  return seed();
}

export const mockDb: MockDb = load();

export function persistMock(): void {
  try {
    globalThis.localStorage?.setItem(LS_KEY, JSON.stringify(mockDb));
  } catch {
    /* ignore */
  }
}

function allFolders(): string[] {
  const set = new Set<string>();
  const add = (f: string) => {
    const parts = normalizeFolder(f).split('/').filter(Boolean);
    for (let i = 1; i <= parts.length; i++) set.add(parts.slice(0, i).join('/'));
  };
  mockDb.folders.forEach(add);
  mockDb.sessions.forEach((s) => add(s.folder));
  return [...set].sort((a, b) => a.localeCompare(b));
}

export function registerCoreMocks(ctx: MockContext): void {
  const changed = () => {
    persistMock();
    ctx.emit('sessions:onChanged');
  };

  /* sessions */
  ctx.handle('sessions:list', () => mockDb.sessions);
  ctx.handle('sessions:save', (s: SessionConfig) => {
    const n = normalizeSession(s);
    n.updatedAt = Date.now();
    const i = mockDb.sessions.findIndex((x) => x.id === n.id);
    if (i >= 0) mockDb.sessions[i] = n;
    else mockDb.sessions.push(n);
    changed();
    return n;
  });
  ctx.handle('sessions:saveMany', (list: SessionConfig[]) => {
    for (const s of list) {
      const n = normalizeSession(s);
      const i = mockDb.sessions.findIndex((x) => x.id === n.id);
      if (i >= 0) mockDb.sessions[i] = n;
      else mockDb.sessions.push(n);
    }
    changed();
  });
  ctx.handle('sessions:delete', (ids: string[]) => {
    mockDb.sessions = mockDb.sessions.filter((s) => !ids.includes(s.id));
    changed();
  });
  ctx.handle('sessions:folders', () => allFolders());
  ctx.handle('sessions:createFolder', (p: string) => {
    mockDb.folders.push(normalizeFolder(p));
    changed();
  });
  ctx.handle('sessions:renameFolder', (from: string, to: string) => {
    const a = normalizeFolder(from);
    const b = normalizeFolder(to);
    const re = (f: string) => (f === a ? b : f.startsWith(a + '/') ? b + f.slice(a.length) : f);
    mockDb.folders = mockDb.folders.map(re);
    mockDb.sessions.forEach((s) => (s.folder = re(s.folder)));
    changed();
  });
  ctx.handle('sessions:deleteFolder', (p: string, deleteSessions: boolean) => {
    const a = normalizeFolder(p);
    const inside = (f: string) => f === a || f.startsWith(a + '/');
    const parent = a.includes('/') ? a.slice(0, a.lastIndexOf('/')) : '';
    mockDb.folders = mockDb.folders.filter((f) => !inside(f));
    if (deleteSessions) mockDb.sessions = mockDb.sessions.filter((s) => !inside(s.folder));
    else mockDb.sessions.forEach((s) => inside(s.folder) && (s.folder = parent + s.folder.slice(a.length).replace(/^\//, parent ? '/' : '')));
    changed();
  });

  /* credentials */
  ctx.handle('credentials:hasPassword', (id: string) => id in mockDb.passwords);
  ctx.handle('credentials:setPassword', (id: string, pw: string) => {
    mockDb.passwords[id] = pw;
    persistMock();
  });
  ctx.handle('credentials:clearPassword', (id: string) => {
    delete mockDb.passwords[id];
    persistMock();
  });

  /* settings */
  ctx.handle('settings:get', () => mockDb.settings);
  ctx.handle('settings:update', (patch: unknown) => {
    mockDb.settings = deepMerge(mockDb.settings, patch);
    persistMock();
    ctx.emit('settings:onChanged', structuredClone(mockDb.settings));
    return mockDb.settings;
  });

  /* known hosts */
  ctx.handle('knownHosts:list', () => mockDb.knownHosts);
  ctx.handle('knownHosts:remove', (ids: string[]) => {
    mockDb.knownHosts = mockDb.knownHosts.filter((k) => !ids.includes(k.id));
    persistMock();
    ctx.emit('knownHosts:onChanged');
  });
  ctx.handle('knownHosts:importOpenSsh', () => 0);

  /* app */
  ctx.handle('app:info', () => ({
    name: 'SSHTermaq',
    version: '0.1.0-mock',
    electron: '',
    chrome: navigator.userAgent,
    node: '',
    platform: 'browser-mock',
    userDataDir: 'C:\\Users\\mock\\AppData\\Roaming\\SSHTermaq',
    portable: false
  }));
  ctx.handle('app:systemPrefersDark', () => globalThis.matchMedia?.('(prefers-color-scheme: dark)').matches ?? true);
  ctx.handle('app:takeOpenRequests', () => []);
  ctx.handle('app:getState', (key: string) => mockDb.state[key]);
  ctx.handle('app:setState', (key: string, value: unknown) => {
    mockDb.state[key] = value;
    persistMock();
  });
  ctx.handle('app:readClipboard', async () => {
    try {
      return await navigator.clipboard.readText();
    } catch {
      return '';
    }
  });
  ctx.on('app:writeClipboard', (text: string) => void navigator.clipboard?.writeText(text).catch(() => undefined));
  ctx.handle('app:logDirectory', () => 'C:\\Users\\mock\\AppData\\Roaming\\SSHTermaq\\logs');
  ctx.handle('app:readFileDataUrl', () => null);
  ctx.handle('app:openExternal', (url: string) => void globalThis.open?.(url, '_blank'));
  ctx.on('app:confirmClose', () => undefined);
  ctx.on('app:setThemeSource', () => undefined);
  ctx.on('app:setBackgroundMaterial', () => undefined);
  ctx.on('app:setUiScale', () => undefined);
  ctx.on('app:beep', () => undefined);
  ctx.on('app:toggleFullscreen', () => {
    if (document.fullscreenElement) void document.exitFullscreen();
    else void document.documentElement.requestFullscreen?.();
  });

  /* window */
  ctx.handle('window:isMaximized', () => false);
  ctx.on('window:minimize', () => undefined);
  ctx.on('window:toggleMaximize', () => undefined);
  ctx.on('window:close', () => undefined);
  ctx.on('window:setTitle', (t: string) => (document.title = t));
  ctx.on('window:flash', () => undefined);

  /* dialogs */
  ctx.handle('dialog:openFile', () => null);
  ctx.handle('dialog:openFiles', () => []);
  ctx.handle('dialog:openDirectory', () => null);
  ctx.handle('dialog:saveFile', () => null);
  ctx.handle('dialog:saveTextFile', () => null);

  /* serial */
  ctx.handle('serial:listPorts', () => [
    { path: 'COM1', friendlyName: 'Communications Port (COM1)' },
    { path: 'COM3', friendlyName: 'USB Serial Device (COM3)', manufacturer: 'FTDI' }
  ]);
}
