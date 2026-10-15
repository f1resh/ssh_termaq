/**
 * Main content area: every open tab stays mounted (only the active one is visible, so terminals keep their
 * state) and renders its split layout with draggable splitters; a file manager tab holds a single files pane.
 * Panes are absolutely positioned siblings keyed by pane id — splitting or closing never remounts a terminal.
 * Panes of a split tab get a header bar (title + close). Tabs with an SFTP panel show it on the right; panels of
 * background tabs stay mounted (hidden) so they keep their folder and connection.
 */
import { useEffect, useMemo, useRef, useState, type PointerEvent as ReactPointerEvent, type RefObject } from 'react';
import { useShallow } from 'zustand/react/shallow';
import { X } from 'lucide-react';
import { api } from '@/bridge/api';
import { closePane, liveSftpPanels } from '@/commands/actions';
import { shortcutLabel } from '@/commands/keybindings';
import { cx } from '@/components/ui';
import { FileManager } from '@/components/files/FileManager';
import { SftpPanel } from '@/components/sftp/SftpPanel';
import { Welcome } from '@/components/welcome/Welcome';
import { TerminalPane } from '@/terminal/TerminalPane';
import { useSettings } from '@/store/data';
import { useUi } from '@/store/ui';
import { paneIdsOf, paneTitle, useWorkspace, type TabState } from '@/store/workspace';
import { computeGeometry, cssLin, dragSizes, evalLin, findSplit, leavesAlong, normSizes, type SplitterGeom } from './paneGeometry';
import { ProtocolIcon, StatusDot } from './sessionVisuals';
import './layout.css';

const GUTTER = 2;
const MIN_PANE_W = 120;
const MIN_PANE_H = 64;

/* -------------------------------------------------------------------------- */

function Splitter({ geom, tabId, containerRef }: { geom: SplitterGeom; tabId: string; containerRef: RefObject<HTMLDivElement | null> }) {
  const [dragging, setDragging] = useState(false);
  const horizontal = geom.direction === 'row';
  const r = geom.rect;

  const onPointerDown = (e: ReactPointerEvent<HTMLDivElement>) => {
    if (e.button !== 0) return;
    const el = containerRef.current;
    const tab = useWorkspace.getState().tabs.find((t) => t.id === tabId);
    const split = tab && findSplit(tab.layout, geom.splitId);
    if (!el || !split) return;
    e.preventDefault();
    e.currentTarget.setPointerCapture(e.pointerId);
    setDragging(true);
    const box = el.getBoundingClientRect();
    const total = horizontal ? box.width : box.height;
    const splitLen = evalLin(horizontal ? geom.splitRect.w : geom.splitRect.h, total);
    const avail = splitLen - GUTTER * (split.children.length - 1);
    const startSizes = normSizes(split.sizes, split.children.length);
    const start = horizontal ? e.clientX : e.clientY;
    const minPx = (i: number) => leavesAlong(split.children[i], geom.direction) * (horizontal ? MIN_PANE_W : MIN_PANE_H);
    let frame = 0;
    let latest = start;
    const apply = () => {
      frame = 0;
      useWorkspace.getState().resizeSplit(geom.splitId, dragSizes(startSizes, geom.index, latest - start, avail, minPx));
    };
    const target = e.currentTarget;
    const onMove = (ev: PointerEvent) => {
      latest = horizontal ? ev.clientX : ev.clientY;
      if (!frame) frame = requestAnimationFrame(apply);
    };
    const onUp = (ev: PointerEvent) => {
      target.releasePointerCapture(ev.pointerId);
      target.removeEventListener('pointermove', onMove);
      target.removeEventListener('pointerup', onUp);
      target.removeEventListener('pointercancel', onUp);
      if (frame) cancelAnimationFrame(frame);
      apply();
      setDragging(false);
    };
    target.addEventListener('pointermove', onMove);
    target.addEventListener('pointerup', onUp);
    target.addEventListener('pointercancel', onUp);
  };

  const onDoubleClick = () => {
    // Equalize the split's children.
    const tab = useWorkspace.getState().tabs.find((t) => t.id === tabId);
    const split = tab && findSplit(tab.layout, geom.splitId);
    if (split) useWorkspace.getState().resizeSplit(split.id, split.children.map(() => 1 / split.children.length));
  };

  const HIT = 3;
  const style = horizontal
    ? { left: `calc(${cssLin(r.x)} - ${HIT}px)`, top: cssLin(r.y), width: `${GUTTER + HIT * 2}px`, height: cssLin(r.h) }
    : { left: cssLin(r.x), top: `calc(${cssLin(r.y)} - ${HIT}px)`, width: cssLin(r.w), height: `${GUTTER + HIT * 2}px` };

  return (
    <div
      className={cx('splitter', horizontal ? 'splitter--row' : 'splitter--column', dragging && 'splitter--dragging')}
      style={style}
      role="separator"
      aria-orientation={horizontal ? 'vertical' : 'horizontal'}
      title="Drag to resize · double-click to equalize"
      onPointerDown={onPointerDown}
      onDoubleClick={onDoubleClick}
    />
  );
}

