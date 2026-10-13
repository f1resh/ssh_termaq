/**
 * PuTTY keyboard emulation (Configuration > Terminal > Keyboard) on top of xterm.js.
 *
 * `translateKey()` is a pure function: given a keydown event, the session's KeyboardOptions and the terminal's
 * current modes it decides whether the key produces a PuTTY-specific byte sequence ('send'), triggers a
 * PuTTY-native local action (Shift+Insert paste, Ctrl+Insert copy, Shift/Ctrl+PageUp/PageDown scrolling,
 * Ctrl+Break) or is left to xterm.js ('default'). TerminalPane installs it via attachCustomKeyEventHandler.
 *
 * Sequences follow PuTTY 0.83 (terminal.c format_function_key / format_small_keypad_key / format_arrow_key /
 * format_numeric_keypad_key and window.c TranslateKey):
 *
 *   Function keys      'esc-n'     F1..F12 = ESC[11~ ESC[12~ ESC[13~ ESC[14~ ESC[15~ ESC[17~ .. ESC[21~ ESC[23~ ESC[24~
 *                                  (F13..F20 = ESC[25~ 26 28 29 31 32 33 34); Shift+F1..F10 = F11..F20
 *                      'linux'     as esc-n but F1..F5 = ESC[[A .. ESC[[E
 *                      'xterm-r6'  as esc-n but F1..F4 = ESC OP .. ESC OS
 *                      'vt400'     as esc-n; edit keys in physical VT220 order; keypad / * - = PF2..PF4
 *                      'vt100plus' codes 11..24 = ESC OP .. ESC O[
 *                      'sco'       F1..F12 = ESC[M..ESC[X, +Shift ESC[Y..ESC[j, +Ctrl ESC[k..ESC[v, +both ESC[w..ESC[{
 *                      'xterm'     (PuTTY "Xterm 216+", also xterm.js native) unmodified as xterm-r6; with modifiers
 *                                  ESC[1;<m>P..S for F1..F4 and ESC[<code>;<m>~ above, m = 1 + Shift + 2*Alt + 4*Ctrl
 *   Home/End           'standard' ESC[1~ / ESC[4~, 'rxvt' ESC[H / ESC Ow, 'xterm' ESC[H / ESC[F (SS3 in app cursor mode)
 *   Arrows             legacy modes: Ctrl toggles application mode (PuTTY default "Ctrl toggles app mode");
 *                      'xterm' mode: xterm-style modifier bitmap (xterm.js)
 *   Backspace          configured ^? / ^H; Shift+ or Ctrl+Backspace sends the other one
 *   Alt                prefixes ESC (legacy modes); Ctrl+Alt = AltGr unless ctrlAltIsAltGr is false, in which case
 *                      Ctrl+Alt+<letter> sends ESC + Ctrl-<letter>
 */
import type { FunctionKeyMode, KeyboardOptions } from '@shared/types';

/** The subset of KeyboardEvent the translator needs (so it can be unit-tested without a DOM). */
export interface KeyEventLike {
  key: string;
  code: string;
  shiftKey: boolean;
  ctrlKey: boolean;
  altKey: boolean;
  metaKey: boolean;
  /** AltGr held (KeyboardEvent.getModifierState('AltGraph')). */
  altGraph?: boolean;
}

/** Terminal modes set by the remote side (DECCKM / DECKPAM), taken from xterm's `term.modes`. */
export interface TerminalKeyModes {
  applicationCursorKeys: boolean;
  applicationKeypad: boolean;
}

export type KeyAction =
  /** Send these bytes to the connection (bypassing xterm.js' own key handling). */
  | { kind: 'send'; data: string }
  /** Let xterm.js handle the key. */
  | { kind: 'default' }
  | { kind: 'copy' }
  | { kind: 'paste' }
  /** Scroll the viewport locally: `pages` or `lines` (negative = up). */
  | { kind: 'scroll'; pages?: number; lines?: number }
  /** Send a special command (e.g. Ctrl+Break -> 'break'). */
  | { kind: 'special'; id: string };

const ESC = '\x1b';
const CSI = '\x1b[';
const SS3 = '\x1bO';

const DEFAULT: KeyAction = { kind: 'default' };
const send = (data: string): KeyAction => ({ kind: 'send', data });

/** PuTTY's key_number_to_tilde_code: F1..F20 -> the n of ESC[n~ (index 0 unused). */
const TILDE_CODE = [-1, 11, 12, 13, 14, 15, 17, 18, 19, 20, 21, 23, 24, 25, 26, 28, 29, 31, 32, 33, 34];

const SCO_FKEYS = 'MNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz@[\\]^_`{';

