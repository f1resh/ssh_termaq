/**
 * Keybinding parsing/matching. Format: modifiers in the order Ctrl+Alt+Shift+Meta, then the key:
 * A-Z, 0-9, F1-F24, Tab, Enter, Escape, Space, Backspace, Delete, Insert, Home, End, PageUp, PageDown,
 * ArrowUp/Down/Left/Right, and unshifted punctuation from KeyboardEvent.code: = - , . / ; ' [ ] \ `
 *
 * Letters follow the active keyboard layout when it produces a Latin letter (so Ctrl+Shift+T means the key
 * labelled T on AZERTY too) and fall back to the physical key otherwise (Cyrillic, Greek...). Digits and
 * punctuation always use the physical (US-position) key so Shift does not change them.
 *
 * Bindings come from settings.keybindings (already merged with DEFAULT_KEYBINDINGS by normalizeSettings);
 * user-entered strings are normalised, so "shift+ctrl+t" and "Ctrl+Shift+T" are equivalent and a few
 * aliases are understood (Plus, Minus, Esc, Del, Left, PgUp, Return, Win/Cmd, ...).
 */
import { DEFAULT_KEYBINDINGS } from '@shared/defaults';
import type { CommandId } from '@shared/types';
import { useSettings } from '@/store/data';
import { useUi } from '@/store/ui';

type KeyLike = Pick<KeyboardEvent, 'key' | 'code' | 'ctrlKey' | 'altKey' | 'shiftKey' | 'metaKey'>;

const MODIFIER_ORDER = ['Ctrl', 'Alt', 'Shift', 'Meta'] as const;
type Modifier = (typeof MODIFIER_ORDER)[number];

const CODE_PUNCTUATION: Record<string, string> = {
  Equal: '=',
  Minus: '-',
  Comma: ',',
  Period: '.',
  Slash: '/',
  Semicolon: ';',
  Quote: "'",
  BracketLeft: '[',
  BracketRight: ']',
  Backslash: '\\',
  Backquote: '`',
  IntlBackslash: '\\',
  NumpadAdd: '=',
  NumpadSubtract: '-',
  NumpadDecimal: '.',
  NumpadComma: ',',
  NumpadDivide: '/',
  NumpadEqual: '='
};

const NAMED_KEYS = new Set([
  'Tab',
  'Enter',
  'Escape',
  'Space',
  'Backspace',
  'Delete',
  'Insert',
  'Home',
  'End',
  'PageUp',
  'PageDown',
  'ArrowUp',
  'ArrowDown',
  'ArrowLeft',
  'ArrowRight'
]);

/** e.key values that never form a chord on their own. */
const MODIFIER_KEYS = new Set(['Control', 'Alt', 'Shift', 'Meta', 'OS', 'AltGraph', 'CapsLock', 'NumLock', 'ScrollLock', 'Fn', 'FnLock', 'Hyper', 'Super']);

/** Aliases accepted in user-written bindings (lower-case) -> canonical key. */
const KEY_ALIASES: Record<string, string> = {
  plus: '=',
  '+': '=',
  equal: '=',
  equals: '=',
  minus: '-',
  dash: '-',
  hyphen: '-',
  comma: ',',
  period: '.',
  dot: '.',
  slash: '/',
  semicolon: ';',
  quote: "'",
  apostrophe: "'",
  backquote: '`',
  backtick: '`',
  grave: '`',
  tilde: '`',
  backslash: '\\',
  bracketleft: '[',
  bracketright: ']',
  esc: 'Escape',
  escape: 'Escape',
  del: 'Delete',
  delete: 'Delete',
  ins: 'Insert',
  insert: 'Insert',
  return: 'Enter',
  enter: 'Enter',
  tab: 'Tab',
  space: 'Space',
  spacebar: 'Space',
  backspace: 'Backspace',
  bksp: 'Backspace',
  home: 'Home',
  end: 'End',
  pgup: 'PageUp',
  pageup: 'PageUp',
  pgdn: 'PageDown',
  pgdown: 'PageDown',
  pagedown: 'PageDown',
  up: 'ArrowUp',
  down: 'ArrowDown',
  left: 'ArrowLeft',
  right: 'ArrowRight',
  arrowup: 'ArrowUp',
  arrowdown: 'ArrowDown',
  arrowleft: 'ArrowLeft',
  arrowright: 'ArrowRight'
};

const MODIFIER_ALIASES: Record<string, Modifier> = {
  ctrl: 'Ctrl',
  control: 'Ctrl',
  ctl: 'Ctrl',
  alt: 'Alt',
  option: 'Alt',
  opt: 'Alt',
  shift: 'Shift',
  meta: 'Meta',
  win: 'Meta',
  windows: 'Meta',
  cmd: 'Meta',
  command: 'Meta',
  super: 'Meta'
};