/* -------------------------------------------------------------------------- */

/** Header bar of a pane in a split tab: protocol, status, title, close button. */
function PaneHeader({ paneId, active }: { paneId: string; active: boolean }) {
  const titleMode = useSettings((s) => s.settings.appearance.tabTitle);
  const info = useWorkspace(
    useShallow((s) => {
      const p = s.panes[paneId];
      return p ? { title: paneTitle(p, titleMode), status: p.status, protocol: p.session.protocol } : null;
    })
  );
  if (!info) return null;
  const keys = shortcutLabel('pane.close');
  return (
    <div className={cx('pane-head', active && 'pane-head--active')}>
      <ProtocolIcon protocol={info.protocol} size={12} className="pane-head__icon" />
      <StatusDot status={info.status} />
      <span className="pane-head__title" title={info.title}>
        {info.title}
      </span>
      <button
        className="pane-head__close"
        aria-label="Close pane"
        title={keys ? `Close this pane (${keys})` : 'Close this pane'}
        onClick={() => void closePane(paneId)}
      >
        <X size={13} />
      </button>
    </div>
  );
}

function TabView({ tab, visible }: { tab: TabState; visible: boolean }) {
  const ref = useRef<HTMLDivElement>(null);
  const geom = useMemo(() => computeGeometry(tab.layout, GUTTER), [tab.layout]);
  const paneIds = useMemo(() => paneIdsOf(tab.layout), [tab.layout]);
  const kinds = useWorkspace((s) => paneIds.map((id) => s.panes[id]?.kind ?? 'terminal').join(','));
  const multi = paneIds.length > 1;

  return (
    <div
      ref={ref}
      className={cx('tabview', visible && 'tabview--visible', multi && 'tabview--split')}
      role="tabpanel"
      aria-hidden={!visible}
    >
      {paneIds.map((id) => {
        const r = geom.panes.get(id);
        if (!r) return null;
        const active = id === tab.activePaneId;
        return (
          <div
            key={id}
            className={cx('pane-slot', multi && active && 'pane-slot--active', tab.broadcast && 'pane-slot--broadcast')}
            style={{ left: cssLin(r.x), top: cssLin(r.y), width: cssLin(r.w), height: cssLin(r.h) }}
            onMouseDownCapture={() => {
              const ws = useWorkspace.getState();
              const t = ws.tabs.find((x) => x.id === tab.id);
              if (t && t.activePaneId !== id) ws.activatePane(id);
            }}
          >
            {/* The content stays the first child so adding / removing the header never remounts it. */}
            <div key="body" className={cx('pane-slot__body', multi && 'pane-slot__body--headed')}>
              {kinds.split(',')[paneIds.indexOf(id)] === 'files' ? (
                <FileManager paneId={id} visible={visible} />
              ) : (
                <TerminalPane paneId={id} active={active} visible={visible} />
              )}
            </div>
            {multi && <PaneHeader key="head" paneId={id} active={active} />}
          </div>
        );
      })}
      {geom.splitters.map((s) => (
        <Splitter key={`${s.splitId}:${s.index}`} geom={s} tabId={tab.id} containerRef={ref} />
      ))}
    </div>
  );
}

