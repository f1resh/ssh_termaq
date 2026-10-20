/**
 * Mock handlers for the `sftp` namespace: an in-memory fake server filesystem plus simulated transfers (progress,
 * conflicts, moves into / out of the mock local disks), find, synchronize and edit events, so the file manager works
 * in the browser preview.
 */
import type {
  ConflictResolution,
  EditEvent,
  FindRequest,
  SftpEntry,
  SyncItem,
  SyncRequest,
  TransferConflict,
  TransferOptions,
  TransferProgress
} from '@shared/types';
import type { MockContext } from './index';
import { mockLocal, mockLocalFs, touchLocalDir } from './local';

type NodeType = SftpEntry['type'];

interface FsNode {
  name: string;
  type: NodeType;
  size: number;
  mtime: number;
  mode: number;
  children?: Map<string, FsNode>;
  /** Symlink target (absolute path). */
  target?: string;
}

const S_IFDIR = 0o040000;
const S_IFREG = 0o100000;
const S_IFLNK = 0o120000;
const HOME = '/home/demo';
const DAY = 86_400_000;

function typeBits(t: NodeType): number {
  return t === 'dir' ? S_IFDIR : t === 'link' ? S_IFLNK : S_IFREG;
}

function permString(mode: number): string {
  const t = (mode & 0o170000) === S_IFDIR ? 'd' : (mode & 0o170000) === S_IFLNK ? 'l' : '-';
  let s = t;
  const rwx = 'rwx';
  for (let i = 0; i < 9; i++) s += mode & (0o400 >> i) ? rwx[i % 3] : '-';
  return s;
}

function dir(name: string, mode = 0o755, ageDays = 3, children: FsNode[] = []): FsNode {
  return {
    name,
    type: 'dir',
    size: 4096,
    mtime: Date.now() - ageDays * DAY,
    mode: S_IFDIR | mode,
    children: new Map(children.map((c) => [c.name, c]))
  };
}

function file(name: string, size: number, ageDays = 1, mode = 0o644): FsNode {
  return { name, type: 'file', size, mtime: Date.now() - ageDays * DAY, mode: S_IFREG | mode };
}

function link(name: string, target: string): FsNode {
  return { name, type: 'link', size: target.length, mtime: Date.now() - 20 * DAY, mode: S_IFLNK | 0o777, target };
}

function buildTree(): FsNode {
  const projectFiles = [
    file('README.md', 4_812, 0.2),
    file('CMakeLists.txt', 6_204, 0.5),
    file('package.json', 1_022, 2),
    dir('src', 0o755, 0.1, [
      file('main.cpp', 12_403, 0.1),
      file('SftpService.cpp', 48_211, 0.05),
      file('SftpService.h', 1_402, 0.05)
    ]),
    dir('docs', 0o755, 4, [file('ARCHITECTURE.md', 18_330, 4)]),
    dir('.git', 0o755, 0.1, [file('HEAD', 21, 0.1), file('config', 312, 30)])
  ];
  const logs = Array.from({ length: 40 }, (_, i) =>
    file(`app-2026-09-${String(i % 30 + 1).padStart(2, '0')}${i >= 30 ? '.1' : ''}.log`, 40_000 + i * 7_919, i + 1)
  );
  const home = dir('demo', 0o750, 0.01, [
    file('.bashrc', 3_771, 120),
    file('.profile', 807, 120),
    file('.bash_history', 18_420, 0.01, 0o600),
    dir('.ssh', 0o700, 60, [file('authorized_keys', 1_162, 60, 0o600), file('known_hosts', 2_840, 5, 0o644)]),
    dir('projects', 0o755, 0.1, [dir('sshtermaq', 0o755, 0.1, projectFiles), dir('dotfiles', 0o755, 90)]),
    dir('logs', 0o755, 0.5, logs),
    dir('Downloads', 0o755, 10, [file('ubuntu-24.04-server.iso', 2_754_981_888, 10), file('photo.jpg', 3_481_002, 12)]),
    file('backup-2026-10-01.tar.gz', 1_288_490_188, 12),
    file('notes.txt', 2_048, 0.3),
    file('deploy.sh', 1_337, 6, 0o755),
    file('report.pdf', 845_112, 2),
    link('current', `${HOME}/projects/sshtermaq`),
    link('latest.log', `${HOME}/logs/app-2026-09-01.log`)
  ]);
  return dir('', 0o755, 200, [
    dir('home', 0o755, 200, [home]),
    dir('etc', 0o755, 30, [file('hostname', 12, 200), file('hosts', 221, 40), file('ssh_config', 1_650, 100)]),
    dir('var', 0o755, 30, [dir('log', 0o755, 0.01, [file('syslog', 2_311_022, 0.01, 0o640), file('auth.log', 88_133, 0.01, 0o640)])]),
    dir('tmp', 0o777, 0.01),
    dir('usr', 0o755, 200, [dir('bin', 0o755, 30), dir('lib', 0o755, 30), dir('share', 0o755, 30)]),
    dir('root', 0o700, 50)
  ]);
}

