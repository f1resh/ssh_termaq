/**
 * SFTP file browser side panel for an SSH pane: navigation, sortable multi-select list, transfers with progress,
 * drag & drop upload from Explorer, context menu, rename / delete / new folder / permissions.
 */
import { useCallback, useEffect, useMemo, useRef, useState, type DragEvent, type KeyboardEvent as ReactKeyboardEvent } from 'react';
import {
  ArrowUp,
  Download,
  Eye,
  EyeOff,
  File as FileIcon,
  FileSymlink,
  Folder,
  FolderPlus,
  Home,
  Loader2,
  RefreshCw,
  Upload,
  X
} from 'lucide-react';
import { api } from '@/bridge/api';
import { Button, Checkbox, IconButton, Modal, cx, showContextMenu, toast, type MenuItem } from '@/components/ui';
import { confirmDialog, inputDialog } from '@/store/ui';
import { useWorkspace } from '@/store/workspace';
import type { SftpEntry, TransferProgress } from '@shared/types';
import './sftp.css';

export interface SftpPanelProps {
  paneId: string;
  onClose: () => void;
}

type SortKey = 'name' | 'size' | 'mtime' | 'permissions';

const parentOf = (p: string) => {
  if (p === '/' || !p) return '/';
  const t = p.replace(/\/+$/, '');
  const i = t.lastIndexOf('/');
  return i <= 0 ? '/' : t.slice(0, i);
};
const join = (dir: string, name: string) => (dir.endsWith('/') ? dir + name : `${dir}/${name}`);
const fmtSize = (n: number) => {
  if (n < 1024) return `${n} B`;
  const u = ['KB', 'MB', 'GB', 'TB'];
  let v = n / 1024;
  let i = 0;
  while (v >= 1024 && i < u.length - 1) {
    v /= 1024;
    i++;
  }
  return `${v < 10 ? v.toFixed(1) : Math.round(v)} ${u[i]}`;
};
const fmtDate = (ms: number) => {
  if (!ms) return '';
  const d = new Date(ms);
  const p = (x: number) => String(x).padStart(2, '0');
  return `${d.getFullYear()}-${p(d.getMonth() + 1)}-${p(d.getDate())} ${p(d.getHours())}:${p(d.getMinutes())}`;
};
const isDir = (e: SftpEntry) => e.type === 'dir' || (e.type === 'link' && !!e.linkTargetIsDir);

function errorText(e: unknown) {
  return e instanceof Error ? e.message : String(e);
}

