/**
 * Transfer queue of one remote session: overall progress, per-transfer progress with speed / ETA / file counts,
 * cancel, retry and "show in Explorer / Finder". Collapsible; remembers its state per app.
 */
import { useEffect, useMemo, useState } from 'react';
import { ArrowDownToLine, ArrowUpFromLine, ChevronDown, ChevronUp, FolderSearch, RotateCcw, Trash2, X } from 'lucide-react';
import { api } from '@/bridge/api';
import { Button, IconButton, cx, toast } from '@/components/ui';
import { osTerms } from '@shared/platform';
import { isFinal, summarize, useFiles, type TransferEntry } from './filesStore';
import { fmtDuration, fmtSize } from './paths';

const errorText = (e: unknown) => (e instanceof Error ? e.message : String(e));

function stateLabel(t: TransferEntry): string {
  switch (t.state) {
    case 'queued':
      return 'Queued';
    case 'error':
      return 'Failed';
    case 'cancelled':
      return 'Cancelled';
    case 'done':
      return t.skipped ? `Done, ${t.skipped} skipped` : t.direction === 'delete' ? 'Deleted' : fmtSize(t.total);
    default: {
      if (t.listing) return `Listing… ${t.filesTotal ?? 0} files · ${fmtSize(t.total)}`;
      const pct = t.total > 0 ? Math.min(100, Math.floor((t.transferred / t.total) * 100)) : 0;
      const eta = t.speed > 0 && t.total > t.transferred ? ` · ${fmtDuration((t.total - t.transferred) / t.speed)}` : '';
      return `${pct}%${t.speed > 0 ? ` · ${fmtSize(t.speed)}/s` : ''}${eta}`;
    }
  }
}

async function retry(t: TransferEntry) {
  const info = useFiles.getState().retry[t.transferId];
  if (!info) return;
  try {
    const ids =
      info.kind === 'download'
        ? await api.sftp.download(info.sftpId, info.sources, info.targetDir, info.opts)
        : await api.sftp.upload(info.sftpId, info.targetDir, info.sources, info.opts);
    useFiles.getState().rememberRetry(ids, info);
  } catch (e) {
    toast.error('Retry failed', errorText(e));
  }
}

export function TransferQueue({ sftpId }: { sftpId: string | null }) {
  const all = useFiles((s) => s.transfers);
  const retryable = useFiles((s) => s.retry);
  const [open, setOpen] = useState(true);
  const [, tick] = useState(0);

  const list = useMemo(
    () =>
      Object.values(all)
        .filter((t) => t.sftpId === sftpId && t.origin !== 'drag')
        .sort((a, b) => Number(isFinal(a)) - Number(isFinal(b)) || b.started - a.started),
    [all, sftpId]
  );
  const sum = summarize(list);
  const finished = list.length - sum.active;

  // Refresh the "speed" display even when no event arrives for a while.
  useEffect(() => {
    if (!sum.active) return;
    const t = setInterval(() => tick((n) => n + 1), 1000);
    return () => clearInterval(t);
  }, [sum.active]);

  if (!list.length) return null;
  const pct = sum.total > 0 ? Math.min(100, (sum.transferred / sum.total) * 100) : 0;

  return (
    <div className={cx('fm-queue', !open && 'fm-queue--closed')}>
      <div className="fm-queue__head">
        <button className="fm-queue__toggle" onClick={() => setOpen((v) => !v)} aria-expanded={open}>
          {open ? <ChevronDown size={14} /> : <ChevronUp size={14} />}
          <span>Transfers</span>
        </button>
        {sum.active > 0 ? (
          <span className="fm-queue__summary">
            {sum.active} active · {Math.floor(pct)}%{sum.speed > 0 ? ` · ${fmtSize(sum.speed)}/s` : ''}
            {sum.speed > 0 && sum.total > sum.transferred ? ` · ${fmtDuration((sum.total - sum.transferred) / sum.speed)} left` : ''}
          </span>
        ) : (
          <span className="fm-queue__summary fm-muted">All done</span>
        )}
        {sum.active > 0 && (
          <span className="fm-bar fm-bar--inline">
            <span style={{ width: `${pct}%` }} />
          </span>
        )}
        <span className="fm-spacer" />
        {finished > 0 && (
          <Button size="sm" variant="ghost" icon={<Trash2 size={13} />} onClick={() => useFiles.getState().clearFinished(sftpId ?? undefined)}>
            Clear finished
          </Button>
        )}
      </div>
      {open && (
        <div className="fm-queue__list">
          {list.slice(0, 200).map((t) => {
            const pctT = t.total > 0 ? Math.min(100, (t.transferred / t.total) * 100) : t.state === 'done' ? 100 : 0;
            const Icon = t.direction === 'upload' ? ArrowUpFromLine : t.direction === 'download' ? ArrowDownToLine : Trash2;
            return (
              <div key={t.transferId} className={cx('fm-transfer', `fm-transfer--${t.state}`)}>
                <Icon size={13} className="fm-transfer__icon" />
                <div className="fm-transfer__main">
                  <div className="fm-transfer__line">
                    <span className="fm-transfer__name" title={t.direction === 'upload' ? `${t.localPath} → ${t.remotePath}` : `${t.remotePath} → ${t.localPath}`}>
                      {t.name}
                      {t.move ? <span className="fm-tag">move</span> : null}
                      {t.origin === 'sync' ? <span className="fm-tag">sync</span> : null}
                      {t.origin === 'edit' ? <span className="fm-tag">edit</span> : null}
                    </span>
                    {t.filesTotal && t.filesTotal > 1 && !t.listing ? (
                      <span className="fm-transfer__files">
                        {t.filesDone ?? 0}/{t.filesTotal} files
                      </span>
                    ) : null}
                    <span className="fm-transfer__meta" title={t.error}>
                      {stateLabel(t)}
                    </span>
                  </div>
                  {t.state === 'running' || t.state === 'queued' ? (
                    <div className={cx('fm-bar', t.listing && 'fm-bar--busy')}>
                      <span style={{ width: t.listing ? undefined : `${pctT}%` }} />
                    </div>
                  ) : t.state === 'error' ? (
                    <div className="fm-transfer__error">{t.error}</div>
                  ) : null}
                  {t.state === 'running' && t.listing ? (
                    <div className="fm-transfer__current">Listing {t.currentFile || t.name}</div>
                  ) : t.state === 'running' && t.currentFile && t.filesTotal && t.filesTotal > 1 ? (
                    <div className="fm-transfer__current">{t.currentFile}</div>
                  ) : null}
                </div>
                <div className="fm-transfer__actions">
                  {(t.state === 'running' || t.state === 'queued') && (
                    <IconButton size="sm" label="Cancel" onClick={() => api.sftp.cancelTransfer(t.transferId)}>
                      <X size={12} />
                    </IconButton>
                  )}
                  {(t.state === 'error' || t.state === 'cancelled') && retryable[t.transferId] && (
                    <IconButton size="sm" label="Retry" onClick={() => void retry(t)}>
                      <RotateCcw size={12} />
                    </IconButton>
                  )}
                  {t.state === 'done' && t.direction === 'download' && t.origin !== 'edit' && t.origin !== 'open' && (
                    <IconButton size="sm" label={`Show in ${osTerms.fileBrowser}`} onClick={() => void api.local.reveal(t.localPath).catch(() => undefined)}>
                      <FolderSearch size={12} />
                    </IconButton>
                  )}
                </div>
              </div>
            );
          })}
        </div>
      )}
    </div>
  );
}