function split(path: string): string[] {
  return path.split('/').filter((p) => p && p !== '.');
}

function normalize(path: string, cwd = HOME): string {
  let p = path.trim();
  if (p === '' || p === '.') return cwd;
  if (p === '~') p = HOME;
  else if (p.startsWith('~/')) p = HOME + p.slice(1);
  else if (!p.startsWith('/')) p = `${cwd}/${p}`;
  const out: string[] = [];
  for (const part of split(p)) {
    if (part === '..') out.pop();
    else out.push(part);
  }
  return '/' + out.join('/');
}

function join(dirPath: string, name: string): string {
  return dirPath === '/' ? `/${name}` : `${dirPath}/${name}`;
}

function parentOf(path: string): string {
  const i = path.lastIndexOf('/');
  return i <= 0 ? '/' : path.slice(0, i);
}

function baseName(path: string): string {
  return path.slice(path.lastIndexOf('/') + 1) || '/';
}

function err(message: string): Error {
  return new Error(message);
}

export class MockFs {
  root = buildTree();

  lookup(path: string, follow = true, depth = 0): FsNode | undefined {
    let node: FsNode | undefined = this.root;
    let cur = '/';
    for (const part of split(normalize(path))) {
      if (node?.type === 'link' && node.target && depth < 8) node = this.lookup(node.target, true, depth + 1);
      if (!node || node.type !== 'dir') return undefined;
      node = node.children!.get(part);
      cur = join(cur, part);
    }
    if (follow && node?.type === 'link' && node.target && depth < 8) return this.lookup(node.target, true, depth + 1);
    return node;
  }

  private dirOf(path: string): FsNode {
    const d = this.lookup(path);
    if (!d) throw err(`No such file or directory: ${path}`);
    if (d.type !== 'dir') throw err(`Not a directory: ${path}`);
    return d;
  }

  entry(dirPath: string, n: FsNode): SftpEntry {
    const e: SftpEntry = {
      name: n.name,
      path: join(dirPath, n.name),
      type: n.type,
      size: n.size,
      mtime: n.mtime,
      mode: n.mode,
      permissions: permString(n.mode),
      uid: 1000,
      gid: 1000
    };
    if (n.type === 'link') e.linkTargetIsDir = this.lookup(e.path)?.type === 'dir';
    return e;
  }

  list(path: string): SftpEntry[] {
    const p = normalize(path);
    const d = this.dirOf(p);
    if ((d.mode & 0o005) === 0 && p !== HOME && !p.startsWith(HOME + '/')) throw err(`Permission denied: ${p}`);
    return [...d.children!.values()].map((n) => this.entry(p, n));
  }

