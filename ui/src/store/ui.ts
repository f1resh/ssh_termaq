/**
 * Transient UI state: which dialog is open, command palette, sidebar search.
 * Dialog components are rendered by <DialogHost/> based on `dialog.type`.
 */
import { create } from 'zustand';
import type { ImportSource, SessionConfig } from '@shared/types';

export type DialogState =
  | { type: 'sessionEditor'; sessionId?: string; draft?: SessionConfig; folder?: string; section?: string }
  | { type: 'settings'; section?: string }
  | { type: 'import'; source: ImportSource | 'json' }
  | { type: 'keygen' }
  | { type: 'about' }
  | { type: 'eventLog'; paneId: string }
  | { type: 'quickConnect'; initial?: string }
  | {
      type: 'confirm';
      title: string;
      message: string;
      confirmLabel?: string;
      danger?: boolean;
      onConfirm: () => void;
      onCancel?: () => void;
    }
  | {
      type: 'input';
      title: string;
      label: string;
      initial?: string;
      placeholder?: string;
      confirmLabel?: string;
      validate?: (v: string) => string | null;
      onSubmit: (value: string) => void;
    };

export interface UiState {
  dialog: DialogState | null;
  paletteOpen: boolean;
  paletteMode: 'all' | 'sessions' | 'commands';
  /** Sidebar search text. */
  sidebarFilter: string;
  fullscreen: boolean;
  windowFocused: boolean;
  maximized: boolean;
  /** Tab whose title is being edited inline in the tab strip (null = none). */
  renamingTabId: string | null;
  /** Initial query of the command palette when it opens (e.g. '>' or '@'). */
  paletteQuery: string;
  /**
   * Last request to give a pane keyboard focus (e.g. clicking its tab). The pane focuses its terminal, or the
   * password / passphrase prompt shown over it; `seq` changes on every request.
   */
  focusRequest: { paneId: string; seq: number } | null;
  /** SFTP side panels: tab id -> the pane whose remote files the tab's panel shows. */
  sftpPanels: Record<string, string>;

  openDialog(d: DialogState): void;
  closeDialog(): void;
  setPalette(open: boolean, mode?: UiState['paletteMode']): void;
  setSidebarFilter(v: string): void;
  set(patch: Partial<Pick<UiState, 'fullscreen' | 'windowFocused' | 'maximized'>>): void;
  setRenamingTab(tabId: string | null): void;
  /** Opens the palette with a prefilled query (mode derived from the prefix). */
  openPalette(query?: string): void;
  requestPaneFocus(paneId: string): void;
  /** Shows the tab's SFTP panel for `paneId`, or closes it (null). */
  setSftpPanel(tabId: string, paneId: string | null): void;
}

export const useUi = create<UiState>((set) => ({
  dialog: null,
  paletteOpen: false,
  paletteMode: 'all',
  sidebarFilter: '',
  fullscreen: false,
  windowFocused: true,
  maximized: false,
  renamingTabId: null,
  paletteQuery: '',
  focusRequest: null,
  sftpPanels: {},
  openDialog: (dialog) => set({ dialog, paletteOpen: false }),
  closeDialog: () => set({ dialog: null }),
  setPalette: (paletteOpen, paletteMode = 'all') =>
    set({ paletteOpen, paletteMode, paletteQuery: paletteMode === 'commands' ? '>' : paletteMode === 'sessions' ? '@' : '' }),
  setSidebarFilter: (sidebarFilter) => set({ sidebarFilter }),
  set: (patch) => set(patch),
  setRenamingTab: (renamingTabId) => set({ renamingTabId }),
  openPalette: (query = '') =>
    set({
      paletteOpen: true,
      paletteQuery: query,
      paletteMode: query.startsWith('>') ? 'commands' : query.startsWith('@') ? 'sessions' : 'all'
    }),
  requestPaneFocus: (paneId) => set((s) => ({ focusRequest: { paneId, seq: (s.focusRequest?.seq ?? 0) + 1 } })),
  setSftpPanel: (tabId, paneId) =>
    set((s) => {
      if ((s.sftpPanels[tabId] ?? null) === paneId) return {};
      const sftpPanels = { ...s.sftpPanels };
      if (paneId) sftpPanels[tabId] = paneId;
      else delete sftpPanels[tabId];
      return { sftpPanels };
    })
}));

/** Promise-based confirm helper built on the 'confirm' dialog. */
export function confirmDialog(opts: { title: string; message: string; confirmLabel?: string; danger?: boolean }): Promise<boolean> {
  return new Promise((resolve) => {
    useUi.getState().openDialog({
      type: 'confirm',
      ...opts,
      onConfirm: () => resolve(true),
      onCancel: () => resolve(false)
    });
  });
}

/** Promise-based text input helper built on the 'input' dialog. Resolves null when cancelled. */
export function inputDialog(opts: {
  title: string;
  label: string;
  initial?: string;
  placeholder?: string;
  confirmLabel?: string;
  validate?: (v: string) => string | null;
}): Promise<string | null> {
  return new Promise((resolve) => {
    let done = false;
    const unsub = useUi.subscribe((s) => {
      if (!done && s.dialog?.type !== 'input') {
        done = true;
        unsub();
        resolve(null);
      }
    });
    useUi.getState().openDialog({
      type: 'input',
      ...opts,
      onSubmit: (v) => {
        done = true;
        unsub();
        resolve(v);
      }
    });
  });
}
