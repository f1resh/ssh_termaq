/**
 * Applies the active app theme to the document and keeps the native host in sync (theme source for the
 * window frame / native dialogs, Mica/Acrylic backdrop, UI zoom). Render <ThemeController/> once.
 *
 * Also exposes hooks other components use to resolve colours reactively:
 *   useSystemDark()            Windows "dark mode" preference (live)
 *   useAppTheme()              resolved AppTheme
 *   useTerminalScheme(session) resolved terminal scheme for a session (or the global default)
 */
import { useEffect, useMemo } from 'react';
import { create } from 'zustand';
import { api, isNativeHost } from '@/bridge/api';
import { useSettings } from '@/store/data';
import type { AppTheme, ColorScheme, SessionConfig } from '@shared/types';
import { applyAppTheme } from './applyTheme';
import { resolveAppTheme, resolveTerminalScheme } from './resolve';

interface SystemThemeState {
  dark: boolean;
  known: boolean;
}

const initialDark = (() => {
  try {
    return globalThis.matchMedia?.('(prefers-color-scheme: dark)').matches ?? true;
  } catch {
    return true;
  }
})();

export const useSystemThemeStore = create<SystemThemeState>(() => ({ dark: initialDark, known: false }));

export function useSystemDark(): boolean {
  return useSystemThemeStore((s) => s.dark);
}

export function useAppTheme(): AppTheme {
  const settings = useSettings((s) => s.settings);
  const dark = useSystemDark();
  return useMemo(() => resolveAppTheme(settings, dark), [settings, dark]);
}

export function useTerminalScheme(session?: SessionConfig): ColorScheme {
  const settings = useSettings((s) => s.settings);
  const dark = useSystemDark();
  return useMemo(() => resolveTerminalScheme(settings, session, dark), [settings, session, dark]);
}

let systemSubscribed = false;

/** Subscribes to the OS light/dark preference via the host (idempotent). */
function initSystemTheme(): void {
  if (systemSubscribed) return;
  systemSubscribed = true;
  api.app
    .systemPrefersDark()
    .then((dark) => {
      if (typeof dark === 'boolean') useSystemThemeStore.setState({ dark, known: true });
    })
    .catch(() => undefined);
  api.app.onSystemThemeChanged((dark) => useSystemThemeStore.setState({ dark: !!dark, known: true }));
}

export function ThemeController(): null {
  const appearance = useSettings((s) => s.settings.appearance);
  const loaded = useSettings((s) => s.loaded);
  const settings = useSettings((s) => s.settings);
  const dark = useSystemDark();
  const theme = useMemo(() => resolveAppTheme(settings, dark), [settings, dark]);
  const scheme = useMemo(() => resolveTerminalScheme(settings, undefined, dark), [settings, dark]);

  useEffect(() => initSystemTheme(), []);

  // In a plain browser (mock) there is no translucent window behind the page, so materials are only
  // rendered inside the native host.
  const material = isNativeHost() ? appearance.backgroundMaterial : 'none';

  useEffect(() => {
    applyAppTheme(theme, appearance.accentColor || undefined, material);
    const root = document.documentElement;
    root.style.setProperty('--terminal-bg', scheme.background);
    root.style.setProperty('--terminal-fg', scheme.foreground);
  }, [theme, appearance.accentColor, material, scheme]);

  useEffect(() => {
    if (!loaded) return;
    api.app.setThemeSource(appearance.theme === 'system' ? 'system' : theme.variant);
  }, [loaded, appearance.theme, theme.variant]);

  useEffect(() => {
    if (!loaded) return;
    api.app.setBackgroundMaterial(appearance.backgroundMaterial);
  }, [loaded, appearance.backgroundMaterial]);

  useEffect(() => {
    if (!loaded) return;
    const scale = Number.isFinite(appearance.uiScale) && appearance.uiScale > 0 ? appearance.uiScale : 1;
    api.app.setUiScale(Math.min(3, Math.max(0.5, scale)));
  }, [loaded, appearance.uiScale]);

  return null;
}
