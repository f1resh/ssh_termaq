/**
 * Terminal font helpers: curated monospace suggestions, installed-font discovery via the Local Font Access
 * API (window.queryLocalFonts, Chromium/WebView2) and CSS font-family string helpers.
 */

/** Popular monospace families (shown first; Windows-bundled ones at the top). */
export const MONO_FONT_SUGGESTIONS = [
  'Cascadia Mono',
  'Cascadia Code',
  'Consolas',
  'Lucida Console',
  'Courier New',
  'JetBrains Mono',
  'Fira Code',
  'Fira Mono',
  'Source Code Pro',
  'Hack',
  'Iosevka',
  'IBM Plex Mono',
  'Ubuntu Mono',
  'Roboto Mono',
  'DejaVu Sans Mono',
  'Inconsolata',
  'Victor Mono',
  'MesloLGS NF',
  'CaskaydiaCove Nerd Font'
] as const;

interface LocalFontData {
  family: string;
  fullName: string;
  style: string;
}

let installedCache: Promise<string[]> | null = null;

/** Whether the Local Font Access API is available in this runtime. */
export function canQueryLocalFonts(): boolean {
  return typeof (globalThis as { queryLocalFonts?: unknown }).queryLocalFonts === 'function';
}

/**
 * Installed font family names (unique, sorted). Needs transient user activation the first time (the runtime
 * may show a permission prompt), so call it from a click/focus handler. Resolves [] when unavailable/denied.
 */
export function queryInstalledFonts(): Promise<string[]> {
  if (!canQueryLocalFonts()) return Promise.resolve([]);
  if (!installedCache) {
    const q = (globalThis as unknown as { queryLocalFonts: () => Promise<LocalFontData[]> }).queryLocalFonts;
    installedCache = q()
      .then((fonts) => [...new Set(fonts.map((f) => f.family))].sort((a, b) => a.localeCompare(b)))
      .catch(() => {
        installedCache = null;
        return [];
      });
  }
  return installedCache;
}

/** First family of a CSS font-family list, unquoted ("'Cascadia Mono', Consolas" -> "Cascadia Mono"). */
export function primaryFamily(css: string): string {
  const first = splitFamilies(css)[0] ?? '';
  return first;
}

/** Splits a CSS font-family list into unquoted family names. */
export function splitFamilies(css: string): string[] {
  const out: string[] = [];
  let cur = '';
  let quote: string | null = null;
  for (const ch of css) {
    if (quote) {
      if (ch === quote) quote = null;
      else cur += ch;
    } else if (ch === '"' || ch === "'") {
      quote = ch;
    } else if (ch === ',') {
      if (cur.trim()) out.push(cur.trim());
      cur = '';
    } else {
      cur += ch;
    }
  }
  if (cur.trim()) out.push(cur.trim());
  return out;
}

const GENERIC = new Set(['monospace', 'serif', 'sans-serif', 'cursive', 'fantasy', 'system-ui', 'ui-monospace']);

/** CSS font-family value for a picked family, with a monospace fallback. */
export function familyToCss(family: string): string {
  const f = family.trim();
  if (!f) return '';
  if (f.includes(',')) return f; // already a list
  if (GENERIC.has(f.toLowerCase())) return f;
  return `'${f.replace(/'/g, "\\'")}', monospace`;
}
