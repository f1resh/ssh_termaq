/**
 * Tab strip in the title bar: scrollable tabs (drag to reorder, middle-click close, double-click rename,
 * colour accent, status / activity / bell indicators, context menu) and the "+" split button.
 */
import { useEffect, useLayoutEffect, useRef, useState, type CSSProperties, type DragEvent, type KeyboardEvent as ReactKeyboardEvent } from 'react';
import { useShallow } from 'zustand/react/shallow';
import {
  Bell,
  ChevronDown,
  Columns2,
  Copy,
  FolderOpen,
  Palette,
  Pencil,
  Plus,
  Radio,
  RotateCw,
  Rows2,
  X,
  Zap,
  SquarePlus,
  History,
  Server
} from 'lucide-react';
import { cx, showContextMenu, type MenuItem } from '@/components/ui';
import { runCommand } from '@/commands/commands';
import { shortcutLabel } from '@/commands/keybindings';
import { closeOtherTabs, closeTab, closeTabsToRight, connectSavedSession, focusTabPane, newSessionDraft } from '@/commands/actions';
import { useSessions, useSettings } from '@/store/data';
import { useUi } from '@/store/ui';
import { paneIdsOf, tabTitle, useWorkspace } from '@/store/workspace';
import { connOfFilesPane } from '@/components/files/filesConn';
import { sessionLabel } from '@shared/defaults';
import type { SessionConfig } from '@shared/types';
import { COLOR_SWATCHES, ProtocolIcon, STATUS_LABELS, StatusDot, Swatch, sessionSubtitle } from '@/components/layout/sessionVisuals';

export const TAB_MIME = 'application/x-sshterm-tab';
export const SESSION_MIME = 'application/x-sshterm-session';

/* -------------------------------------------------------------------------- */
/* Menus                                                                       */
/* -------------------------------------------------------------------------- */

function tabMenu(tabId: string): MenuItem[] {
  const ws = useWorkspace.getState();
  const tab = ws.tabs.find((t) => t.id === tabId);
  if (!tab) return [];
  const index = ws.tabs.indexOf(tab);
  const target = { tabId };
  const pane = ws.panes[tab.activePaneId];
  const sshTerminal = !!pane && pane.kind !== 'files' && pane.session.protocol === 'ssh';
  return [
    { label: 'Rename…', icon: <Pencil size={14} />, shortcut: shortcutLabel('tab.rename'), onClick: () => runCommand('tab.rename', target) },
    {
      label: 'Color',
      icon: <Palette size={14} />,
      submenu: [
        ...COLOR_SWATCHES.map(
          (c): MenuItem => ({
            label: c.name,
            icon: <Swatch color={c.value} />,
            checked: tab.color?.toLowerCase() === c.value,
            onClick: () => useWorkspace.getState().setTabColor(tabId, c.value)
          })
        ),
        { type: 'separator' },
        { label: 'None', icon: <Swatch />, checked: !tab.color, onClick: () => useWorkspace.getState().setTabColor(tabId, undefined) }
      ]
    },
    { type: 'separator' },
    { label: 'Duplicate', icon: <Copy size={14} />, shortcut: shortcutLabel('tab.duplicate'), onClick: () => runCommand('tab.duplicate', target) },
    { label: 'Split right', icon: <Columns2 size={14} />, shortcut: shortcutLabel('pane.splitRight'), onClick: () => runCommand('pane.splitRight', target) },
    { label: 'Split down', icon: <Rows2 size={14} />, shortcut: shortcutLabel('pane.splitDown'), onClick: () => runCommand('pane.splitDown', target) },
    {
      label: 'Broadcast input',
      icon: <Radio size={14} />,
      checked: tab.broadcast,
      shortcut: shortcutLabel('pane.toggleBroadcast'),
      onClick: () => runCommand('pane.toggleBroadcast', target)
    },
    { label: 'Restart', icon: <RotateCw size={14} />, onClick: () => runCommand('pane.restart', target) },
    { type: 'separator' },
    {
      label: 'Open file manager (SFTP/SCP)',
      icon: <FolderOpen size={14} />,
      disabled: !sshTerminal,
      shortcut: shortcutLabel('pane.fileManager'),
      onClick: () => runCommand('pane.fileManager', target)
    },
    { type: 'separator' },
    { label: 'Close', icon: <X size={14} />, shortcut: shortcutLabel('tab.close'), onClick: () => void closeTab(tabId) },
    { label: 'Close others', disabled: ws.tabs.length < 2, onClick: () => void closeOtherTabs(tabId) },
    { label: 'Close to the right', disabled: index >= ws.tabs.length - 1, onClick: () => void closeTabsToRight(tabId) }
  ];
}

