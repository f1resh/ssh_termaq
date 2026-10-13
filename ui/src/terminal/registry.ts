/**
 * Registry of live terminal instances, keyed by pane id. TerminalPane registers a handle on mount;
 * commands (copy/paste/find/zoom...) and other components act on terminals through it.
 */
export interface TerminalHandle {
  paneId: string;
  focus(): void;
  copySelection(): Promise<boolean>;
  paste(text?: string): Promise<void>;
  copyAll(): Promise<void>;
  selectAll(): void;
  clearScrollback(): void;
  reset(): void;
  openFind(): void;
  /** delta in px (+1 / -1); 0 resets to the configured size. */
  zoom(delta: number): void;
  scrollToBottom(): void;
  /** Send text as if typed (goes through broadcast handling). */
  sendText(text: string): void;
  getSelection(): string;
  /** Re-fit to container (after layout changes). */
  fit(): void;
}

const handles = new Map<string, TerminalHandle>();
const listeners = new Set<() => void>();

export const terminalRegistry = {
  register(h: TerminalHandle): () => void {
    handles.set(h.paneId, h);
    listeners.forEach((l) => l());
    return () => {
      if (handles.get(h.paneId) === h) handles.delete(h.paneId);
      listeners.forEach((l) => l());
    };
  },
  get(paneId: string | null | undefined): TerminalHandle | undefined {
    return paneId ? handles.get(paneId) : undefined;
  },
  all(): TerminalHandle[] {
    return [...handles.values()];
  },
  subscribe(l: () => void): () => void {
    listeners.add(l);
    return () => void listeners.delete(l);
  }
};
