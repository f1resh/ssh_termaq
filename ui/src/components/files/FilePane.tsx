/**
 * One side of the file manager (local disks or the server): navigation (breadcrumbs / typed path, back / forward /
 * up / home, bookmarks), a sortable multi-select list with type-ahead, quick filter, inline rename and context menu,
 * drag sources (native drag through the host) and Explorer drop targets. File operations that involve the other
 * side are delegated to the FileManager through `ops`.
 */
import {
  forwardRef,
  useCallback,
  useEffect,
  useImperativeHandle,
  useLayoutEffect,
  useMemo,
  useRef,
  useState,
  type DragEvent,
  type KeyboardEvent as ReactKeyboardEvent,
  type ReactNode
} from 'react';
import {
  ArrowLeft,
  ArrowRight,
  ArrowUp,
  Disc,
  Download,
  File as FileIcon,
  FileSymlink,
  FileText,
  Folder,
  FolderPlus,
  HardDrive,
  Home,
  Loader2,
  Monitor,
  Network,
  RefreshCw,
  Search,
  Star,
  Usb,
  User,
  X
} from 'lucide-react';
import { api } from '@/bridge/api';
import { IconButton, cx, showContextMenu, toast, type MenuItem } from '@/components/ui';
import { formatChord } from '@/commands/chords';
import { inputDialog } from '@/store/ui';
import { isMac, osTerms } from '@shared/platform';
import {
  fmtDate,
  fmtSize,
  historyBack,
  historyForward,
  historyVisit,
  matchesFilter,
  nameError,
  nextSelection,
  pathsOf,
  sortItems,
  type Crumb,
  type FileItem,
  type NavHistory,
  type Side,
  type SortKey
} from './paths';
import { listAction, listShortcut, ownKeys } from './listKeys';
import type { PaneSource } from './sources';

/** The data type of the page's own drag of server items (between the panes). */
const PAGE_DRAG = 'application/x-sshtermaq-items';

export interface PaneOps {
  /** Opens a file (double click / Enter on a file). */
  openFile(item: FileItem): void;
  /** F5 / F6: to the other pane's folder. */
  transfer(items: FileItem[], move: boolean): void;
  /** F4 (remote): edit with auto-upload. */
  edit?(item: FileItem): void;
  remove(items: FileItem[]): Promise<void>;
  chmod?(item: FileItem): void;
  /** Files dropped from Explorer / Finder onto this pane (targetDir = folder under the cursor or the current folder). */
  dropPaths(paths: string[], targetDir: string): void;
  /** A drag of the selected items started: local items as a native drag (Explorer / Finder take it too), server items
   *  as the page's own drag, which only the panes take (dropItems). */
  startDrag(items: FileItem[]): void;
  /** The page's own drag (server items) ended, dropped or not. */
  endDrag(): void;
  /** The page's own drag dropped on this pane: on `dir` (onFolder: a folder row); move: Shift (macOS: or Command). */
  dropItems(dir: string, onFolder: boolean, move: boolean): void;
  /** Extra context menu entries for a selection. */
  menuExtras?(items: FileItem[]): MenuItem[];
  switchPane(): void;
  find?(): void;
}

export interface FilePaneHandle {
  refresh(): void;
  focus(): void;
  selected(): FileItem[];
  path(): string;
  items(): FileItem[];
}

export interface FilePaneProps {
  side: Side;
  source: PaneSource | null;
  title: string;
  badge?: ReactNode;
  path: string;
  onNavigate(path: string): void;
  active: boolean;
  onActivate(): void;
  showHidden: boolean;
  bookmarks: Crumb[];
  onToggleBookmark(path: string): void;
  ops: PaneOps;
  /** Shown instead of the list when there is no source (e.g. not connected yet). */
  placeholder?: ReactNode;
  /** The pane a drag of ours started from (the other pane is highlighted as its target), null for none. */
  dragFrom?: Side | null;
}

const errorText = (e: unknown) => (e instanceof Error ? e.message : String(e));