function sessionItem(s: SessionConfig, prefixFolder: boolean): MenuItem {
  const label = sessionLabel(s);
  return {
    label: prefixFolder && s.folder ? `${s.folder} / ${label}` : label,
    icon: <ProtocolIcon protocol={s.protocol} />,
    onClick: () => connectSavedSession(s.id)
  };
}

export function newTabMenu(): MenuItem[] {
  const sessions = useSessions.getState().sessions;
  const recent = [...sessions].filter((s) => s.lastUsedAt > 0).sort((a, b) => b.lastUsedAt - a.lastUsedAt).slice(0, 5);
  const all = [...sessions].sort((a, b) => a.folder.localeCompare(b.folder) || sessionLabel(a).localeCompare(sessionLabel(b)));
  const LIMIT = 40;
  const items: MenuItem[] = [
    { label: 'Quick connect…', icon: <Zap size={14} />, shortcut: shortcutLabel('app.quickConnect'), onClick: () => runCommand('app.quickConnect') },
    { label: 'New session…', icon: <SquarePlus size={14} />, shortcut: shortcutLabel('app.newSession'), onClick: () => newSessionDraft() }
  ];
  if (recent.length) {
    items.push({ type: 'separator' }, { type: 'header', label: 'Recent' }, ...recent.map((s) => sessionItem(s, true)));
  }
  if (all.length) {
    const sub: MenuItem[] = all.slice(0, LIMIT).map((s) => sessionItem(s, true));
    if (all.length > LIMIT) {
      sub.push({ type: 'separator' }, { label: `All ${all.length} sessions…`, onClick: () => useUi.getState().openPalette('@') });
    }
    items.push({ type: 'separator' }, { label: 'Saved sessions', icon: <Server size={14} />, submenu: sub });
  }
  if (!recent.length && !all.length) {
    items.push({ type: 'separator' }, { label: 'No saved sessions yet', icon: <History size={14} />, disabled: true });
  }
  return items;
}

/* -------------------------------------------------------------------------- */
/* Tab                                                                         */
/* -------------------------------------------------------------------------- */

interface DropMark {
  index: number;
  side: 'before' | 'after';
}

