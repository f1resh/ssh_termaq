/**
 * Stores mirroring host-side data: saved sessions/folders, settings, pending prompts.
 * Call `initDataStores()` once at startup (App.tsx); it loads data and subscribes to host change events.
 */
import { create } from 'zustand';
import { api } from '@/bridge/api';
import { createDefaultSettings, normalizeSettings } from '@shared/defaults';
import type { AppSettings, DeepPartial, PromptRequest, PromptResponse, SessionConfig } from '@shared/types';

/* -------------------------------------------------------------------------- */

interface SessionsState {
  sessions: SessionConfig[];
  folders: string[];
  loaded: boolean;
  reload(): Promise<void>;
  byId(id: string | null | undefined): SessionConfig | undefined;
}

export const useSessions = create<SessionsState>((set, get) => ({
  sessions: [],
  folders: [],
  loaded: false,
  async reload() {
    const [sessions, folders] = await Promise.all([api.sessions.list(), api.sessions.folders()]);
    set({ sessions, folders, loaded: true });
  },
  byId(id) {
    return id ? get().sessions.find((s) => s.id === id) : undefined;
  }
}));

/* -------------------------------------------------------------------------- */

interface SettingsState {
  settings: AppSettings;
  loaded: boolean;
  update(patch: DeepPartial<AppSettings>): Promise<void>;
}

export const useSettings = create<SettingsState>((set) => ({
  settings: createDefaultSettings(),
  loaded: false,
  async update(patch) {
    const s = await api.settings.update(patch);
    set({ settings: normalizeSettings(s) });
  }
}));

/* -------------------------------------------------------------------------- */

interface PromptsState {
  pending: PromptRequest[];
  respond(id: string, response: PromptResponse): void;
}

export const usePrompts = create<PromptsState>((set, get) => ({
  pending: [],
  respond(id, response) {
    api.prompts.respond(id, response);
    set({ pending: get().pending.filter((p) => p.id !== id) });
  }
}));

/* -------------------------------------------------------------------------- */

let initialized = false;

export async function initDataStores(): Promise<void> {
  if (initialized) return;
  initialized = true;

  api.sessions.onChanged(() => void useSessions.getState().reload());
  api.settings.onChanged((s) => useSettings.setState({ settings: normalizeSettings(s) }));
  api.prompts.onPrompt((p) => usePrompts.setState((st) => ({ pending: [...st.pending, p] })));
  api.prompts.onCancel((id) => usePrompts.setState((st) => ({ pending: st.pending.filter((p) => p.id !== id) })));

  const [settings] = await Promise.all([api.settings.get(), useSessions.getState().reload()]);
  useSettings.setState({ settings: normalizeSettings(settings), loaded: true });
}
