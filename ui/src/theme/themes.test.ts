import { describe, expect, it } from 'vitest';
import { createDefaultSettings } from '@shared/defaults';
import type { AppThemeColors, ColorScheme } from '@shared/types';
import { APP_THEMES } from './appThemes';
import { relativeLuminance, resolveAppTheme, resolveAppThemeId, resolveTerminalScheme } from './resolve';
import { BUILTIN_SCHEMES } from './schemes';

const REQUIRED_COLORS: (keyof AppThemeColors)[] = [
  'bg', 'bgElevated', 'bgSunken', 'surface', 'surfaceHover', 'surfaceActive', 'border', 'borderStrong', 'text',
  'textMuted', 'textFaint', 'accent', 'accentHover', 'accentFg', 'danger', 'warning', 'success', 'info', 'titlebar',
  'titlebarFg', 'sidebar', 'tabActive', 'tabHover', 'overlay', 'scrollbar', 'scrollbarHover'
];
const SCHEME_COLORS: (keyof ColorScheme)[] = [
  'foreground', 'background', 'cursor', 'cursorAccent', 'selectionBackground', 'black', 'red', 'green', 'yellow', 'blue',
  'magenta', 'cyan', 'white', 'brightBlack', 'brightRed', 'brightGreen', 'brightYellow', 'brightBlue', 'brightMagenta',
  'brightCyan', 'brightWhite'
];
const HEX = /^#([0-9a-f]{3}|[0-9a-f]{6}|[0-9a-f]{8})$/i;

function contrast(a: string, b: string): number {
  const [la, lb] = [relativeLuminance(a)!, relativeLuminance(b)!];
  return (Math.max(la, lb) + 0.05) / (Math.min(la, lb) + 0.05);
}

describe('built-in themes and schemes', () => {
  it('every app theme has all colours and pairs with an existing terminal scheme', () => {
    const schemeIds = new Set(BUILTIN_SCHEMES.map((s) => s.id));
    expect(new Set(APP_THEMES.map((t) => t.id)).size).toBe(APP_THEMES.length);
    for (const t of APP_THEMES) {
      for (const key of REQUIRED_COLORS) expect(t.colors[key], `${t.id}.${key}`).toBeTruthy();
      expect(schemeIds.has(t.defaultScheme), `${t.id} -> ${t.defaultScheme}`).toBe(true);
    }
  });

  it('every terminal scheme has a unique id and hex colours', () => {
    expect(new Set(BUILTIN_SCHEMES.map((s) => s.id)).size).toBe(BUILTIN_SCHEMES.length);
    for (const s of BUILTIN_SCHEMES) for (const key of SCHEME_COLORS) expect(s[key], `${s.id}.${key}`).toMatch(HEX);
  });
});

describe('SSHTermaq Neon', () => {
  const neon = APP_THEMES.find((t) => t.id === 'neon')!;

  it('is the default theme, with its terminal scheme for "auto"', () => {
    const settings = createDefaultSettings();
    expect(settings.appearance.theme).toBe('neon');
    expect(settings.terminal.colorScheme).toBe('auto');
    for (const systemDark of [true, false]) {
      expect(resolveAppTheme(settings, systemDark).id).toBe('neon');
      expect(resolveTerminalScheme(settings, undefined, systemDark).id).toBe('neon');
    }
  });

  it('is dark, has the pink second accent, and button text stays readable', () => {
    expect(neon.variant).toBe('dark');
    expect(neon.colors.accent2).toBe('#ff3db8');
    expect(contrast(neon.colors.accentFg, neon.colors.accent)).toBeGreaterThanOrEqual(4.5);
    expect(contrast(neon.colors.text, neon.colors.bg)).toBeGreaterThanOrEqual(7);
    expect(contrast(neon.colors.textMuted, neon.colors.bg)).toBeGreaterThanOrEqual(4.5);
  });
});

describe('theme resolution', () => {
  it('"system" follows the OS, unknown ids fall back to Dark / Light by id', () => {
    expect(resolveAppThemeId('system', true)).toBe('dark');
    expect(resolveAppThemeId('system', false)).toBe('light');
    const settings = createDefaultSettings();
    settings.appearance.theme = 'no-such-theme';
    expect(resolveAppTheme(settings, true).id).toBe('dark');
    expect(resolveAppTheme(settings, false).id).toBe('light');
  });
});