function TabItem({
  tabId,
  index,
  dragging,
  drop,
  onDragStartTab,
  onDragOverTab,
  onDropTab,
  onDragEndTab
}: {
  tabId: string;
  index: number;
  dragging: boolean;
  drop: DropMark | null;
  onDragStartTab: (index: number, e: DragEvent) => void;
  onDragOverTab: (index: number, e: DragEvent) => void;
  onDropTab: (index: number, e: DragEvent) => void;
  onDragEndTab: () => void;
}) {
  const titleMode = useSettings((s) => s.settings.appearance.tabTitle);
  const info = useWorkspace(
    useShallow((s) => {
      const tab = s.tabs.find((t) => t.id === tabId);
      const pane = tab ? s.panes[tab.activePaneId] : undefined;
      const ids = tab ? paneIdsOf(tab.layout) : [];
      const panes = ids.map((id) => s.panes[id]).filter(Boolean);
      return {
        title: tab ? tabTitle(tab, s.panes, titleMode) : '',
        color: tab?.color ?? '',
        broadcast: !!tab?.broadcast,
        active: s.activeTabId === tabId,
        status: pane?.kind === 'files' ? connOfFilesPane(pane, s.panes).status : (pane?.status ?? 'idle'),
        protocol: pane?.session.protocol ?? 'ssh',
        files: pane?.kind === 'files',
        subtitle: pane ? sessionSubtitle(pane.session) : '',
        message: pane?.statusMessage ?? '',
        activity: panes.some((p) => p.hasActivity),
        bell: panes.some((p) => p.bell),
        paneCount: panes.length
      };
    })
  );
  const renaming = useUi((s) => s.renamingTabId === tabId);
  const ref = useRef<HTMLDivElement>(null);

  useEffect(() => {
    if (info.active) ref.current?.scrollIntoView({ block: 'nearest', inline: 'nearest' });
  }, [info.active]);

  const tooltip = [info.title, info.subtitle !== info.title ? info.subtitle : '', STATUS_LABELS[info.status] + (info.message ? ` — ${info.message}` : '')]
    .filter(Boolean)
    .join('\n');

  return (
    <div
      ref={ref}
      role="tab"
      aria-selected={info.active}
      tabIndex={info.active ? 0 : -1}
      title={renaming ? undefined : tooltip}
      className={cx(
        'tab no-drag',
        info.active && 'tab--active',
        info.color && 'tab--colored',
        dragging && 'tab--dragging',
        drop?.index === index && `tab--drop-${drop.side}`
      )}
      style={info.color ? ({ '--tab-color': info.color } as CSSProperties) : undefined}
      draggable={!renaming}
      onMouseDown={(e) => {
        if (e.button === 1) e.preventDefault(); // no autoscroll
        if (e.button === 0) useWorkspace.getState().activateTab(tabId);
      }}
      onClick={() => {
        // The mouse-down focused the tab itself; hand focus on to the terminal (or its password prompt).
        if (!renaming) focusTabPane(tabId);
      }}
      onAuxClick={(e) => {
        if (e.button === 1) {
          e.preventDefault();
          void closeTab(tabId);
        }
      }}
      onDoubleClick={(e) => {
        if ((e.target as HTMLElement).closest('.tab__close')) return;
        useUi.getState().setRenamingTab(tabId);
      }}
      onContextMenu={(e) => {
        e.preventDefault();
        useWorkspace.getState().activateTab(tabId);
        showContextMenu(e.clientX, e.clientY, tabMenu(tabId));
      }}
      onKeyDown={(e) => {
        if (renaming) return;
        if (e.key === 'F2') useUi.getState().setRenamingTab(tabId);
        if (e.key === 'Delete') void closeTab(tabId);
      }}
      onDragStart={(e) => onDragStartTab(index, e)}
      onDragOver={(e) => onDragOverTab(index, e)}
      onDrop={(e) => onDropTab(index, e)}
      onDragEnd={onDragEndTab}
    >
      <span className="tab__icon">
        {info.files ? <FolderOpen size={14} strokeWidth={1.75} aria-hidden /> : <ProtocolIcon protocol={info.protocol} size={14} />}
        <StatusDot status={info.status} className="tab__status" />
      </span>
      {renaming ? <RenameInput tabId={tabId} initial={info.title} /> : <span className="tab__title">{info.title}</span>}
      {info.broadcast && <Radio size={12} className="tab__badge tab__badge--broadcast" aria-label="Broadcasting input" />}
      {info.bell ? (
        <Bell size={12} className="tab__badge tab__badge--bell" aria-label="Bell" />
      ) : (
        info.activity && !info.active && <span className="tab__activity" aria-label="New output" />
      )}
      {info.paneCount > 1 && <span className="tab__count" title={`${info.paneCount} panes`}>{info.paneCount}</span>}
      <button
        className="tab__close"
        tabIndex={-1}
        aria-label="Close tab"
        title={`Close (${shortcutLabel('tab.close') || 'middle-click'})`}
        onMouseDown={(e) => e.stopPropagation()}
        onClick={(e) => {
          e.stopPropagation();
          void closeTab(tabId);
        }}
      >
        <X size={14} />
      </button>
    </div>
  );
}

function RenameInput({ tabId, initial }: { tabId: string; initial: string }) {
  const [value, setValue] = useState(initial);
  const ref = useRef<HTMLInputElement>(null);
  const done = useRef(false);
  useLayoutEffect(() => {
    ref.current?.focus();
    ref.current?.select();
  }, []);
  const finish = (commit: boolean) => {
    if (done.current) return;
    done.current = true;
    if (commit) {
      const tab = useWorkspace.getState().tabs.find((t) => t.id === tabId);
      const trimmed = value.trim();
      // Committing the automatic title unchanged keeps the tab "automatic".
      if (tab && (trimmed !== initial || tab.customTitle)) useWorkspace.getState().renameTab(tabId, trimmed || undefined);
    }
    useUi.getState().setRenamingTab(null);
  };
  const onKeyDown = (e: ReactKeyboardEvent) => {
    e.stopPropagation();
    if (e.key === 'Enter') finish(true);
    else if (e.key === 'Escape') finish(false);
  };
  return (
    <input
      ref={ref}
      className="tab__rename"
      value={value}
      aria-label="Tab title"
      spellCheck={false}
      onChange={(e) => setValue(e.target.value)}
      onKeyDown={onKeyDown}
      onBlur={() => finish(true)}
      onMouseDown={(e) => e.stopPropagation()}
      onDoubleClick={(e) => e.stopPropagation()}
    />
  );
}

/* -------------------------------------------------------------------------- */
/* Strip                                                                       */
/* -------------------------------------------------------------------------- */

