/**
 * Transient UI state: which dialog is open, command palette, SFTP panel, sidebar search.
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
  /** Pane id whose SFTP browser is shown in the side panel (null = closed). */
  sftpPaneId: string | null;
  sidebarFilter: string;
  fullscreen: boolean;
  windowFocused: boolean;
  maximized: boolean;

  openDialog(d: DialogState): void;
  closeDialog(): void;
  setPalette(open: boolean, mode?: UiState['paletteMode']): void;
  setSftpPane(paneId: string | null): void;
  setSidebarFilter(v: string): void;
  set(patch: Partial<Pick<UiState, 'fullscreen' | 'windowFocused' | 'maximized'>>): void;
}

export const useUi = create<UiState>((set) => ({
  dialog: null,
  paletteOpen: false,
  paletteMode: 'all',
  sftpPaneId: null,
  sidebarFilter: '',
  fullscreen: false,
  windowFocused: true,
  maximized: false,
  openDialog: (dialog) => set({ dialog, paletteOpen: false }),
  closeDialog: () => set({ dialog: null }),
  setPalette: (paletteOpen, paletteMode = 'all') => set({ paletteOpen, paletteMode }),
  setSftpPane: (sftpPaneId) => set({ sftpPaneId }),
  setSidebarFilter: (sidebarFilter) => set({ sidebarFilter }),
  set: (patch) => set(patch)
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
