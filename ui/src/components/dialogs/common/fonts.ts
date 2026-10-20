/**
 * Terminal font helpers: curated monospace suggestions, installed-font discovery (the Local Font Access API,
 * window.queryLocalFonts, in Chromium / WebView2; the host on macOS, whose WKWebView has none) and CSS font-family
 * string helpers.
 */
import { api } from '@/bridge/api';
import { isMac } from '@shared/platform';

/** Monospace families that come with the OS. */
const OS_MONO_FONTS = isMac ? ['Menlo', 'Monaco', 'Andale Mono', 'Courier New'] : ['Cascadia Mono', 'Cascadia Code', 'Consolas', 'Lucida Console', 'Courier New'];

/** Popular monospace families (shown first; the OS's own ones at the top). */
export const MONO_FONT_SUGGESTIONS = [
  ...OS_MONO_FONTS,
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

/** Whether installed fonts can be listed here (the Local Font Access API, or the macOS host). */
export function canQueryLocalFonts(): boolean {
  return typeof (globalThis as { queryLocalFonts?: unknown }).queryLocalFonts === 'function' || isMac;
}

/**
 * Installed font family names (unique, sorted). Needs transient user activation the first time (the runtime
 * may show a permission prompt), so call it from a click/focus handler. Resolves [] when unavailable/denied.
 */
export function queryInstalledFonts(): Promise<string[]> {
  if (!canQueryLocalFonts()) return Promise.resolve([]);
  if (!installedCache && typeof (globalThis as { queryLocalFonts?: unknown }).queryLocalFonts !== 'function') {
    installedCache = api.app.listFonts().catch(() => {
      installedCache = null;
      return [];
    });
  }
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