export function TabStrip() {
  const tabIds = useWorkspace(useShallow((s) => s.tabs.map((t) => t.id)));
  const scrollRef = useRef<HTMLDivElement>(null);
  const [dragIndex, setDragIndex] = useState<number | null>(null);
  const [drop, setDrop] = useState<DropMark | null>(null);
  const [overflow, setOverflow] = useState({ left: false, right: false });

  const updateOverflow = () => {
    const el = scrollRef.current;
    if (!el) return;
    const left = el.scrollLeft > 1;
    const right = el.scrollLeft + el.clientWidth < el.scrollWidth - 1;
    setOverflow((o) => (o.left === left && o.right === right ? o : { left, right }));
  };

  useLayoutEffect(() => {
    const el = scrollRef.current;
    if (!el) return;
    updateOverflow();
    const ro = new ResizeObserver(updateOverflow);
    ro.observe(el);
    return () => ro.disconnect();
  });

  const acceptsDrop = (e: DragEvent) => e.dataTransfer.types.includes(TAB_MIME) || e.dataTransfer.types.includes(SESSION_MIME);

  const onDragStartTab = (index: number, e: DragEvent) => {
    e.dataTransfer.setData(TAB_MIME, String(index));
    e.dataTransfer.effectAllowed = 'move';
    setDragIndex(index);
  };
  const onDragOverTab = (index: number, e: DragEvent) => {
    if (!acceptsDrop(e)) return;
    e.preventDefault();
    e.stopPropagation();
    e.dataTransfer.dropEffect = e.dataTransfer.types.includes(TAB_MIME) ? 'move' : 'copy';
    const r = (e.currentTarget as HTMLElement).getBoundingClientRect();
    const side = e.clientX < r.left + r.width / 2 ? 'before' : 'after';
    setDrop((d) => (d?.index === index && d.side === side ? d : { index, side }));
  };
  const finishDrag = () => {
    setDragIndex(null);
    setDrop(null);
  };
  const onDropTab = (index: number, e: DragEvent) => {
    e.preventDefault();
    e.stopPropagation();
    const sessionId = e.dataTransfer.getData(SESSION_MIME);
    if (sessionId) {
      connectSavedSession(sessionId);
      finishDrag();
      return;
    }
    const raw = e.dataTransfer.getData(TAB_MIME);
    const from = raw === '' ? dragIndex : Number(raw);
    if (from !== null && Number.isFinite(from)) {
      const side = drop?.index === index ? drop.side : 'after';
      let to = side === 'before' ? index : index + 1;
      if (from < to) to -= 1;
      useWorkspace.getState().moveTab(from, to);
    }
    finishDrag();
  };

  return (
    <div className="tabstrip" role="presentation">
      <div
        ref={scrollRef}
        className={cx('tabstrip__scroll', overflow.left && 'tabstrip__scroll--fade-left', overflow.right && 'tabstrip__scroll--fade-right')}
        role="tablist"
        aria-label="Open tabs"
        onScroll={updateOverflow}
        onWheel={(e) => {
          const el = scrollRef.current;
          if (el && Math.abs(e.deltaY) > Math.abs(e.deltaX)) el.scrollLeft += e.deltaY;
        }}
        onDragOver={(e) => {
          if (!acceptsDrop(e)) return;
          e.preventDefault();
        }}
        onDragLeave={(e) => {
          if (!e.currentTarget.contains(e.relatedTarget as Node)) setDrop(null);
        }}
        onDrop={(e) => {
          // Dropped on empty strip space: sessions open, tabs move to the end.
          e.preventDefault();
          const sessionId = e.dataTransfer.getData(SESSION_MIME);
          if (sessionId) connectSavedSession(sessionId);
          else if (dragIndex !== null) useWorkspace.getState().moveTab(dragIndex, tabIds.length - 1);
          finishDrag();
        }}
      >
        {tabIds.map((id, i) => (
          <TabItem
            key={id}
            tabId={id}
            index={i}
            dragging={dragIndex === i}
            drop={drop}
            onDragStartTab={onDragStartTab}
            onDragOverTab={onDragOverTab}
            onDropTab={onDropTab}
            onDragEndTab={finishDrag}
          />
        ))}
      </div>
      <div className="newtab no-drag">
        <button className="newtab__btn" title={`New tab (${shortcutLabel('tab.new')})`} aria-label="New tab" onClick={() => runCommand('tab.new')}>
          <Plus size={16} />
        </button>
        <button
          className="newtab__btn newtab__btn--menu"
          title="Open a session"
          aria-label="New tab options"
          onClick={(e) => {
            const r = (e.currentTarget as HTMLElement).getBoundingClientRect();
            showContextMenu(r.left, r.bottom + 4, newTabMenu());
          }}
        >
          <ChevronDown size={14} />
        </button>
      </div>
    </div>
  );
}
