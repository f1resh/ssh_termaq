/**
 * Pure helpers for the import wizard (unit tested): conflict detection against saved sessions, filtering,
 * request building and display strings.
 */
import { DEFAULT_PORTS, normalizeFolder } from '@shared/defaults';
import { isMac } from '@shared/platform';
import type { ImportCandidate, ImportPreview, ImportRequest, ImportSource, SessionConfig } from '@shared/types';

export type ConflictPolicy = ImportRequest['conflict'];

export interface ImportOptions {
  targetFolder: string;
  conflict: ConflictPolicy;
  importHostKeys: boolean;
  useDefaultSettings: boolean;
}

export const SOURCE_LABELS: Record<ImportSource, string> = {
  'putty-registry': isMac ? 'PuTTY (~/.putty)' : 'PuTTY (this PC)',
  'putty-regfile': 'PuTTY .reg file',
  'ssh-config': 'OpenSSH config',
  json: 'JSON backup'
};

export function isPuttySource(source: ImportSource): boolean {
  return source === 'putty-registry' || source === 'putty-regfile';
}

/** Folder imported sessions go to by default ("PuTTY" for PuTTY sources, root otherwise). */
export function defaultTargetFolder(source: ImportSource): string {
  return isPuttySource(source) ? 'PuTTY' : '';
}

export function defaultImportOptions(preview: ImportPreview): ImportOptions {
  return {
    targetFolder: defaultTargetFolder(preview.source),
    conflict: 'rename',
    importHostKeys: preview.hostKeys.length > 0,
    useDefaultSettings: false
  };
}

/** Final folder of a candidate: target folder + the candidate's own folder. */
export function destinationFolder(c: ImportCandidate, targetFolder: string): string {
  return normalizeFolder([targetFolder, c.session.folder].filter(Boolean).join('/'));
}

/** Whether importing `c` under `targetFolder` collides with a saved session (same name + folder, case-insensitive). */
export function candidateExists(c: ImportCandidate, sessions: readonly Pick<SessionConfig, 'name' | 'folder'>[], targetFolder: string): boolean {
  const folder = destinationFolder(c, targetFolder);
  const name = c.name.trim().toLowerCase();
  return sessions.some((s) => s.name.trim().toLowerCase() === name && normalizeFolder(s.folder) === folder);
}

/** "user@host:port" (port only when non-default), serial line for serial sessions. */
export function candidateAddress(s: SessionConfig): string {
  if (s.protocol === 'serial') return `${s.serial.line} @ ${s.serial.baudRate}`;
  const user = s.username ? `${s.username}@` : '';
  const host = s.host.includes(':') ? `[${s.host}]` : s.host;
  const port = s.port && s.port !== DEFAULT_PORTS[s.protocol] ? `:${s.port}` : '';
  return `${user}${host}${port}`;
}

/** Case-insensitive match on name, address and protocol. */
export function filterCandidates(list: readonly ImportCandidate[], query: string): ImportCandidate[] {
  const q = query.trim().toLowerCase();
  if (!q) return [...list];
  return list.filter((c) => `${c.name} ${candidateAddress(c.session)} ${c.session.protocol}`.toLowerCase().includes(q));
}

/** Builds the apply request for the selected candidate keys. */
export function buildImportRequest(preview: ImportPreview, selected: ReadonlySet<string>, opts: ImportOptions): ImportRequest {
  const req: ImportRequest = {
    candidates: preview.candidates.filter((c) => selected.has(c.key)),
    hostKeys: opts.importHostKeys ? preview.hostKeys : [],
    conflict: opts.conflict,
    targetFolder: normalizeFolder(opts.targetFolder)
  };
  if (opts.useDefaultSettings && preview.defaultSettings) req.defaultSettings = preview.defaultSettings;
  return req;
}

/** Keys selected initially: every candidate. */
export function initialSelection(preview: ImportPreview): Set<string> {
  return new Set(preview.candidates.map((c) => c.key));
}
