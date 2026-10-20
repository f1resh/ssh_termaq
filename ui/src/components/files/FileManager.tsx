/**
 * The file manager tab: this PC on the left, the server on the right (SFTP, or SCP when the server has no SFTP),
 * the transfer queue below. Works on its own shell-less SSH connection, or on the connection of the terminal it was
 * opened from. Copy / move between the panes (F5 / F6, drag & drop, context menus), Explorer drag in and out,
 * edit with auto-upload, synchronize, find, bookmarks, conflict questions.
 */
import { useCallback, useEffect, useMemo, useRef, useState, type PointerEvent as ReactPointerEvent } from 'react';
import { useShallow } from 'zustand/react/shallow';
import { Eye, EyeOff, FilePen, FolderSync, Link2, Loader2, Plug, RotateCcw, Search, SquareTerminal, X } from 'lucide-react';
import { api } from '@/bridge/api';
import { connectSession } from '@/commands/actions';
import { Button, IconButton, Select, cx, showContextMenu, toast, type MenuItem } from '@/components/ui';
import { useSessions, useSettings, usePrompts } from '@/store/data';
import { confirmDialog } from '@/store/ui';
import { useWorkspace } from '@/store/workspace';
import { PANE_PROMPT_KINDS } from '@/terminal/connEvents';
import { PromptOverlay } from '@/terminal/PromptOverlay';
import { isMac, osTerms } from '@shared/platform';
import type { ConflictPolicy, SftpOpenResult, TransferOptions } from '@shared/types';
import { ChmodDialog } from './ChmodDialog';
import { ConflictDialog } from './ConflictDialog';
import { connOfFilesPane } from './filesConn';
import { isFinal, useFiles } from './filesStore';
import { FilePane, type FilePaneHandle, type PaneOps } from './FilePane';
import { FindDialog } from './FindDialog';
import { local, remote, type Crumb, type FileItem, type Side } from './paths';
import { listShortcut } from './listKeys';
import { localSource, remoteSource } from './sources';
import { SyncDialog } from './SyncDialog';
import { TransferQueue } from './TransferQueue';
import './files.css';

const BOOKMARKS_KEY = 'fm.bookmarks';
const SPLIT_KEY = 'fm.split';
const LAST_LOCAL_KEY = 'fm.lastLocal';
const errorText = (e: unknown) => (e instanceof Error ? e.message : String(e));

interface Bookmarks {
  local: Crumb[];
  /** Per server ("user@host:port"). */
  remote: Record<string, Crumb[]>;
}

const CONFLICT_OPTIONS: { value: ConflictPolicy; label: string }[] = [
  { value: 'ask', label: 'If a file exists: ask' },
  { value: 'overwrite', label: 'If a file exists: overwrite' },
  { value: 'newer', label: 'If a file exists: overwrite if newer' },
  { value: 'skip', label: 'If a file exists: skip' },
  { value: 'resume', label: 'If a file exists: resume' }
];

