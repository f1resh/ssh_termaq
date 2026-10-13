import type { AppTheme, AppThemeColors } from '@shared/types';

const toVar = (k: string) => '--' + k.replace(/[A-Z]/g, (m) => '-' + m.toLowerCase());

/** Writes the theme's colours as CSS variables on :root (bgElevated -> --bg-elevated). */
export function applyAppTheme(theme: AppTheme, accentOverride?: string): void {
  const root = document.documentElement;
  const colors: AppThemeColors = { ...theme.colors };
  if (accentOverride) {
    colors.accent = accentOverride;
    colors.accentHover = `color-mix(in srgb, ${accentOverride} 85%, ${theme.variant === 'dark' ? 'white' : 'black'})`;
    colors.info = accentOverride;
  }
  for (const [k, v] of Object.entries(colors)) root.style.setProperty(toVar(k), v);
  root.dataset.variant = theme.variant;
  root.dataset.theme = theme.id;
}