/* -------------------------------------------------------------------------- */

const SFTP_WIDTH_KEY = 'shell.sftpWidth';
const clampSftpWidth = (w: number) => Math.min(Math.max(320, window.innerWidth - 360), Math.max(280, w));

/** Resizable right-hand container of one tab's SFTP panel. */
function SftpSide({ paneId, visible, width, onWidth, onClose }: {
  paneId: string;
  visible: boolean;
  width: number;
  onWidth: (w: number, done: boolean) => void;
  onClose: () => void;
}) {
  const onPointerDown = (e: ReactPointerEvent<HTMLDivElement>) => {
    if (e.button !== 0) return;
    e.preventDefault();
    const target = e.currentTarget;
    target.setPointerCapture(e.pointerId);
    const startX = e.clientX;
    const startW = width;
    let latest = startW;
    const onMove = (ev: PointerEvent) => onWidth((latest = clampSftpWidth(startW - (ev.clientX - startX))), false);
    const onUp = (ev: PointerEvent) => {
      target.releasePointerCapture(ev.pointerId);
      target.removeEventListener('pointermove', onMove);
      target.removeEventListener('pointerup', onUp);
      target.removeEventListener('pointercancel', onUp);
      onWidth(latest, true);
    };
    target.addEventListener('pointermove', onMove);
    target.addEventListener('pointerup', onUp);
    target.addEventListener('pointercancel', onUp);
  };

  return (
    <aside className={cx('sftp-side', !visible && 'sftp-side--hidden')} style={{ width }} aria-hidden={!visible}>
      <div className="sftp-side__resizer" role="separator" aria-orientation="vertical" title="Drag to resize" onPointerDown={onPointerDown} />
      <div className="sftp-side__body">
        <SftpPanel paneId={paneId} visible={visible} onClose={onClose} />
      </div>
    </aside>
  );
}

/* -------------------------------------------------------------------------- */

export function Workspace() {
  const tabs = useWorkspace((s) => s.tabs);
  const activeTabId = useWorkspace((s) => s.activeTabId);
  const panels = useUi((s) => s.sftpPanels);
  // [tabId, paneId, tabId, paneId, ...] of the panels whose tab still holds their pane.
  const live = useWorkspace(useShallow((s) => Object.entries(liveSftpPanels(panels, s.panes)).flat()));
  const [sftpWidth, setSftpWidth] = useState(420);

  useEffect(() => {
    api.app
      .getState<number>(SFTP_WIDTH_KEY)
      .then((w) => {
        if (typeof w === 'number' && Number.isFinite(w)) setSftpWidth(clampSftpWidth(w));
      })
      .catch(() => undefined);
  }, []);

  // Forget panels of closed tabs and panes.
  useEffect(() => {
    if (live.length / 2 !== Object.keys(panels).length) useUi.setState({ sftpPanels: liveSftpPanels(panels, useWorkspace.getState().panes) });
  }, [live, panels]);

  const sides: [string, string][] = [];
  for (let i = 0; i + 1 < live.length; i += 2) sides.push([live[i], live[i + 1]]);

  return (
    <div className="workspace">
      <div className="workspace__main">
        {tabs.map((t) => (
          <TabView key={t.id} tab={t} visible={t.id === activeTabId} />
        ))}
        {tabs.length === 0 && <Welcome />}
      </div>
      {sides.map(([tabId, paneId]) => (
        <SftpSide
          key={`${tabId}:${paneId}`}
          paneId={paneId}
          visible={tabId === activeTabId}
          width={sftpWidth}
          onWidth={(w, done) => {
            setSftpWidth(w);
            if (done) api.app.setState(SFTP_WIDTH_KEY, w).catch(() => undefined);
          }}
          onClose={() => useUi.getState().setSftpPanel(tabId, null)}
        />
      ))}
    </div>
  );
}

