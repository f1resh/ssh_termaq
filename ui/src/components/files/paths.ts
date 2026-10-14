/**
 * Pure helpers of the file manager: Windows and POSIX path handling, breadcrumbs, size / date / speed formatting,
 * and the item model shared by the local and the remote pane.
 */

export type Side = 'local' | 'remote';

/** One row of a file pane (local or remote). */
export interface FileItem {
  name: string;
  path: string;
  /** 'drive' / 'place' only in the local "This PC" list. */
  type: 'file' | 'dir' | 'link' | 'other' | 'drive' | 'place';
  /** Can be opened as a folder (folders, drives, places, links to folders). */
  isDir: boolean;
  size: number;
  /** ms since epoch, 0 = unknown */
  mtime: number;
  hidden: boolean;
  /** Remote: 'drwxr-xr-x'; local: attribute letters ('RHS'). */
  permissions: string;
  mode?: number;
  link?: boolean;
  label?: string;
  freeBytes?: number;
  totalBytes?: number;
  icon?: string;
}

export interface Crumb {
  label: string;
  path: string;
}

/* ---- remote (POSIX) -------------------------------------------------------- */

export const remote = {
  root: '/',
  join(dir: string, name: string): string {
    if (!dir) return name;
    return dir.endsWith('/') ? dir + name : `${dir}/${name}`;
  },
  /** null at the root. */
  parent(p: string): string | null {
    if (!p || p === '/') return null;
    const t = p.replace(/\/+$/, '');
    const i = t.lastIndexOf('/');
    return i <= 0 ? '/' : t.slice(0, i);
  },
  baseName(p: string): string {
    const t = p.replace(/\/+$/, '');
    return t.slice(t.lastIndexOf('/') + 1) || '/';
  },
  crumbs(p: string): Crumb[] {
    const out: Crumb[] = [{ label: '/', path: '/' }];
    let cur = '';
    for (const part of p.split('/').filter(Boolean)) {
      cur += '/' + part;
      out.push({ label: part, path: cur });
    }
    return out;
  },
  /** Typed paths: trims, collapses "//", drops a trailing '/'. Relative paths resolve against `cwd`. */
  normalize(input: string, cwd = '/'): string {
    let p = input.trim();
    if (!p) return cwd;
    if (!p.startsWith('/')) p = remote.join(cwd, p);
    const parts: string[] = [];
    for (const part of p.split('/')) {
      if (!part || part === '.') continue;
      if (part === '..') parts.pop();
      else parts.push(part);
    }
    return '/' + parts.join('/');
  },
  /** True when `p` is `dir` or inside it. */
  isInside(p: string, dir: string): boolean {
    return p === dir || p.startsWith(dir.endsWith('/') ? dir : dir + '/');
  }
};

/* ---- local (Windows) ------------------------------------------------------ */

/** '' is "This PC" (drives and known folders). */
export const THIS_PC = '';

