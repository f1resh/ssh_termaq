/**
 * Model for the SSH › Algorithms preference lists.
 *
 * Stored value: algorithm names in preference order, enabled ones only; [] = library defaults.
 * Editor rows: every known algorithm (plus unknown names from the stored list, e.g. imported from PuTTY)
 * with an enabled flag, so disabled entries keep a position while editing.
 */
import { WEAK_ALGORITHMS } from '@shared/algorithms';

export interface AlgoRow {
  name: string;
  enabled: boolean;
  weak: boolean;
  /** Listed in the known catalogue (unknown names come from imports / older versions). */
  known: boolean;
}

/** Rows for a stored value. Empty value => catalogue order with weak algorithms disabled (library defaults). */
export function algoRows(catalogue: readonly string[], value: readonly string[]): AlgoRow[] {
  const known = new Set(catalogue);
  if (value.length === 0) {
    return catalogue.map((name) => ({ name, enabled: !WEAK_ALGORITHMS.has(name), weak: WEAK_ALGORITHMS.has(name), known: true }));
  }
  const seen = new Set<string>();
  const rows: AlgoRow[] = [];
  for (const name of value) {
    if (seen.has(name)) continue;
    seen.add(name);
    rows.push({ name, enabled: true, weak: WEAK_ALGORITHMS.has(name), known: known.has(name) });
  }
  for (const name of catalogue) {
    if (!seen.has(name)) rows.push({ name, enabled: false, weak: WEAK_ALGORITHMS.has(name), known: true });
  }
  return rows;
}

/** Stored value for rows: enabled names in order. */
export function rowsToValue(rows: readonly AlgoRow[]): string[] {
  return rows.filter((r) => r.enabled).map((r) => r.name);
}

/** Whether rows are equivalent to the library defaults (so [] can be stored). */
export function isDefaultRows(catalogue: readonly string[], rows: readonly AlgoRow[]): boolean {
  const def = rowsToValue(algoRows(catalogue, []));
  const cur = rowsToValue(rows);
  return def.length === cur.length && def.every((n, i) => n === cur[i]);
}

/** Move row `from` to index `to` (array splice semantics). */
export function moveRow<T>(rows: readonly T[], from: number, to: number): T[] {
  if (from === to || from < 0 || from >= rows.length) return [...rows];
  const out = [...rows];
  const [r] = out.splice(from, 1);
  out.splice(Math.max(0, Math.min(to, out.length)), 0, r);
  return out;
}

/** Value to store after an edit: [] when the result equals the library defaults. */
export function valueForRows(catalogue: readonly string[], rows: readonly AlgoRow[]): string[] {
  return isDefaultRows(catalogue, rows) ? [] : rowsToValue(rows);
}
