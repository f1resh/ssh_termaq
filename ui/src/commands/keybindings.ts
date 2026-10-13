/**
 * Keybindings: store-backed lookup + the global keydown listener. The chord format and the pure
 * parse/match helpers live in ./chords.ts (re-exported here so callers have a single import).
 *
 * Bindings come from settings.keybindings merged over DEFAULT_KEYBINDINGS. The listener runs in the
 * CAPTURE phase on window, so app shortcuts never reach xterm.js (which listens on its helper textarea).
 */
import type { CommandId } from '@shared/types';
import { useSettings } from '@/store/data';
import { useUi } from '@/store/ui';
import { buildKeymap, chordOf, formatChord, shouldHandle, type Bindings } from './chords';

export {
  buildKeymap,
  chordOf,
  formatChord,
  keyOf,
  matchChord,
  normalizeChord,
  shouldHandle,
  type Bindings,
  type KeyLike
} from './chords';

/* -------------------------------------------------------------------------- */
/* Lookup (cached per settings.keybindings object identity)                    */
/* -------------------------------------------------------------------------- */

let cacheSource: Bindings | null = null;
let cacheMap = new Map<string, CommandId>();
let cacheById = new Map<CommandId, string>();

function ensureCache(): void {
  const src = useSettings.getState().settings.keybindings ?? {};
  if (src === cacheSource) return;
  cacheSource = src;
  cacheMap = buildKeymap(src);
  cacheById = new Map();
  for (const [chord, id] of cacheMap) if (!cacheById.has(id)) cacheById.set(id, chord);
}

/** The command bound to this event under the current settings, if any. */
export function matchKeybinding(e: Pick<KeyboardEvent, 'key' | 'code' | 'ctrlKey' | 'altKey' | 'shiftKey' | 'metaKey'>): CommandId | null {
  const chord = chordOf(e);
  if (!chord) return null;
  ensureCache();
  return cacheMap.get(chord) ?? null;
}

/** Canonical chord currently bound to a command ('' when unbound). */
export function keybindingFor(id: CommandId): string {
  ensureCache();
  return cacheById.get(id) ?? '';
}

/** Display string for a command's shortcut ('' when unbound). */
export function shortcutLabel(id: CommandId): string {
  return formatChord(keybindingFor(id));
}

/** React hook variant: re-renders when keybindings change. */
export function useShortcutLabel(id: CommandId): string {
  useSettings((s) => s.settings.keybindings);
  return shortcutLabel(id);
}

/* -------------------------------------------------------------------------- */
/* Global listener                                                             */
/* -------------------------------------------------------------------------- */

/** True for focused elements that consume plain typing (inputs, textareas, contenteditable) — not xterm's textarea. */
export function isTextEntry(el: Element | null): boolean {
  if (!el) return false;
  const h = el as HTMLElement;
  if (h.classList?.contains('xterm-helper-textarea')) return false;
  if (h.isContentEditable) return true;
  if (el instanceof HTMLTextAreaElement) return true;
  if (el instanceof HTMLSelectElement) return true;
  if (el instanceof HTMLInputElement) {
    const t = (el.type || 'text').toLowerCase();
    return !['checkbox', 'radio', 'button', 'submit', 'reset', 'range', 'color', 'file', 'image'].includes(t);
  }
  return false;
}

/** Elements can opt out of global shortcuts (e.g. a keybinding recorder) with data-capture-keys. */
function capturesKeys(el: Element | null): boolean {
  return !!(el as HTMLElement | null)?.closest?.('[data-capture-keys]');
}

/**
 * Installs the global keydown listener (capture phase, so xterm never sees app shortcuts).
 * `run` executes a matched command. Returns an uninstall function.
 */
export function installKeybindings(run: (id: CommandId) => void): () => void {
  const onKeyDown = (e: KeyboardEvent) => {
    if (e.defaultPrevented || e.isComposing) return;
    const active = document.activeElement;
    if (capturesKeys(active)) return;
    const id = matchKeybinding(e);
    if (!id) return;
    const ui = useUi.getState();
    const dialogOpen = ui.dialog !== null;
    const textEntryFocused = isTextEntry(active);
    // The palette handles its own keys except its own toggle chord and fullscreen.
    if (ui.paletteOpen && id !== 'app.commandPalette' && id !== 'app.fullscreen') return;
    if (!shouldHandle(id, e, { dialogOpen, textEntryFocused })) {
      // Swallow repeats of a bound chord so a held key doesn't leak into the terminal.
      if (e.repeat && !dialogOpen && !textEntryFocused) {
        e.preventDefault();
        e.stopPropagation();
      }
      return;
    }
    e.preventDefault();
    e.stopPropagation();
    run(id);
  };
  window.addEventListener('keydown', onKeyDown, true);
  return () => window.removeEventListener('keydown', onKeyDown, true);
}