/** xterm modifier parameter: 1 + Shift + 2*Alt + 4*Ctrl. */
function modifierParam(shift: boolean, alt: boolean, ctrl: boolean): number {
  return 1 + (shift ? 1 : 0) + (alt ? 2 : 0) + (ctrl ? 4 : 0);
}

/**
 * Function key F<n> (1..20) in the given mode. Returns the sequence and whether Alt was encoded in it (so the
 * caller must not add the usual ESC prefix).
 */
export function formatFunctionKey(
  mode: FunctionKeyMode,
  n: number,
  shift: boolean,
  ctrl: boolean,
  alt: boolean
): { data: string; consumedAlt: boolean } {
  if (n < 1 || n > 20) return { data: '', consumedAlt: false };

  if (mode === 'sco') {
    let index = n <= 12 ? n - 1 : 0;
    if (shift) index += 12;
    if (ctrl) index += 24;
    return { data: CSI + SCO_FKEYS[index], consumedAlt: false };
  }

  if (mode === 'xterm') {
    const code = TILDE_CODE[n];
    if (shift || ctrl || alt) {
      const m = modifierParam(shift, alt, ctrl);
      const data = code >= 11 && code <= 14 ? `${CSI}1;${m}${String.fromCharCode(code - 11 + 0x50)}` : `${CSI}${code};${m}~`;
      return { data, consumedAlt: true };
    }
    if (code >= 11 && code <= 14) return { data: SS3 + String.fromCharCode(code - 11 + 0x50), consumedAlt: false };
    return { data: `${CSI}${code}~`, consumedAlt: false };
  }

  // Legacy modes: Shift+F1..F10 act as F11..F20.
  const index = shift && n <= 10 ? n + 10 : n;
  const code = TILDE_CODE[index];
  let data: string;
  if (mode === 'vt100plus' && code >= 11 && code <= 24) {
    let off = 0;
    if (code > 15) off++;
    if (code > 21) off++;
    data = SS3 + String.fromCharCode(0x50 + code - 11 - off);
  } else if (mode === 'linux' && code >= 11 && code <= 15) {
    data = CSI + '[' + String.fromCharCode(0x41 + code - 11);
  } else if (mode === 'xterm-r6' && code >= 11 && code <= 14) {
    data = SS3 + String.fromCharCode(0x50 + code - 11);
  } else {
    data = `${CSI}${code}~`;
  }
  return { data, consumedAlt: false };
}

export type SmallKeypadKey = 'Home' | 'Insert' | 'Delete' | 'End' | 'PageUp' | 'PageDown';
const SKK_CODE: Record<SmallKeypadKey, number> = { Home: 1, Insert: 2, Delete: 3, End: 4, PageUp: 5, PageDown: 6 };

/**
 * Editing keypad (Home/Insert/Delete/End/PageUp/PageDown) in a legacy function key mode, or with a non-xterm
 * Home/End setting. Returns null when xterm.js' native handling should be used.
 */
export function formatSmallKeypadKey(
  key: SmallKeypadKey,
  opts: Pick<KeyboardOptions, 'functionKeys' | 'homeEnd' | 'disableAppCursorKeys'>,
  modes: TerminalKeyModes,
  shift: boolean,
  ctrl: boolean,
  alt: boolean
): { data: string; consumedAlt: boolean } | null {
  const mode = opts.functionKeys;
  const isHomeEnd = key === 'Home' || key === 'End';

  if (mode === 'sco') {
    const sco: Record<SmallKeypadKey, string> = {
      Home: CSI + 'H',
      Insert: CSI + 'L',
      Delete: '\x7f',
      End: CSI + 'F',
      PageUp: CSI + 'I',
      PageDown: CSI + 'G'
    };
    return { data: sco[key], consumedAlt: false };
  }

  // Home/End follow the Home/End setting (applied to the logical keys, before any VT400 re-ordering).
  if (isHomeEnd) {
    if (opts.homeEnd === 'rxvt') return { data: key === 'Home' ? CSI + 'H' : SS3 + 'w', consumedAlt: false };
    if (opts.homeEnd === 'xterm') {
      if (mode === 'xterm') {
        // xterm.js native, except when application cursor keys are disabled by configuration.
        if (!shift && !ctrl && !alt && modes.applicationCursorKeys && opts.disableAppCursorKeys) {
          return { data: CSI + (key === 'Home' ? 'H' : 'F'), consumedAlt: false };
        }
        return null;
      }
      const app = modes.applicationCursorKeys && !opts.disableAppCursorKeys;
      return { data: (app ? SS3 : CSI) + (key === 'Home' ? 'H' : 'F'), consumedAlt: false };
    }
    // 'standard' falls through to ESC[1~ / ESC[4~.
  } else if (mode === 'xterm') {
    return null; // Insert/Delete/PageUp/PageDown: xterm.js native (ESC[2~ .. with modifier parameters)
  }

  let code = SKK_CODE[key];
  // VT400: physical VT220 editing keypad order (Find Insert Remove / Select Prev Next).
  if (mode === 'vt400') code = [0, 2, 1, 4, 5, 3, 6][code];
  // Only reachable in 'xterm' mode for Home/End with the 'standard' setting: xterm-style modifier parameter.
  if (mode === 'xterm' && (shift || ctrl || alt)) {
    return { data: `${CSI}${code};${modifierParam(shift, alt, ctrl)}~`, consumedAlt: true };
  }
  return { data: `${CSI}${code}~`, consumedAlt: false };
}

