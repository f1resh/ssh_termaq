/**
 * Main content area: every open tab stays mounted (only the active one is visible, so terminals keep their
 * state) and renders its split layout with draggable splitters; a file manager tab holds a single files pane.
 * Panes are absolutely positioned siblings keyed by pane id — splitting or closing never remounts a terminal.
 */
import { useMemo, useRef, useState, type PointerEvent as ReactPointerEvent, type RefObject } from 'react';
import { cx } from '@/components/ui';
import { FileManager } from '@/components/files/FileManager';
import { Welcome } from '@/components/welcome/Welcome';
import { TerminalPane } from '@/terminal/TerminalPane';
import { paneIdsOf, useWorkspace, type TabState } from '@/store/workspace';
import { computeGeometry, cssLin, dragSizes, evalLin, findSplit, leavesAlong, normSizes, type SplitterGeom } from './paneGeometry';
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
            {kinds.split(',')[paneIds.indexOf(id)] === 'files' ? (
              <FileManager paneId={id} visible={visible} />
            ) : (
              <TerminalPane paneId={id} active={active} visible={visible} />
            )}
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

/* -------------------------------------------------------------------------- */

export function Workspace() {
  const tabs = useWorkspace((s) => s.tabs);
  const activeTabId = useWorkspace((s) => s.activeTabId);

  return (
    <div className="workspace">
      <div className="workspace__main">
        {tabs.map((t) => (
          <TabView key={t.id} tab={t} visible={t.id === activeTabId} />
        ))}
        {tabs.length === 0 && <Welcome />}
      </div>
    </div>
  );
}

