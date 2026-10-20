/**
 * Session tree model for the sidebar (pure; unit-tested): folders from the explicit folder list and from
 * session paths, search filtering over name/host/user/folder/notes, sorting, and flattening into the visible
 * row list used for rendering and keyboard navigation.
 */
import { normalizeFolder, sessionLabel } from '@shared/defaults';
import type { SessionConfig } from '@shared/types';

export interface FolderNode {
  kind: 'folder';
  /** Full '/'-separated path ('' for the root). */
  path: string;
  name: string;
  folders: FolderNode[];
  sessions: SessionConfig[];
  /** Sessions in this folder and every sub-folder (after filtering). */
  count: number;
}

export type Row =
  | { kind: 'folder'; key: string; depth: number; folder: FolderNode; expanded: boolean }
  | { kind: 'session'; key: string; depth: number; session: SessionConfig };

export const folderKey = (path: string) => `f:${path}`;
export const sessionKey = (id: string) => `s:${id}`;

/** Lower-cased search tokens ('' => none). */
export function searchTokens(query: string): string[] {
  return query
    .toLowerCase()
    .split(/\s+/)
    .map((t) => t.trim())
    .filter(Boolean);
}

function haystack(s: SessionConfig): string {
  return [sessionLabel(s), s.name, s.host, s.username, s.folder, s.notes, s.protocol, s.protocol === 'serial' ? s.serial.line : '']
    .join('\n')
    .toLowerCase();
}

/** Every token must appear somewhere in name / host / user / folder / notes. */
export function sessionMatches(s: SessionConfig, tokens: string[]): boolean {
  if (!tokens.length) return true;
  const h = haystack(s);
  return tokens.every((t) => h.includes(t));
}

const collator = new Intl.Collator(undefined, { numeric: true, sensitivity: 'base' });

export function buildTree(sessions: SessionConfig[], folders: string[], tokens: string[] = []): FolderNode {
  const root: FolderNode = { kind: 'folder', path: '', name: '', folders: [], sessions: [], count: 0 };
  const byPath = new Map<string, FolderNode>([['', root]]);
  const ensure = (path: string): FolderNode => {
    const p = normalizeFolder(path);
    const existing = byPath.get(p);
    if (existing) return existing;
    const idx = p.lastIndexOf('/');
    const parent = ensure(idx >= 0 ? p.slice(0, idx) : '');
    const node: FolderNode = { kind: 'folder', path: p, name: idx >= 0 ? p.slice(idx + 1) : p, folders: [], sessions: [], count: 0 };
    parent.folders.push(node);
    byPath.set(p, node);
    return node;
  };
  const filtering = tokens.length > 0;
  if (!filtering) folders.forEach((f) => ensure(f));
  for (const s of sessions) {
    if (!sessionMatches(s, tokens)) continue;
    ensure(s.folder).sessions.push(s);
  }
  const finalize = (n: FolderNode): number => {
    n.folders.sort((a, b) => collator.compare(a.name, b.name));
    n.sessions.sort((a, b) => collator.compare(sessionLabel(a), sessionLabel(b)));
    n.count = n.sessions.length + n.folders.reduce((acc, f) => acc + finalize(f), 0);
    return n.count;
  };
  finalize(root);
  return root;
}

/** Visible rows in display order. `forceExpand` (searching) ignores collapsed folders. */
export function flattenTree(root: FolderNode, collapsed: ReadonlySet<string>, forceExpand = false): Row[] {
  const rows: Row[] = [];
  const walk = (n: FolderNode, depth: number) => {
    for (const f of n.folders) {
      const expanded = forceExpand || !collapsed.has(f.path);
      rows.push({ kind: 'folder', key: folderKey(f.path), depth, folder: f, expanded });
      if (expanded) walk(f, depth + 1);
    }
    for (const s of n.sessions) rows.push({ kind: 'session', key: sessionKey(s.id), depth, session: s });
  };
  walk(root, 0);
  return rows;
}

/** Parent folder path of a folder path ('' for top level). */
export function parentFolder(path: string): string {
  const i = path.lastIndexOf('/');
  return i >= 0 ? path.slice(0, i) : '';
}

/** True when `folder` is `ancestor` or lies beneath it. */
export function isWithin(folder: string, ancestor: string): boolean {
  return folder === ancestor || folder.startsWith(ancestor + '/');
}
