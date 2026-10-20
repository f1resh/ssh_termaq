/**
 * Pure geometry for a tab's split layout. Every coordinate is a linear form `a * total + b` (total = the
 * container width for x/w, height for y/h), so panes can be positioned with CSS calc() without measuring
 * the container first, and splitter drags can be converted to pixels once the container size is known.
 */
import type { LayoutNode, SplitDirection } from '@/store/workspace';

export interface Lin {
  a: number;
  b: number;
}

export interface LinRect {
  x: Lin;
  y: Lin;
  w: Lin;
  h: Lin;
}

export interface SplitterGeom {
  splitId: string;
  /** Splitter sits between children[index] and children[index + 1]. */
  index: number;
  direction: SplitDirection;
  /** Rect of the gap between the two children. */
  rect: LinRect;
  /** Rect of the whole split (for converting drags to fractions). */
  splitRect: LinRect;
  childCount: number;
}

export interface LayoutGeom {
  panes: Map<string, LinRect>;
  splitters: SplitterGeom[];
}

const lin = (a: number, b = 0): Lin => ({ a, b });
const add = (p: Lin, q: Lin): Lin => ({ a: p.a + q.a, b: p.b + q.b });
const scale = (p: Lin, k: number): Lin => ({ a: p.a * k, b: p.b * k });

export function evalLin(l: Lin, total: number): number {
  return l.a * total + l.b;
}

export function cssLin(l: Lin): string {
  const pct = +(l.a * 100).toFixed(4);
  const px = +l.b.toFixed(3);
  if (px === 0) return `${pct}%`;
  if (pct === 0) return `${px}px`;
  return `calc(${pct}% ${px < 0 ? '-' : '+'} ${Math.abs(px)}px)`;
}

/** Normalised sizes (positive, summing to 1) for a split's children. */
export function normSizes(sizes: number[], n: number): number[] {
  const s = Array.from({ length: n }, (_, i) => (Number.isFinite(sizes[i]) && sizes[i] > 0 ? sizes[i] : 1 / n));
  const total = s.reduce((x, y) => x + y, 0) || 1;
  return s.map((v) => v / total);
}

export function computeGeometry(root: LayoutNode, gutter: number): LayoutGeom {
  const panes = new Map<string, LinRect>();
  const splitters: SplitterGeom[] = [];
  const walk = (node: LayoutNode, r: LinRect) => {
    if (node.type === 'pane') {
      panes.set(node.paneId, r);
      return;
    }
    const n = node.children.length;
    const sizes = normSizes(node.sizes, n);
    const horizontal = node.direction === 'row';
    const len = horizontal ? r.w : r.h;
    const avail = add(len, lin(0, -gutter * (n - 1)));
    let pos = horizontal ? r.x : r.y;
    node.children.forEach((child, i) => {
      const size = scale(avail, sizes[i]);
      const cr: LinRect = horizontal ? { x: pos, y: r.y, w: size, h: r.h } : { x: r.x, y: pos, w: r.w, h: size };
      walk(child, cr);
      pos = add(pos, size);
      if (i < n - 1) {
        const gap = lin(0, gutter);
        splitters.push({
          splitId: node.id,
          index: i,
          direction: node.direction,
          rect: horizontal ? { x: pos, y: r.y, w: gap, h: r.h } : { x: r.x, y: pos, w: r.w, h: gap },
          splitRect: r,
          childCount: n
        });
        pos = add(pos, gap);
      }
    });
  };
  walk(root, { x: lin(0), y: lin(0), w: lin(1), h: lin(1) });
  return { panes, splitters };
}

/** How many panes sit side by side along `direction` (for minimum sizes). */
export function leavesAlong(node: LayoutNode, direction: SplitDirection): number {
  if (node.type === 'pane') return 1;
  const counts = node.children.map((c) => leavesAlong(c, direction));
  return node.direction === direction ? counts.reduce((a, b) => a + b, 0) : Math.max(...counts);
}

export function findSplit(node: LayoutNode, id: string): Extract<LayoutNode, { type: 'split' }> | undefined {
  if (node.type === 'pane') return undefined;
  if (node.id === id) return node;
  for (const c of node.children) {
    const f = findSplit(c, id);
    if (f) return f;
  }
  return undefined;
}

/**
 * New sizes after dragging the splitter between children[index] and children[index+1] by `deltaPx`.
 * `availPx` is the split's length minus its gutters; children keep at least `minPx(child)` pixels.
 */
export function dragSizes(
  sizes: number[],
  index: number,
  deltaPx: number,
  availPx: number,
  minPx: (i: number) => number
): number[] {
  const s = normSizes(sizes, sizes.length);
  if (availPx <= 0) return s;
  const a = s[index] * availPx;
  const b = s[index + 1] * availPx;
  const pair = a + b;
  const minA = Math.min(minPx(index), pair / 2);
  const minB = Math.min(minPx(index + 1), pair / 2);
  const na = Math.max(minA, Math.min(pair - minB, a + deltaPx));
  const out = [...s];
  out[index] = na / availPx;
  out[index + 1] = (pair - na) / availPx;
  return out;
}