  realpath(path: string): string {
    const p = normalize(path);
    const n = this.lookup(p, false);
    if (n?.type === 'link' && n.target) return normalize(n.target);
    if (!this.lookup(p)) throw err(`No such file or directory: ${p}`);
    return p;
  }

  add(path: string, node: Omit<FsNode, 'name'>): void {
    const p = normalize(path);
    const d = this.dirOf(parentOf(p));
    const name = baseName(p);
    if (!name || name === '/') throw err('Invalid name');
    d.children!.set(name, { ...node, name });
    d.mtime = Date.now();
  }

  mkdir(path: string): void {
    const p = normalize(path);
    if (this.lookup(p, false)) throw err(`File already exists: ${p}`);
    this.add(p, { type: 'dir', size: 4096, mtime: Date.now(), mode: S_IFDIR | 0o755, children: new Map() });
  }

  rename(from: string, to: string): void {
    const a = normalize(from);
    const b = normalize(to);
    const node = this.lookup(a, false);
    if (!node) throw err(`No such file or directory: ${a}`);
    if (b === a) return;
    if (b.startsWith(a + '/')) throw err('Cannot move a folder into itself');
    const dst = this.dirOf(parentOf(b));
    this.dirOf(parentOf(a)).children!.delete(node.name);
    node.name = baseName(b);
    dst.children!.set(node.name, node);
  }

  remove(path: string, recursive: boolean): void {
    const p = normalize(path);
    if (p === '/') throw err('Refusing to delete /');
    const node = this.lookup(p, false);
    if (!node) throw err(`No such file or directory: ${p}`);
    if (node.type === 'dir' && node.children!.size > 0 && !recursive) throw err(`Directory not empty: ${p}`);
    this.dirOf(parentOf(p)).children!.delete(node.name);
  }

  chmod(path: string, mode: number): void {
    const node = this.lookup(normalize(path), false);
    if (!node) throw err(`No such file or directory: ${path}`);
    node.mode = typeBits(node.type) | (mode & 0o7777);
  }

  /** Total bytes below a path (files only, links not followed). */
  sizeOf(path: string): number {
    const node = this.lookup(path);
    if (!node) throw err(`No such file or directory: ${path}`);
    const walk = (n: FsNode): number =>
      n.type === 'dir' ? [...n.children!.values()].reduce((s, c) => s + (c.type === 'link' ? 0 : walk(c)), 0) : n.size;
    return walk(node);
  }
}

interface MockTransfer {
  p: TransferProgress;
  opts: TransferOptions;
  timer?: ReturnType<typeof setInterval>;
  onDone?: (ok: boolean) => void;
  /** Waiting for a conflict answer. */
  blocked?: boolean;
  /** Folder transfer (adds a folder on the target side). */
  dir?: boolean;
}

