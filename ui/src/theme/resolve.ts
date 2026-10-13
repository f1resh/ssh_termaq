/**
 * Theme resolution helpers. (Basic version — refined by the shell implementation.)
 */
import type { AppSettings, AppTheme, ColorScheme, SessionConfig } from '@shared/types';
import { APP_THEMES } from './appThemes';
import { BUILTIN_SCHEMES } from './schemes';

export function allSchemes(settings: AppSettings): ColorScheme[] {
  return [...BUILTIN_SCHEMES, ...settings.customSchemes];
}

/** Resolve the app theme id ('system' follows the OS preference). */
export function resolveAppTheme(settings: AppSettings, systemDark: boolean): AppTheme {
  const id = settings.appearance.theme === 'system' ? (systemDark ? 'dark' : 'light') : settings.appearance.theme;
  return APP_THEMES.find((t) => t.id === id) ?? APP_THEMES[0];
}

/** Terminal colours for a session: session customScheme > session colorScheme > global scheme > app theme default. */
export function resolveTerminalScheme(settings: AppSettings, session: SessionConfig | undefined, systemDark: boolean): ColorScheme {
  if (session?.appearance.customScheme) return session.appearance.customScheme;
  const schemes = allSchemes(settings);
  const wanted = session?.appearance.colorScheme || settings.terminal.colorScheme;
  const id = !wanted || wanted === 'auto' ? resolveAppTheme(settings, systemDark).defaultScheme : wanted;
  return schemes.find((s) => s.id === id) ?? schemes[0];
}
