/**
 * Sessions sidebar: search (filters name/host/user/folder/notes with highlighting), folder tree with persisted
 * expansion, drag & drop moves, keyboard navigation, context menus, footer actions and a resize handle
 * (width persisted in settings.appearance.sidebarWidth).
 */
import { useCallback, useEffect, useMemo, useRef, useState, type DragEvent, type KeyboardEvent as ReactKeyboardEvent, type MouseEvent as ReactMouseEvent } from 'react';
import {
  ChevronRight,
  ChevronsDownUp,
  Columns2,
  Copy,
  Download,
  FileJson,
  Folder,
  FolderInput,
  FolderOpen,
  HardDriveDownload,
  SquareTerminal,
  FolderPlus,
  Pencil,
  Play,
  Plus,
  Rows2,
  Search,
  Settings,
  SlidersHorizontal,
  SquarePlus,
  Trash2,
  X
} from 'lucide-react';
import { api } from '@/bridge/api';
import { Button, IconButton, cx, showContextMenu, type MenuItem } from '@/components/ui';
import {
  connectSession,
  createFolder,
  deleteFolder,
  deleteSessions,
  duplicateSession,
  editSession,
  exportSessions,
  moveFolder,
  moveSessions,
  newSessionDraft,
  renameFolder,
  renameSession
} from '@/commands/actions';
import { runCommand } from '@/commands/commands';
import { useSessions, useSettings } from '@/store/data';
import { useUi } from '@/store/ui';
import { useWorkspace } from '@/store/workspace';
import { sessionLabel } from '@shared/defaults';
import type { SessionConfig } from '@shared/types';
import { Highlight, ProtocolIcon, StatusDot, sessionSubtitle } from '@/components/layout/sessionVisuals';
import { SESSION_MIME } from '@/components/titlebar/TabStrip';
import { buildTree, flattenTree, folderKey, isWithin, parentFolder, searchTokens, sessionKey, type Row } from './tree';
import './sidebar.css';

const FOLDER_MIME = 'application/x-sshterm-folder';
const COLLAPSED_KEY = 'sidebar.collapsed';
const MIN_WIDTH = 200;
const MAX_WIDTH = 560;

/* -------------------------------------------------------------------------- */
/* Menus                                                                       */
/* -------------------------------------------------------------------------- */

function moveToFolderItems(onPick: (folder: string) => void, exclude?: (f: string) => boolean): MenuItem[] {
  const folders = useSessions.getState().folders.filter((f) => !exclude?.(f));
  const LIMIT = 40;
  return [
    { label: '(Top level)', icon: <FolderInput size={14} />, onClick: () => onPick('') },
    ...(folders.length ? [{ type: 'separator' } as MenuItem] : []),
    ...folders.slice(0, LIMIT).map((f): MenuItem => ({ label: f.split('/').join(' / '), icon: <Folder size={14} />, onClick: () => onPick(f) })),
    { type: 'separator' },
    {
      label: 'New folder…',
      icon: <FolderPlus size={14} />,
      onClick: () =>
        void createFolder().then((p) => {
          if (p !== null) onPick(p);
        })
    }
  ];
}

function sessionMenu(s: SessionConfig): MenuItem[] {
  const ssh = s.protocol === 'ssh';
  const files = ssh && s.openAs === 'files';
  return [
    { label: 'Connect', icon: <Play size={14} />, onClick: () => connectSession(s, 'tab') },
    ...(ssh
      ? [
          files
            ? { label: 'Open terminal', icon: <SquareTerminal size={14} />, onClick: () => connectSession(s, 'tab', undefined, { view: 'terminal' }) }
            : { label: 'Open file manager', icon: <HardDriveDownload size={14} />, onClick: () => connectSession(s, 'tab', undefined, { view: 'files' }) }
        ]
      : []),
    { label: 'Connect in split right', icon: <Columns2 size={14} />, disabled: files, onClick: () => connectSession(s, 'split-right', undefined, { view: 'terminal' }) },
    { label: 'Connect in split down', icon: <Rows2 size={14} />, disabled: files, onClick: () => connectSession(s, 'split-down', undefined, { view: 'terminal' }) },
    { type: 'separator' },
    { label: 'Edit…', icon: <SlidersHorizontal size={14} />, onClick: () => editSession(s.id) },
    { label: 'Duplicate', icon: <Copy size={14} />, onClick: () => void duplicateSession(s) },
    { label: 'Rename…', icon: <Pencil size={14} />, shortcut: 'F2', onClick: () => void renameSession(s) },
    {
      label: 'Move to folder',
      icon: <FolderInput size={14} />,
      submenu: moveToFolderItems((f) => void moveSessions([s], f), (f) => f === s.folder)
    },
    { type: 'separator' },
    { label: 'Export to PuTTY .reg…', icon: <Download size={14} />, onClick: () => void exportSessions([s.id], 'reg') },
    { label: 'Export JSON…', icon: <FileJson size={14} />, onClick: () => void exportSessions([s.id], 'json') },
    { type: 'separator' },
    { label: 'Delete', icon: <Trash2 size={14} />, shortcut: 'Del', danger: true, onClick: () => void deleteSessions([s]) }
  ];
}