/** The key part of a chord for an event, or '' when the key cannot be bound. */
export function keyOf(e: Pick<KeyboardEvent, 'key' | 'code'>): string {
  const key = e.key ?? '';
  const code = e.code ?? '';
  if (MODIFIER_KEYS.has(key)) return '';
  if (/^ControlLeft|ControlRight|AltLeft|AltRight|ShiftLeft|ShiftRight|MetaLeft|MetaRight|OSLeft|OSRight$/.test(code)) return '';

  // Letters: prefer the layout's Latin letter, fall back to the physical key.
  if (key.length === 1 && /[a-z]/i.test(key)) return key.toUpperCase();
  const letter = /^Key([A-Z])$/.exec(code);
  if (letter) return letter[1];

  const digit = /^(?:Digit|Numpad)([0-9])$/.exec(code);
  if (digit) {
    // Numpad digits with NumLock off report navigation keys (Home, ArrowUp, ...).
    if (code.startsWith('Numpad') && NAMED_KEYS.has(key)) return key;
    return digit[1];
  }
  if (CODE_PUNCTUATION[code]) return CODE_PUNCTUATION[code];
  if (code === 'NumpadEnter') return 'Enter';
  if (/^F([1-9]|1[0-9]|2[0-4])$/.test(code)) return code;
  if (code === 'Space' || key === ' ') return 'Space';
  if (NAMED_KEYS.has(code)) return code;
  if (NAMED_KEYS.has(key)) return key;
  if (/^F([1-9]|1[0-9]|2[0-4])$/.test(key)) return key;
  if (key === 'Esc') return 'Escape';
  if (key === 'Del') return 'Delete';

  // No usable code (virtual keyboards / synthetic events): fall back to the produced character.
  if (!code && key.length === 1) {
    if (/[0-9]/.test(key)) return key;
    const punct = Object.values(CODE_PUNCTUATION).find((p) => p === key);
    if (punct) return punct;
    if (key === '+') return '=';
    if (key === '_') return '-';
  }
  return '';
}

/** Normalized chord string for a keyboard event, e.g. "Ctrl+Shift+T". */
export function chordOf(e: KeyLike): string {
  const key = keyOf(e);
  if (!key) return '';
  const mods: string[] = [];
  if (e.ctrlKey) mods.push('Ctrl');
  if (e.altKey) mods.push('Alt');
  if (e.shiftKey) mods.push('Shift');
  if (e.metaKey) mods.push('Meta');
  return [...mods, key].join('+');
}

/**
 * Normalizes a user-written binding into canonical form ("shift+ctrl+t" -> "Ctrl+Shift+T").
 * Returns '' for empty or unparsable input.
 */
export function normalizeChord(binding: string | undefined | null): string {
  if (!binding) return '';
  const raw = binding.trim();
  if (!raw) return '';
  // Split on '+' but keep a trailing literal '+' key ("Ctrl++" / "Ctrl+Plus").
  const parts: string[] = [];
  let cur = '';
  for (let i = 0; i < raw.length; i++) {
    const ch = raw[i];
    if (ch === '+' && cur.trim() !== '') {
      parts.push(cur.trim());
      cur = '';
    } else cur += ch;
  }
  if (cur.trim()) parts.push(cur.trim());
  if (!parts.length) return '';

  const mods = new Set<Modifier>();
  let key = '';
  for (const p of parts) {
    const lower = p.toLowerCase();
    const mod = MODIFIER_ALIASES[lower];
    if (mod && !key) {
      mods.add(mod);
      continue;
    }
    if (key) return ''; // two non-modifier keys
    key = canonicalKey(p);
    if (!key) return '';
  }
  if (!key) return '';
  return [...MODIFIER_ORDER.filter((m) => mods.has(m)), key].join('+');
}

function canonicalKey(p: string): string {
  const lower = p.toLowerCase();
  if (KEY_ALIASES[lower]) return KEY_ALIASES[lower];
  if (/^[a-z]$/i.test(p)) return p.toUpperCase();
  if (/^[0-9]$/.test(p)) return p;
  if (/^f([1-9]|1[0-9]|2[0-4])$/i.test(p)) return p.toUpperCase();
  if (Object.values(CODE_PUNCTUATION).includes(p)) return p;
  if (/^(?:digit|numpad)[0-9]$/i.test(p)) return p.slice(-1);
  if (/^key[a-z]$/i.test(p)) return p.slice(-1).toUpperCase();
  return '';
}