export function FileManager({ paneId, visible }: { paneId: string; visible: boolean }) {
  const pane = useWorkspace((s) => s.panes[paneId]);
  const conn = useWorkspace(
    useShallow((s) => {
      const p = s.panes[paneId];
      return p ? connOfFilesPane(p, s.panes) : { connId: '', status: 'disconnected' as const, message: undefined };
    })
  );
  const fs = useSettings((s) => s.settings.files);
  const prompt = usePrompts((s) => s.pending.find((p) => conn.connId && p.connId === conn.connId && PANE_PROMPT_KINDS.has(p.kind)));
  const rootRef = useRef<HTMLDivElement>(null);
  const localRef = useRef<FilePaneHandle>(null);
  const remoteRef = useRef<FilePaneHandle>(null);

  const [sftp, setSftp] = useState<SftpOpenResult | null>(null);
  const [sftpError, setSftpError] = useState('');
  const [activeSide, setActiveSide] = useState<Side>('remote');
  const [localPath, setLocalPathState] = useState<string | null>(pane?.files?.localPath ?? null);
  const [remotePath, setRemotePathState] = useState<string | null>(pane?.files?.remotePath ?? null);
  const [bookmarks, setBookmarks] = useState<Bookmarks>({ local: [], remote: {} });
  const [split, setSplit] = useState(0.5);
  const [dialog, setDialog] = useState<null | { type: 'sync' } | { type: 'find' } | { type: 'chmod'; item: FileItem }>(null);
  const [dragging, setDragging] = useState<Side | null>(null);
  const serverDrag = useRef<FileItem[] | null>(null);  // server items being dragged in the page

  const session = pane?.session;
  const host = session?.host ?? '';
  const serverKey = session ? `${session.username}@${session.host}:${session.port}` : '';
  const connected = conn.status === 'connected' && !!conn.connId;
  const sftpId = sftp?.sftpId ?? null;

  /* ---- remote file session on the connection ---- */
  useEffect(() => {
    if (!connected || !session) {
      setSftp(null);
      return;
    }
    let alive = true;
    let id: string | null = null;
    setSftpError('');
    api.sftp
      .open(conn.connId, { fileProtocol: session.ssh.fileProtocol, parallelTransfers: fs.parallelTransfers })
      .then((r) => {
        if (!alive) {
          api.sftp.close(r.sftpId);
          return;
        }
        id = r.sftpId;
        setSftp(r);
      })
      .catch((e) => alive && setSftpError(errorText(e)));
    return () => {
      alive = false;
      if (id) api.sftp.close(id);
      setSftp(null);
    };
  }, [connected, conn.connId]); // eslint-disable-line react-hooks/exhaustive-deps

  /* ---- folders (persisted with the workspace) ---- */
  const setLocalPath = useCallback(
    (p: string) => {
      setLocalPathState(p);
      const cur = useWorkspace.getState().panes[paneId];
      if (cur) useWorkspace.getState().updatePane(paneId, { files: { ...cur.files, localPath: p } });
      if (p) api.app.setState(LAST_LOCAL_KEY, p).catch(() => undefined);
    },
    [paneId]
  );
  const setRemotePath = useCallback(
    (p: string) => {
      setRemotePathState(p);
      const cur = useWorkspace.getState().panes[paneId];
      if (cur) useWorkspace.getState().updatePane(paneId, { files: { ...cur.files, remotePath: p } });
    },
    [paneId]
  );

  useEffect(() => {
    if (localPath !== null || !session) return;
    void (async () => {
      let start = session.ssh.localStartPath;
      if (!start) start = (await api.app.getState<string>(LAST_LOCAL_KEY).catch(() => null)) ?? '';
      if (!start) start = await localSource.home().catch(() => '');
      if (start) {
        const st = await api.local.stat(start).catch(() => null);
        if (!st) start = await localSource.home().catch(() => '');
      }
      setLocalPath(start);
    })();
  }, [localPath, session, setLocalPath]);

  useEffect(() => {
    if (!sftp || remotePath !== null) return;
    setRemotePath(session?.ssh.sftpStartPath || sftp.home);
  }, [sftp, remotePath, session, setRemotePath]);

  /* ---- local folder watching ---- */
  useEffect(() => {
    if (!localPath) return;
    let id: string | null = null;
    let alive = true;
    const off = api.local.onChanged((watchId) => {
      if (watchId === id) localRef.current?.refresh();
    });
    api.local
      .watch(localPath)
      .then((w) => {
        if (alive) id = w;
        else api.local.unwatch(w);
      })
      .catch(() => undefined);
    return () => {
      alive = false;
      off();
      if (id) api.local.unwatch(id);
    };
  }, [localPath]);

  /* ---- refresh the server side when transfers finish ---- */
  const finished = useFiles((s) => Object.values(s.transfers).filter((t) => t.sftpId === sftpId && isFinal(t)).length);
  useEffect(() => {
    if (!finished) return;
    const t = setTimeout(() => remoteRef.current?.refresh(), 250);
    return () => clearTimeout(t);
  }, [finished]);

  /* ---- bookmarks + split ratio ---- */
  useEffect(() => {
    api.app
      .getState<Bookmarks>(BOOKMARKS_KEY)
      .then((b) => b && setBookmarks({ local: b.local ?? [], remote: b.remote ?? {} }))
      .catch(() => undefined);
    api.app
      .getState<number>(SPLIT_KEY)
      .then((v) => typeof v === 'number' && v > 0.15 && v < 0.85 && setSplit(v))
      .catch(() => undefined);
  }, []);
  const toggleBookmark = (side: Side, path: string) => {
    setBookmarks((b) => {
      const list = side === 'local' ? b.local : (b.remote[serverKey] ?? []);
      const exists = list.some((c) => c.path === path);
      const label = side === 'local' ? local.baseName(path) || path : remote.baseName(path);
      const nextList = exists ? list.filter((c) => c.path !== path) : [...list, { label: `${label} — ${path}`, path }];
      const next = side === 'local' ? { ...b, local: nextList } : { ...b, remote: { ...b.remote, [serverKey]: nextList } };
      api.app.setState(BOOKMARKS_KEY, next).catch(() => undefined);
      toast.info(exists ? 'Bookmark removed' : 'Bookmark added', path || osTerms.computer);
      return next;
    });
  };

  /* ---- operations ---- */
  const transferOpts = (move: boolean): TransferOptions => ({
    conflict: fs.defaultConflict,
    move,
    preserveTimestamps: fs.preserveTimestamps,
    skipMacFiles: fs.skipMacFiles,
    deleteToRecycleBin: fs.deleteToRecycleBin
  });

  const upload = async (paths: string[], remoteDir: string, move = false) => {
    if (!sftpId) {
      toast.warning('Not connected', 'Connect to the server first.');
      return;
    }
    const opts = transferOpts(move);
    try {
      const ids = await api.sftp.upload(sftpId, remoteDir, paths, opts);
      useFiles.getState().rememberRetry(ids, { kind: 'upload', sftpId, sources: paths, targetDir: remoteDir, opts });
    } catch (e) {
      toast.error('Upload failed', errorText(e));
    }
  };

  const download = async (paths: string[], localDir: string, move = false) => {
    if (!sftpId) return;
    if (!localDir) {
      toast.warning('Choose a folder', 'Open a folder on the left before downloading.');
      return;
    }
    const opts = transferOpts(move);
    try {
      const ids = await api.sftp.download(sftpId, paths, localDir, opts);
      useFiles.getState().rememberRetry(ids, { kind: 'download', sftpId, sources: paths, targetDir: localDir, opts });
    } catch (e) {
      toast.error('Download failed', errorText(e));
    }
  };

  const transfer = (side: Side, items: FileItem[], move: boolean, targetDir?: string) => {
    const paths = items.filter((i) => i.type !== 'drive' && i.type !== 'place').map((i) => i.path);
    if (!paths.length) return;
    if (side === 'local') void upload(paths, targetDir ?? remotePath ?? '/', move);
    else void download(paths, targetDir ?? localPath ?? '', move);
  };

  const remove = async (side: Side, items: FileItem[]) => {
    const list = items.filter((i) => i.type !== 'drive' && i.type !== 'place');
    if (!list.length) return;
    if (fs.confirmDelete) {
      const dirs = list.some((i) => i.isDir);
      const recycle = side === 'local' && fs.deleteToRecycleBin;
      const ok = await confirmDialog({
        title: `Delete ${list.length === 1 ? `"${list[0].name}"` : `${list.length} items`}${side === 'remote' ? ' on the server' : ''}?`,
        message: recycle
          ? `They go to the ${osTerms.trash}.`
          : `${dirs ? 'Folders are deleted with everything inside them. ' : ''}This cannot be undone.`,
        confirmLabel: 'Delete',
        danger: !recycle
      });
      if (!ok) return;
    }
    try {
      if (side === 'local') {
        await api.local.remove(list.map((i) => i.path), fs.deleteToRecycleBin);
        localRef.current?.refresh();
      } else if (sftpId) {
        for (const i of list) await api.sftp.remove(sftpId, i.path, i.type === 'dir');
        remoteRef.current?.refresh();
      }
    } catch (e) {
      toast.error('Delete failed', errorText(e));
      (side === 'local' ? localRef : remoteRef).current?.refresh();
    }
  };

  const edit = async (path: string) => {
    if (!sftpId) return;
    try {
      await api.sftp.edit(sftpId, path, { editor: fs.editor });
    } catch (e) {
      toast.error('Cannot edit the file', errorText(e));
    }
  };

  const openFile = async (side: Side, item: FileItem) => {
    try {
      if (side === 'local') await api.local.open(item.path);
      else if (sftpId) await api.sftp.openRemote(sftpId, item.path);
    } catch (e) {
      toast.error('Cannot open the file', errorText(e));
    }
  };

  /** Drop target of an internal (native) drag, from window client pixels (Windows physical, macOS CSS). */
  const internalTarget = (x: number, y: number): { side: Side; dir: string; onFolder: boolean } | null => {
    const scale = isMac ? 1 : window.devicePixelRatio || 1;
    const el = document.elementFromPoint(x / scale, y / scale) as HTMLElement | null;
    const paneEl = el?.closest('[data-pane-side]') as HTMLElement | null;
    if (!el || !paneEl || !rootRef.current?.contains(paneEl)) return null;
    const side = paneEl.dataset.paneSide as Side;
    const row = el.closest('[data-path]') as HTMLElement | null;
    const handle = side === 'local' ? localRef.current : remoteRef.current;
    const item = row?.dataset.path ? handle?.items().find((i) => i.path === row.dataset.path) : undefined;
    if (item?.isDir && item.type !== 'drive' && item.type !== 'place') return { side, dir: item.path, onFolder: true };
    return { side, dir: paneEl.dataset.panePath ?? '', onFolder: false };
  };

  /** Items dropped back on the panes: to the other pane they are transferred, onto a folder of their own pane moved. */
  const dropBack = async (side: Side, items: FileItem[], target: { side: Side; dir: string; onFolder: boolean }, move: boolean) => {
    if (target.side !== side) {
      transfer(side, items, move, target.dir);
      return;
    }
    if (!target.onFolder || items.some((i) => i.path === target.dir)) return;
    if (side === 'local') {
      await api.local.copy(items.map((i) => i.path), target.dir, true);
      localRef.current?.refresh();
    } else {
      for (const i of items) await api.sftp.rename(sftpId!, i.path, remote.join(target.dir, i.name));
      remoteRef.current?.refresh();
    }
  };

  /** Local items: a native drag, to Explorer / Finder or back onto the panes. */
  const startLocalDrag = async (items: FileItem[]) => {
    if (!items.length) return;
    setDragging('local');
    try {
      const r = await api.local.startDrag(items.map((i) => i.path));
      if (r.result !== 'internal' || r.x === undefined || r.y === undefined) {
        if (r.result === 'external') localRef.current?.refresh();
        return;
      }
      const target = internalTarget(r.x, r.y);
      if (target) await dropBack('local', items, target, !!r.move);
    } catch (e) {
      toast.error('Drag and drop failed', errorText(e));
    } finally {
      setDragging(null);
    }
  };

  /** Server items: the page's own drag, which only the panes take. */
  const startServerDrag = (items: FileItem[]) => {
    if (!items.length || !sftpId) return;
    serverDrag.current = items;
    setDragging('remote');
  };
  const endServerDrag = () => {
    serverDrag.current = null;
    setDragging(null);
  };
  const dropServerItems = (side: Side, dir: string, onFolder: boolean, move: boolean) => {
    const items = serverDrag.current;
    endServerDrag();
    if (!items?.length || !sftpId) return;
    dropBack('remote', items, { side, dir, onFolder }, move).catch((e) => toast.error('Drag and drop failed', errorText(e)));
  };

  const switchPane = () => {
    const next: Side = activeSide === 'local' ? 'remote' : 'local';
    setActiveSide(next);
    (next === 'local' ? localRef : remoteRef).current?.focus();
  };

  const revealRemote = (path: string) => {
    const parent = remote.parent(path) ?? '/';
    setRemotePath(parent);
    setActiveSide('remote');
    setDialog(null);
    setTimeout(() => remoteRef.current?.focus(), 50);
  };

  const opsFor = (side: Side): PaneOps => ({
    openFile: (item) => void openFile(side, item),
    transfer: (items, move) => transfer(side, items, move),
    edit: side === 'remote' ? (item) => void edit(item.path) : undefined,
    remove: (items) => remove(side, items),
    chmod: side === 'remote' ? (item) => setDialog({ type: 'chmod', item }) : undefined,
    dropPaths: (paths, targetDir) => {
      if (side === 'remote') void upload(paths, targetDir);
      else void api.local.copy(paths, targetDir, false).then(() => localRef.current?.refresh(), (e) => toast.error('Copy failed', errorText(e)));
    },
    startDrag: (items) => (side === 'local' ? void startLocalDrag(items) : startServerDrag(items)),
    endDrag: endServerDrag,
    dropItems: (dir, onFolder, move) => dropServerItems(side, dir, onFolder, move),
    switchPane,
    find: side === 'remote' ? () => sftpId && setDialog({ type: 'find' }) : undefined
  });

  /* ---- conflicts / edits of this session ---- */
  const conflict = useFiles((s) => s.conflicts.find((c) => c.sftpId === sftpId));
  const edits = useFiles(useShallow((s) => Object.values(s.edits).filter((e) => e.sftpId === sftpId)));

  /* ---- connection actions ---- */
  const reconnect = () => {
    const ws = useWorkspace.getState();
    const p = ws.panes[paneId];
    if (!p) return;
    if (p.linkedPaneId) ws.updatePane(paneId, { linkedPaneId: undefined });
    ws.restartPane(paneId);
  };
  const cancelConnect = () => {
    for (const pr of usePrompts.getState().pending) if (pr.connId === conn.connId) usePrompts.getState().respond(pr.id, { cancelled: true });
    if (!pane?.linkedPaneId && conn.connId) api.conn.close(conn.connId);
  };
  const openTerminal = () => {
    if (!session) return;
    const saved = useSessions.getState().byId(pane?.sessionId ?? null);
    connectSession(saved ?? session, 'tab', undefined, { view: 'terminal' });
  };

  /* ---- splitter ---- */
  const onSplitDown = (e: ReactPointerEvent<HTMLDivElement>) => {
    if (e.button !== 0) return;
    e.preventDefault();
    const box = (e.currentTarget.parentElement as HTMLElement).getBoundingClientRect();
    const target = e.currentTarget;
    target.setPointerCapture(e.pointerId);
    let latest = split;
    const onMove = (ev: PointerEvent) => {
      latest = Math.min(0.8, Math.max(0.2, (ev.clientX - box.left) / box.width));
      setSplit(latest);
    };
    const onUp = (ev: PointerEvent) => {
      target.releasePointerCapture(ev.pointerId);
      target.removeEventListener('pointermove', onMove);
      target.removeEventListener('pointerup', onUp);
      api.app.setState(SPLIT_KEY, latest).catch(() => undefined);
    };
    target.addEventListener('pointermove', onMove);
    target.addEventListener('pointerup', onUp);
  };

  const remoteSrc = useMemo(() => (sftp ? remoteSource(sftp.sftpId, sftp.home) : null), [sftp]);
  if (!pane || !session) return null;

  const status = conn.status;
  const connecting = status === 'idle' || status === 'connecting' || status === 'authenticating';
  const ended = status === 'disconnected' || status === 'error';
  const title = `${session.username ? session.username + '@' : ''}${session.host}${session.port && session.port !== 22 ? ':' + session.port : ''}`;
  const protocolBadge = sftp ? (
    <span className={cx('fm-badge', `fm-badge--${sftp.protocol}`)} title={sftp.protocol === 'scp' ? 'This server has no SFTP: using SCP and shell commands' : 'SFTP'}>
      {sftp.protocol.toUpperCase()}
    </span>
  ) : null;

  const remotePlaceholder = sftpError ? (
    <div className="fm-placeholder fm-placeholder--error">
      <Plug size={18} />
      <span>{sftpError}</span>
    </div>
  ) : connecting ? (
    <div className="fm-placeholder">
      <Loader2 size={18} className="fm-spin" />
      <span>
        {status === 'authenticating' ? 'Authenticating with' : 'Connecting to'} <b>{host}</b>…
      </span>
      {!prompt && !pane.linkedPaneId && (
        <Button size="sm" variant="ghost" onClick={cancelConnect}>
          Cancel
        </Button>
      )}
    </div>
  ) : ended ? (
    <div className="fm-placeholder fm-placeholder--error">
      <Plug size={18} />
      <span>{conn.message || (status === 'error' ? 'The connection failed.' : 'The connection was closed.')}</span>
      <Button size="sm" variant="primary" icon={<RotateCcw size={13} />} onClick={reconnect}>
        Reconnect
      </Button>
    </div>
  ) : (
    <div className="fm-placeholder">
      <Loader2 size={18} className="fm-spin" />
      <span>Opening the file session…</span>
    </div>
  );

  return (
    <div ref={rootRef} className="fm" data-visible={visible || undefined}>
      <div className="fm-toolbar">
        <span className="fm-toolbar__title" title={title}>
          {title}
        </span>
        {protocolBadge}
        {pane.linkedPaneId && (
          <span className="fm-badge fm-badge--link" title="Uses the connection of the terminal it was opened from">
            <Link2 size={11} /> terminal
          </span>
        )}
        <span className="fm-sep" />
        <Button size="sm" variant="ghost" icon={<FolderSync size={14} />} disabled={!sftpId || !localPath} onClick={() => setDialog({ type: 'sync' })}>
          Synchronize
        </Button>
        <Button size="sm" variant="ghost" icon={<Search size={14} />} disabled={!sftpId} onClick={() => setDialog({ type: 'find' })} title={`Find files (${listShortcut('find')})`}>
          Find
        </Button>
        <Button
          size="sm"
          variant="ghost"
          icon={<FilePen size={14} />}
          disabled={!sftpId}
          title="Edit the selected server file (F4)"
          onClick={() => {
            const sel = remoteRef.current?.selected() ?? [];
            const file = sel.find((i) => !i.isDir);
            if (file) void edit(file.path);
            else toast.info('Select a file on the server', 'Then press Edit (F4).');
          }}
        >
          Edit
        </Button>
        {edits.length > 0 && (
          <Button
            size="sm"
            variant="ghost"
            className="fm-edits"
            onClick={(e) => {
              const r = (e.currentTarget as HTMLElement).getBoundingClientRect();
              const items: MenuItem[] = [
                { type: 'header', label: 'Uploaded after every save' },
                ...edits.map((ed) => ({
                  label: `${remote.baseName(ed.remotePath)}${ed.state === 'uploading' ? ' (uploading…)' : ed.state === 'error' ? ' (not saved)' : ''}`,
                  submenu: [
                    { label: 'Stop watching', onClick: () => api.sftp.stopEdit(ed.editId) },
                    { label: 'Show local copy', onClick: () => void api.local.reveal(ed.localPath) }
                  ]
                }))
              ];
              showContextMenu(r.left, r.bottom + 2, items);
            }}
          >
            {edits.length} editing
          </Button>
        )}
        <span className="fm-spacer" />
        <Select
          className="fm-conflict-select"
          value={fs.defaultConflict}
          options={CONFLICT_OPTIONS}
          onChange={(v) => void useSettings.getState().update({ files: { defaultConflict: v } })}
          aria-label="When a file already exists"
        />
        <IconButton
          size="sm"
          label={fs.showHidden ? 'Hide hidden files' : 'Show hidden files'}
          pressed={fs.showHidden}
          onClick={() => void useSettings.getState().update({ files: { showHidden: !fs.showHidden } })}
        >
          {fs.showHidden ? <Eye size={14} /> : <EyeOff size={14} />}
        </IconButton>
        <IconButton size="sm" label="Open a terminal to this server" onClick={openTerminal}>
          <SquareTerminal size={14} />
        </IconButton>
      </div>

      <div className="fm-panes" style={{ gridTemplateColumns: `${split}fr 6px ${1 - split}fr` }}>
        <FilePane
          ref={localRef}
          side="local"
          source={localPath === null ? null : localSource}
          title={osTerms.computer}
          path={localPath ?? ''}
          onNavigate={setLocalPath}
          active={activeSide === 'local'}
          onActivate={() => setActiveSide('local')}
          showHidden={fs.showHidden}
          bookmarks={bookmarks.local}
          onToggleBookmark={(p) => toggleBookmark('local', p)}
          ops={opsFor('local')}
          placeholder={
            <div className="fm-placeholder">
              <Loader2 size={18} className="fm-spin" />
            </div>
          }
          dragFrom={dragging}
        />
        <div className="fm-splitter" role="separator" aria-orientation="vertical" onPointerDown={onSplitDown} onDoubleClick={() => setSplit(0.5)} />
        <FilePane
          ref={remoteRef}
          side="remote"
          source={remotePath === null ? null : remoteSrc}
          title="Server"
          path={remotePath ?? '/'}
          onNavigate={setRemotePath}
          active={activeSide === 'remote'}
          onActivate={() => setActiveSide('remote')}
          showHidden={fs.showHidden}
          bookmarks={bookmarks.remote[serverKey] ?? []}
          onToggleBookmark={(p) => toggleBookmark('remote', p)}
          ops={opsFor('remote')}
          placeholder={remotePlaceholder}
          dragFrom={dragging}
        />
      </div>

      <TransferQueue sftpId={sftpId} />

      {prompt && (
        <div className="fm-prompt">
          <PromptOverlay prompt={prompt} host={host} />
        </div>
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
      {visible && dialog?.type === 'sync' && sftpId && localPath !== null && (
        <SyncDialog
          sftpId={sftpId}
          localDir={localPath}
          remoteDir={remotePath ?? '/'}
          deleteToRecycleBin={fs.deleteToRecycleBin}
          onClose={() => setDialog(null)}
        />
      )}
      {visible && dialog?.type === 'find' && sftpId && (
        <FindDialog
          sftpId={sftpId}
          root={remotePath ?? '/'}
          onClose={() => setDialog(null)}
          onReveal={revealRemote}
          onDownload={(paths) => void download(paths, localPath ?? '')}
          onEdit={(p) => void edit(p)}
        />
      )}
      {visible && dialog?.type === 'chmod' && sftpId && (
        <ChmodDialog
          item={dialog.item}
          onClose={() => setDialog(null)}
          onApply={async (mode) => {
            try {
              await api.sftp.chmod(sftpId, dialog.item.path, mode);
              setDialog(null);
              remoteRef.current?.refresh();
            } catch (e) {
              toast.error('Could not change permissions', errorText(e));
            }
          }}
        />
      )}
      {ended && pane.linkedPaneId && !sftpError && (
        <div className="fm-banner">
          <Plug size={14} />
          <span>{conn.message || 'The terminal connection ended.'}</span>
          <Button size="sm" variant="primary" icon={<RotateCcw size={13} />} onClick={reconnect}>
            Reconnect
          </Button>
          <IconButton size="sm" label="Close" onClick={() => useWorkspace.getState().closePane(paneId)}>
            <X size={13} />
          </IconButton>
        </div>
      )}
    </div>
  );
}