/** Cursor key ('A' up, 'B' down, 'C' right, 'D' left, 'G' keypad centre). Null => xterm.js native. */
export function formatArrowKey(
  xkey: 'A' | 'B' | 'C' | 'D' | 'G',
  opts: Pick<KeyboardOptions, 'functionKeys' | 'disableAppCursorKeys'>,
  modes: TerminalKeyModes,
  shift: boolean,
  ctrl: boolean,
  alt: boolean
): string | null {
  let app = modes.applicationCursorKeys && !opts.disableAppCursorKeys;
  if (opts.functionKeys === 'xterm') {
    if (shift || ctrl || alt) {
      if (xkey === 'G') return `${CSI}1;${modifierParam(shift, alt, ctrl)}G`;
      return null; // xterm.js: ESC[1;<m>A ..
    }
    if (xkey === 'G' || (modes.applicationCursorKeys && opts.disableAppCursorKeys)) return (app ? SS3 : CSI) + xkey;
    return null;
  }
  // PuTTY default "Ctrl toggles app mode"; Shift has no effect.
  if (ctrl) app = !app;
  return (app ? SS3 : CSI) + xkey;
}

/**
 * Numeric keypad key in application keypad mode (and VT400's always-on PF keys). `key` is the keypad legend:
 * '0'..'9', '.', '+', '-', '*', '/', 'Enter' or 'NumLock'. Returns null when the key should type normally.
 */
export function formatNumericKeypadKey(
  key: string,
  opts: Pick<KeyboardOptions, 'functionKeys' | 'disableAppKeypad'>,
  modes: TerminalKeyModes,
  shift: boolean
): string | null {
  const mode = opts.functionKeys;
  const app = modes.applicationKeypad && !opts.disableAppKeypad;
  const xtermish = mode === 'xterm' || mode === 'xterm-r6';
  let xkey = '';

  if (mode === 'vt400' || ((mode === 'esc-n' || mode === 'linux') && app)) {
    if (key === 'NumLock') xkey = 'P';
    else if (key === '/') xkey = 'Q';
    else if (key === '*') xkey = 'R';
    else if (key === '-') xkey = 'S';
  }
  if (app) {
    if (key.length === 1 && key >= '0' && key <= '9') xkey = String.fromCharCode(0x70 + key.charCodeAt(0) - 0x30);
    else if (key === '.') xkey = 'n';
    else if (key === '+') xkey = xtermish ? (shift ? 'l' : 'k') : shift ? 'm' : 'l';
    else if (key === '/' && xtermish) xkey = 'o';
    else if (key === '*' && xtermish) xkey = 'j';
    else if (key === '-' && xtermish) xkey = 'm';
    else if (key === 'Enter') xkey = 'M';
  }
  return xkey ? SS3 + xkey : null;
}

/** Control character for Ctrl+<key> (PuTTY / xterm conventions), or null if the key has none. */
export function controlChar(code: string, key: string): string | null {
  const letter = /^Key([A-Z])$/.exec(code);
  if (letter) return String.fromCharCode(letter[1].charCodeAt(0) - 0x40);
  switch (code) {
    case 'Space':
    case 'Digit2':
      return '\x00';
    case 'BracketLeft':
    case 'Digit3':
      return '\x1b';
    case 'Backslash':
    case 'IntlBackslash':
    case 'Digit4':
      return '\x1c';
    case 'BracketRight':
    case 'Digit5':
      return '\x1d';
    case 'Digit6':
      return '\x1e';
    case 'Minus':
    case 'Slash':
    case 'Digit7':
      return '\x1f';
    case 'Digit8':
      return '\x7f';
  }
  if (key === '@') return '\x00';
  if (key === '^' || key === '~') return '\x1e';
  if (key === '_' || key === '?') return '\x1f';
  return null;
}

