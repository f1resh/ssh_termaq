/**
 * Mock handlers for the `importer` namespace (owner: ui-dialogs): fake PuTTY registry / .reg / ssh_config /
 * JSON backup previews (with warnings, host keys and PuTTY "Default Settings") and an `apply` that really
 * writes into the mock session store, honouring the conflict policy.
 */
import { createDefaultSession, newId, normalizeFolder } from '@shared/defaults';
import type { ImportCandidate, ImportPreview, ImportRequest, ImportResult, ImportSource, KnownHost, SessionConfig } from '@shared/types';
import { mockDb, persistMock } from './core';
import type { MockContext } from './index';

const delay = (ms: number) => new Promise((r) => setTimeout(r, ms));

function candidate(source: ImportSource, key: string, s: Partial<SessionConfig>, warnings: string[] = []): ImportCandidate {
  const session = createDefaultSession({ name: key, ...s });
  return { key, name: session.name, source, session, warnings, exists: false };
}

function markExisting(list: ImportCandidate[], targetFolder: string): ImportCandidate[] {
  return list.map((c) => {
    const folder = normalizeFolder([targetFolder, c.session.folder].filter(Boolean).join('/'));
    const exists = mockDb.sessions.some((s) => s.name.toLowerCase() === c.name.toLowerCase() && normalizeFolder(s.folder) === folder);
    return { ...c, exists };
  });
}

function hostKey(host: string, port: number, keyType: string, seed: number): KnownHost {
  const chars = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';
  let fp = '';
  for (let i = 0; i < 43; i++) fp += chars[(seed * 31 + i * 17 + i * i) % 64];
  return {
    id: newId(),
    host,
    port,
    keyType,
    key: `AAAAC3NzaC1lZDI1NTE5AAAAI${fp.slice(0, 40)}`,
    fingerprint: `SHA256:${fp}`,
    addedAt: Date.now(),
    source: 'putty-import',
    comment: ''
  };
}

function puttyPreview(source: 'putty-registry' | 'putty-regfile'): ImportPreview {
  const pc = (key: string, s: Partial<SessionConfig>, w?: string[]) => candidate(source, key, s, w);
  const candidates = [
    pc('raspberrypi', { host: 'raspberrypi.local', username: 'pi' }),
    pc('router', { protocol: 'telnet', host: '192.168.1.1', port: 23 }),
    pc('build-server', {
      host: 'build.internal.example',
      username: 'ci',
      ssh: { ...createDefaultSession().ssh, compression: true, auth: { ...createDefaultSession().ssh.auth, privateKeyPath: 'C:\\Users\\mock\\keys\\ci.ppk' } }
    }),
    pc('legacy-switch', { host: '10.20.0.2', username: 'admin' }, [
      'Uses SSH-1 only ("SshProt"=0); SSH-1 is not supported, the session will try SSH-2.',
      'Cipher list contains "des", which is not supported and was dropped.'
    ]),
    pc('db via jump', {
      host: 'db-01.prod.example',
      username: 'postgres',
      proxy: { ...createDefaultSession().proxy, type: 'command', localCommand: 'plink.exe -nc %host:%port bastion' }
    }, ['Local proxy command refers to plink.exe; make sure it is on PATH.']),
    pc('Console COM3', {
      protocol: 'serial',
      port: 0,
      serial: { line: 'COM3', baudRate: 115200, dataBits: 8, stopBits: 1, parity: 'none', flowControl: 'none' }
    }),
    pc('old-colours', {
      host: 'vintage.example.org',
      appearance: {
        customScheme: {
          id: 'putty-old-colours',
          name: 'old-colours (PuTTY)',
          variant: 'dark',
          foreground: '#bbbbbb',
          background: '#000040',
          cursor: '#00ff00',
          cursorAccent: '#000000',
          selectionBackground: '#bbbbbb',
          black: '#000000',
          red: '#bb0000',
          green: '#00bb00',
          yellow: '#bbbb00',
          blue: '#0000bb',
          magenta: '#bb00bb',
          cyan: '#00bbbb',
          white: '#bbbbbb',
          brightBlack: '#555555',
          brightRed: '#ff5555',
          brightGreen: '#55ff55',
          brightYellow: '#ffff55',
          brightBlue: '#5555ff',
          brightMagenta: '#ff55ff',
          brightCyan: '#55ffff',
          brightWhite: '#ffffff'
        }
      }
    }, ['Font "Fixedsys" is not a TrueType font; the global terminal font is used instead.']),
    pc('tunnels-only', {
      host: 'gateway.example.com',
      username: 'tunnel',
      ssh: {
        ...createDefaultSession().ssh,
        noShell: true,
        tunnels: [
          { id: newId(), type: 'local', enabled: true, bindAddress: '', sourcePort: 5432, destHost: 'db.internal', destPort: 5432, addressFamily: 'auto', description: '' },
          { id: newId(), type: 'dynamic', enabled: true, bindAddress: '', sourcePort: 1080, destHost: '', destPort: 0, addressFamily: 'auto', description: '' }
        ]
      }
    }, ['Port forwarding "4L8080" requested IPv4 only; kept as IPv4.'])
  ];
  return {
    source,
    candidates: markExisting(candidates, 'PuTTY'),
    hostKeys: [
      hostKey('raspberrypi.local', 22, 'ssh-ed25519', 3),
      hostKey('build.internal.example', 22, 'ecdsa-sha2-nistp256', 7),
      hostKey('10.20.0.2', 22, 'ssh-rsa', 11),
      hostKey('gateway.example.com', 22, 'ssh-ed25519', 13)
    ],
    defaultSettings: createDefaultSession({
      id: 'default',
      name: 'Default Settings',
      terminal: { ...createDefaultSession().terminal, scrollbackLines: 20000, bell: 'none' },
      appearance: { fontFamily: "'Consolas', monospace", fontSize: 12 }
    }),
    errors: source === 'putty-registry' ? ['Session "broken%20entry": invalid port number 99999; skipped.'] : [],
    origin: source === 'putty-registry' ? 'HKEY_CURRENT_USER\\Software\\SimonTatham\\PuTTY\\Sessions' : 'C:\\Users\\mock\\Desktop\\putty-sessions.reg'
  };
}

