/**
 * Mock handlers for the `local` namespace: an in-memory "This PC" (known folders + two drives) or, in a browser on a
 * Mac, "Computer" (known folders, "/" and a volume) so the file manager's left pane works in the browser preview. The
 * sftp mock copies files into it on download.
 */
import { isMac } from '@shared/platform';
import type { DragResult, LocalEntry } from '@shared/types';
import type { MockContext } from './index';

interface LNode {
  name: string;
  type: 'file' | 'dir';
  size: number;
  mtime: number;
  hidden?: boolean;
  children?: Map<string, LNode>;
}

const DAY = 86_400_000;
const SEP = isMac ? '/' : '\\';
const HOME = isMac ? '/Users/demo' : 'C:\\Users\\demo';

/** Local paths of the mock (the sftp mock's downloads, edits and synchronizations). */
export const mockLocal = {
  home: HOME,
  /** dir + name; a relative POSIX path ("a/b") gets the local separator. */
  join: (dir: string, rel: string) => `${dir.endsWith(SEP) ? dir.slice(0, -1) : dir}${SEP}${isMac ? rel : rel.replace(/\//g, SEP)}`,
  parent(p: string): string {
    const i = p.lastIndexOf(SEP);
    if (isMac) return i <= 0 ? '/' : p.slice(0, i);
    return i <= 2 ? p.slice(0, 3) : p.slice(0, i);
  },
  temp: isMac ? '/private/var/folders/mock/T/SSHTermaq/mock' : 'C:\\Users\\demo\\AppData\\Local\\Temp\\SSHTermaq\\mock'
};

function d(name: string, children: LNode[] = [], ageDays = 2, hidden = false): LNode {
  return { name, type: 'dir', size: 0, mtime: Date.now() - ageDays * DAY, hidden, children: new Map(children.map((c) => [c.name.toLowerCase(), c])) };
}
function f(name: string, size: number, ageDays = 1, hidden = false): LNode {
  return { name, type: 'file', size, mtime: Date.now() - ageDays * DAY, hidden };
}

