/**
 * Theme resolution helpers (pure; no DOM access).
 *
 *   resolveAppTheme       settings.appearance.theme ('system' follows Windows light/dark)
 *   resolveTerminalScheme session customScheme > session colorScheme > global scheme > app theme default
 *   allSchemes            built-ins + user schemes (a user scheme with a built-in id overrides it)
 */
import type { AppSettings, AppTheme, ColorScheme, SessionConfig } from '@shared/types';
import { APP_THEMES } from './appThemes';
import { BUILTIN_SCHEMES } from './schemes';

export function allSchemes(settings: AppSettings): ColorScheme[] {
  const custom = settings.customSchemes ?? [];
  if (!custom.length) return BUILTIN_SCHEMES;
  const overridden = new Set(custom.map((s) => s.id));
  return [...BUILTIN_SCHEMES.filter((s) => !overridden.has(s.id)), ...custom];
}

/** The concrete theme id for a settings theme value ('system' -> 'dark' / 'light'). */
export function resolveAppThemeId(themeSetting: string, systemDark: boolean): string {
  if (!themeSetting || themeSetting === 'system') return systemDark ? 'dark' : 'light';
  return themeSetting;
}

/** Resolve the app theme id ('system' follows the OS preference). */
export function resolveAppTheme(settings: AppSettings, systemDark: boolean): AppTheme {
  const id = resolveAppThemeId(settings.appearance.theme, systemDark);
  return APP_THEMES.find((t) => t.id === id) ?? (systemDark ? APP_THEMES[0] : APP_THEMES[1]) ?? APP_THEMES[0];
}

/** Terminal colours for a session: session customScheme > session colorScheme > global scheme > app theme default. */
export function resolveTerminalScheme(settings: AppSettings, session: SessionConfig | undefined, systemDark: boolean): ColorScheme {
  if (session?.appearance?.customScheme) return session.appearance.customScheme;
  const schemes = allSchemes(settings);
  const wanted = session?.appearance?.colorScheme || settings.terminal.colorScheme;
  const id = !wanted || wanted === 'auto' ? resolveAppTheme(settings, systemDark).defaultScheme : wanted;
  return schemes.find((s) => s.id === id) ?? schemes.find((s) => s.id === resolveAppTheme(settings, systemDark).defaultScheme) ?? schemes[0];
}

/* -------------------------------------------------------------------------- */
/* Colour utilities                                                            */
/* -------------------------------------------------------------------------- */

/** Parses #rgb / #rrggbb / #rrggbbaa into 0..255 channels; null for anything else. */
export function parseHex(color: string): { r: number; g: number; b: number } | null {
  const m = /^#([0-9a-f]{3}|[0-9a-f]{6}|[0-9a-f]{8})$/i.exec(color.trim());
  if (!m) return null;
  let h = m[1];
  if (h.length === 3) h = h.split('').map((c) => c + c).join('');
  return { r: parseInt(h.slice(0, 2), 16), g: parseInt(h.slice(2, 4), 16), b: parseInt(h.slice(4, 6), 16) };
}

/** WCAG relative luminance (0 = black, 1 = white); null for non-hex colours. */
export function relativeLuminance(color: string): number | null {
  const c = parseHex(color);
  if (!c) return null;
  const lin = (v: number) => {
    const s = v / 255;
    return s <= 0.03928 ? s / 12.92 : ((s + 0.055) / 1.055) ** 2.4;
  };
  return 0.2126 * lin(c.r) + 0.7152 * lin(c.g) + 0.0722 * lin(c.b);
}

/** Black or white, whichever reads better on `background`. */
export function readableOn(background: string, light = '#ffffff', dark = '#111318'): string {
  const l = relativeLuminance(background);
  if (l === null) return light;
  const contrastWhite = 1.05 / (l + 0.05);
  const contrastBlack = (l + 0.05) / 0.05;
  return contrastWhite >= contrastBlack * 0.8 ? light : dark;
}
