import { describe, expect, it } from 'vitest';
import { suggestName } from './ConflictDialog';
import {
  attributeString,
  fmtDuration,
  fmtSize,
  historyBack,
  historyForward,
  historyVisit,
  local,
  matchesFilter,
  nameError,
  nextSelection,
  remote,
  sortItems,
  THIS_PC,
  type FileItem
} from './paths';
import { syncTotals } from './SyncDialog';

const item = (name: string, isDir = false, size = 0, mtime = 0): FileItem => ({
  name,
  path: '/' + name,
  type: isDir ? 'dir' : 'file',
  isDir,
  size,
  mtime,
  hidden: name.startsWith('.'),
  permissions: ''
});

describe('remote paths', () => {
  it('joins, finds parents and names', () => {
    expect(remote.join('/', 'a')).toBe('/a');
    expect(remote.join('/x/', 'a')).toBe('/x/a');
    expect(remote.join('/x', 'a')).toBe('/x/a');
    expect(remote.parent('/a/b')).toBe('/a');
    expect(remote.parent('/a')).toBe('/');
    expect(remote.parent('/')).toBeNull();
    expect(remote.baseName('/a/b/')).toBe('b');
  });
  it('builds breadcrumbs', () => {
    expect(remote.crumbs('/var/www')).toEqual([
      { label: '/', path: '/' },
      { label: 'var', path: '/var' },
      { label: 'www', path: '/var/www' }
    ]);
  });
  it('normalizes typed paths', () => {
    expect(remote.normalize('/a//b/./c/../d/')).toBe('/a/b/d');
    expect(remote.normalize('logs', '/home/u')).toBe('/home/u/logs');
    expect(remote.normalize('', '/home/u')).toBe('/home/u');
    expect(remote.normalize('/..')).toBe('/');
    expect(remote.isInside('/a/b', '/a')).toBe(true);
    expect(remote.isInside('/ab', '/a')).toBe(false);
  });
});

describe('local paths', () => {
  it('walks up to This PC', () => {
    expect(local.parent('C:\\a\\b')).toBe('C:\\a');
    expect(local.parent('C:\\a')).toBe('C:\\');
    expect(local.parent('C:\\')).toBe(THIS_PC);
    expect(local.parent('\\\\srv\\share')).toBe(THIS_PC);
    expect(local.parent('\\\\srv\\share\\x')).toBe('\\\\srv\\share');
    expect(local.parent(THIS_PC)).toBeNull();
  });
  it('joins and names', () => {
    expect(local.join('C:\\', 'x')).toBe('C:\\x');
    expect(local.join('C:\\a', 'x')).toBe('C:\\a\\x');
    expect(local.baseName('C:\\a\\b\\')).toBe('b');
  });
  it('builds breadcrumbs', () => {
    expect(local.crumbs('c:\\Users\\demo').map((c) => c.label)).toEqual(['This PC', 'C:', 'Users', 'demo']);
    expect(local.crumbs('C:\\Users').map((c) => c.path)).toEqual(['', 'C:\\', 'C:\\Users']);
    expect(local.crumbs('\\\\srv\\share\\docs').map((c) => c.path)).toEqual(['', '\\\\srv\\share', '\\\\srv\\share\\docs']);
  });
  it('normalizes typed paths', () => {
    expect(local.normalize('c:')).toBe('C:\\');
    expect(local.normalize('c:/Users//demo/')).toBe('C:\\Users\\demo');
    expect(local.normalize('..', 'C:\\a\\b')).toBe('C:\\a');
    expect(local.normalize('..\\..\\..', 'C:\\a')).toBe('C:\\');
    expect(local.normalize('docs', 'D:\\data')).toBe('D:\\data\\docs');
    expect(local.normalize('\\\\srv\\share\\x\\..')).toBe('\\\\srv\\share');
    expect(local.isInside('c:\\A\\b', 'C:\\a')).toBe(true);
  });
});