function folderMenu(path: string): MenuItem[] {
  const inside = useSessions.getState().sessions.filter((s) => isWithin(s.folder, path));
  return [
    { label: 'New session here…', icon: <SquarePlus size={14} />, onClick: () => newSessionDraft(path) },
    { label: 'New folder…', icon: <FolderPlus size={14} />, onClick: () => void createFolder(path) },
    { type: 'separator' },
    { label: 'Rename…', icon: <Pencil size={14} />, shortcut: 'F2', onClick: () => void renameFolder(path) },
    {
      label: 'Move to',
      icon: <FolderInput size={14} />,
      submenu: moveToFolderItems((f) => void moveFolder(path, f), (f) => isWithin(f, path) || f === parentFolder(path))
    },
    { type: 'separator' },
    {
      label: `Export ${inside.length} session${inside.length === 1 ? '' : 's'} to PuTTY .reg…`,
      icon: <Download size={14} />,
      disabled: !inside.length,
      onClick: () => void exportSessions(inside.map((s) => s.id), 'reg')
    },
    {
      label: 'Export JSON…',
      icon: <FileJson size={14} />,
      disabled: !inside.length,
      onClick: () => void exportSessions(inside.map((s) => s.id), 'json')
    },
    { type: 'separator' },
    { label: 'Delete…', icon: <Trash2 size={14} />, shortcut: 'Del', danger: true, onClick: () => void deleteFolder(path) }
  ];
}

function rootMenu(): MenuItem[] {
  return [
    { label: 'New session…', icon: <SquarePlus size={14} />, onClick: () => newSessionDraft() },
    { label: 'New folder…', icon: <FolderPlus size={14} />, onClick: () => void createFolder() },
    { type: 'separator' },
    { label: 'Import…', icon: <Download size={14} />, onClick: () => runCommand('app.importPutty') }
  ];
}

/* -------------------------------------------------------------------------- */
/* Hooks                                                                       */
/* -------------------------------------------------------------------------- */

function useCollapsedFolders(): [Set<string>, (path: string, collapsed: boolean) => void, (all: string[]) => void] {
  const [collapsed, setCollapsed] = useState<Set<string>>(() => new Set());
  const loaded = useRef(false);
  useEffect(() => {
    api.app
      .getState<string[]>(COLLAPSED_KEY)
      .then((v) => {
        if (Array.isArray(v)) setCollapsed(new Set(v.filter((x) => typeof x === 'string')));
      })
      .catch(() => undefined)
      .finally(() => (loaded.current = true));
  }, []);
  const persist = (next: Set<string>) => {
    if (loaded.current) api.app.setState(COLLAPSED_KEY, [...next]).catch(() => undefined);
  };
  const set = useCallback((path: string, value: boolean) => {
    setCollapsed((prev) => {
      if (prev.has(path) === value) return prev;
      const next = new Set(prev);
      if (value) next.add(path);
      else next.delete(path);
      persist(next);
      return next;
    });
  }, []);
  const setAll = useCallback((all: string[]) => {
    const next = new Set(all);
    setCollapsed(next);
    persist(next);
  }, []);
  return [collapsed, set, setAll];
}