function sshConfigPreview(path?: string): ImportPreview {
  const sc = (key: string, s: Partial<SessionConfig>, w?: string[]) => candidate('ssh-config', key, s, w);
  return {
    source: 'ssh-config',
    candidates: markExisting(
      [
        sc('github.com', { host: 'github.com', username: 'git', ssh: { ...createDefaultSession().ssh, auth: { ...createDefaultSession().ssh.auth, privateKeyPath: '~/.ssh/id_ed25519' } } }),
        sc('bastion', { host: 'bastion.example.com', username: 'ops', port: 2222 }),
        sc('app-*', { host: 'app-1.internal', username: 'deploy' }, ['Host pattern "app-*" contains wildcards; imported with the first concrete name.']),
        sc('staging', { host: 'staging.example.com', username: 'ubuntu' }, ['"Match" blocks are not supported and were ignored.'])
      ],
      'SSH config'
    ),
    hostKeys: [],
    errors: [],
    origin: path || 'C:\\Users\\mock\\.ssh\\config'
  };
}

function jsonPreview(): ImportPreview {
  const candidates = mockDb.sessions.slice(0, 4).map((s) => ({
    key: s.id,
    name: s.name,
    source: 'json' as const,
    session: { ...structuredClone(s), id: newId() },
    warnings: [],
    exists: false
  }));
  return {
    source: 'json',
    candidates: markExisting(candidates, ''),
    hostKeys: [],
    errors: [],
    origin: 'C:\\Users\\mock\\Documents\\sshterm-backup.json'
  };
}

function uniqueName(name: string, folder: string): string {
  const taken = (n: string) => mockDb.sessions.some((s) => s.name.toLowerCase() === n.toLowerCase() && normalizeFolder(s.folder) === folder);
  if (!taken(name)) return name;
  for (let i = 2; ; i++) if (!taken(`${name} (${i})`)) return `${name} (${i})`;
}

function apply(req: ImportRequest, ctx: MockContext): ImportResult {
  const result: ImportResult = { imported: 0, overwritten: 0, skipped: 0, hostKeysImported: 0, errors: [] };
  for (const c of req.candidates) {
    const folder = normalizeFolder([req.targetFolder, c.session.folder].filter(Boolean).join('/'));
    const s: SessionConfig = { ...structuredClone(c.session), folder, name: c.name, updatedAt: Date.now() };
    const existing = mockDb.sessions.findIndex((x) => x.name.toLowerCase() === c.name.toLowerCase() && normalizeFolder(x.folder) === folder);
    if (existing >= 0) {
      if (req.conflict === 'skip') {
        result.skipped++;
        continue;
      }
      if (req.conflict === 'overwrite') {
        mockDb.sessions[existing] = { ...s, id: mockDb.sessions[existing].id };
        result.overwritten++;
        continue;
      }
      s.name = uniqueName(c.name, folder);
    }
    s.id = newId();
    mockDb.sessions.push(s);
    result.imported++;
  }
  if (req.targetFolder && !mockDb.folders.includes(normalizeFolder(req.targetFolder))) mockDb.folders.push(normalizeFolder(req.targetFolder));
  for (const k of req.hostKeys) {
    const dup = mockDb.knownHosts.some((h) => h.host === k.host && h.port === k.port && h.key === k.key);
    if (!dup) {
      mockDb.knownHosts.push({ ...k, id: newId(), addedAt: Date.now() });
      result.hostKeysImported++;
    }
  }
  if (req.defaultSettings) {
    mockDb.settings.defaultSession = { ...structuredClone(req.defaultSettings), id: 'default', name: 'Default Settings' };
    ctx.emit('settings:onChanged', structuredClone(mockDb.settings));
  }
  persistMock();
  ctx.emit('sessions:onChanged');
  if (result.hostKeysImported) ctx.emit('knownHosts:onChanged');
  return result;
}

export function registerImporterMocks(ctx: MockContext): void {
  ctx.handle('importer:scanPuttyRegistry', async () => {
    await delay(250);
    return puttyPreview('putty-registry');
  });
  ctx.handle('importer:parsePuttyRegFile', async () => {
    await delay(200);
    return puttyPreview('putty-regfile');
  });
  ctx.handle('importer:scanSshConfig', async (path?: string) => {
    await delay(150);
    return sshConfigPreview(path ?? undefined);
  });
  ctx.handle('importer:parseJsonFile', async () => {
    await delay(150);
    return jsonPreview();
  });
  ctx.handle('importer:apply', async (req: ImportRequest) => {
    await delay(500);
    return apply(req, ctx);
  });
  ctx.handle('importer:exportPuttyRegFile', async (ids: string[]) => {
    await delay(100);
    return ids.length ? 'C:\\Users\\mock\\Documents\\sshterm-sessions.reg' : null;
  });
  ctx.handle('importer:exportJsonFile', async () => {
    await delay(100);
    return 'C:\\Users\\mock\\Documents\\sshterm-sessions.json';
  });
}
