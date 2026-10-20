/**
 * Synchronize a local and a remote folder: options -> comparison preview (every change can be unchecked) -> apply
 * (transfers and deletes go through the transfer queue). Deletes need an extra confirmation.
 */
import { useEffect, useMemo, useState } from 'react';
import { ArrowDownToLine, ArrowUpFromLine, FolderSync, Loader2, Trash2 } from 'lucide-react';
import { api } from '@/bridge/api';
import { Button, Checkbox, Field, Modal, TextInput, cx, toast } from '@/components/ui';
import { confirmDialog } from '@/store/ui';
import { osTerms } from '@shared/platform';
import type { SyncDirection, SyncItem, SyncRequest } from '@shared/types';
import { fmtDate, fmtSize } from './paths';

const STATE_KEY = 'fm.syncOptions';
const errorText = (e: unknown) => (e instanceof Error ? e.message : String(e));

type Options = Omit<SyncRequest, 'localDir' | 'remoteDir'>;

const DEFAULTS: Options = {
  direction: 'upload',
  compareTime: true,
  compareSize: true,
  deleteExtra: false,
  exclude: '',
  includeHidden: true
};

const DIRECTIONS: { value: SyncDirection; label: string; hint: string }[] = [
  { value: 'upload', label: `Server ← ${osTerms.thisComputer}`, hint: 'Make the server folder match the local one' },
  { value: 'download', label: `${osTerms.ThisComputer} ← server`, hint: 'Make the local folder match the server folder' },
  { value: 'both', label: 'Both ways', hint: 'Copy newer files in each direction (never deletes)' }
];

export function actionLabel(a: SyncItem['action']): string {
  return a === 'upload' ? 'Upload' : a === 'download' ? 'Download' : a === 'deleteRemote' ? 'Delete on server' : `Delete on ${osTerms.thisComputer}`;
}

/** Totals of a selection of sync items (bytes counted for files only). */
export function syncTotals(items: SyncItem[]) {
  const t = { upload: 0, download: 0, deletes: 0, bytes: 0 };
  for (const i of items) {
    if (i.action === 'upload') {
      t.upload++;
      t.bytes += i.local?.size ?? 0;
    } else if (i.action === 'download') {
      t.download++;
      t.bytes += i.remote?.size ?? 0;
    } else {
      t.deletes++;
    }
  }
  return t;
}