/** Live status of sessions that are open somewhere (best status wins). */
function useOpenSessionStatus(): Map<string, string> {
  const panes = useWorkspace((s) => s.panes);
  return useMemo(() => {
    const rank: Record<string, number> = { connected: 5, authenticating: 4, connecting: 3, error: 2, disconnected: 1, idle: 0 };
    const m = new Map<string, string>();
    for (const p of Object.values(panes)) {
      if (!p.sessionId) continue;
      const cur = m.get(p.sessionId);
      if (!cur || rank[p.status] > rank[cur]) m.set(p.sessionId, p.status);
    }
    return m;
  }, [panes]);
}

/* -------------------------------------------------------------------------- */
/* Sidebar                                                                     */
/* -------------------------------------------------------------------------- */

export function Sidebar() {
  const sessions = useSessions((s) => s.sessions);
  const folders = useSessions((s) => s.folders);
  const loaded = useSessions((s) => s.loaded);
  const filter = useUi((s) => s.sidebarFilter);
  const setFilter = useUi((s) => s.setSidebarFilter);
  const width = useSettings((s) => s.settings.appearance.sidebarWidth);
  const doubleClick = useSettings((s) => s.settings.behavior.sessionDoubleClick);
  const [collapsed, setCollapsed, setAllCollapsed] = useCollapsedFolders();
  const [selected, setSelected] = useState<string | null>(null);
  const [dropTarget, setDropTarget] = useState<string | null>(null);
  const [liveWidth, setLiveWidth] = useState<number | null>(null);
  const treeRef = useRef<HTMLDivElement>(null);
  const searchRef = useRef<HTMLInputElement>(null);
  const openStatus = useOpenSessionStatus();

  const tokens = useMemo(() => searchTokens(filter), [filter]);
  const tree = useMemo(() => buildTree(sessions, folders, tokens), [sessions, folders, tokens]);
  const rows = useMemo(() => flattenTree(tree, collapsed, tokens.length > 0), [tree, collapsed, tokens]);

  // Keep the selection valid as rows change.
  useEffect(() => {
    if (selected && !rows.some((r) => r.key === selected)) setSelected(null);
  }, [rows, selected]);

  useEffect(() => {
    if (!selected) return;
    treeRef.current?.querySelector(`[data-key="${CSS.escape(selected)}"]`)?.scrollIntoView({ block: 'nearest' });
  }, [selected]);

  const connectDefault = (s: SessionConfig) => {
    const hasTab = useWorkspace.getState().tabs.length > 0;
    connectSession(s, doubleClick === 'split' && hasTab ? 'split-right' : 'tab');
  };

  /* ---- drag & drop ---- */
  const onDragStartSession = (e: DragEvent, s: SessionConfig) => {
    e.dataTransfer.setData(SESSION_MIME, s.id);
    e.dataTransfer.setData('text/plain', sessionLabel(s));
    e.dataTransfer.effectAllowed = 'copyMove';
  };
  const onDragStartFolder = (e: DragEvent, path: string) => {
    e.dataTransfer.setData(FOLDER_MIME, path);
    e.dataTransfer.setData('text/plain', path);
    e.dataTransfer.effectAllowed = 'move';
  };
  const canDrop = (e: DragEvent) => e.dataTransfer.types.includes(SESSION_MIME) || e.dataTransfer.types.includes(FOLDER_MIME);
  const onDragOverTarget = (e: DragEvent, folder: string) => {
    if (!canDrop(e)) return;
    e.preventDefault();
    e.stopPropagation();
    e.dataTransfer.dropEffect = 'move';
    const key = folderKey(folder);
    if (dropTarget !== key) setDropTarget(key);
  };
  const onDropTarget = (e: DragEvent, folder: string) => {
    e.preventDefault();
    e.stopPropagation();
    setDropTarget(null);
    const sid = e.dataTransfer.getData(SESSION_MIME);
    const fpath = e.dataTransfer.getData(FOLDER_MIME);
    if (sid) {
      const s = useSessions.getState().byId(sid);
      if (s) void moveSessions([s], folder);
    } else if (fpath && !isWithin(folder, fpath)) {
      void moveFolder(fpath, folder);
      if (folder) setCollapsed(folder, false);
    }
  };

  /* ---- keyboard ---- */
  const onTreeKeyDown = (e: ReactKeyboardEvent) => {
    if (!rows.length) return;
    const i = selected ? rows.findIndex((r) => r.key === selected) : -1;
    const row = i >= 0 ? rows[i] : undefined;
    const select = (idx: number) => setSelected(rows[Math.max(0, Math.min(rows.length - 1, idx))].key);
    switch (e.key) {
      case 'ArrowDown':
        e.preventDefault();
        select(i + 1);
        break;
      case 'ArrowUp':
        e.preventDefault();
        if (i <= 0) searchRef.current?.focus();
        else select(i - 1);
        break;
      case 'Home':
        e.preventDefault();
        select(0);
        break;
      case 'End':
        e.preventDefault();
        select(rows.length - 1);
        break;
      case 'ArrowRight':
        e.preventDefault();
        if (row?.kind === 'folder') {
          if (!row.expanded) setCollapsed(row.folder.path, false);
          else select(i + 1);
        }
        break;
      case 'ArrowLeft': {
        e.preventDefault();
        if (row?.kind === 'folder' && row.expanded && !tokens.length) setCollapsed(row.folder.path, true);
        else if (row) {
          const parent = row.kind === 'folder' ? parentFolder(row.folder.path) : row.session.folder;
          if (parent) setSelected(folderKey(parent));
        }
        break;
      }
      case 'Enter':
        e.preventDefault();
        if (row?.kind === 'session') connectSession(row.session, e.altKey ? 'split-right' : 'tab');
        else if (row?.kind === 'folder' && !tokens.length) setCollapsed(row.folder.path, row.expanded);
        break;
      case 'F2':
        e.preventDefault();
        if (row?.kind === 'session') void renameSession(row.session);
        else if (row?.kind === 'folder') void renameFolder(row.folder.path);
        break;
      case 'Delete':
        e.preventDefault();
        if (row?.kind === 'session') void deleteSessions([row.session]);
        else if (row?.kind === 'folder') void deleteFolder(row.folder.path);
        break;
      case 'ContextMenu':
      case 'F10': {
        if (e.key === 'F10' && !e.shiftKey) return;
        e.preventDefault();
        const el = treeRef.current?.querySelector(`[data-key="${CSS.escape(selected ?? '')}"]`);
        const r = el?.getBoundingClientRect();
        if (row && r) showContextMenu(r.left + 24, r.bottom, row.kind === 'session' ? sessionMenu(row.session) : folderMenu(row.folder.path));
        break;
      }
      default:
        // Type-to-search: printable characters jump to the search box.
        if (e.key.length === 1 && !e.ctrlKey && !e.altKey && !e.metaKey) {
          searchRef.current?.focus();
        }
    }
  };

  /* ---- resize ---- */
  const onResizeStart = (e: ReactMouseEvent) => {
    if (e.button !== 0) return;
    e.preventDefault();
    const startX = e.clientX;
    const startW = width;
    let w = startW;
    const onMove = (ev: MouseEvent) => {
      w = Math.round(Math.min(MAX_WIDTH, Math.max(MIN_WIDTH, startW + ev.clientX - startX)));
      setLiveWidth(w);
    };
    const onUp = () => {
      window.removeEventListener('mousemove', onMove);
      window.removeEventListener('mouseup', onUp);
      document.body.classList.remove('is-resizing-col');
      void useSettings
        .getState()
        .update({ appearance: { sidebarWidth: w } })
        .finally(() => setLiveWidth(null));
    };
    document.body.classList.add('is-resizing-col');
    window.addEventListener('mousemove', onMove);
    window.addEventListener('mouseup', onUp);
  };

  const effectiveWidth = Math.min(MAX_WIDTH, Math.max(MIN_WIDTH, liveWidth ?? width ?? 260));
  const empty = loaded && sessions.length === 0 && folders.length === 0;

  return (
    <aside className="sidebar" style={{ width: effectiveWidth }} aria-label="Saved sessions">
      <div className="sidebar__header">
        <span className="sidebar__title">Sessions</span>
        <div className="sidebar__header-actions">
          <IconButton size="sm" label="New folder" onClick={() => void createFolder()}>
            <FolderPlus size={15} />
          </IconButton>
          <IconButton size="sm" label="Collapse all folders" onClick={() => setAllCollapsed(folders)} disabled={!folders.length}>
            <ChevronsDownUp size={15} />
          </IconButton>
          <IconButton size="sm" label="New session" onClick={() => newSessionDraft()}>
            <Plus size={16} />
          </IconButton>
        </div>
      </div>

      <div className="sidebar__search">
        <Search size={14} className="sidebar__search-icon" />
        <input
          ref={searchRef}
          className="sidebar__search-input"
          value={filter}
          placeholder="Search sessions"
          aria-label="Search sessions"
          spellCheck={false}
          onChange={(e) => setFilter(e.target.value)}
          onKeyDown={(e) => {
            if (e.key === 'Escape') {
              if (filter) {
                e.stopPropagation();
                setFilter('');
              } else searchRef.current?.blur();
            } else if (e.key === 'ArrowDown' || (e.key === 'Enter' && rows.length)) {
              e.preventDefault();
              const first = rows.find((r) => r.kind === 'session') ?? rows[0];
              if (e.key === 'Enter' && tokens.length && first?.kind === 'session') {
                connectSession(first.session, 'tab');
                return;
              }
              if (first) setSelected(first.key);
              treeRef.current?.focus();
            }
          }}
        />
        {filter && (
          <button className="sidebar__search-clear" aria-label="Clear search" title="Clear" onClick={() => setFilter('')}>
            <X size={13} />
          </button>
        )}
      </div>

      <div
        ref={treeRef}
        className={cx('sidebar__tree', dropTarget === folderKey('') && 'sidebar__tree--drop')}
        role="tree"
        aria-label="Sessions"
        tabIndex={0}
        aria-activedescendant={selected ? `row-${selected}` : undefined}
        onKeyDown={onTreeKeyDown}
        onFocus={() => {
          if (!selected && rows.length) setSelected(rows[0].key);
        }}
        onContextMenu={(e) => {
          if ((e.target as HTMLElement).closest('.tree-row')) return;
          e.preventDefault();
          showContextMenu(e.clientX, e.clientY, rootMenu());
        }}
        onDragOver={(e) => onDragOverTarget(e, '')}
        onDragLeave={(e) => {
          if (!e.currentTarget.contains(e.relatedTarget as Node)) setDropTarget(null);
        }}
        onDrop={(e) => onDropTarget(e, '')}
      >
        {rows.map((row) => (
          <TreeRow
            key={row.key}
            row={row}
            tokens={tokens}
            selected={selected === row.key}
            dropTarget={dropTarget}
            liveStatus={row.kind === 'session' ? openStatus.get(row.session.id) : undefined}
            onSelect={() => setSelected(row.key)}
            onToggle={(path, expanded) => setCollapsed(path, expanded)}
            onConnect={connectDefault}
            onDragStartSession={onDragStartSession}
            onDragStartFolder={onDragStartFolder}
            onDragOverTarget={onDragOverTarget}
            onDropTarget={onDropTarget}
            onDragEnd={() => setDropTarget(null)}
          />
        ))}

        {empty && (
          <div className="sidebar__empty">
            <div className="sidebar__empty-icon">
              <FolderOpen size={28} strokeWidth={1.5} />
            </div>
            <div className="sidebar__empty-title">No saved sessions</div>
            <div className="sidebar__empty-text">Bring your PuTTY sessions over in one step, or create a new one.</div>
            <Button variant="primary" size="sm" icon={<Download size={14} />} onClick={() => runCommand('app.importPutty')}>
              Import from PuTTY
            </Button>
            <Button size="sm" icon={<SquarePlus size={14} />} onClick={() => newSessionDraft()}>
              New session
            </Button>
          </div>
        )}
        {!empty && tokens.length > 0 && rows.length === 0 && (
          <div className="sidebar__empty sidebar__empty--search">
            <Search size={20} strokeWidth={1.5} />
            <div className="sidebar__empty-text">No sessions match “{filter}”.</div>
            <Button size="sm" variant="ghost" onClick={() => useUi.getState().openDialog({ type: 'quickConnect', initial: filter.trim() })}>
              Quick connect to “{filter.trim()}”
            </Button>
          </div>
        )}
      </div>

      <div className="sidebar__footer">
        <button className="sidebar__footer-btn" onClick={() => newSessionDraft()} title="New session">
          <SquarePlus size={15} />
          <span>New</span>
        </button>
        <button className="sidebar__footer-btn" onClick={() => runCommand('app.importPutty')} title="Import sessions">
          <Download size={15} />
          <span>Import</span>
        </button>
        <button className="sidebar__footer-btn" onClick={() => runCommand('app.settings')} title="Settings">
          <Settings size={15} />
          <span>Settings</span>
        </button>
      </div>

      <div className="sidebar__resizer" onMouseDown={onResizeStart} role="separator" aria-orientation="vertical" aria-label="Resize sidebar" />
    </aside>
  );
}