export function SftpPanel({ paneId, onClose }: SftpPanelProps) {
  const pane = useWorkspace((s) => s.panes[paneId]);
  const connected = pane?.status === 'connected' && pane.session.protocol === 'ssh';
  const [sftpId, setSftpId] = useState<string | null>(null);
  const [home, setHome] = useState('/');
  const [path, setPath] = useState('');
  const [pathText, setPathText] = useState('');
  const [entries, setEntries] = useState<SftpEntry[]>([]);
  const [loading, setLoading] = useState(false);
  const [error, setError] = useState('');
  const [selected, setSelected] = useState<Set<string>>(new Set());
  const [anchor, setAnchor] = useState<string | null>(null);
  const [showHidden, setShowHidden] = useState(false);
  const [sort, setSort] = useState<{ key: SortKey; asc: boolean }>({ key: 'name', asc: true });
  const [transfers, setTransfers] = useState<Record<string, TransferProgress & { started: number }>>({});
  const [dragOver, setDragOver] = useState(false);
  const [chmodEntry, setChmodEntry] = useState<SftpEntry | null>(null);
  const listRef = useRef<HTMLDivElement>(null);

  /* ---- session ---- */
  useEffect(() => {
    if (!connected || !pane) return;
    let id: string | null = null;
    let alive = true;
    setError('');
    setLoading(true);
    api.sftp
      .open(pane.connId)
      .then((r) => {
        if (!alive) {
          api.sftp.close(r.sftpId);
          return;
        }
        id = r.sftpId;
        setSftpId(r.sftpId);
        setHome(r.home);
        setPath(pane.session.ssh.sftpStartPath || r.home);
      })
      .catch((e) => {
        setError(errorText(e));
        setLoading(false);
      });
    return () => {
      alive = false;
      if (id) api.sftp.close(id);
      setSftpId(null);
    };
  }, [connected, pane?.connId]); // eslint-disable-line react-hooks/exhaustive-deps

  useEffect(
    () =>
      api.sftp.onProgress((p) => {
        if (p.sftpId !== sftpId) return;
        setTransfers((cur) => ({ ...cur, [p.transferId]: { ...p, started: cur[p.transferId]?.started ?? Date.now() } }));
        if (p.state === 'done' && p.direction === 'upload') void refresh();
        if (p.state === 'error') toast.error(`${p.direction === 'upload' ? 'Upload' : 'Download'} failed`, `${p.name}: ${p.error}`);
      }),
    [sftpId] // eslint-disable-line react-hooks/exhaustive-deps
  );

  const load = useCallback(
    async (dir: string) => {
      if (!sftpId) return;
      setLoading(true);
      setError('');
      try {
        const list = await api.sftp.list(sftpId, dir);
        setEntries(list);
        setPath(dir);
        setPathText(dir);
        setSelected(new Set());
      } catch (e) {
        setError(errorText(e));
        setPathText(path);
      } finally {
        setLoading(false);
      }
    },
    [sftpId, path]
  );

  useEffect(() => {
    if (sftpId && path) void load(path);
  }, [sftpId]); // eslint-disable-line react-hooks/exhaustive-deps

  const refresh = () => load(path);

  const visible = useMemo(() => {
    const list = entries.filter((e) => e.name !== '.' && e.name !== '..' && (showHidden || !e.name.startsWith('.')));
    const dirFirst = (a: SftpEntry, b: SftpEntry) => Number(isDir(b)) - Number(isDir(a));
    const cmp: Record<SortKey, (a: SftpEntry, b: SftpEntry) => number> = {
      name: (a, b) => a.name.localeCompare(b.name, undefined, { numeric: true, sensitivity: 'base' }),
      size: (a, b) => a.size - b.size,
      mtime: (a, b) => a.mtime - b.mtime,
      permissions: (a, b) => a.permissions.localeCompare(b.permissions)
    };
    return list.sort((a, b) => dirFirst(a, b) || (sort.asc ? 1 : -1) * cmp[sort.key](a, b) || cmp.name(a, b));
  }, [entries, showHidden, sort]);

  const selectedEntries = visible.filter((e) => selected.has(e.path));

  /* ---- actions ---- */
  const open = (e: SftpEntry) => {
    if (isDir(e)) void load(e.path);
    else if (sftpId) api.sftp.openRemote(sftpId, e.path).catch((err) => toast.error('Could not open the file', errorText(err)));
  };
  const download = async (list = selectedEntries) => {
    if (!sftpId || !list.length) return;
    try {
      await api.sftp.download(sftpId, list.map((e) => e.path));
    } catch (e) {
      toast.error('Download failed', errorText(e));
    }
  };
  const upload = async (paths?: string[]) => {
    if (!sftpId) return;
    try {
      await api.sftp.upload(sftpId, path, paths);
    } catch (e) {
      toast.error('Upload failed', errorText(e));
    }
  };
  const mkdir = async () => {
    if (!sftpId) return;
    const name = await inputDialog({ title: 'New folder', label: 'Folder name', confirmLabel: 'Create', validate: (v) => (!v.trim() ? 'Enter a name' : v.includes('/') ? 'Names cannot contain "/"' : null) });
    if (!name) return;
    try {
      await api.sftp.mkdir(sftpId, join(path, name.trim()));
      await refresh();
    } catch (e) {
      toast.error('Could not create the folder', errorText(e));
    }
  };
  const rename = async (e: SftpEntry) => {
    if (!sftpId) return;
    const name = await inputDialog({ title: 'Rename', label: 'New name', initial: e.name, confirmLabel: 'Rename', validate: (v) => (!v.trim() ? 'Enter a name' : v.includes('/') ? 'Names cannot contain "/"' : null) });
    if (!name || name === e.name) return;
    try {
      await api.sftp.rename(sftpId, e.path, join(parentOf(e.path), name.trim()));
      await refresh();
    } catch (err) {
      toast.error('Rename failed', errorText(err));
    }
  };
  const remove = async (list = selectedEntries) => {
    if (!sftpId || !list.length) return;
    const dirs = list.filter(isDir).length;
    const ok = await confirmDialog({
      title: `Delete ${list.length === 1 ? `"${list[0].name}"` : `${list.length} items`}?`,
      message: dirs ? 'Folders are deleted with everything inside them. This cannot be undone.' : 'This cannot be undone.',
      confirmLabel: 'Delete',
      danger: true
    });
    if (!ok) return;
    try {
      for (const e of list) await api.sftp.remove(sftpId, e.path, isDir(e));
    } catch (err) {
      toast.error('Delete failed', errorText(err));
    }
    await refresh();
  };

  /* ---- selection / keyboard ---- */
  const select = (e: SftpEntry, ev: { ctrlKey: boolean; shiftKey: boolean; metaKey?: boolean }) => {
    if (ev.shiftKey && anchor) {
      const a = visible.findIndex((x) => x.path === anchor);
      const b = visible.findIndex((x) => x.path === e.path);
      const [lo, hi] = a < b ? [a, b] : [b, a];
      setSelected(new Set(visible.slice(lo, hi + 1).map((x) => x.path)));
      return;
    }
    if (ev.ctrlKey || ev.metaKey) {
      setSelected((cur) => {
        const n = new Set(cur);
        if (n.has(e.path)) n.delete(e.path);
        else n.add(e.path);
        return n;
      });
    } else {
      setSelected(new Set([e.path]));
    }
    setAnchor(e.path);
  };

  const onKeyDown = (ev: ReactKeyboardEvent) => {
    const idx = visible.findIndex((x) => x.path === anchor);
    if (ev.key === 'ArrowDown' || ev.key === 'ArrowUp') {
      ev.preventDefault();
      const next = visible[Math.max(0, Math.min(visible.length - 1, idx + (ev.key === 'ArrowDown' ? 1 : -1)))];
      if (next) {
        select(next, ev);
        listRef.current?.querySelector(`[data-path="${CSS.escape(next.path)}"]`)?.scrollIntoView({ block: 'nearest' });
      }
    } else if (ev.key === 'Enter' && idx >= 0) {
      open(visible[idx]);
    } else if (ev.key === 'Backspace') {
      ev.preventDefault();
      void load(parentOf(path));
    } else if (ev.key === 'Delete') {
      void remove();
    } else if (ev.key === 'F2' && idx >= 0) {
      void rename(visible[idx]);
    } else if (ev.key === 'F5') {
      ev.preventDefault();
      void refresh();
    } else if ((ev.ctrlKey || ev.metaKey) && ev.key.toLowerCase() === 'a') {
      ev.preventDefault();
      setSelected(new Set(visible.map((x) => x.path)));
    }
  };

  const menu = (x: number, y: number, target?: SftpEntry) => {
    const list = target && !selected.has(target.path) ? [target] : selectedEntries;
    const one = list.length === 1 ? list[0] : undefined;
    const items: MenuItem[] = target
      ? [
          { label: one && isDir(one) ? 'Open' : 'Open with default app', disabled: !one, onClick: () => one && open(one) },
          { label: list.length > 1 ? `Download ${list.length} items` : 'Download', onClick: () => void download(list) },
          { type: 'separator' },
          { label: 'Rename…', shortcut: 'F2', disabled: !one, onClick: () => one && void rename(one) },
          { label: 'Permissions…', disabled: !one, onClick: () => one && setChmodEntry(one) },
          { label: 'Copy path', disabled: !one, onClick: () => one && api.app.writeClipboard(one.path) },
          { type: 'separator' },
          { label: 'Delete', shortcut: 'Del', danger: true, onClick: () => void remove(list) }
        ]
      : [
          { label: 'Upload files…', onClick: () => void upload() },
          { label: 'New folder…', onClick: () => void mkdir() },
          { label: 'Refresh', shortcut: 'F5', onClick: () => void refresh() },
          { label: 'Copy current path', onClick: () => api.app.writeClipboard(path) }
        ];
    showContextMenu(x, y, items);
  };

  /* ---- drag & drop from Explorer ---- */
  const onDrop = async (e: DragEvent) => {
    e.preventDefault();
    setDragOver(false);
    if (!e.dataTransfer.files.length) return;
    const paths = await api.files.pathsForFiles(e.dataTransfer.files);
    if (paths.length) void upload(paths);
  };

  const transferList = Object.values(transfers).sort((a, b) => b.started - a.started);
  const activeTransfers = transferList.filter((t) => t.state === 'queued' || t.state === 'running').length;
  const crumbs = path.split('/').filter(Boolean);

  if (!pane) return null;
  return (
    <div className="sftp">
      <div className="sftp__header">
        <span className="sftp__title">SFTP</span>
        <span className="sftp__host" title={pane.session.host}>
          {pane.session.username ? `${pane.session.username}@` : ''}
          {pane.session.host}
        </span>
        <IconButton size="sm" label="Close SFTP browser" onClick={onClose}>
          <X size={14} />
        </IconButton>
      </div>

      {!connected ? (
        <div className="sftp__state">
          <span className="muted">{pane.session.protocol === 'ssh' ? 'Connect the session to browse its files.' : 'SFTP is available for SSH sessions only.'}</span>
        </div>
      ) : (
        <>
          <div className="sftp__toolbar">
            <IconButton size="sm" label="Up (Backspace)" onClick={() => void load(parentOf(path))} disabled={path === '/'}>
              <ArrowUp size={14} />
            </IconButton>
            <IconButton size="sm" label="Home" onClick={() => void load(home)}>
              <Home size={14} />
            </IconButton>
            <IconButton size="sm" label="Refresh (F5)" onClick={() => void refresh()}>
              <RefreshCw size={14} className={cx(loading && 'sftp__spin')} />
            </IconButton>
            <span className="sftp__sep" />
            <IconButton size="sm" label="New folder" onClick={() => void mkdir()}>
              <FolderPlus size={14} />
            </IconButton>
            <IconButton size="sm" label="Upload files" onClick={() => void upload()}>
              <Upload size={14} />
            </IconButton>
            <IconButton size="sm" label="Download selected" onClick={() => void download()} disabled={!selectedEntries.length}>
              <Download size={14} />
            </IconButton>
            <span className="sftp__spacer" />
            <IconButton size="sm" label={showHidden ? 'Hide hidden files' : 'Show hidden files'} pressed={showHidden} onClick={() => setShowHidden((v) => !v)}>
              {showHidden ? <Eye size={14} /> : <EyeOff size={14} />}
            </IconButton>
          </div>

          <form
            className="sftp__path"
            onSubmit={(e) => {
              e.preventDefault();
              void load(pathText.trim() || '/');
            }}
          >
            <div className="sftp__crumbs">
              <button type="button" className="sftp__crumb" onClick={() => void load('/')}>
                /
              </button>
              {crumbs.map((c, i) => (
                <button key={i} type="button" className="sftp__crumb" onClick={() => void load('/' + crumbs.slice(0, i + 1).join('/'))}>
                  {c}
                </button>
              ))}
            </div>
            <input className="input sftp__path-input" value={pathText} onChange={(e) => setPathText(e.target.value)} spellCheck={false} aria-label="Path" />
          </form>

          <div
            ref={listRef}
            className={cx('sftp__list', dragOver && 'sftp__list--drop')}
            tabIndex={0}
            onKeyDown={onKeyDown}
            onContextMenu={(e) => {
              e.preventDefault();
              menu(e.clientX, e.clientY);
            }}
            onDragOver={(e) => {
              if (!e.dataTransfer.types.includes('Files')) return;
              e.preventDefault();
              setDragOver(true);
            }}
            onDragLeave={() => setDragOver(false)}
            onDrop={(e) => void onDrop(e)}
          >
            <div className="sftp__row sftp__row--head">
              {(['name', 'size', 'mtime', 'permissions'] as SortKey[]).map((k) => (
                <button key={k} className={cx('sftp__col', `sftp__col--${k}`)} onClick={() => setSort((s) => ({ key: k, asc: s.key === k ? !s.asc : true }))}>
                  {{ name: 'Name', size: 'Size', mtime: 'Modified', permissions: 'Perms' }[k]}
                  {sort.key === k && <span className="sftp__sort">{sort.asc ? '▲' : '▼'}</span>}
                </button>
              ))}
            </div>
            {error && <div className="sftp__error">{error}</div>}
            {!error && loading && !entries.length && (
              <div className="sftp__state">
                <Loader2 size={18} className="sftp__spin" />
              </div>
            )}
            {!error && !loading && visible.length === 0 && <div className="sftp__state muted">This folder is empty. Drop files here to upload.</div>}
            {visible.map((e) => (
              <div
                key={e.path}
                data-path={e.path}
                className={cx('sftp__row', selected.has(e.path) && 'sftp__row--selected')}
                onMouseDown={(ev) => ev.button === 0 && select(e, ev)}
                onDoubleClick={() => open(e)}
                onContextMenu={(ev) => {
                  ev.preventDefault();
                  ev.stopPropagation();
                  if (!selected.has(e.path)) select(e, { ctrlKey: false, shiftKey: false });
                  menu(ev.clientX, ev.clientY, e);
                }}
                title={e.path}
              >
                <span className="sftp__col sftp__col--name">
                  {e.type === 'link' ? (
                    <FileSymlink size={14} className="sftp__icon" />
                  ) : isDir(e) ? (
                    <Folder size={14} className="sftp__icon sftp__icon--dir" />
                  ) : (
                    <FileIcon size={14} className="sftp__icon" />
                  )}
                  <span className="sftp__name">{e.name}</span>
                </span>
                <span className="sftp__col sftp__col--size">{isDir(e) ? '' : fmtSize(e.size)}</span>
                <span className="sftp__col sftp__col--mtime">{fmtDate(e.mtime)}</span>
                <span className="sftp__col sftp__col--permissions mono">{e.permissions}</span>
              </div>
            ))}
          </div>

          {transferList.length > 0 && (
            <div className="sftp__transfers">
              <div className="sftp__transfers-head">
                <span>Transfers{activeTransfers ? ` · ${activeTransfers} active` : ''}</span>
                <Button size="sm" variant="ghost" onClick={() => setTransfers((cur) => Object.fromEntries(Object.entries(cur).filter(([, t]) => t.state === 'running' || t.state === 'queued')))}>
                  Clear finished
                </Button>
              </div>
              {transferList.slice(0, 20).map((t) => {
                const pct = t.total > 0 ? Math.min(100, (t.transferred / t.total) * 100) : t.state === 'done' ? 100 : 0;
                const secs = (Date.now() - t.started) / 1000;
                const rate = secs > 0.5 && t.state === 'running' ? `${fmtSize(t.transferred / secs)}/s` : '';
                return (
                  <div key={t.transferId} className={cx('sftp__transfer', `sftp__transfer--${t.state}`)}>
                    <div className="sftp__transfer-line">
                      {t.direction === 'upload' ? <Upload size={12} /> : <Download size={12} />}
                      <span className="sftp__transfer-name" title={t.direction === 'upload' ? t.localPath : t.remotePath}>
                        {t.name}
                      </span>
                      <span className="sftp__transfer-meta">
                        {t.state === 'running' ? `${Math.round(pct)}% ${rate}` : t.state === 'error' ? 'Failed' : t.state === 'cancelled' ? 'Cancelled' : t.state === 'queued' ? 'Queued' : fmtSize(t.total)}
                      </span>
                      {(t.state === 'running' || t.state === 'queued') && (
                        <IconButton size="sm" label="Cancel" onClick={() => api.sftp.cancelTransfer(t.transferId)}>
                          <X size={12} />
                        </IconButton>
                      )}
                    </div>
                    <div className="sftp__bar">
                      <span style={{ width: `${pct}%` }} />
                    </div>
                  </div>
                );
              })}
            </div>
          )}
        </>
      )}
      {chmodEntry && sftpId && (
        <ChmodDialog
          entry={chmodEntry}
          onClose={() => setChmodEntry(null)}
          onApply={async (mode) => {
            try {
              await api.sftp.chmod(sftpId, chmodEntry.path, mode);
              setChmodEntry(null);
              await refresh();
            } catch (e) {
              toast.error('Could not change permissions', errorText(e));
            }
          }}
        />
      )}
    </div>
  );
}