export const local = {
  root: THIS_PC,
  join(dir: string, name: string): string {
    if (!dir) return name;
    return dir.endsWith('\\') ? dir + name : `${dir}\\${name}`;
  },
  /** "C:\a\b" -> "C:\a", "C:\" -> This PC, "\\srv\share" -> This PC, This PC -> null. */
  parent(p: string): string | null {
    if (p === THIS_PC) return null;
    const t = p.replace(/\\+$/, '');
    if (/^[A-Za-z]:$/.test(t)) return THIS_PC;
    if (/^\\\\[^\\]+\\[^\\]+$/.test(t)) return THIS_PC;
    const i = t.lastIndexOf('\\');
    if (i < 0) return THIS_PC;
    const up = t.slice(0, i);
    return /^[A-Za-z]:$/.test(up) ? up + '\\' : up;
  },
  baseName(p: string): string {
    const t = p.replace(/\\+$/, '');
    return t.slice(t.lastIndexOf('\\') + 1) || t;
  },
  crumbs(p: string): Crumb[] {
    const out: Crumb[] = [{ label: 'This PC', path: THIS_PC }];
    if (p === THIS_PC) return out;
    const t = p.replace(/\\+$/, '');
    if (t.startsWith('\\\\')) {
      const parts = t.slice(2).split('\\').filter(Boolean);
      if (parts.length >= 2) {
        let cur = `\\\\${parts[0]}\\${parts[1]}`;
        out.push({ label: cur, path: cur });
        for (const part of parts.slice(2)) {
          cur += '\\' + part;
          out.push({ label: part, path: cur });
        }
      }
      return out;
    }
    const parts = t.split('\\').filter(Boolean);
    let cur = '';
    parts.forEach((part, i) => {
      cur = i === 0 ? part + '\\' : local.join(cur, part);
      out.push({ label: i === 0 ? part.toUpperCase() : part, path: cur });
    });
    return out;
  },
  /** "c:/a//b/" -> "C:\a\b", "c:" -> "C:\". Relative input resolves against cwd. */
  normalize(input: string, cwd = THIS_PC): string {
    let p = input.trim().replace(/\//g, '\\');
    if (!p) return cwd;
    if (/^[A-Za-z]:$/.test(p)) return p.toUpperCase() + '\\';
    const unc = p.startsWith('\\\\');
    if (!unc && !/^[A-Za-z]:\\/.test(p)) p = local.join(cwd, p);
    const prefix = unc ? '\\\\' : '';
    const body = unc ? p.slice(2) : p;
    const parts: string[] = [];
    for (const part of body.split('\\')) {
      if (!part || part === '.') continue;
      if (part === '..') {
        if (parts.length > (unc ? 2 : 1)) parts.pop();
      } else {
        parts.push(part);
      }
    }
    if (!unc && parts.length) parts[0] = parts[0].toUpperCase();
    if (!unc && parts.length === 1) return parts[0] + '\\';
    return prefix + parts.join('\\');
  },
  isInside(p: string, dir: string): boolean {
    const a = p.toLowerCase();
    const b = dir.toLowerCase();
    return a === b || a.startsWith(b.endsWith('\\') ? b : b + '\\');
  }
};

export function pathsOf(side: Side) {
  return side === 'local' ? local : remote;
}

/* ---- formatting ------------------------------------------------------------- */

export function fmtSize(n: number): string {
  if (!Number.isFinite(n) || n < 0) return '';
  if (n < 1024) return `${n} B`;
  const units = ['KB', 'MB', 'GB', 'TB', 'PB'];
  let v = n / 1024;
  let i = 0;
  while (v >= 1024 && i < units.length - 1) {
    v /= 1024;
    i++;
  }
  return `${v < 10 ? v.toFixed(1) : Math.round(v)} ${units[i]}`;
}

export function fmtDate(ms: number): string {
  if (!ms) return '';
  const d = new Date(ms);
  const p = (x: number) => String(x).padStart(2, '0');
  return `${d.getFullYear()}-${p(d.getMonth() + 1)}-${p(d.getDate())} ${p(d.getHours())}:${p(d.getMinutes())}`;
}

export function fmtDuration(seconds: number): string {
  if (!Number.isFinite(seconds) || seconds < 0) return '';
  if (seconds < 60) return `${Math.max(1, Math.round(seconds))}s`;
  const m = Math.floor(seconds / 60);
  if (m < 60) return `${m}m ${Math.round(seconds % 60)}s`;
  return `${Math.floor(m / 60)}h ${m % 60}m`;
}

/** Local attribute letters as shown in the "Attributes" column. */
export function attributeString(e: { readonly?: boolean; hidden?: boolean; system?: boolean; link?: boolean }): string {
  return `${e.readonly ? 'R' : ''}${e.hidden ? 'H' : ''}${e.system ? 'S' : ''}${e.link ? 'L' : ''}`;
}

/** File name problems that the remote side (POSIX) or Windows would reject. */
export function nameError(name: string, side: Side): string | null {
  const t = name.trim();
  if (!t) return 'Enter a name';
  if (t === '.' || t === '..') return 'This name is reserved';
  if (side === 'remote' && t.includes('/')) return 'Names cannot contain "/"';
  if (side === 'local' && /[<>:"/\\|?*\x00-\x1f]/.test(t)) return 'Names cannot contain < > : " / \\ | ? *';
  return null;
}

/* ---- sorting ---------------------------------------------------------------- */

export type SortKey = 'name' | 'size' | 'mtime' | 'permissions';

/** Folders first, then by key (stable tie-break on name). */
export function sortItems(items: FileItem[], key: SortKey, asc: boolean): FileItem[] {
  const byName = (a: FileItem, b: FileItem) => a.name.localeCompare(b.name, undefined, { numeric: true, sensitivity: 'base' });
  const cmp: Record<SortKey, (a: FileItem, b: FileItem) => number> = {
    name: byName,
    size: (a, b) => a.size - b.size,
    mtime: (a, b) => a.mtime - b.mtime,
    permissions: (a, b) => a.permissions.localeCompare(b.permissions)
  };
  return [...items].sort((a, b) => Number(b.isDir) - Number(a.isDir) || (asc ? 1 : -1) * cmp[key](a, b) || byName(a, b));
}

/** Range / toggle selection logic shared by mouse and keyboard. */
export function nextSelection(
  visible: readonly string[],
  current: ReadonlySet<string>,
  anchor: string | null,
  target: string,
  mode: 'single' | 'toggle' | 'range'
): Set<string> {
  if (mode === 'range' && anchor !== null) {
    const a = visible.indexOf(anchor);
    const b = visible.indexOf(target);
    if (a >= 0 && b >= 0) {
      const [lo, hi] = a < b ? [a, b] : [b, a];
      return new Set(visible.slice(lo, hi + 1));
    }
  }
  if (mode === 'toggle') {
    const n = new Set(current);
    if (n.has(target)) n.delete(target);
    else n.add(target);
    return n;
  }
  return new Set([target]);
}

/** Filter-as-you-type: case-insensitive substring, or a glob when it contains * or ?. */
export function matchesFilter(name: string, filter: string): boolean {
  const f = filter.trim().toLowerCase();
  if (!f) return true;
  const n = name.toLowerCase();
  if (!/[*?]/.test(f)) return n.includes(f);
  const re = new RegExp('^' + f.replace(/[.+^${}()|[\]\\]/g, '\\$&').replace(/\*/g, '.*').replace(/\?/g, '.') + '$');
  return re.test(n);
}

/* ---- back / forward history -------------------------------------------------- */

export interface NavHistory {
  back: string[];
  forward: string[];
}

const HISTORY_MAX = 100;

/** A normal navigation from `from`: remembered for Back, Forward is cleared. */
export function historyVisit(h: NavHistory, from: string): NavHistory {
  if (h.back[h.back.length - 1] === from) return { back: h.back, forward: [] };
  return { back: [...h.back, from].slice(-HISTORY_MAX), forward: [] };
}

/** Back from `current`: the folder to open and the new history, or null when there is nothing to go back to. */
export function historyBack(h: NavHistory, current: string): { target: string; history: NavHistory } | null {
  if (!h.back.length) return null;
  return { target: h.back[h.back.length - 1], history: { back: h.back.slice(0, -1), forward: [...h.forward, current] } };
}

export function historyForward(h: NavHistory, current: string): { target: string; history: NavHistory } | null {
  if (!h.forward.length) return null;
  return { target: h.forward[h.forward.length - 1], history: { back: [...h.back, current], forward: h.forward.slice(0, -1) } };
}