const NUMPAD_LEGEND: Record<string, string> = {
  Numpad0: '0',
  Numpad1: '1',
  Numpad2: '2',
  Numpad3: '3',
  Numpad4: '4',
  Numpad5: '5',
  Numpad6: '6',
  Numpad7: '7',
  Numpad8: '8',
  Numpad9: '9',
  NumpadDecimal: '.',
  NumpadAdd: '+',
  NumpadSubtract: '-',
  NumpadMultiply: '*',
  NumpadDivide: '/',
  NumpadEnter: 'Enter',
  NumLock: 'NumLock'
};

const ARROWS: Record<string, 'A' | 'B' | 'C' | 'D' | 'G'> = {
  ArrowUp: 'A',
  ArrowDown: 'B',
  ArrowRight: 'C',
  ArrowLeft: 'D',
  Clear: 'G'
};

/**
 * Decide what a keydown does. Only keydown events should be passed in; app-level shortcuts are handled by the
 * shell in the capture phase before xterm.js (and this function) ever sees them.
 */
export function translateKey(ev: KeyEventLike, opts: KeyboardOptions, modes: TerminalKeyModes): KeyAction {
  const { shiftKey: shift, ctrlKey: ctrl, metaKey: meta } = ev;
  if (meta) return DEFAULT;
  // Ctrl+Alt acting as AltGr (or a real AltGr) types the third-level character: xterm.js handles that.
  const altGr = !!ev.altGraph || (ctrl && ev.altKey && opts.ctrlAltIsAltGr);
  if (altGr && ev.key.length === 1) return DEFAULT;
  const alt = ev.altKey && !altGr;
  const withAlt = (data: string, consumed = false) => send(alt && !consumed ? ESC + data : data);

  // PuTTY-native local actions.
  if (ev.key === 'Insert' && !alt && !(ctrl && shift)) {
    if (shift) return { kind: 'paste' };
    if (ctrl) return { kind: 'copy' };
  }
  if ((ev.key === 'PageUp' || ev.key === 'PageDown') && !alt) {
    const dir = ev.key === 'PageUp' ? -1 : 1;
    if (shift && !ctrl) return { kind: 'scroll', pages: dir };
    if (ctrl && !shift) return { kind: 'scroll', lines: dir };
  }
  if ((ev.key === 'Cancel' || ev.code === 'Pause') && ctrl && !alt) return { kind: 'special', id: 'break' };
  if (ev.key === 'Pause' && !ctrl && !alt && !shift) return send('\x1a');

  // Backspace: configured code; Shift or Ctrl sends the other one.
  if (ev.key === 'Backspace') {
    const configured = opts.backspace === 'ctrl-h' ? '\x08' : '\x7f';
    const other = configured === '\x7f' ? '\x08' : '\x7f';
    return withAlt(shift || ctrl ? other : configured);
  }

  // Ctrl+Alt is *not* AltGr: Alt prefixes ESC to the control character.
  if (ctrl && ev.altKey && !altGr && !shift) {
    const cc = controlChar(ev.code, ev.key);
    if (cc !== null) return send(ESC + cc);
  }

  // Function keys.
  const fkey = /^F(\d{1,2})$/.exec(ev.key);
  if (fkey) {
    const n = Number(fkey[1]);
    if (n >= 1 && n <= 20) {
      const r = formatFunctionKey(opts.functionKeys, n, shift, ctrl, alt);
      return withAlt(r.data, r.consumedAlt);
    }
    return DEFAULT;
  }

  // Numeric keypad (application keypad mode / VT400 PF keys). Any key located on the keypad counts, so in
  // application mode the keypad sends sequences regardless of NumLock (PuTTY behaviour).
  const legend = NUMPAD_LEGEND[ev.code];
  if (legend && !alt && !ctrl) {
    const isDigitKey = /^[0-9.]$/.test(legend);
    const appKeypad = modes.applicationKeypad && !opts.disableAppKeypad;
    if (!isDigitKey || appKeypad) {
      const data = formatNumericKeypadKey(legend, opts, modes, shift);
      if (data) return send(data);
    }
  }

  // Cursor keys.
  const arrow = ARROWS[ev.key];
  if (arrow) {
    const data = formatArrowKey(arrow, opts, modes, shift, ctrl, alt);
    if (data === null) return DEFAULT;
    return withAlt(data, opts.functionKeys === 'xterm');
  }

  // Editing keypad.
  if (ev.key in SKK_CODE) {
    const r = formatSmallKeypadKey(ev.key as SmallKeypadKey, opts, modes, shift, ctrl, alt);
    if (!r) return DEFAULT;
    return withAlt(r.data, r.consumedAlt);
  }

  return DEFAULT;
}