function ItemIcon({ item }: { item: FileItem }) {
  const size = 15;
  if (item.type === 'drive') {
    if (item.icon === 'removable') return <Usb size={size} className="fm-icon fm-icon--drive" />;
    if (item.icon === 'network') return <Network size={size} className="fm-icon fm-icon--drive" />;
    if (item.icon === 'cdrom') return <Disc size={size} className="fm-icon fm-icon--drive" />;
    return <HardDrive size={size} className="fm-icon fm-icon--drive" />;
  }
  if (item.type === 'place') {
    if (item.icon === 'desktop') return <Monitor size={size} className="fm-icon fm-icon--place" />;
    if (item.icon === 'documents') return <FileText size={size} className="fm-icon fm-icon--place" />;
    if (item.icon === 'downloads') return <Download size={size} className="fm-icon fm-icon--place" />;
    return <User size={size} className="fm-icon fm-icon--place" />;
  }
  if (item.link && !item.isDir) return <FileSymlink size={size} className="fm-icon" />;
  if (item.isDir) return <Folder size={size} className={cx('fm-icon fm-icon--dir', item.link && 'fm-icon--link')} />;
  return <FileIcon size={size} className="fm-icon" />;
}

export const FilePane = forwardRef<FilePaneHandle, FilePaneProps>(function FilePane(props, ref) {
  const { side, source, title, badge, path, onNavigate, active, onActivate, showHidden, bookmarks, onToggleBookmark, ops, placeholder, dragFrom } =
    props;
  const P = pathsOf(side);
  const [entries, setEntries] = useState<FileItem[]>([]);
  const [shownPath, setShownPath] = useState<string | null>(null);
  const [loading, setLoading] = useState(false);
  const [error, setError] = useState('');
  const [selected, setSelected] = useState<Set<string>>(new Set());
  const [anchor, setAnchor] = useState<string | null>(null);
  const [cursor, setCursor] = useState<string | null>(null);
  const [sort, setSort] = useState<{ key: SortKey; asc: boolean }>({ key: 'name', asc: true });
  const [filter, setFilter] = useState('');
  const [filterOpen, setFilterOpen] = useState(false);
  const [editingPath, setEditingPath] = useState(false);
  const [pathText, setPathText] = useState(path);
  const [renaming, setRenaming] = useState<string | null>(null);
  const [renameText, setRenameText] = useState('');
  const [dragOver, setDragOver] = useState<string | null>(null);
  const [reloadTick, setReloadTick] = useState(0);
  const [history, setHistory] = useState<NavHistory>({ back: [], forward: [] });
  /** The next folder change comes from Back / Forward (already recorded). */
  const historyNav = useRef(false);
  const listRef = useRef<HTMLDivElement>(null);
  const crumbsRef = useRef<HTMLDivElement>(null);
  const typeAhead = useRef<{ text: string; at: number }>({ text: '', at: 0 });
  const loadSeq = useRef(0);
  const prevPath = useRef(path);

  /* ---- loading ---- */
  useEffect(() => {
    if (!source) {
      setEntries([]);
      setShownPath(null);
      return;
    }
    const seq = ++loadSeq.current;
    setLoading(true);
    setError('');
    source
      .list(path)
      .then((list) => {
        if (seq !== loadSeq.current) return;
        setEntries(list);
        setShownPath(path);
        setSelected((cur) => new Set([...cur].filter((p) => list.some((e) => e.path === p))));
      })
      .catch((e) => {
        if (seq !== loadSeq.current) return;
        setError(errorText(e));
        setEntries([]);
        setShownPath(path);
      })
      .finally(() => {
        if (seq === loadSeq.current) setLoading(false);
      });
  }, [source, path, reloadTick]);

  // History + reset per-folder state when the folder changes.
  useEffect(() => {
    if (prevPath.current !== path) {
      const from = prevPath.current;
      if (!historyNav.current) setHistory((h) => historyVisit(h, from));
      historyNav.current = false;
      prevPath.current = path;
      setSelected(new Set());
      setAnchor(null);
      setCursor(null);
      setFilter('');
      setFilterOpen(false);
      setRenaming(null);
    }
    setPathText(path);
  }, [path]);

  const refresh = useCallback(() => setReloadTick((n) => n + 1), []);

  // Long paths: keep the current folder (the end of the breadcrumb bar) in view.
  useLayoutEffect(() => {
    const el = crumbsRef.current;
    if (el) el.scrollLeft = el.scrollWidth;
  }, [path, editingPath]);

  const visible = useMemo(
    () => sortItems(entries.filter((e) => (showHidden || !e.hidden) && matchesFilter(e.name, filter)), sort.key, sort.asc),
    [entries, showHidden, filter, sort]
  );
  const visiblePaths = useMemo(() => visible.map((e) => e.path), [visible]);
  const selectedItems = useMemo(() => visible.filter((e) => selected.has(e.path)), [visible, selected]);

  useImperativeHandle(
    ref,
    () => ({
      refresh,
      focus: () => listRef.current?.focus(),
      selected: () => selectedItems,
      path: () => path,
      items: () => visible
    }),
    [refresh, selectedItems, path, visible]
  );

  /* ---- navigation ---- */
  const navigate = (p: string) => {
    if (p !== path) onNavigate(p);
    else refresh();
  };
  const goBack = () => {
    const r = historyBack(history, path);
    if (!r) return;
    historyNav.current = true;
    setHistory(r.history);
    onNavigate(r.target);
  };
  const goForward = () => {
    const r = historyForward(history, path);
    if (!r) return;
    historyNav.current = true;
    setHistory(r.history);
    onNavigate(r.target);
  };
  const goUp = () => {
    const parent = P.parent(path);
    if (parent !== null) onNavigate(parent);
  };
  const goHome = async () => {
    if (!source) return;
    try {
      onNavigate(await source.home());
    } catch (e) {
      toast.error('Cannot open the home folder', errorText(e));
    }
  };

  const openItem = (item: FileItem) => {
    if (item.isDir) navigate(item.path);
    else ops.openFile(item);
  };

  /* ---- selection ---- */
  const select = (item: FileItem, ev: { ctrlKey: boolean; shiftKey: boolean; metaKey?: boolean }) => {
    const mode = ev.shiftKey ? 'range' : ev.ctrlKey || ev.metaKey ? 'toggle' : 'single';
    setSelected((cur) => nextSelection(visiblePaths, cur, anchor, item.path, mode));
    if (!ev.shiftKey) setAnchor(item.path);
    setCursor(item.path);
  };
  const moveCursor = (index: number, ev: { shiftKey: boolean; ctrlKey: boolean }) => {
    if (!visible.length) return;
    const i = Math.max(0, Math.min(visible.length - 1, index));
    const target = visible[i];
    if (ev.ctrlKey && !ev.shiftKey) setCursor(target.path);
    else select(target, { ctrlKey: false, shiftKey: ev.shiftKey });
    listRef.current?.querySelector(`[data-path="${CSS.escape(target.path)}"]`)?.scrollIntoView({ block: 'nearest' });
  };
  const targets = (item?: FileItem) => (item && !selected.has(item.path) ? [item] : selectedItems.length ? selectedItems : item ? [item] : []);

  /* ---- operations ---- */
  const mkdir = async () => {
    if (!source || shownPath === null || (side === 'local' && path === '')) return;
    const name = await inputDialog({ title: 'New folder', label: 'Folder name', confirmLabel: 'Create', validate: (v) => nameError(v, side) });
    if (!name) return;
    try {
      const target = P.join(path, name.trim());
      await source.mkdir(target);
      refresh();
      setSelected(new Set([target]));
      setCursor(target);
    } catch (e) {
      toast.error('Could not create the folder', errorText(e));
    }
  };
  const beginRename = (item: FileItem) => {
    if (item.type === 'drive' || item.type === 'place') return;
    setRenaming(item.path);
    setRenameText(item.name);
  };
  const commitRename = async () => {
    const item = visible.find((e) => e.path === renaming);
    setRenaming(null);
    if (!item || !source) return;
    const name = renameText.trim();
    if (!name || name === item.name) return;
    const err = nameError(name, side);
    if (err) {
      toast.warning('Cannot rename', err);
      return;
    }
    const parent = P.parent(item.path) ?? path;
    try {
      await source.rename(item.path, P.join(parent, name));
      refresh();
    } catch (e) {
      toast.error('Rename failed', errorText(e));
    }
  };

  const copyPath = (items: FileItem[]) => api.app.writeClipboard(items.map((i) => i.path).join('\r\n'));

  const menu = (x: number, y: number, item?: FileItem) => {
    const list = targets(item);
    const one = list.length === 1 ? list[0] : undefined;
    const special = list.some((i) => i.type === 'drive' || i.type === 'place');
    const other = side === 'local' ? 'server' : osTerms.thisComputer;
    const items: MenuItem[] = item
      ? [
          { label: one?.isDir ? 'Open' : 'Open with default app', disabled: !one, onClick: () => one && openItem(one) },
          ...(ops.edit && one && !one.isDir ? [{ label: 'Edit', shortcut: 'F4', onClick: () => ops.edit?.(one) }] : []),
          { type: 'separator' },
          { label: side === 'local' ? `Upload to ${other}` : `Download to ${other}`, shortcut: 'F5', disabled: special, onClick: () => ops.transfer(list, false) },
          { label: side === 'local' ? 'Upload and delete here (move)' : 'Download and delete on server (move)', shortcut: 'F6', disabled: special, onClick: () => ops.transfer(list, true) },
          ...(ops.menuExtras?.(list) ?? []),
          { type: 'separator' },
          { label: 'Rename…', shortcut: 'F2', disabled: !one || special, onClick: () => one && beginRename(one) },
          ...(ops.chmod && one ? [{ label: 'Permissions…', onClick: () => ops.chmod?.(one) }] : []),
          { label: list.length > 1 ? 'Copy paths' : 'Copy path', onClick: () => copyPath(list) },
          ...(side === 'local' && one && !special ? [{ label: `Show in ${osTerms.fileBrowser}`, onClick: () => void api.local.reveal(one.path) }] : []),
          { type: 'separator' },
          { label: 'Delete', shortcut: listShortcut('delete', 'Del'), danger: true, disabled: special, onClick: () => void ops.remove(list) }
        ]
      : [
          { label: 'New folder…', shortcut: listShortcut('newFolder', 'F7'), disabled: side === 'local' && path === '', onClick: () => void mkdir() },
          { label: 'Refresh', shortcut: listShortcut('refresh'), onClick: refresh },
          { label: 'Select all', shortcut: listShortcut('selectAll'), onClick: () => setSelected(new Set(visiblePaths)) },
          { type: 'separator' },
          { label: 'Copy current path', onClick: () => api.app.writeClipboard(path) },
          ...(side === 'local' && path ? [{ label: `Open in ${osTerms.fileBrowser}`, onClick: () => void api.local.open(path) }] : [])
        ];
    showContextMenu(x, y, items);
  };

  /* ---- keyboard ---- */
  const onKeyDown = (ev: ReactKeyboardEvent) => {
    if (renaming) return;
    const idx = visible.findIndex((x) => x.path === cursor);
    const current = idx >= 0 ? visible[idx] : undefined;
    const key = ev.key;
    const handled = () => {
      ev.preventDefault();
      ev.stopPropagation();
    };
    const plain = !ev.ctrlKey && !ev.altKey && !ev.metaKey;
    switch (listAction(ev)) {
      case 'refresh':
        handled();
        return refresh();
      case 'find':
        if (ops.find) {
          handled();
          ops.find();
        }
        return;
      case 'bookmark':
        handled();
        return onToggleBookmark(path);
      case 'path':
        handled();
        return setEditingPath(true);
      case 'selectAll':
        handled();
        return setSelected(new Set(visiblePaths));
      case 'up':
        handled();
        return goUp();
      case 'open':
        if (current) {
          handled();
          openItem(current);
        }
        return;
      case 'back':
        handled();
        return goBack();
      case 'forward':
        handled();
        return goForward();
      case 'newFolder':
        if (source && !(side === 'local' && path === '')) {
          handled();
          void mkdir();
        }
        return;
      case 'delete': {
        handled();
        const list = targets(current);
        if (list.length) void ops.remove(list);
        return;
      }
      case null:
        break;
    }
    if (key === 'ArrowDown' || key === 'ArrowUp') {
      handled();
      if (ev.altKey && key === 'ArrowUp') goUp();
      else moveCursor(idx < 0 ? 0 : idx + (key === 'ArrowDown' ? 1 : -1), ev);
    } else if (key === 'Home' || key === 'End') {
      handled();
      moveCursor(key === 'Home' ? 0 : visible.length - 1, ev);
    } else if (key === 'PageDown' || key === 'PageUp') {
      handled();
      const rows = Math.max(1, Math.floor((listRef.current?.clientHeight ?? 260) / 26) - 1);
      moveCursor((idx < 0 ? 0 : idx) + (key === 'PageDown' ? rows : -rows), ev);
    } else if (key === 'Enter' && current) {
      handled();
      openItem(current);
    } else if (key === 'Backspace' && plain) {
      handled();
      goUp();
    } else if (ev.altKey && (key === 'ArrowLeft' || key === 'ArrowRight')) {
      handled();
      if (key === 'ArrowLeft') goBack();
      else goForward();
    } else if (key === 'Insert' || (key === ' ' && plain)) {
      handled();
      if (current) {
        setSelected((cur) => nextSelection(visiblePaths, cur, anchor, current.path, 'toggle'));
        moveCursor(idx + 1, { shiftKey: false, ctrlKey: true });
      }
    } else if (key === 'Tab' && plain && !ev.shiftKey) {
      handled();
      ops.switchPane();
    } else if (key === 'F2' && current) {
      handled();
      beginRename(current);
    } else if (key === 'F4' && current && ops.edit && !current.isDir) {
      handled();
      ops.edit(current);
    } else if ((key === 'F5' || key === 'F6') && plain) {
      handled();
      const list = targets(current);
      if (list.length) ops.transfer(list, key === 'F6');
    } else if (key === 'F7' && plain) {
      handled();
      void mkdir();
    } else if ((key === 'Delete' || key === 'F8') && !ev.ctrlKey && !ev.altKey) {
      handled();
      const list = targets(current);
      if (list.length) void ops.remove(list);
    } else if (key === 'Escape') {
      if (filter || filterOpen) {
        handled();
        setFilter('');
        setFilterOpen(false);
      } else if (selected.size) {
        handled();
        setSelected(new Set());
      }
    } else if (key.length === 1 && plain && key !== ' ') {
      // Type-ahead: jump to the first name starting with the typed text.
      const now = Date.now();
      const ta = typeAhead.current;
      ta.text = now - ta.at > 700 ? key.toLowerCase() : ta.text + key.toLowerCase();
      ta.at = now;
      const start = Math.max(0, idx);
      const hit = [...visible.slice(start), ...visible.slice(0, start)].find((x) => x.name.toLowerCase().startsWith(ta.text));
      if (hit) {
        handled();
        moveCursor(visible.indexOf(hit), { shiftKey: false, ctrlKey: false });
      }
    }
  };

  /* ---- drag & drop ---- */
  /** The folder a drop goes to: the folder row under the cursor, else the pane's folder. */
  const dropTarget = (el: EventTarget | null): { dir: string; onFolder: boolean } => {
    const row = (el as HTMLElement | null)?.closest?.('[data-path]') as HTMLElement | null;
    const p = row?.dataset.path;
    const item = p ? visible.find((x) => x.path === p) : undefined;
    return item?.isDir && item.type !== 'drive' && item.type !== 'place' ? { dir: item.path, onFolder: true } : { dir: path, onFolder: false };
  };
  const isMove = (e: DragEvent) => e.shiftKey || (isMac && e.metaKey);
  const onDragOver = (e: DragEvent) => {
    const ours = e.dataTransfer.types.includes(PAGE_DRAG);
    if ((!ours && !e.dataTransfer.types.includes('Files')) || !source || (side === 'local' && path === '')) return;
    const target = dropTarget(e.target);
    if (dragFrom === side) {
      // Within its own pane only a folder takes a drag, and the items move there.
      if (!target.onFolder) {
        setDragOver(null);
        return;
      }
      e.preventDefault();
      e.dataTransfer.dropEffect = 'move';
    } else {
      // To the other pane: copied, or moved with Shift (macOS: or Command); from Explorer / Finder: copied.
      e.preventDefault();
      e.dataTransfer.dropEffect = dragFrom && isMove(e) ? 'move' : 'copy';
    }
    setDragOver(target.dir);
  };
  const onDrop = async (e: DragEvent) => {
    e.preventDefault();
    const target = dropTarget(e.target);
    setDragOver(null);
    if (e.dataTransfer.types.includes(PAGE_DRAG)) {
      ops.dropItems(target.dir, target.onFolder, isMove(e));
      return;
    }
    if (!e.dataTransfer.files.length) return;
    const paths = await api.files.pathsForFiles(e.dataTransfer.files);
    if (paths.length) ops.dropPaths(paths, target.dir);
  };
  const onRowDragStart = (e: DragEvent, item: FileItem) => {
    if (item.type === 'drive' || item.type === 'place') {
      e.preventDefault();
      return;
    }
    const list = selected.has(item.path) ? selectedItems : [item];
    if (!selected.has(item.path)) {
      setSelected(new Set([item.path]));
      setAnchor(item.path);
      setCursor(item.path);
    }
    if (side === 'local') {
      // The host runs a native drag (Explorer / Finder understand it); the browser's own drag is not used.
      e.preventDefault();
    } else {
      // Server items stay in the page: only the panes take this drag.
      e.dataTransfer.setData(PAGE_DRAG, '1');
      e.dataTransfer.effectAllowed = 'copyMove';
    }
    ops.startDrag(list.filter((i) => i.type !== 'drive' && i.type !== 'place'));
  };

  /* ---- render ---- */
  const crumbs = P.crumbs(path);
  const bookmarked = bookmarks.some((b) => b.path === path);
  const selSize = selectedItems.reduce((n, i) => n + (i.isDir ? 0 : i.size), 0);
  const columns: { key: SortKey; label: string }[] = [
    { key: 'name', label: 'Name' },
    { key: 'size', label: path === '' && side === 'local' ? 'Total' : 'Size' },
    { key: 'mtime', label: 'Modified' },
    { key: 'permissions', label: side === 'remote' ? 'Permissions' : 'Attr' }
  ];

  return (
    <section
      className={cx('fm-pane', `fm-pane--${side}`, active && 'fm-pane--active', dragFrom && dragFrom !== side && 'fm-pane--drophint')}
      data-pane-side={side}
      data-pane-path={path}
      onMouseDownCapture={onActivate}
    >
      <header className="fm-pane__head">
        <span className="fm-pane__title" title={title}>
          {title}
        </span>
        {badge}
        <span className="fm-spacer" />
        <IconButton size="sm" label={`Back (${listShortcut('back', formatChord('Alt+ArrowLeft'))})`} onClick={goBack} disabled={!history.back.length}>
          <ArrowLeft size={14} />
        </IconButton>
        <IconButton size="sm" label={`Forward (${listShortcut('forward', formatChord('Alt+ArrowRight'))})`} onClick={goForward} disabled={!history.forward.length}>
          <ArrowRight size={14} />
        </IconButton>
        <IconButton size="sm" label={`Up (${listShortcut('up', 'Backspace')})`} onClick={goUp} disabled={P.parent(path) === null}>
          <ArrowUp size={14} />
        </IconButton>
        <IconButton size="sm" label="Home" onClick={() => void goHome()} disabled={!source}>
          <Home size={14} />
        </IconButton>
        <IconButton size="sm" label={`Refresh (${listShortcut('refresh')})`} onClick={refresh} disabled={!source}>
          <RefreshCw size={14} className={cx(loading && 'fm-spin')} />
        </IconButton>
        <IconButton
          size="sm"
          label={`Bookmarks (${listShortcut('bookmark')} adds this folder)`}
          pressed={bookmarked}
          disabled={!source}
          onClick={(e) => {
            const r = (e.currentTarget as HTMLElement).getBoundingClientRect();
            const items: MenuItem[] = [
              { label: bookmarked ? 'Remove this folder' : 'Bookmark this folder', shortcut: listShortcut('bookmark'), onClick: () => onToggleBookmark(path) },
              ...(bookmarks.length ? [{ type: 'separator' as const }] : []),
              ...bookmarks.map((b) => ({ label: b.label, onClick: () => navigate(b.path) }))
            ];
            showContextMenu(r.left, r.bottom + 2, items);
          }}
        >
          <Star size={14} className={cx(bookmarked && 'fm-star--on')} />
        </IconButton>
        <IconButton size="sm" label={`New folder (${listShortcut('newFolder', 'F7')})`} onClick={() => void mkdir()} disabled={!source || (side === 'local' && path === '')}>
          <FolderPlus size={14} />
        </IconButton>
        <IconButton size="sm" label="Filter" pressed={filterOpen || !!filter} onClick={() => setFilterOpen((v) => !v)} disabled={!source}>
          <Search size={14} />
        </IconButton>
      </header>

      <div className="fm-pane__path">
        {editingPath ? (
          <form
            onSubmit={(e) => {
              e.preventDefault();
              setEditingPath(false);
              navigate(P.normalize(pathText, path));
            }}
          >
            <input
              className="input fm-path-input"
              value={pathText}
              autoFocus
              spellCheck={false}
              aria-label="Folder"
              onChange={(e) => setPathText(e.target.value)}
              onBlur={() => setEditingPath(false)}
              onKeyDown={(e) => {
                if (e.key === 'Escape') {
                  e.stopPropagation();
                  setEditingPath(false);
                  setPathText(path);
                }
              }}
            />
          </form>
        ) : (
          <div ref={crumbsRef} className="fm-crumbs" onClick={(e) => e.target === e.currentTarget && setEditingPath(true)} title={`Click empty space to type a path (${listShortcut('path')})`}>
            {crumbs.map((c, i) => (
              <button key={c.path + i} type="button" className="fm-crumb" onClick={() => navigate(c.path)}>
                {c.label}
              </button>
            ))}
          </div>
        )}
        {(filterOpen || filter) && (
          <div className="fm-filter">
            <input
              className="input fm-filter__input"
              placeholder="Filter (e.g. *.log)"
              value={filter}
              autoFocus
              spellCheck={false}
              onChange={(e) => setFilter(e.target.value)}
              onKeyDown={(e) => {
                if (e.key === 'Escape') {
                  e.stopPropagation();
                  setFilter('');
                  setFilterOpen(false);
                  listRef.current?.focus();
                } else if (e.key === 'ArrowDown' || e.key === 'Enter') {
                  e.preventDefault();
                  listRef.current?.focus();
                  if (visible[0]) moveCursor(0, { shiftKey: false, ctrlKey: false });
                }
              }}
            />
            <IconButton
              size="sm"
              label="Clear filter"
              onClick={() => {
                setFilter('');
                setFilterOpen(false);
              }}
            >
              <X size={12} />
            </IconButton>
          </div>
        )}
      </div>

      {!source ? (
        <div className="fm-pane__placeholder">{placeholder}</div>
      ) : (
        <div
          ref={listRef}
          className={cx('fm-list', dragOver === path && 'fm-list--drop')}
          tabIndex={0}
          role="grid"
          aria-label={title}
          data-own-keys={ownKeys()}
          onKeyDown={onKeyDown}
          onContextMenu={(e) => {
            e.preventDefault();
            menu(e.clientX, e.clientY);
          }}
          onDragOver={onDragOver}
          onDragLeave={(e) => {
            if (!e.currentTarget.contains(e.relatedTarget as Node | null)) setDragOver(null);
          }}
          onDrop={(e) => void onDrop(e)}
        >
          <div className="fm-row fm-row--head" role="row">
            {columns.map((c) => (
              <button
                key={c.key}
                className={cx('fm-col', `fm-col--${c.key}`)}
                onClick={() => setSort((s) => ({ key: c.key, asc: s.key === c.key ? !s.asc : true }))}
              >
                {c.label}
                {sort.key === c.key && <span className="fm-sort">{sort.asc ? '▲' : '▼'}</span>}
              </button>
            ))}
          </div>
          {error && <div className="fm-state fm-state--error">{error}</div>}
          {!error && loading && shownPath !== path && (
            <div className="fm-state">
              <Loader2 size={18} className="fm-spin" />
            </div>
          )}
          {!error && !loading && shownPath === path && visible.length === 0 && (
            <div className="fm-state fm-state--muted">{filter ? 'Nothing matches the filter.' : 'This folder is empty.'}</div>
          )}
          {(shownPath === path || !loading) &&
            visible.map((e) => (
              <div
                key={e.path}
                data-path={e.path}
                role="row"
                aria-selected={selected.has(e.path)}
                className={cx(
                  'fm-row',
                  selected.has(e.path) && 'fm-row--selected',
                  cursor === e.path && 'fm-row--cursor',
                  e.hidden && 'fm-row--hidden',
                  dragOver === e.path && 'fm-row--drop'
                )}
                draggable={e.type !== 'drive' && e.type !== 'place'}
                onDragStart={(ev) => onRowDragStart(ev, e)}
                onDragEnd={() => side === 'remote' && ops.endDrag()}
                onMouseDown={(ev) => {
                  if (ev.button !== 0) return;
                  if (selected.has(e.path) && !ev.ctrlKey && !ev.shiftKey && !ev.metaKey) {
                    setCursor(e.path);  // keep a multi-selection for dragging
                    return;
                  }
                  select(e, ev);
                }}
                onClick={(ev) => {
                  if (!ev.ctrlKey && !ev.shiftKey && !ev.metaKey && selected.size > 1) select(e, ev);
                }}
                onDoubleClick={() => openItem(e)}
                onContextMenu={(ev) => {
                  ev.preventDefault();
                  ev.stopPropagation();
                  if (!selected.has(e.path)) select(e, { ctrlKey: false, shiftKey: false });
                  menu(ev.clientX, ev.clientY, e);
                }}
                title={e.label && e.label !== e.name ? `${e.name} — ${e.label}` : e.path}
              >
                <span className="fm-col fm-col--name">
                  <ItemIcon item={e} />
                  {renaming === e.path ? (
                    <input
                      className="input fm-rename"
                      value={renameText}
                      autoFocus
                      spellCheck={false}
                      onChange={(ev) => setRenameText(ev.target.value)}
                      onFocus={(ev) => {
                        const dot = e.isDir ? -1 : ev.currentTarget.value.lastIndexOf('.');
                        ev.currentTarget.setSelectionRange(0, dot > 0 ? dot : ev.currentTarget.value.length);
                      }}
                      onBlur={() => void commitRename()}
                      onMouseDown={(ev) => ev.stopPropagation()}
                      onKeyDown={(ev) => {
                        ev.stopPropagation();
                        if (ev.key === 'Enter') void commitRename().then(() => listRef.current?.focus());
                        if (ev.key === 'Escape') {
                          setRenaming(null);
                          listRef.current?.focus();
                        }
                      }}
                    />
                  ) : (
                    <span className="fm-name">{e.name}</span>
                  )}
                </span>
                <span className="fm-col fm-col--size">
                  {e.type === 'drive' && e.totalBytes ? (
                    <span className="fm-drive-meter" title={`${fmtSize(e.freeBytes ?? 0)} free of ${fmtSize(e.totalBytes)}`}>
                      <span style={{ width: `${Math.round((1 - (e.freeBytes ?? 0) / e.totalBytes) * 100)}%` }} />
                    </span>
                  ) : e.isDir ? (
                    ''
                  ) : (
                    fmtSize(e.size)
                  )}
                </span>
                <span className="fm-col fm-col--mtime">{e.type === 'drive' ? (e.freeBytes !== undefined ? `${fmtSize(e.freeBytes)} free` : '') : fmtDate(e.mtime)}</span>
                <span className="fm-col fm-col--permissions mono">{e.permissions}</span>
              </div>
            ))}
        </div>
      )}

      <footer className="fm-pane__status">
        {source ? (
          <>
            <span>
              {visible.length} item{visible.length === 1 ? '' : 's'}
              {entries.length !== visible.length && ` (${entries.length - visible.length} hidden)`}
            </span>
            {selectedItems.length > 0 && (
              <span>
                · {selectedItems.length} selected{selSize ? `, ${fmtSize(selSize)}` : ''}
              </span>
            )}
          </>
        ) : (
          <span>&nbsp;</span>
        )}
      </footer>
    </section>
  );
});
