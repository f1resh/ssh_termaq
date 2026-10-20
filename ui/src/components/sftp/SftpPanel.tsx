/**
 * SFTP side panel of an SSH terminal: browses the server over the terminal's own connection (SFTP, or SCP for
 * servers without SFTP) with a sortable multi-select list, upload / download with progress, drag & drop upload from
 * Explorer, rename, delete, new folder, permissions and edit with upload on save. Transfers, conflict questions and
 * transfer errors use the file manager's shared state (filesStore); the full two-pane file manager is one click away.
 */
import { useCallback, useEffect, useMemo, useRef, useState, type DragEvent, type KeyboardEvent as ReactKeyboardEvent } from 'react';
import { useShallow } from 'zustand/react/shallow';
import {
  ArrowUp,
  Download,
  Eye,
  EyeOff,
  File as FileIcon,
  FileSymlink,
  Folder,
  FolderPlus,
  FolderTree,
  Home,
  Loader2,
  RefreshCw,
  Upload,
  X
} from 'lucide-react';
import { api } from '@/bridge/api';
import { openFileManagerFor } from '@/commands/actions';
import { Button, IconButton, cx, showContextMenu, toast, type MenuItem } from '@/components/ui';
import { ChmodDialog } from '@/components/files/ChmodDialog';
import { ConflictDialog } from '@/components/files/ConflictDialog';
import { isFinal, useFiles } from '@/components/files/filesStore';
import { listAction, listShortcut, ownKeys, type ListAction } from '@/components/files/listKeys';
import { fmtDate, fmtSize, nameError, remote, sortItems, type FileItem, type SortKey } from '@/components/files/paths';
import { fromRemote } from '@/components/files/sources';
import { useSettings } from '@/store/data';
import { confirmDialog, inputDialog } from '@/store/ui';
import { useWorkspace } from '@/store/workspace';
import type { RemoteProtocol, TransferOptions } from '@shared/types';
import './sftp.css';

export interface SftpPanelProps {
  paneId: string;
  /** False while the panel's tab is in the background (the panel stays mounted to keep its place). */
  visible: boolean;
  onClose: () => void;
}

const errorText = (e: unknown) => (e instanceof Error ? e.message : String(e));
/** The list shortcuts the panel handles (components/files/listKeys.ts). */
const SFTP_KEYS: readonly ListAction[] = ['refresh', 'selectAll', 'up', 'open', 'delete'];
const COLUMNS: { key: SortKey; label: string }[] = [
  { key: 'name', label: 'Name' },
  { key: 'size', label: 'Size' },
  { key: 'mtime', label: 'Modified' },
  { key: 'permissions', label: 'Perms' }
];

