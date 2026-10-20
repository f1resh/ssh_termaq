/**
 * Global state of the file manager shared by all its tabs: transfers (with speed), pending conflict questions and
 * remote files open for editing. One subscription per bridge event, set up by initFilesEvents().
 */
import { create } from 'zustand';
import { api } from '@/bridge/api';
import { toast } from '@/components/ui';
import type { EditEvent, TransferConflict, TransferOptions, TransferProgress } from '@shared/types';

/** How to re-issue a transfer ("Retry" in the queue). */
export interface RetryInfo {
  kind: 'download' | 'upload';
  sftpId: string;
  sources: string[];
  targetDir: string;
  opts: TransferOptions;
}

export interface TransferEntry extends TransferProgress {
  started: number;
  updated: number;
  /** Smoothed bytes per second while running. */
  speed: number;
}

export interface FilesState {
  transfers: Record<string, TransferEntry>;
  /** Questions waiting for an answer, oldest first. */
  conflicts: TransferConflict[];
  edits: Record<string, EditEvent>;
  retry: Record<string, RetryInfo>;
  rememberRetry(ids: string[], info: RetryInfo): void;
  clearFinished(sftpId?: string): void;
  dropConflict(conflictId: string): void;
}

const FINAL = new Set(['done', 'error', 'cancelled']);

export const useFiles = create<FilesState>((set) => ({
  transfers: {},
  conflicts: [],
  edits: {},
  retry: {},
  rememberRetry: (ids, info) =>
    set((s) => {
      const retry = { ...s.retry };
      // One entry per transfer, each re-issuing only its own source.
      ids.forEach((id, i) => (retry[id] = { ...info, sources: info.sources.length === ids.length ? [info.sources[i]] : info.sources }));
      return { retry };
    }),
  clearFinished: (sftpId) =>
    set((s) => {
      const transfers: Record<string, TransferEntry> = {};
      const retry = { ...s.retry };
      for (const [id, t] of Object.entries(s.transfers)) {
        if (FINAL.has(t.state) && (!sftpId || t.sftpId === sftpId)) delete retry[id];
        else transfers[id] = t;
      }
      return { transfers, retry };
    }),
  dropConflict: (conflictId) => set((s) => ({ conflicts: s.conflicts.filter((c) => c.conflictId !== conflictId) }))
}));

export function isFinal(t: Pick<TransferProgress, 'state'>): boolean {
  return FINAL.has(t.state);
}

let initialized = false;

/** Idempotent: subscribes to sftp:onProgress / onConflict / onEdit once. */
export function initFilesEvents(): void {
  if (initialized) return;
  initialized = true;

  api.sftp.onProgress((p) => {
    const now = Date.now();
    useFiles.setState((s) => {
      const prev = s.transfers[p.transferId];
      let speed = prev?.speed ?? 0;
      if (prev && p.state === 'running') {
        const dt = (now - prev.updated) / 1000;
        if (dt > 0.05) speed = speed ? speed * 0.7 + ((p.transferred - prev.transferred) / dt) * 0.3 : (p.transferred - prev.transferred) / dt;
      }
      const entry: TransferEntry = { ...p, started: prev?.started ?? now, updated: now, speed: Math.max(0, speed) };
      // A finished transfer can no longer be asked anything.
      const conflicts = FINAL.has(p.state) ? s.conflicts.filter((c) => c.transferId !== p.transferId) : s.conflicts;
      return { transfers: { ...s.transfers, [p.transferId]: entry }, conflicts };
    });
    if (p.state === 'error' && p.origin !== 'edit') {
      toast.error(`${p.direction === 'upload' ? 'Upload' : p.direction === 'download' ? 'Download' : 'Delete'} failed`, `${p.name}: ${p.error ?? ''}`);
    }
  });

  api.sftp.onConflict((c) => useFiles.setState((s) => ({ conflicts: [...s.conflicts, c] })));

  api.sftp.onEdit((e) => {
    useFiles.setState((s) => {
      const edits = { ...s.edits };
      if (e.state === 'closed') delete edits[e.editId];
      else edits[e.editId] = e;
      return { edits };
    });
    const name = e.remotePath.slice(e.remotePath.lastIndexOf('/') + 1);
    if (e.state === 'uploaded') toast.success('Saved to the server', name);
    if (e.state === 'error') toast.error('Not saved to the server', `${name}: ${e.error ?? ''}`);
  });
}

/** Overall progress of one remote session's active transfers. */
export function summarize(transfers: TransferEntry[]): { active: number; transferred: number; total: number; speed: number } {
  let active = 0;
  let transferred = 0;
  let total = 0;
  let speed = 0;
  for (const t of transfers) {
    if (FINAL.has(t.state)) continue;
    active++;
    transferred += t.transferred;
    total += t.total;
    if (t.state === 'running') speed += t.speed;
  }
  return { active, transferred, total, speed };
}