/** Path parts; Windows: the first is the drive ("C:"). */
const split = (p: string) => (isMac ? p.split('/') : p.replace(/\//g, '\\').split('\\')).filter(Boolean);
/** The path of parts (Windows: a drive alone gets its root backslash). */
const joinParts = (parts: string[]) => (isMac ? '/' + parts.join('/') : parts.join('\\') + (parts.length === 1 ? '\\' : ''));

const demoHome = () =>
  d('demo', [
    d('Desktop', [f('todo.txt', 312, 0.2), f('screenshot.png', 482_113, 3)]),
    d('Documents', [
      d('projects', [d('site', [f('index.html', 4_210, 1), f('app.css', 2_048, 1), d('img', [f('logo.svg', 1_811, 9)])]), d('scripts', [f('deploy.sh', 1_022, 5)])]),
      f('quarterly-report.xlsx', 88_441, 4),
      f('notes.txt', 2_048, 0.3),
      isMac ? f('.DS_Store', 6_148, 200, true) : f('desktop.ini', 282, 200, true)
    ]),
    d('Downloads', [isMac ? f('putty-0.83.dmg', 3_662_336, 30) : f('putty-0.83-installer.msi', 3_662_336, 30), f('dataset.csv', 12_884_901, 2)]),
    d('.ssh', [f('id_ed25519', 411, 60), f('id_ed25519.pub', 97, 60), f('known_hosts', 2_840, 5)], 60, true)
  ]);
const backups = () => [d('Backups', [f('home-2026-09.tar.gz', 812_441_002, 20)]), d('Media', [])];

export class MockLocalFs {
  /** Windows: the drives by "c:"; macOS: "/" alone. */
  drives = new Map<string, LNode>(
    isMac
      ? [['/', d('/', [d('Users', [demoHome()]), d('Applications', [], 100), d('Volumes', [d('Data', backups(), 120)], 120)], 400)]]
      : [
          ['c:', d('C:', [d('Users', [demoHome()]), d('Windows', [], 300), d('Program Files', [], 100)], 400)],
          ['d:', d('D:', backups(), 120)]
        ]
  );

  private node(path: string): LNode | undefined {
    const parts = split(path);
    if (isMac && path.startsWith('/')) parts.unshift('/');
    if (!parts.length) return undefined;
    let n = this.drives.get(parts[0].toLowerCase());
    for (const part of parts.slice(1)) {
      if (!n || n.type !== 'dir') return undefined;
      n = n.children!.get(part.toLowerCase());
    }
    return n;
  }

  private parent(path: string): { dir: LNode; name: string } {
    const parts = split(path);
    const name = parts.pop() ?? '';
    const dir = this.node(joinParts(parts));
    if (!dir || dir.type !== 'dir') throw new Error(`The system cannot find the path specified: ${path}`);
    return { dir, name };
  }

  private entry(dirPath: string, n: LNode): LocalEntry {
    const path = dirPath.endsWith(SEP) ? dirPath + n.name : `${dirPath}${SEP}${n.name}`;
    return { name: n.name, path, type: n.type, size: n.size, mtime: n.mtime, hidden: !!n.hidden, system: false, readonly: false, link: false };
  }

  places(): LocalEntry[] {
    const place = (name: string, path: string, icon: LocalEntry['icon']): LocalEntry => ({
      name,
      path,
      type: 'place',
      size: 0,
      mtime: 0,
      hidden: false,
      system: false,
      readonly: false,
      link: false,
      label: name,
      icon
    });
    const drive = (name: string, path: string, freeBytes: number, totalBytes: number, icon: LocalEntry['icon'] = 'drive'): LocalEntry => ({
      ...place(name, path, icon),
      type: 'drive',
      freeBytes,
      totalBytes
    });
    return [
      place('Desktop', `${HOME}${SEP}Desktop`, 'desktop'),
      place('Documents', `${HOME}${SEP}Documents`, 'documents'),
      place('Downloads', `${HOME}${SEP}Downloads`, 'downloads'),
      place('demo', HOME, 'home'),
      ...(isMac
        ? [drive('Macintosh HD', '/', 182e9, 511e9), drive('Data', '/Volumes/Data', 1.2e12, 2e12, 'removable')]
        : [drive('Local Disk (C:)', 'C:\\', 182e9, 511e9), drive('Data (D:)', 'D:\\', 1.2e12, 2e12)])
    ];
  }

  list(path: string): LocalEntry[] {
    if (!path) return this.places();
    const n = this.node(path);
    if (!n) throw new Error(`The system cannot find the path specified: ${path}`);
    if (n.type !== 'dir') throw new Error(`The directory name is invalid: ${path}`);
    return [...n.children!.values()].map((c) => this.entry(path, c));
  }

  stat(path: string): LocalEntry | null {
    const n = this.node(path);
    if (!n) return null;
    const parts = split(path);
    if (isMac) return parts.length ? this.entry(joinParts(parts.slice(0, -1)), n) : { ...this.entry('', n), path: '/' };
    return this.entry(parts.slice(0, -1).join('\\') || parts[0], n);
  }

  sizeOf(path: string): number {
    const n = this.node(path);
    if (!n) throw new Error(`Cannot read ${path}`);
    const walk = (x: LNode): number => (x.type === 'dir' ? [...x.children!.values()].reduce((s, c) => s + walk(c), 0) : x.size);
    return walk(n);
  }

  exists(path: string): boolean {
    return !!this.node(path);
  }

  add(path: string, size: number, dir = false): void {
    const { dir: parent, name } = this.parent(path);
    parent.children!.set(name.toLowerCase(), dir ? d(name, [], 0) : { ...f(name, size, 0), mtime: Date.now() });
  }

  mkdir(path: string): void {
    if (this.node(path)) throw new Error('Cannot create a file when that file already exists.');
    this.add(path, 0, true);
  }

  rename(from: string, to: string): void {
    const n = this.node(from);
    if (!n) throw new Error('The system cannot find the file specified.');
    if (this.node(to)) throw new Error('Cannot create a file when that file already exists.');
    const a = this.parent(from);
    const b = this.parent(to);
    a.dir.children!.delete(a.name.toLowerCase());
    n.name = b.name;
    b.dir.children!.set(b.name.toLowerCase(), n);
  }

  remove(path: string): void {
    const { dir, name } = this.parent(path);
    if (!dir.children!.delete(name.toLowerCase())) throw new Error('The system cannot find the file specified.');
  }

  copy(from: string, toDir: string, move: boolean): void {
    const n = this.node(from);
    const target = this.node(toDir);
    if (!n || !target || target.type !== 'dir') throw new Error('The system cannot find the path specified.');
    const clone = (x: LNode): LNode => ({ ...x, children: x.children ? new Map([...x.children].map(([k, v]) => [k, clone(v)])) : undefined });
    target.children!.set(n.name.toLowerCase(), move ? n : clone(n));
    if (move) this.remove(from);
  }
}

export const mockLocalFs = new MockLocalFs();

const watches = new Map<string, string>();
let nextWatch = 1;

/** Fires local:onChanged for every watch on `dir` (also used by the sftp mock after downloads). */
export function touchLocalDir(ctx: MockContext, dir: string): void {
  for (const [id, p] of watches) if (p.toLowerCase() === dir.toLowerCase()) ctx.emit('local:onChanged', id);
}

export function registerLocalMocks(ctx: MockContext): void {
  const fs = mockLocalFs;
  const changed = (path: string) => touchLocalDir(ctx, path);
  const parentOf = (p: string) => {
    const parts = split(p);
    parts.pop();
    return joinParts(parts);
  };

  ctx.handle('local:list', (path: string) => fs.list(path));
  ctx.handle('local:stat', (path: string) => fs.stat(path));
  ctx.handle('local:home', () => (isMac ? HOME : `${HOME}\\Documents`));
  ctx.handle('local:mkdir', (path: string) => {
    fs.mkdir(path);
    changed(parentOf(path));
  });
  ctx.handle('local:rename', (from: string, to: string) => {
    fs.rename(from, to);
    changed(parentOf(from));
  });
  ctx.handle('local:remove', (paths: string[]) => {
    for (const p of paths) {
      fs.remove(p);
      changed(parentOf(p));
    }
  });
  ctx.handle('local:copy', (paths: string[], targetDir: string, move: boolean) => {
    for (const p of paths) {
      // Paths dropped from the "Explorer" of the browser preview do not exist: create placeholders.
      if (fs.exists(p)) fs.copy(p, targetDir, move);
      else fs.add(`${targetDir}${SEP}${split(p).pop()}`, 1024);
    }
    changed(targetDir);
  });
  ctx.handle('local:open', () => undefined);
  ctx.handle('local:reveal', () => undefined);
  ctx.handle('local:watch', (path: string) => {
    const id = `mock-watch-${nextWatch++}`;
    watches.set(id, path);
    return id;
  });
  ctx.on('local:unwatch', (id: string) => void watches.delete(id));
  ctx.handle('local:startDrag', (): DragResult => ({ result: 'cancelled' }));
}