function ChmodDialog({ entry, onClose, onApply }: { entry: SftpEntry; onClose: () => void; onApply: (mode: number) => void }) {
  const [mode, setMode] = useState(entry.mode & 0o7777);
  const bit = (b: number) => (mode & b) !== 0;
  const toggle = (b: number) => setMode((m) => m ^ b);
  const rows: [string, number][] = [
    ['Owner', 6],
    ['Group', 3],
    ['Others', 0]
  ];
  return (
    <Modal
      open
      title={`Permissions — ${entry.name}`}
      onClose={onClose}
      width={380}
      footer={
        <>
          <Button onClick={onClose}>Cancel</Button>
          <Button variant="primary" onClick={() => onApply(mode)}>
            Apply
          </Button>
        </>
      }
    >
      <div className="chmod">
        <div className="chmod__head">
          <span />
          <span>Read</span>
          <span>Write</span>
          <span>Execute</span>
        </div>
        {rows.map(([label, shift]) => (
          <div key={label} className="chmod__row">
            <span>{label}</span>
            {[4, 2, 1].map((b) => (
              <Checkbox key={b} checked={bit(b << shift)} onChange={() => toggle(b << shift)} />
            ))}
          </div>
        ))}
        <label className="chmod__octal">
          Octal
          <input
            className="input mono"
            value={mode.toString(8).padStart(3, '0')}
            onChange={(e) => {
              const v = parseInt(e.target.value || '0', 8);
              if (!Number.isNaN(v) && v <= 0o7777) setMode(v);
            }}
          />
        </label>
      </div>
    </Modal>
  );
}