export function SyncDialog(props: {
  sftpId: string;
  localDir: string;
  remoteDir: string;
  deleteToRecycleBin: boolean;
  onClose: () => void;
}) {
  const { sftpId, deleteToRecycleBin, onClose } = props;
  const [localDir, setLocalDir] = useState(props.localDir);
  const [remoteDir, setRemoteDir] = useState(props.remoteDir);
  const [opts, setOpts] = useState<Options>(DEFAULTS);
  const [phase, setPhase] = useState<'options' | 'comparing' | 'preview' | 'applying'>('options');
  const [items, setItems] = useState<SyncItem[]>([]);
  const [checked, setChecked] = useState<Set<string>>(new Set());

  useEffect(() => {
    api.app
      .getState<Options>(STATE_KEY)
      .then((v) => v && setOpts({ ...DEFAULTS, ...v }))
      .catch(() => undefined);
  }, []);

  const req: SyncRequest = { ...opts, localDir, remoteDir };
  const keyOf = (i: SyncItem) => `${i.action}:${i.relPath}`;
  const chosen = useMemo(() => items.filter((i) => checked.has(keyOf(i))), [items, checked]);
  const totals = syncTotals(chosen);

  const compare = async () => {
    if (!localDir.trim() || !remoteDir.trim()) return;
    setPhase('comparing');
    api.app.setState(STATE_KEY, opts).catch(() => undefined);
    try {
      const list = await api.sftp.syncCompare(sftpId, req);
      setItems(list);
      setChecked(new Set(list.map(keyOf)));
      setPhase('preview');
    } catch (e) {
      toast.error('Comparison failed', errorText(e));
      setPhase('options');
    }
  };

  const apply = async () => {
    if (!chosen.length) return;
    if (totals.deletes) {
      const ok = await confirmDialog({
        title: 'Delete files?',
        message: `${totals.deletes} item${totals.deletes === 1 ? '' : 's'} will be deleted${
          chosen.some((i) => i.action === 'deleteLocal') && deleteToRecycleBin ? ` (local files go to the ${osTerms.trash})` : ''
        }. Continue?`,
        confirmLabel: 'Synchronize',
        danger: true
      });
      if (!ok) return;
    }
    setPhase('applying');
    try {
      await api.sftp.syncApply(sftpId, req, chosen, { conflict: 'overwrite', deleteToRecycleBin });
      onClose();
    } catch (e) {
      toast.error('Synchronize failed', errorText(e));
      setPhase('preview');
    }
  };

  const set = <K extends keyof Options>(k: K, v: Options[K]) => setOpts((o) => ({ ...o, [k]: v }));

  return (
    <Modal
      open
      width={phase === 'preview' || phase === 'applying' ? 860 : 560}
      icon={<FolderSync size={18} />}
      title="Synchronize folders"
      onClose={onClose}
      footer={
        phase === 'preview' || phase === 'applying' ? (
          <>
            <span className="fm-sync__totals">
              {items.length === 0
                ? 'Nothing to do'
                : `${totals.upload} upload${totals.upload === 1 ? '' : 's'} · ${totals.download} download${totals.download === 1 ? '' : 's'} · ${totals.deletes} delete${totals.deletes === 1 ? '' : 's'}${totals.bytes ? ` · ${fmtSize(totals.bytes)}` : ''}`}
            </span>
            <span className="fm-spacer" />
            <Button onClick={() => setPhase('options')}>Back</Button>
            <Button variant="primary" disabled={!chosen.length || phase === 'applying'} onClick={() => void apply()}>
              Synchronize
            </Button>
          </>
        ) : (
          <>
            <Button onClick={onClose}>Cancel</Button>
            <Button variant="primary" disabled={phase === 'comparing' || !localDir.trim() || !remoteDir.trim()} onClick={() => void compare()}>
              {phase === 'comparing' ? 'Comparing…' : 'Compare'}
            </Button>
          </>
        )
      }
    >
      {phase === 'options' || phase === 'comparing' ? (
        <div className="fm-sync">
          <Field label="Local folder">
            <TextInput value={localDir} onChange={setLocalDir} className="mono" />
          </Field>
          <Field label="Server folder">
            <TextInput value={remoteDir} onChange={setRemoteDir} className="mono" />
          </Field>
          <div className="fm-sync__dirs" role="radiogroup" aria-label="Direction">
            {DIRECTIONS.map((d) => (
              <label key={d.value} className={cx('fm-sync__dir', opts.direction === d.value && 'fm-sync__dir--on')}>
                <input type="radio" name="sync-direction" checked={opts.direction === d.value} onChange={() => set('direction', d.value)} />
                <span className="fm-sync__dir-label">{d.label}</span>
                <span className="fm-sync__dir-hint">{d.hint}</span>
              </label>
            ))}
          </div>
          <div className="fm-sync__row">
            <span className="field__label">Compare by</span>
            <Checkbox checked={opts.compareTime} onChange={(v) => set('compareTime', v)} label="Modification time" />
            <Checkbox checked={opts.compareSize} onChange={(v) => set('compareSize', v)} label="Size" />
          </div>
          <Checkbox
            checked={opts.deleteExtra && opts.direction !== 'both'}
            disabled={opts.direction === 'both'}
            onChange={(v) => set('deleteExtra', v)}
            label={`Delete files that only exist on ${opts.direction === 'download' ? osTerms.thisComputer : 'the server'}`}
          />
          <Checkbox checked={opts.includeHidden} onChange={(v) => set('includeHidden', v)} label="Include hidden files (names starting with a dot)" />
          <Field label="Exclude" hint='Separate masks with ";". A trailing "/" matches folders only, e.g. *.tmp; .git/; node_modules/'>
            <TextInput value={opts.exclude} onChange={(v) => set('exclude', v)} placeholder="*.tmp; .git/" />
          </Field>
          {phase === 'comparing' && (
            <div className="fm-sync__busy">
              <Loader2 size={16} className="fm-spin" /> Comparing folders…
            </div>
          )}
        </div>
      ) : items.length === 0 ? (
        <div className="fm-sync__empty">The folders are already in sync.</div>
      ) : (
        <div className="fm-sync__preview">
          <div className="fm-sync__item fm-sync__item--head">
            <Checkbox
              checked={chosen.length === items.length}
              onChange={(v) => setChecked(v ? new Set(items.map(keyOf)) : new Set())}
              title="Select all"
            />
            <span>Action</span>
            <span>Path</span>
            <span>{osTerms.ThisComputer}</span>
            <span>Server</span>
          </div>
          <div className="fm-sync__list">
            {items.map((i) => {
              const k = keyOf(i);
              const Icon = i.action === 'upload' ? ArrowUpFromLine : i.action === 'download' ? ArrowDownToLine : Trash2;
              return (
                <label key={k} className={cx('fm-sync__item', `fm-sync__item--${i.action}`)}>
                  <Checkbox
                    checked={checked.has(k)}
                    onChange={(v) =>
                      setChecked((cur) => {
                        const n = new Set(cur);
                        if (v) n.add(k);
                        else n.delete(k);
                        return n;
                      })
                    }
                  />
                  <span className="fm-sync__action">
                    <Icon size={13} /> {actionLabel(i.action)}
                    <span className="fm-tag">{i.reason}</span>
                  </span>
                  <span className="fm-sync__path mono" title={i.relPath}>
                    {i.relPath}
                    {i.isDir ? '/' : ''}
                  </span>
                  <span className="fm-sync__side">{i.local ? (i.isDir ? 'folder' : `${fmtSize(i.local.size)} · ${fmtDate(i.local.mtime)}`) : '—'}</span>
                  <span className="fm-sync__side">{i.remote ? (i.isDir ? 'folder' : `${fmtSize(i.remote.size)} · ${fmtDate(i.remote.mtime)}`) : '—'}</span>
                </label>
              );
            })}
          </div>
        </div>
      )}
    </Modal>
  );
}