describe('formatting', () => {
  it('formats sizes and durations', () => {
    expect(fmtSize(512)).toBe('512 B');
    expect(fmtSize(1536)).toBe('1.5 KB');
    expect(fmtSize(5 * 1024 * 1024 * 1024)).toBe('5.0 GB');
    expect(fmtDuration(42)).toBe('42s');
    expect(fmtDuration(125)).toBe('2m 5s');
    expect(fmtDuration(3 * 3600 + 120)).toBe('3h 2m');
    expect(attributeString({ readonly: true, hidden: true })).toBe('RH');
  });
  it('validates names per side', () => {
    expect(nameError('', 'remote')).toBeTruthy();
    expect(nameError('a/b', 'remote')).toBeTruthy();
    expect(nameError('a:b', 'remote')).toBeNull();
    expect(nameError('a:b', 'local')).toBeTruthy();
    expect(nameError('..', 'local')).toBeTruthy();
    expect(nameError('report.pdf', 'local')).toBeNull();
  });
});

describe('list model', () => {
  it('sorts folders first, then by key', () => {
    const list = [item('b.txt', false, 10, 3), item('zdir', true), item('a.txt', false, 30, 1), item('Adir', true)];
    expect(sortItems(list, 'name', true).map((i) => i.name)).toEqual(['Adir', 'zdir', 'a.txt', 'b.txt']);
    expect(sortItems(list, 'size', false).map((i) => i.name)).toEqual(['Adir', 'zdir', 'a.txt', 'b.txt']);
    expect(sortItems(list, 'mtime', false).map((i) => i.name)).toEqual(['Adir', 'zdir', 'b.txt', 'a.txt']);
    expect(sortItems([item('file10'), item('file9')], 'name', true).map((i) => i.name)).toEqual(['file9', 'file10']);
  });
  it('selects single, toggle and range', () => {
    const v = ['a', 'b', 'c', 'd'];
    expect([...nextSelection(v, new Set(), null, 'b', 'single')]).toEqual(['b']);
    expect([...nextSelection(v, new Set(['b']), 'b', 'd', 'toggle')].sort()).toEqual(['b', 'd']);
    expect([...nextSelection(v, new Set(['b', 'd']), 'b', 'd', 'toggle')]).toEqual(['b']);
    expect([...nextSelection(v, new Set(['b']), 'b', 'd', 'range')]).toEqual(['b', 'c', 'd']);
    expect([...nextSelection(v, new Set(), 'd', 'a', 'range')]).toEqual(['a', 'b', 'c', 'd']);
  });
  it('filters by substring or glob', () => {
    expect(matchesFilter('Report.PDF', 'port')).toBe(true);
    expect(matchesFilter('Report.PDF', '*.pdf')).toBe(true);
    expect(matchesFilter('Report.PDF', '*.txt')).toBe(false);
    expect(matchesFilter('a(1).txt', 'a(?).txt')).toBe(true);
    expect(matchesFilter('anything', '')).toBe(true);
  });
});

describe('dialog helpers', () => {
  it('suggests a free-looking name', () => {
    expect(suggestName('report.pdf')).toBe('report (1).pdf');
    expect(suggestName('.bashrc')).toBe('.bashrc (1)');
    expect(suggestName('Makefile')).toBe('Makefile (1)');
  });
  it('totals synchronize selections', () => {
    expect(
      syncTotals([
        { relPath: 'a', isDir: false, action: 'upload', reason: 'new', local: { size: 10, mtime: 0 } },
        { relPath: 'b', isDir: false, action: 'download', reason: 'newer', remote: { size: 5, mtime: 0 }, local: { size: 1, mtime: 0 } },
        { relPath: 'c', isDir: true, action: 'deleteRemote', reason: 'extra', remote: { size: 0, mtime: 0 } }
      ])
    ).toEqual({ upload: 1, download: 1, deletes: 1, bytes: 15 });
  });
});

describe('back / forward history', () => {
  it('goes back and forward without losing either side', () => {
    let h = { back: [] as string[], forward: [] as string[] };
    h = historyVisit(h, '/a');
    h = historyVisit(h, '/b');
    // now at /c
    const b1 = historyBack(h, '/c')!;
    expect(b1.target).toBe('/b');
    const b2 = historyBack(b1.history, '/b')!;
    expect(b2.target).toBe('/a');
    expect(historyBack(b2.history, '/a')).toBeNull();
    const f1 = historyForward(b2.history, '/a')!;
    expect(f1.target).toBe('/b');
    const f2 = historyForward(f1.history, '/b')!;
    expect(f2.target).toBe('/c');
    expect(historyForward(f2.history, '/c')).toBeNull();
    // a normal visit clears Forward
    expect(historyVisit(b1.history, '/b').forward).toEqual([]);
  });
});
