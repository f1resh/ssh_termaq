/**
 * Small React hooks shared by the dialogs.
 *
 *  - useStableCallback: identity-stable callback that always calls the latest closure. The shared <Modal/>
 *    re-runs its focus/keyboard effect whenever `onClose` changes identity, so every onClose handed to a
 *    Modal must be stable or focus would jump back to the first field on each render.
 *  - useBuffered: local edit buffer for inputs bound to async, host-backed state (settings). Controlled inputs
 *    whose value only updates after a bridge round trip would otherwise revert keystrokes.
 *  - Close guards: let nested layers (inline confirms, chord recording) intercept Escape / close requests
 *    before the owning dialog closes.
 */
import { createContext, useCallback, useContext, useEffect, useLayoutEffect, useMemo, useRef, useState } from 'react';

export function useStableCallback<A extends unknown[], R>(fn: (...args: A) => R): (...args: A) => R {
  const ref = useRef(fn);
  useLayoutEffect(() => {
    ref.current = fn;
  });
  return useCallback((...args: A) => ref.current(...args), []);
}

/**
 * Local buffer for a value that is committed asynchronously. Edits update the buffer immediately and are
 * committed after `delay` ms of inactivity (or right away via `flush`). While no edit is pending the buffer
 * follows the external value.
 */
export function useBuffered<T>(value: T, commit: (v: T) => void, delay = 300): [T, (v: T) => void, () => void] {
  const [local, setLocal] = useState(value);
  const pending = useRef<{ v: T } | null>(null);
  const timer = useRef<ReturnType<typeof setTimeout> | null>(null);
  const commitRef = useRef(commit);
  commitRef.current = commit;

  const flush = useCallback(() => {
    if (timer.current) clearTimeout(timer.current);
    timer.current = null;
    const p = pending.current;
    pending.current = null;
    if (p) commitRef.current(p.v);
  }, []);

  const set = useCallback(
    (v: T) => {
      setLocal(v);
      pending.current = { v };
      if (timer.current) clearTimeout(timer.current);
      timer.current = setTimeout(flush, delay);
    },
    [delay, flush]
  );

  // Follow external changes when nothing is pending.
  useEffect(() => {
    if (!pending.current) setLocal(value);
  }, [value]);

  // Commit outstanding edits on unmount.
  useEffect(() => flush, [flush]);

  return [local, set, flush];
}

/* -------------------------------------------------------------------------- */
/* Close guards                                                               */
/* -------------------------------------------------------------------------- */

/** Returns true when the guard consumed the close request. */
export type CloseGuard = () => boolean;

interface GuardRegistry {
  add(g: CloseGuard): () => void;
  run(): boolean;
}

function createRegistry(): GuardRegistry {
  const guards: CloseGuard[] = [];
  return {
    add(g) {
      guards.push(g);
      return () => {
        const i = guards.lastIndexOf(g);
        if (i >= 0) guards.splice(i, 1);
      };
    },
    run() {
      for (let i = guards.length - 1; i >= 0; i--) if (guards[i]()) return true;
      return false;
    }
  };
}

export const CloseGuardContext = createContext<GuardRegistry | null>(null);

/**
 * Creates a guard registry for a dialog. Wrap the dialog content in
 * `<CloseGuardContext.Provider value={registry}>` and pass `guardedClose` as the Modal's onClose.
 */
export function useCloseGuards(close: () => void): { registry: GuardRegistry; guardedClose: () => void } {
  const registry = useMemo(createRegistry, []);
  const guardedClose = useStableCallback(() => {
    if (!registry.run()) close();
  });
  return { registry, guardedClose };
}

/** While `active`, a close request of the surrounding dialog calls `onIntercept` instead of closing. */
export function useCloseGuard(active: boolean, onIntercept: () => void): void {
  const registry = useContext(CloseGuardContext);
  const cb = useStableCallback(onIntercept);
  useEffect(() => {
    if (!active || !registry) return;
    return registry.add(() => {
      cb();
      return true;
    });
  }, [active, registry, cb]);
}

/** Focus an element shortly after mount (after the Modal's own initial-focus logic has run). */
export function useInitialFocus<T extends HTMLElement>(enabled = true) {
  const ref = useRef<T>(null);
  useEffect(() => {
    if (!enabled) return;
    const t = setTimeout(() => ref.current?.focus(), 30);
    return () => clearTimeout(t);
  }, [enabled]);
  return ref;
}

/** Runs an async loader on mount / when deps change; ignores results of stale runs. */
export function useAsync<T>(load: () => Promise<T>, deps: unknown[]): { data: T | undefined; error: string | null; loading: boolean; reload: () => void } {
  const [state, setState] = useState<{ data: T | undefined; error: string | null; loading: boolean }>({
    data: undefined,
    error: null,
    loading: true
  });
  const [tick, setTick] = useState(0);
  const loadRef = useRef(load);
  loadRef.current = load;
  useEffect(() => {
    let alive = true;
    setState((s) => ({ ...s, loading: true, error: null }));
    loadRef
      .current()
      .then((data) => alive && setState({ data, error: null, loading: false }))
      .catch((e: unknown) => alive && setState({ data: undefined, error: errorMessage(e), loading: false }));
    return () => {
      alive = false;
    };
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [...deps, tick]);
  const reload = useCallback(() => setTick((t) => t + 1), []);
  return { ...state, reload };
}

export function errorMessage(e: unknown): string {
  if (e instanceof Error) return e.message;
  if (typeof e === 'string') return e;
  try {
    return JSON.stringify(e);
  } catch {
    return String(e);
  }
}