export function SftpPanel({ paneId, visible, onClose }: SftpPanelProps) {
  const pane = useWorkspace((s) => s.panes[paneId]);
  const fs = useSettings((s) => s.settings.files);
  const connected = pane?.status === 'connected' && pane.session.protocol === 'ssh';
  const [sftpId, setSftpId] = useState<string | null>(null);
  const [protocol, setProtocol] = useState<RemoteProtocol>('sftp');
  const [home, setHome] = useState('/');
  const [path, setPath] = useState('');
  const [pathText, setPathText] = useState('');
  const [items, setItems] = useState<FileItem[]>([]);
  const [loading, setLoading] = useState(false);
  const [error, setError] = useState('');
  const [selected, setSelected] = useState<Set<string>>(new Set());
  const [anchor, setAnchor] = useState<string | null>(null);
  const [showHidden, setShowHidden] = useState(fs.showHidden);
  const [sort, setSort] = useState<{ key: SortKey; asc: boolean }>({ key: 'name', asc: true });
  const [dragOver, setDragOver] = useState(false);
  const [chmodItem, setChmodItem] = useState<FileItem | null>(null);
  const listRef = useRef<HTMLDivElement>(null);

  const transfers = useFiles(useShallow((s) => (sftpId ? Object.values(s.transfers).filter((t) => t.sftpId === sftpId) : [])));
  const conflict = useFiles((s) => (sftpId ? s.conflicts.find((c) => c.sftpId === sftpId) : undefined));
  const uploadsDone = useFiles((s) => Object.values(s.transfers).filter((t) => t.sftpId === sftpId && t.direction === 'upload' && t.state === 'done').length);

  /* ---- session on the terminal's connection ---- */
  useEffect(() => {
    if (!connected || !pane) return;
    let id: string | null = null;
    let alive = true;
    setError('');
    setLoading(true);
    api.sftp
      .open(pane.connId, { fileProtocol: pane.session.ssh.fileProtocol, parallelTransfers: fs.parallelTransfers })
      .then((r) => {
        if (!alive) {
          api.sftp.close(r.sftpId);
          return;
        }
        id = r.sftpId;
        setSftpId(r.sftpId);
        setProtocol(r.protocol);
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

  const load = useCallback(
    async (dir: string) => {
      if (!sftpId) return;
      setLoading(true);
      setError('');
      try {
        const list = await api.sftp.list(sftpId, dir);
        setItems(list.map(fromRemote));
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
  const refresh = () => load(path);

  useEffect(() => {
    if (sftpId && path) void load(path);
  }, [sftpId]); // eslint-disable-line react-hooks/exhaustive-deps

  // Uploads land in the folder on screen most of the time: show them.
  useEffect(() => {
    if (uploadsDone && sftpId) void refresh();
  }, [uploadsDone]); // eslint-disable-line react-hooks/exhaustive-deps

  const visibleItems = useMemo(
    () => sortItems(showHidden ? items : items.filter((i) => !i.hidden), sort.key, sort.asc),
    [items, showHidden, sort]
  );
  const selectedItems = visibleItems.filter((i) => selected.has(i.path));
  const transferOpts = (): TransferOptions => ({
    conflict: fs.defaultConflict,
    preserveTimestamps: fs.preserveTimestamps,
    skipMacFiles: fs.skipMacFiles
  });

  /* ---- actions ---- */
  const open = (i: FileItem) => {
    if (i.isDir) void load(i.path);
    else if (sftpId) api.sftp.openRemote(sftpId, i.path).catch((e) => toast.error('Could not open the file', errorText(e)));
  };
  const edit = (i: FileItem) => {
    if (!sftpId) return;
    api.sftp.edit(sftpId, i.path, { editor: fs.editor || undefined }).catch((e) => toast.error('Could not edit the file', errorText(e)));
  };
  const download = async (list = selectedItems) => {
    if (!sftpId || !list.length) return;
    try {
      await api.sftp.download(sftpId, list.map((i) => i.path), undefined, transferOpts());
    } catch (e) {
      toast.error('Download failed', errorText(e));
    }
  };
  const upload = async (paths?: string[]) => {
    if (!sftpId) return;
    try {
      await api.sftp.upload(sftpId, path, paths, transferOpts());
    } catch (e) {
      toast.error('Upload failed', errorText(e));
    }
  };
  const mkdir = async () => {
    if (!sftpId) return;
    const name = await inputDialog({ title: 'New folder', label: 'Folder name', confirmLabel: 'Create', validate: (v) => nameError(v, 'remote') });
    if (!name) return;
    try {
      await api.sftp.mkdir(sftpId, remote.join(path, name.trim()));
      await refresh();
    } catch (e) {
      toast.error('Could not create the folder', errorText(e));
    }
  };
  const rename = async (i: FileItem) => {
    if (!sftpId) return;
    const name = await inputDialog({ title: 'Rename', label: 'New name', initial: i.name, confirmLabel: 'Rename', validate: (v) => nameError(v, 'remote') });
    if (!name || name.trim() === i.name) return;
    try {
      await api.sftp.rename(sftpId, i.path, remote.join(remote.parent(i.path) ?? '/', name.trim()));
      await refresh();
    } catch (e) {
      toast.error('Rename failed', errorText(e));
    }
  };
  const remove = async (list = selectedItems) => {
    if (!sftpId || !list.length) return;
    if (fs.confirmDelete) {
      const ok = await confirmDialog({
        title: `Delete ${list.length === 1 ? `"${list[0].name}"` : `${list.length} items`}?`,
        message: list.some((i) => i.isDir && !i.link) ? 'Folders are deleted with everything inside them. This cannot be undone.' : 'This cannot be undone.',
        confirmLabel: 'Delete',
        danger: true
      });
      if (!ok) return;
    }
    try {
      for (const i of list) await api.sftp.remove(sftpId, i.path, i.isDir && !i.link);
    } catch (e) {
      toast.error('Delete failed', errorText(e));
    }
    await refresh();
  };

  /* ---- selection / keyboard ---- */
  const select = (i: FileItem, ev: { ctrlKey: boolean; shiftKey: boolean; metaKey?: boolean }) => {
    if (ev.shiftKey && anchor) {
      const a = visibleItems.findIndex((x) => x.path === anchor);
      const b = visibleItems.findIndex((x) => x.path === i.path);
      const [lo, hi] = a < b ? [a, b] : [b, a];
      setSelected(new Set(visibleItems.slice(lo, hi + 1).map((x) => x.path)));
      return;
    }
    if (ev.ctrlKey || ev.metaKey) {
      setSelected((cur) => {
        const next = new Set(cur);
        if (next.has(i.path)) next.delete(i.path);
        else next.add(i.path);
        return next;
      });
    } else {
      setSelected(new Set([i.path]));
    }
    setAnchor(i.path);
  };

  const onKeyDown = (ev: ReactKeyboardEvent) => {
    const idx = visibleItems.findIndex((x) => x.path === anchor);
    const action = listAction(ev);
    if (action && SFTP_KEYS.includes(action)) ev.preventDefault();
    if (action === 'refresh') {
      void refresh();
    } else if (action === 'selectAll') {
      setSelected(new Set(visibleItems.map((x) => x.path)));
    } else if (action === 'up') {
      const up = remote.parent(path);
      if (up) void load(up);
    } else if (action === 'open') {
      if (idx >= 0) open(visibleItems[idx]);
    } else if (action === 'delete') {
      void remove();
    } else if (ev.key === 'ArrowDown' || ev.key === 'ArrowUp') {
      ev.preventDefault();
      const next = visibleItems[Math.max(0, Math.min(visibleItems.length - 1, idx + (ev.key === 'ArrowDown' ? 1 : -1)))];
      if (next) {
        select(next, ev);
        listRef.current?.querySelector(`[data-path="${CSS.escape(next.path)}"]`)?.scrollIntoView({ block: 'nearest' });
      }
    } else if (ev.key === 'Enter' && idx >= 0) {
      open(visibleItems[idx]);
    } else if (ev.key === 'Backspace') {
      ev.preventDefault();
      const up = remote.parent(path);
      if (up) void load(up);
    } else if (ev.key === 'Delete') {
      void remove();
    } else if (ev.key === 'F2' && idx >= 0) {
      void rename(visibleItems[idx]);
    } else if (ev.key === 'F4' && idx >= 0 && !visibleItems[idx].isDir) {
      edit(visibleItems[idx]);
    } else if (ev.key === 'F5') {
      ev.preventDefault();
      void refresh();
    }
  };

  const menu = (x: number, y: number, target?: FileItem) => {
    const list = target && !selected.has(target.path) ? [target] : selectedItems;
    const one = list.length === 1 ? list[0] : undefined;
    const items: MenuItem[] = target
      ? [
          { label: one?.isDir ? 'Open' : 'Open with default app', disabled: !one, onClick: () => one && open(one) },
          { label: 'Edit (upload on save)', shortcut: 'F4', disabled: !one || one.isDir, onClick: () => one && edit(one) },
          { label: list.length > 1 ? `Download ${list.length} items…` : 'Download…', onClick: () => void download(list) },
          { type: 'separator' },
          { label: 'Rename…', shortcut: 'F2', disabled: !one, onClick: () => one && void rename(one) },
          { label: 'Permissions…', disabled: !one, onClick: () => one && setChmodItem(one) },
          { label: 'Copy path', disabled: !one, onClick: () => one && api.app.writeClipboard(one.path) },
          { type: 'separator' },
          { label: 'Delete', shortcut: listShortcut('delete', 'Del'), danger: true, onClick: () => void remove(list) }
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

  const transferList = [...transfers].sort((a, b) => b.started - a.started);
  const activeTransfers = transferList.filter((t) => !isFinal(t)).length;

  if (!pane) return null;
  return (
    <div className="sftp">
      <div className="sftp__header">
        <span className="sftp__title">{protocol.toUpperCase()}</span>
        <span className="sftp__host" title={pane.session.host}>
          {pane.session.username ? `${pane.session.username}@` : ''}
          {pane.session.host}
        </span>
        <IconButton size="sm" label="Open in the file manager" onClick={() => openFileManagerFor(paneId)}>
          <FolderTree size={14} />
        </IconButton>
        <IconButton size="sm" label="Close the SFTP panel" onClick={onClose}>
          <X size={14} />
        </IconButton>
      </div>

      {!connected ? (
        <div className="sftp__state">
          <span className="muted">{pane.session.protocol === 'ssh' ? 'Connect the session to browse its files.' : 'The SFTP panel needs an SSH session.'}</span>
        </div>
      ) : (
        <>
          <div className="sftp__toolbar">
            <IconButton size="sm" label="Up (Backspace)" onClick={() => void load(remote.parent(path) ?? '/')} disabled={!remote.parent(path)}>
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
            <IconButton size="sm" label="Download selected" onClick={() => void download()} disabled={!selectedItems.length}>
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
              void load(remote.normalize(pathText, path || '/'));
            }}
          >
            <div className="sftp__crumbs">
              {remote.crumbs(path).map((c) => (
                <button key={c.path} type="button" className="sftp__crumb" onClick={() => void load(c.path)}>
                  {c.label}
                </button>
              ))}
            </div>
            <input className="input sftp__path-input" value={pathText} onChange={(e) => setPathText(e.target.value)} spellCheck={false} aria-label="Path" />
          </form>

          <div
            ref={listRef}
            className={cx('sftp__list', dragOver && 'sftp__list--drop')}
            tabIndex={0}
            data-own-keys={ownKeys(SFTP_KEYS)}
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
              {COLUMNS.map((c) => (
                <button key={c.key} className={cx('sftp__col', `sftp__col--${c.key}`)} onClick={() => setSort((s) => ({ key: c.key, asc: s.key === c.key ? !s.asc : true }))}>
                  {c.label}
                  {sort.key === c.key && <span className="sftp__sort">{sort.asc ? '▲' : '▼'}</span>}
                </button>
              ))}
            </div>
            {error && <div className="sftp__error">{error}</div>}
            {!error && loading && !items.length && (
              <div className="sftp__state">
                <Loader2 size={18} className="sftp__spin" />
              </div>
            )}
            {!error && !loading && visibleItems.length === 0 && <div className="sftp__state muted">This folder is empty. Drop files here to upload.</div>}
            {visibleItems.map((i) => (
              <div
                key={i.path}
                data-path={i.path}
                className={cx('sftp__row', selected.has(i.path) && 'sftp__row--selected')}
                onMouseDown={(ev) => ev.button === 0 && select(i, ev)}
                onDoubleClick={() => open(i)}
                onContextMenu={(ev) => {
                  ev.preventDefault();
                  ev.stopPropagation();
                  if (!selected.has(i.path)) select(i, { ctrlKey: false, shiftKey: false });
                  menu(ev.clientX, ev.clientY, i);
                }}
                title={i.path}
              >
                <span className="sftp__col sftp__col--name">
                  {i.link ? (
                    <FileSymlink size={14} className="sftp__icon" />
                  ) : i.isDir ? (
                    <Folder size={14} className="sftp__icon sftp__icon--dir" />
                  ) : (
                    <FileIcon size={14} className="sftp__icon" />
                  )}
                  <span className="sftp__name">{i.name}</span>
                </span>
                <span className="sftp__col sftp__col--size">{i.isDir ? '' : fmtSize(i.size)}</span>
                <span className="sftp__col sftp__col--mtime">{fmtDate(i.mtime)}</span>
                <span className="sftp__col sftp__col--permissions mono">{i.permissions}</span>
              </div>
            ))}
          </div>

          {transferList.length > 0 && (
            <div className="sftp__transfers">
              <div className="sftp__transfers-head">
                <span>Transfers{activeTransfers ? ` · ${activeTransfers} active` : ''}</span>
                <Button size="sm" variant="ghost" onClick={() => useFiles.getState().clearFinished(sftpId ?? undefined)}>
                  Clear finished
                </Button>
              </div>
              {transferList.slice(0, 20).map((t) => {
                const pct = t.total > 0 ? Math.min(100, (t.transferred / t.total) * 100) : t.state === 'done' ? 100 : 0;
                const rate = t.state === 'running' && t.speed > 0 ? ` ${fmtSize(t.speed)}/s` : '';
                return (
                  <div key={t.transferId} className={cx('sftp__transfer', `sftp__transfer--${t.state}`)}>
                    <div className="sftp__transfer-line">
                      {t.direction === 'upload' ? <Upload size={12} /> : <Download size={12} />}
                      <span className="sftp__transfer-name" title={t.direction === 'upload' ? t.localPath : t.remotePath}>
                        {t.name}
                      </span>
                      <span className="sftp__transfer-meta">
                        {t.state === 'running'
                          ? `${Math.round(pct)}%${rate}`
                          : t.state === 'error'
                            ? 'Failed'
                            : t.state === 'cancelled'
                              ? 'Cancelled'
                              : t.state === 'queued'
                                ? 'Queued'
                                : fmtSize(t.total)}
                      </span>
                      {!isFinal(t) && (
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

      {visible && conflict && (
        <ConflictDialog
          conflict={conflict}
          onAnswer={(r) => {
            api.sftp.resolveConflict(conflict.conflictId, r);
            useFiles.getState().dropConflict(conflict.conflictId);
          }}
        />
      )}
      {visible && chmodItem && sftpId && (
        <ChmodDialog
          item={chmodItem}
          onClose={() => setChmodItem(null)}
          onApply={async (mode) => {
            try {
              await api.sftp.chmod(sftpId, chmodItem.path, mode);
              setChmodItem(null);
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
