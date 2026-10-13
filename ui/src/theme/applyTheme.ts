import type { AppTheme, AppThemeColors, BackgroundMaterial } from '@shared/types';
import { readableOn } from './resolve';

const toVar = (k: string) => '--' + k.replace(/[A-Z]/g, (m) => '-' + m.toLowerCase());

/**
 * How opaque (in %) the window chrome stays when a Mica / Acrylic backdrop shows through. Terminal panes,
 * menus and dialogs stay opaque; only the large chrome surfaces become translucent.
 */
const MATERIAL_OPACITY: Record<Exclude<BackgroundMaterial, 'none'>, Partial<Record<keyof AppThemeColors, number>>> = {
  mica: { titlebar: 30, sidebar: 55, bg: 72, tabHover: 60 },
  acrylic: { titlebar: 45, sidebar: 62, bg: 78, tabHover: 65 }
};

/**
 * Writes the theme's colours as CSS variables on :root (bgElevated -> --bg-elevated).
 * `accentOverride` replaces the accent family (the foreground is picked for contrast); `material`
 * makes chrome surfaces translucent and sets `data-material` on :root.
 */
export function applyAppTheme(theme: AppTheme, accentOverride?: string, material: BackgroundMaterial = 'none'): void {
  const root = document.documentElement;
  const colors: AppThemeColors = { ...theme.colors };
  if (accentOverride) {
    colors.accent = accentOverride;
    colors.accentHover = `color-mix(in srgb, ${accentOverride} 85%, ${theme.variant === 'dark' ? 'white' : 'black'})`;
    colors.accentFg = readableOn(accentOverride);
    colors.info = accentOverride;
  }
  if (material !== 'none') {
    for (const [k, pct] of Object.entries(MATERIAL_OPACITY[material]) as [keyof AppThemeColors, number][]) {
      colors[k] = `color-mix(in srgb, ${colors[k]} ${pct}%, transparent)`;
    }
  }
  for (const [k, v] of Object.entries(colors)) root.style.setProperty(toVar(k), v);
  root.dataset.variant = theme.variant;
  root.dataset.theme = theme.id;
  root.dataset.material = material;
}