/* -------------------------------------------------------------------------- */
/* Row                                                                         */
/* -------------------------------------------------------------------------- */

interface TreeRowProps {
  row: Row;
  tokens: string[];
  selected: boolean;
  dropTarget: string | null;
  liveStatus?: string;
  onSelect: () => void;
  onToggle: (path: string, collapse: boolean) => void;
  onConnect: (s: SessionConfig) => void;
  onDragStartSession: (e: DragEvent, s: SessionConfig) => void;
  onDragStartFolder: (e: DragEvent, path: string) => void;
  onDragOverTarget: (e: DragEvent, folder: string) => void;
  onDropTarget: (e: DragEvent, folder: string) => void;
  onDragEnd: () => void;
}

function TreeRow(p: TreeRowProps) {
  const { row, tokens, selected } = p;
  const indent = { paddingLeft: 8 + row.depth * 14 };

  if (row.kind === 'folder') {
    const f = row.folder;
    return (
      <div
        id={`row-${row.key}`}
        data-key={row.key}
        role="treeitem"
        aria-expanded={row.expanded}
        aria-selected={selected}
        aria-level={row.depth + 1}
        className={cx('tree-row tree-row--folder', selected && 'tree-row--selected', p.dropTarget === row.key && 'tree-row--drop')}
        style={indent}
        draggable
        onMouseDown={p.onSelect}
        onClick={() => !tokens.length && p.onToggle(f.path, row.expanded)}
        onContextMenu={(e) => {
          e.preventDefault();
          p.onSelect();
          showContextMenu(e.clientX, e.clientY, folderMenu(f.path));
        }}
        onDragStart={(e) => p.onDragStartFolder(e, f.path)}
        onDragOver={(e) => p.onDragOverTarget(e, f.path)}
        onDrop={(e) => p.onDropTarget(e, f.path)}
        onDragEnd={p.onDragEnd}
        title={f.path}
      >
        <ChevronRight size={14} className={cx('tree-row__chevron', row.expanded && 'tree-row__chevron--open')} />
        {row.expanded ? <FolderOpen size={15} className="tree-row__folder-icon" /> : <Folder size={15} className="tree-row__folder-icon" />}
        <span className="tree-row__name">
          <Highlight text={f.name} tokens={tokens} />
        </span>
        <span className="tree-row__count">{f.count}</span>
      </div>
    );
  }

  const s = row.session;
  const label = sessionLabel(s);
  const subtitle = sessionSubtitle(s);
  return (
    <div
      id={`row-${row.key}`}
      data-key={row.key}
      role="treeitem"
      aria-selected={selected}
      aria-level={row.depth + 1}
      className={cx('tree-row tree-row--session', selected && 'tree-row--selected')}
      style={{ paddingLeft: 8 + row.depth * 14 + 18 }}
      draggable
      onMouseDown={p.onSelect}
      onDoubleClick={() => p.onConnect(s)}
      onContextMenu={(e) => {
        e.preventDefault();
        p.onSelect();
        showContextMenu(e.clientX, e.clientY, sessionMenu(s));
      }}
      onDragStart={(e) => p.onDragStartSession(e, s)}
      onDragOver={(e) => p.onDragOverTarget(e, s.folder)}
      onDrop={(e) => p.onDropTarget(e, s.folder)}
      onDragEnd={p.onDragEnd}
      title={[label, subtitle, s.notes].filter(Boolean).join('\n')}
    >
      <span className="tree-row__proto" style={s.color ? { color: s.color } : undefined}>
        <ProtocolIcon protocol={s.protocol} size={14} />
      </span>
      <span className="tree-row__name">
        <Highlight text={label} tokens={tokens} />
      </span>
      {subtitle !== label && (
        <span className="tree-row__sub">
          <Highlight text={subtitle} tokens={tokens} />
        </span>
      )}
      {p.liveStatus && <StatusDot status={p.liveStatus as never} className="tree-row__live" />}
      {s.color && <span className="tree-row__color" style={{ background: s.color }} aria-hidden />}
    </div>
  );
}

export { sessionKey };
