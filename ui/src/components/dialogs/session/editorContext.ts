/**
 * Context shared by the session editor's section components.
 */
import { createContext, useContext } from 'react';
import type { AppSettings, ColorScheme, SessionConfig } from '@shared/types';
import type { NestedConfirmOptions } from '../common/NestedConfirm';

export interface PasswordChange {
  /** 'set' = store `value` on save, 'clear' = remove the stored password on save. */
  action: 'set' | 'clear';
  value: string;
}

export interface EditorContextValue {
  draft: SessionConfig;
  initial: SessionConfig;
  /** Apply a mutation to a clone of the draft. */
  update: (mutate: (d: SessionConfig) => void) => void;
  /** Error to display for a field path (respects pristine/attempted state). */
  fieldError: (path: string) => string | undefined;
  warning: (path: string) => string | undefined;
  errors: Record<string, string>;
  isDefault: boolean;
  isNew: boolean;
  settings: AppSettings;
  sessions: SessionConfig[];
  folders: string[];
  /** Imported colours (session.appearance.customScheme) kept while editing so the user can switch back. */
  stashedCustomScheme: ColorScheme | undefined;
  setStashedCustomScheme: (s: ColorScheme | undefined) => void;
  passwordChange: PasswordChange | null;
  setPasswordChange: (c: PasswordChange | null) => void;
  confirm: (o: NestedConfirmOptions) => Promise<boolean>;
}

export const EditorContext = createContext<EditorContextValue | null>(null);

export function useEditor(): EditorContextValue {
  const ctx = useContext(EditorContext);
  if (!ctx) throw new Error('useEditor must be used inside the session editor');
  return ctx;
}

/** Read a dotted path ('proxy.host') from an object. */
export function getPath(obj: unknown, path: string): unknown {
  let cur: unknown = obj;
  for (const k of path.split('.')) {
    if (cur === null || typeof cur !== 'object') return undefined;
    cur = (cur as Record<string, unknown>)[k];
  }
  return cur;
}