/** Human readable form for menus / tooltips ("Ctrl+Shift+=" -> "Ctrl+Shift+Plus" is avoided: shown as typed). */
export function formatChord(chord: string): string {
  if (!chord) return '';
  return chord
    .split('+')
    .map((p, i, arr) => {
      if (i < arr.length - 1) return p === 'Meta' ? 'Win' : p;
      switch (p) {
        case 'ArrowUp':
          return '↑';
        case 'ArrowDown':
          return '↓';
        case 'ArrowLeft':
          return '←';
        case 'ArrowRight':
          return '→';
        case 'Escape':
          return 'Esc';
        case 'Delete':
          return 'Del';
        case 'PageUp':
          return 'PgUp';
        case 'PageDown':
          return 'PgDn';
        default:
          return p === '' ? '+' : p;
      }
    })
    .join('+');
}

/* -------------------------------------------------------------------------- */
/* Lookup                                                                      */
/* -------------------------------------------------------------------------- */

type Bindings = Partial<Record<CommandId, string>>;

let cacheSource: Bindings | null = null;
let cacheMap = new Map<string, CommandId>();
let cacheById = new Map<CommandId, string>();

function effectiveBindings(): Bindings {
  return useSettings.getState().settings.keybindings ?? DEFAULT_KEYBINDINGS;
}

/** Builds chord -> command for a binding table (merged over the defaults). Later duplicates lose. */
export function buildKeymap(bindings: Bindings): Map<string, CommandId> {
  const merged: Bindings = { ...DEFAULT_KEYBINDINGS, ...bindings };
  const map = new Map<string, CommandId>();
  for (const [id, chord] of Object.entries(merged) as [CommandId, string][]) {
    const n = normalizeChord(chord);
    if (n && !map.has(n)) map.set(n, id);
  }
  return map;
}

function ensureCache(): void {
  const src = effectiveBindings();
  if (src === cacheSource) return;
  cacheSource = src;
  cacheMap = buildKeymap(src);
  cacheById = new Map();
  for (const [chord, id] of cacheMap) cacheById.set(id, chord);
}

/** Command bound to an event for an explicit binding table (pure; used by tests and the settings UI). */
export function matchChord(e: KeyLike, bindings: Bindings): CommandId | null {
  const chord = chordOf(e);
  if (!chord) return null;
  return buildKeymap(bindings).get(chord) ?? null;
}

/** The command bound to this event, if any. */
export function matchKeybinding(e: KeyboardEvent): CommandId | null {
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

/* -------------------------------------------------------------------------- */
/* Global listener                                                             */
/* -------------------------------------------------------------------------- */

/** Commands that may auto-repeat while the key is held. */
const REPEATABLE = new Set<CommandId>([
  'tab.next',
  'tab.prev',
  'pane.focusNext',
  'pane.focusPrev',
  'term.zoomIn',
  'term.zoomOut'
]);

/** Commands still honoured while a modal dialog is open. */
const ALLOWED_WITH_DIALOG = new Set<CommandId>(['app.fullscreen']);

function isTextEntry(el: Element | null): boolean {
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

/**
 * Decides whether a matched command should run for this event. Exported for tests.
 * - chords without Ctrl/Alt are left to focused text inputs (not xterm's helper textarea)
 * - while a dialog is open only a few global commands run (the dialog owns the keyboard)
 */
export function shouldHandle(
  id: CommandId,
  e: Pick<KeyboardEvent, 'ctrlKey' | 'altKey' | 'repeat'>,
  ctx: { dialogOpen: boolean; textEntryFocused: boolean }
): boolean {
  if (e.repeat && !REPEATABLE.has(id)) return false;
  if (ctx.dialogOpen && !ALLOWED_WITH_DIALOG.has(id)) return false;
  if (ctx.textEntryFocused && !e.ctrlKey && !e.altKey) return false;
  return true;
}

/**
 * Installs the global keydown listener (capture phase, so xterm never sees app shortcuts).
 * `run` executes a matched command. Returns an uninstall function.
 */
export function installKeybindings(run: (id: CommandId) => void): () => void {
  const onKeyDown = (e: KeyboardEvent) => {
    if (e.defaultPrevented || e.isComposing) return;
    const id = matchKeybinding(e);
    if (!id) return;
    const ui = useUi.getState();
    const dialogOpen = ui.dialog !== null;
    // The palette handles its own keys except its own toggle chord.
    if (ui.paletteOpen && id !== 'app.commandPalette' && id !== 'app.fullscreen') return;
    if (!shouldHandle(id, e, { dialogOpen, textEntryFocused: isTextEntry(document.activeElement) })) {
      // Swallow repeats of a bound chord so a held key doesn't leak into the terminal.
      if (e.repeat && !dialogOpen && !isTextEntry(document.activeElement)) {
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