export function registerSftpMocks(ctx: MockContext): void {
  const fs = new MockFs();
  const lfs = mockLocalFs;
  const sessions = new Map<string, string>(); // sftpId -> connId
  const transfers = new Map<string, MockTransfer>();
  const conflicts = new Map<string, MockTransfer>();
  const queue: MockTransfer[] = [];
  const finds = new Map<string, { cancel: boolean }>();
  let nextId = 1;
  /** Simulated link speed in bytes/s (fast enough that demos finish, slow enough to watch). */
  const SPEED = 24 * 1024 * 1024;

  const session = (id: string) => {
    if (!sessions.has(id)) throw err('Unknown or closed file session');
    return id;
  };
  const emit = (t: MockTransfer) => ctx.emit('sftp:onProgress', structuredClone(t.p));
  const running = () => [...transfers.values()].filter((t) => t.p.state === 'running').length;
  const localParent = mockLocal.parent;

  const pump = () => {
    while (running() < 2) {
      const next = queue.find((t) => !t.blocked);
      if (!next) break;
      queue.splice(queue.indexOf(next), 1);
      start(next);
    }
  };

  const finish = (t: MockTransfer, state: TransferProgress['state'], error?: string) => {
    if (t.timer) clearInterval(t.timer);
    t.p.state = state;
    if (error) t.p.error = error;
    if (state === 'done') t.p.filesDone = t.p.filesTotal;
    emit(t);
    transfers.delete(t.p.transferId);
    t.onDone?.(state === 'done');
    pump();
  };

  const complete = (t: MockTransfer) => {
    try {
      if (t.p.direction === 'upload') {
        fs.add(t.p.remotePath, t.dir
          ? { type: 'dir', size: 4096, mtime: Date.now(), mode: S_IFDIR | 0o755, children: new Map() }
          : { type: 'file', size: t.p.total, mtime: Date.now(), mode: S_IFREG | 0o644 });
        if (t.opts.move && lfs.exists(t.p.localPath)) lfs.remove(t.p.localPath);
      } else if (t.p.direction === 'download') {
        if (t.p.origin !== 'open' && t.p.origin !== 'edit') {
          if (lfs.exists(t.p.localPath)) lfs.remove(t.p.localPath);
          lfs.add(t.p.localPath, t.p.total, !!t.dir);
          touchLocalDir(ctx, localParent(t.p.localPath));
        }
        if (t.opts.move) fs.remove(t.p.remotePath, true);
      } else {
        if (t.p.remotePath && fs.lookup(t.p.remotePath, false)) fs.remove(t.p.remotePath, true);
        if (t.p.localPath && lfs.exists(t.p.localPath)) lfs.remove(t.p.localPath);
      }
      finish(t, 'done');
    } catch (e) {
      finish(t, 'error', (e as Error).message);
    }
  };

  const start = (t: MockTransfer) => {
    t.p.state = 'running';
    emit(t);
    const tick = 100;
    t.timer = setInterval(() => {
      // vary the speed a little so the speed readout looks alive
      const step = Math.round(((SPEED * tick) / 1000) * (0.7 + Math.random() * 0.6));
      t.p.transferred = Math.min(t.p.total, t.p.transferred + step);
      if (t.p.filesTotal && t.p.filesTotal > 1) {
        t.p.filesDone = Math.floor((t.p.transferred / Math.max(1, t.p.total)) * t.p.filesTotal);
        t.p.currentFile = `file-${(t.p.filesDone ?? 0) + 1}.dat`;
      }
      if (t.p.transferred >= t.p.total) complete(t);
      else emit(t);
    }, tick);
  };

  /** Target exists: per policy skip / overwrite / rename right away, or ask (blocks the transfer). */
  const checkConflict = (t: MockTransfer, targetExists: boolean, target: { size: number; mtime: number }) => {
    if (!targetExists || t.dir) return;
    const policy = t.opts.conflict ?? 'ask';
    if (policy === 'overwrite' || policy === 'resume') return;
    if (policy === 'skip' || (policy === 'newer' && target.mtime >= Date.now() - DAY)) {
      t.p.skipped = 1;
      t.p.total = 0;
      return;
    }
    if (policy === 'rename') {
      t.p.name = t.p.name.replace(/(\.[^.]*)?$/, ' (1)$1');
      return;
    }
    if (policy !== 'ask') return;
    t.blocked = true;
    const conflictId = `mock-conflict-${nextId++}`;
    conflicts.set(conflictId, t);
    const c: TransferConflict = {
      conflictId,
      transferId: t.p.transferId,
      sftpId: t.p.sftpId,
      direction: t.p.direction === 'upload' ? 'upload' : 'download',
      name: t.p.name,
      source: { path: t.p.direction === 'upload' ? t.p.localPath : t.p.remotePath, size: t.p.total, mtime: Date.now() - DAY / 4 },
      target: { path: t.p.direction === 'upload' ? t.p.remotePath : t.p.localPath, size: target.size, mtime: target.mtime },
      canResume: target.size < t.p.total,
      reason: 'exists',
      multiple: false
    };
    setTimeout(() => ctx.emit('sftp:onConflict', c), 30);
  };

  const enqueue = (
    p: Omit<TransferProgress, 'transferId' | 'state' | 'transferred'>,
    opts: TransferOptions = {},
    extra: { dir?: boolean; targetExists?: boolean; target?: { size: number; mtime: number }; onDone?: (ok: boolean) => void } = {}
  ) => {
    const t: MockTransfer = {
      p: { origin: 'transfer', ...p, move: !!opts.move, transferId: `mock-transfer-${nextId++}`, state: 'queued', transferred: 0 },
      opts,
      dir: extra.dir,
      onDone: extra.onDone
    };
    transfers.set(t.p.transferId, t);
    checkConflict(t, !!extra.targetExists, extra.target ?? { size: 0, mtime: 0 });
    if (t.p.skipped) {
      setTimeout(() => finish(t, 'done'), 0);
      return t.p.transferId;
    }
    queue.push(t);
    emit(t);
    setTimeout(pump, 0);
    return t.p.transferId;
  };

  ctx.handle('sftp:open', (connId: string) => {
    const sftpId = `mock-sftp-${nextId++}`;
    sessions.set(sftpId, connId);
    return { sftpId, home: HOME, protocol: 'sftp', capabilities: { resume: true, preciseTimes: true } };
  });
  ctx.handle('sftp:list', async (id: string, path: string) => {
    session(id);
    await new Promise((r) => setTimeout(r, 60 + Math.random() * 90)); // network latency
    return fs.list(path);
  });
  ctx.handle('sftp:stat', (id: string, path: string) => {
    session(id);
    const n = fs.lookup(path);
    return n ? fs.entry(parentOf(normalize(path)), n) : null;
  });
  ctx.handle('sftp:realpath', (id: string, path: string) => fs.realpath(session(id) && path));
  ctx.handle('sftp:mkdir', (id: string, path: string) => void fs.mkdir(session(id) && path));
  ctx.handle('sftp:rename', (id: string, from: string, to: string) => void fs.rename(session(id) && from, to));
  ctx.handle('sftp:remove', (id: string, path: string, recursive: boolean) => void fs.remove(session(id) && path, !!recursive));
  ctx.handle('sftp:chmod', (id: string, path: string, mode: number) => void fs.chmod(session(id) && path, mode));
  ctx.handle('sftp:download', (id: string, remotePaths: string[], localDir?: string, opts?: TransferOptions) => {
    session(id);
    const dirPath = localDir ?? mockLocal.join(mockLocal.home, 'Downloads');
    return remotePaths.map((rp) => {
      const node = fs.lookup(rp);
      const localPath = mockLocal.join(dirPath, baseName(rp));
      const existing = lfs.stat(localPath);
      return enqueue(
        {
          sftpId: id,
          direction: 'download',
          name: baseName(rp),
          localPath,
          remotePath: rp,
          total: fs.sizeOf(rp),
          filesTotal: node?.type === 'dir' ? 12 : 1,
          filesDone: 0
        },
        opts,
        { dir: node?.type === 'dir', targetExists: !!existing, target: existing ? { size: existing.size, mtime: existing.mtime } : undefined }
      );
    });
  });
  ctx.handle('sftp:upload', (id: string, remoteDir: string, localPaths?: string[], opts?: TransferOptions) => {
    session(id);
    const picked = localPaths ?? [mockLocal.join(mockLocal.home, 'Documents/quarterly-report.xlsx')];
    return picked.map((lp) => {
      const name = lp.split(/[\\/]/).pop() || 'upload.bin';
      const remotePath = join(normalize(remoteDir), name);
      const local = lfs.stat(lp);
      const existing = fs.lookup(remotePath);
      return enqueue(
        {
          sftpId: id,
          direction: 'upload',
          name,
          localPath: lp,
          remotePath,
          total: local ? lfs.sizeOf(lp) : 200_000 + Math.round(Math.random() * 30_000_000),
          filesTotal: local?.type === 'dir' ? 8 : 1,
          filesDone: 0
        },
        opts,
        { dir: local?.type === 'dir', targetExists: !!existing, target: existing ? { size: existing.size, mtime: existing.mtime } : undefined }
      );
    });
  });
  ctx.on('sftp:resolveConflict', (conflictId: string, r: ConflictResolution) => {
    const t = conflicts.get(conflictId);
    conflicts.delete(conflictId);
    if (!t) return;
    t.blocked = false;
    if (r.action === 'cancel') {
      queue.splice(queue.indexOf(t), 1);
      finish(t, 'cancelled');
      return;
    }
    if (r.action === 'skip') {
      queue.splice(queue.indexOf(t), 1);
      t.p.skipped = 1;
      t.p.total = 0;
      finish(t, 'done');
      return;
    }
    if (r.action === 'rename') {
      const name = r.newName || t.p.name.replace(/(\.[^.]*)?$/, ' (1)$1');
      t.p.name = name;
      if (t.p.direction === 'upload') t.p.remotePath = join(parentOf(t.p.remotePath), name);
      else t.p.localPath = mockLocal.join(localParent(t.p.localPath), name);
    }
    pump();
  });
  ctx.on('sftp:cancelTransfer', (transferId: string) => {
    const t = transfers.get(transferId);
    if (!t) return;
    const i = queue.indexOf(t);
    if (i >= 0) queue.splice(i, 1);
    finish(t, 'cancelled');
  });
  ctx.handle('sftp:openRemote', (id: string, remotePath: string) => {
    session(id);
    const node = fs.lookup(remotePath);
    if (!node || node.type === 'dir') throw err(`Not a file: ${remotePath}`);
    return new Promise<void>((resolve, reject) => {
      enqueue(
        {
          sftpId: id,
          direction: 'download',
          name: baseName(remotePath),
          localPath: mockLocal.join(mockLocal.temp, baseName(remotePath)),
          remotePath,
          total: node.size,
          origin: 'open'
        },
        { conflict: 'overwrite' },
        { onDone: (ok) => (ok ? resolve() : reject(err('Cancelled'))) }
      );
    });
  });
  ctx.handle('sftp:find', (id: string, req: FindRequest) => {
    session(id);
    const findId = `mock-find-${nextId++}`;
    const job = { cancel: false };
    finds.set(findId, job);
    const masks = req.pattern
      .split(';')
      .map((m) => m.trim())
      .filter(Boolean)
      .map((m) => new RegExp('^' + m.replace(/[.+^${}()|[\]\\]/g, '\\$&').replace(/\*/g, '.*').replace(/\?/g, '.') + '$', req.caseSensitive ? '' : 'i'));
    const dirs = [normalize(req.root || HOME)];
    let scanned = 0;
    const step = () => {
      if (job.cancel || !dirs.length) {
        ctx.emit('sftp:onFind', { findId, sftpId: id, results: [], scannedDirs: scanned, done: true });
        finds.delete(findId);
        return;
      }
      const dir = dirs.shift()!;
      const results: SftpEntry[] = [];
      try {
        for (const e of fs.list(dir)) {
          if (!req.includeHidden && e.name.startsWith('.')) continue;
          if (masks.some((m) => m.test(e.name))) results.push(e);
          if (e.type === 'dir') dirs.push(e.path);
        }
      } catch {
        /* unreadable folder */
      }
      scanned++;
      ctx.emit('sftp:onFind', { findId, sftpId: id, results, scannedDirs: scanned, currentDir: dir, done: false });
      setTimeout(step, 40);
    };
    setTimeout(step, 30);
    return findId;
  });
  ctx.on('sftp:cancelFind', (findId: string) => {
    const job = finds.get(findId);
    if (job) job.cancel = true;
  });
  ctx.handle('sftp:syncCompare', async (id: string, req: SyncRequest) => {
    session(id);
    await new Promise((r) => setTimeout(r, 400));
    const items: SyncItem[] = [];
    let local: { name: string; type: string; size: number; mtime: number }[] = [];
    try {
      local = lfs.list(req.localDir);
    } catch {
      throw err(`Cannot read folder '${req.localDir}'`);
    }
    const remote = fs.list(req.remoteDir).filter((e) => req.includeHidden || !e.name.startsWith('.'));
    for (const l of local) {
      const r = remote.find((x) => x.name === l.name);
      const isDir = l.type === 'dir';
      if (!r) {
        if (req.direction !== 'download') items.push({ relPath: l.name, isDir, action: 'upload', reason: 'new', local: { size: l.size, mtime: l.mtime } });
        else if (req.deleteExtra) items.push({ relPath: l.name, isDir, action: 'deleteLocal', reason: 'extra', local: { size: l.size, mtime: l.mtime } });
      } else if (!isDir && (r.size !== l.size || Math.abs(r.mtime - l.mtime) > 2000)) {
        const localNewer = l.mtime > r.mtime;
        const action = req.direction === 'both' ? (localNewer ? 'upload' : 'download') : req.direction;
        items.push({ relPath: l.name, isDir, action, reason: r.size !== l.size ? 'size' : 'newer', local: { size: l.size, mtime: l.mtime }, remote: { size: r.size, mtime: r.mtime } });
      }
    }
    for (const r of remote) {
      if (local.some((l) => l.name === r.name)) continue;
      const isDir = r.type === 'dir';
      if (req.direction !== 'upload') items.push({ relPath: r.name, isDir, action: 'download', reason: 'new', remote: { size: r.size, mtime: r.mtime } });
      else if (req.deleteExtra) items.push({ relPath: r.name, isDir, action: 'deleteRemote', reason: 'extra', remote: { size: r.size, mtime: r.mtime } });
    }
    return items;
  });
  ctx.handle('sftp:syncApply', (id: string, req: SyncRequest, items: SyncItem[], opts?: TransferOptions) => {
    session(id);
    const o: TransferOptions = { ...opts, conflict: 'overwrite' };
    return items.map((i) => {
      const localPath = mockLocal.join(req.localDir, i.relPath);
      const remotePath = join(normalize(req.remoteDir), i.relPath);
      const size = i.action === 'upload' ? (i.local?.size ?? 0) : (i.remote?.size ?? 0);
      return enqueue(
        {
          sftpId: id,
          direction: i.action === 'upload' ? 'upload' : i.action === 'download' ? 'download' : 'delete',
          name: i.relPath,
          localPath: i.action === 'deleteRemote' ? '' : localPath,
          remotePath: i.action === 'deleteLocal' ? '' : remotePath,
          total: Math.max(1, size),
          origin: 'sync'
        },
        o,
        { dir: i.isDir }
      );
    });
  });
  ctx.handle('sftp:edit', (id: string, remotePath: string) => {
    session(id);
    const editId = `mock-edit-${nextId++}`;
    const ev = (state: EditEvent['state']) =>
      ctx.emit('sftp:onEdit', { editId, sftpId: id, remotePath, localPath: mockLocal.join(mockLocal.temp, baseName(remotePath)), state });
    setTimeout(() => ev('opened'), 50);
    setTimeout(() => ev('uploading'), 3000);
    setTimeout(() => ev('uploaded'), 3600);
    return editId;
  });
  ctx.on('sftp:stopEdit', (editId: string) => ctx.emit('sftp:onEdit', { editId, sftpId: '', remotePath: '', localPath: '', state: 'closed' }));
  ctx.on('sftp:close', (id: string) => {
    sessions.delete(id);
    for (const t of [...transfers.values()]) {
      if (t.p.sftpId !== id) continue;
      const i = queue.indexOf(t);
      if (i >= 0) queue.splice(i, 1);
      finish(t, 'cancelled');
    }
  });
}
