import { describe, expect, it } from 'vitest';
import type { FunctionKeyMode, KeyboardOptions } from '@shared/types';
import { controlChar, formatFunctionKey, translateKey, type KeyEventLike, type TerminalKeyModes } from './keymap';

const ESC = '\x1b';

function opts(over: Partial<KeyboardOptions> = {}): KeyboardOptions {
  return {
    backspace: 'del',
    homeEnd: 'xterm',
    functionKeys: 'xterm',
    disableAppCursorKeys: false,
    disableAppKeypad: false,
    ctrlAltIsAltGr: true,
    ...over
  };
}

const NORMAL: TerminalKeyModes = { applicationCursorKeys: false, applicationKeypad: false };
const APP: TerminalKeyModes = { applicationCursorKeys: true, applicationKeypad: true };

type Mods = Partial<Pick<KeyEventLike, 'shiftKey' | 'ctrlKey' | 'altKey' | 'metaKey' | 'altGraph'>>;

function key(k: string, mods: Mods = {}, code?: string): KeyEventLike {
  return {
    key: k,
    code: code ?? (k.length === 1 ? `Key${k.toUpperCase()}` : k),
    shiftKey: false,
    ctrlKey: false,
    altKey: false,
    metaKey: false,
    ...mods
  };
}

/** Bytes sent for a key, or 'default' when xterm.js handles it natively. */
function out(ev: KeyEventLike, o = opts(), modes = NORMAL): string {
  const a = translateKey(ev, o, modes);
  if (a.kind === 'send') return a.data;
  return a.kind;
}

const fk = (mode: FunctionKeyMode, n: number, mods: Mods = {}) => out(key(`F${n}`, mods), opts({ functionKeys: mode }));

describe('function keys', () => {
  const tilde = ['', '11', '12', '13', '14', '15', '17', '18', '19', '20', '21', '23', '24', '25', '26', '28', '29', '31', '32', '33', '34'];

  it('ESC[n~ mode: F1..F20 and Shift+F1..F10 = F11..F20', () => {
    for (let n = 1; n <= 20; n++) expect(fk('esc-n', n)).toBe(`${ESC}[${tilde[n]}~`);
    for (let n = 1; n <= 10; n++) expect(fk('esc-n', n, { shiftKey: true })).toBe(`${ESC}[${tilde[n + 10]}~`);
    // Shift has no effect above F10; Ctrl is ignored.
    expect(fk('esc-n', 11, { shiftKey: true })).toBe(`${ESC}[23~`);
    expect(fk('esc-n', 1, { ctrlKey: true })).toBe(`${ESC}[11~`);
  });

  it('Linux mode: F1..F5 = ESC[[A..E, rest as ESC[n~', () => {
    expect(['A', 'B', 'C', 'D', 'E'].map((_c, i) => fk('linux', i + 1))).toEqual(
      ['A', 'B', 'C', 'D', 'E'].map((c) => `${ESC}[[${c}`)
    );
    for (let n = 6; n <= 20; n++) expect(fk('linux', n)).toBe(`${ESC}[${tilde[n]}~`);
    expect(fk('linux', 1, { shiftKey: true })).toBe(`${ESC}[23~`);
  });

  it('Xterm R6 mode: F1..F4 = ESC OP..OS, rest as ESC[n~', () => {
    expect([1, 2, 3, 4].map((n) => fk('xterm-r6', n))).toEqual(['P', 'Q', 'R', 'S'].map((c) => `${ESC}O${c}`));
    for (let n = 5; n <= 20; n++) expect(fk('xterm-r6', n)).toBe(`${ESC}[${tilde[n]}~`);
    expect(fk('xterm-r6', 2, { shiftKey: true })).toBe(`${ESC}[24~`);
  });

  it('VT400 mode: function keys as ESC[n~', () => {
    for (let n = 1; n <= 20; n++) expect(fk('vt400', n)).toBe(`${ESC}[${tilde[n]}~`);
  });

  it('VT100+ mode: ESC OP .. ESC O[', () => {
    const want = ['P', 'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z', '['];
    for (let n = 1; n <= 12; n++) expect(fk('vt100plus', n)).toBe(`${ESC}O${want[n - 1]}`);
    expect(fk('vt100plus', 1, { shiftKey: true })).toBe(`${ESC}OZ`);
    expect(fk('vt100plus', 2, { shiftKey: true })).toBe(`${ESC}O[`);
    expect(fk('vt100plus', 3, { shiftKey: true })).toBe(`${ESC}[25~`);
    expect(fk('vt100plus', 13)).toBe(`${ESC}[25~`);
  });

  it('SCO mode: plain / Shift / Ctrl / Ctrl+Shift ranges', () => {
    expect(fk('sco', 1)).toBe(`${ESC}[M`);
    expect(fk('sco', 12)).toBe(`${ESC}[X`);
    expect(fk('sco', 1, { shiftKey: true })).toBe(`${ESC}[Y`);
    expect(fk('sco', 12, { shiftKey: true })).toBe(`${ESC}[j`);
    expect(fk('sco', 1, { ctrlKey: true })).toBe(`${ESC}[k`);
    expect(fk('sco', 12, { ctrlKey: true })).toBe(`${ESC}[v`);
    expect(fk('sco', 1, { ctrlKey: true, shiftKey: true })).toBe(`${ESC}[w`);
    expect(fk('sco', 12, { ctrlKey: true, shiftKey: true })).toBe(`${ESC}[{`);
  });

  it('xterm (216+) mode: unmodified like R6, modified with bitmap parameter', () => {
    expect(fk('xterm', 1)).toBe(`${ESC}OP`);
    expect(fk('xterm', 4)).toBe(`${ESC}OS`);
    expect(fk('xterm', 5)).toBe(`${ESC}[15~`);
    expect(fk('xterm', 12)).toBe(`${ESC}[24~`);
    expect(fk('xterm', 1, { shiftKey: true })).toBe(`${ESC}[1;2P`);
    expect(fk('xterm', 3, { altKey: true })).toBe(`${ESC}[1;3R`);
    expect(fk('xterm', 4, { ctrlKey: true })).toBe(`${ESC}[1;5S`);
    expect(fk('xterm', 5, { shiftKey: true })).toBe(`${ESC}[15;2~`);
    expect(fk('xterm', 12, { ctrlKey: true, shiftKey: true })).toBe(`${ESC}[24;6~`);
    // Alt is encoded in the parameter, so no ESC prefix.
    expect(fk('xterm', 10, { altKey: true })).toBe(`${ESC}[21;3~`);
  });

  it('Alt prefixes ESC in legacy modes', () => {
    expect(fk('esc-n', 1, { altKey: true })).toBe(`${ESC}${ESC}[11~`);
    expect(fk('linux', 1, { altKey: true })).toBe(`${ESC}${ESC}[[A`);
  });

  it('formatFunctionKey rejects out-of-range keys', () => {
    expect(formatFunctionKey('esc-n', 21, false, false, false).data).toBe('');
  });
});

describe('Home / End and editing keypad', () => {
  it('standard: ESC[1~ / ESC[4~', () => {
    const o = opts({ homeEnd: 'standard', functionKeys: 'esc-n' });
    expect(out(key('Home'), o)).toBe(`${ESC}[1~`);
    expect(out(key('End'), o)).toBe(`${ESC}[4~`);
    expect(out(key('Insert'), o)).toBe(`${ESC}[2~`);
    expect(out(key('Delete'), o)).toBe(`${ESC}[3~`);
    expect(out(key('PageUp'), o)).toBe(`${ESC}[5~`);
    expect(out(key('PageDown'), o)).toBe(`${ESC}[6~`);
  });

  it('rxvt: ESC[H / ESC Ow', () => {
    for (const fkm of ['esc-n', 'xterm', 'linux'] as FunctionKeyMode[]) {
      const o = opts({ homeEnd: 'rxvt', functionKeys: fkm });
      expect(out(key('Home'), o)).toBe(`${ESC}[H`);
      expect(out(key('End'), o)).toBe(`${ESC}Ow`);
    }
  });

  it('xterm: native in xterm mode, ESC[H/ESC[F (SS3 in app mode) otherwise', () => {
    expect(out(key('Home'), opts())).toBe('default');
    expect(out(key('End'), opts(), APP)).toBe('default');
    const o = opts({ functionKeys: 'esc-n' });
    expect(out(key('Home'), o)).toBe(`${ESC}[H`);
    expect(out(key('End'), o)).toBe(`${ESC}[F`);
    expect(out(key('Home'), o, APP)).toBe(`${ESC}OH`);
    expect(out(key('End'), opts({ functionKeys: 'esc-n', disableAppCursorKeys: true }), APP)).toBe(`${ESC}[F`);
    // Disabled application cursor keys also override xterm.js native handling.
    expect(out(key('Home'), opts({ disableAppCursorKeys: true }), APP)).toBe(`${ESC}[H`);
  });

  it('xterm mode with standard Home/End adds modifier parameter', () => {
    const o = opts({ homeEnd: 'standard' });
    expect(out(key('Home'), o)).toBe(`${ESC}[1~`);
    expect(out(key('End', { ctrlKey: true }), o)).toBe(`${ESC}[4;5~`);
  });

  it('xterm mode leaves Insert/Delete/PageUp/PageDown to xterm.js', () => {
    for (const k of ['Insert', 'Delete', 'PageUp', 'PageDown']) expect(out(key(k))).toBe('default');
  });

  it('VT400: edit keys in physical VT220 order', () => {
    const o = opts({ homeEnd: 'standard', functionKeys: 'vt400' });
    expect(out(key('Insert'), o)).toBe(`${ESC}[1~`);
    expect(out(key('Home'), o)).toBe(`${ESC}[2~`);
    expect(out(key('PageUp'), o)).toBe(`${ESC}[3~`);
    expect(out(key('Delete'), o)).toBe(`${ESC}[4~`);
    expect(out(key('End'), o)).toBe(`${ESC}[5~`);
    expect(out(key('PageDown'), o)).toBe(`${ESC}[6~`);
  });

  it('SCO: H L DEL F I G', () => {
    const o = opts({ homeEnd: 'standard', functionKeys: 'sco' });
    expect(out(key('Home'), o)).toBe(`${ESC}[H`);
    expect(out(key('Insert'), o)).toBe(`${ESC}[L`);
    expect(out(key('Delete'), o)).toBe('\x7f');
    expect(out(key('End'), o)).toBe(`${ESC}[F`);
    expect(out(key('PageUp'), o)).toBe(`${ESC}[I`);
    expect(out(key('PageDown'), o)).toBe(`${ESC}[G`);
  });

  it('Alt prefixes ESC in legacy modes', () => {
    expect(out(key('Delete', { altKey: true }), opts({ functionKeys: 'esc-n' }))).toBe(`${ESC}${ESC}[3~`);
  });
});

describe('cursor keys', () => {
  it('xterm mode: native unless application cursor keys are disabled', () => {
    expect(out(key('ArrowUp'))).toBe('default');
    expect(out(key('ArrowUp'), opts(), APP)).toBe('default');
    expect(out(key('ArrowUp', { ctrlKey: true }))).toBe('default');
    expect(out(key('ArrowUp'), opts({ disableAppCursorKeys: true }), APP)).toBe(`${ESC}[A`);
  });

  it('legacy modes: normal / application / Ctrl toggles', () => {
    const o = opts({ functionKeys: 'esc-n' });
    expect(['ArrowUp', 'ArrowDown', 'ArrowRight', 'ArrowLeft'].map((k) => out(key(k), o))).toEqual(
      ['A', 'B', 'C', 'D'].map((c) => `${ESC}[${c}`)
    );
    expect(out(key('ArrowUp'), o, APP)).toBe(`${ESC}OA`);
    expect(out(key('ArrowLeft', { ctrlKey: true }), o)).toBe(`${ESC}OD`);
    expect(out(key('ArrowLeft', { ctrlKey: true }), o, APP)).toBe(`${ESC}[D`);
    expect(out(key('ArrowUp', { shiftKey: true }), o)).toBe(`${ESC}[A`);
    expect(out(key('ArrowUp'), opts({ functionKeys: 'esc-n', disableAppCursorKeys: true }), APP)).toBe(`${ESC}[A`);
    expect(out(key('ArrowDown', { altKey: true }), o)).toBe(`${ESC}${ESC}[B`);
  });

  it('keypad centre (Clear) sends ESC[G', () => {
    expect(out(key('Clear', {}, 'Numpad5'), opts({ functionKeys: 'esc-n' }))).toBe(`${ESC}[G`);
    expect(out(key('Clear', {}, 'Numpad5'))).toBe(`${ESC}[G`);
  });
});

describe('numeric keypad', () => {
  const np = (k: string, code: string, o: KeyboardOptions, modes = APP, mods: Mods = {}) => out(key(k, mods, code), o, modes);

  it('application keypad: digits ESC Op..Oy, "." ESC On, Enter ESC OM', () => {
    const o = opts();
    for (let d = 0; d <= 9; d++) {
      expect(np(String(d), `Numpad${d}`, o)).toBe(`${ESC}O${String.fromCharCode(0x70 + d)}`);
    }
    expect(np('.', 'NumpadDecimal', o)).toBe(`${ESC}On`);
    expect(np('Enter', 'NumpadEnter', o)).toBe(`${ESC}OM`);
    // NumLock off: the key reports as navigation but still sits on the keypad.
    expect(np('Home', 'Numpad7', o)).toBe(`${ESC}Ow`);
  });

  it('normal keypad types characters', () => {
    expect(np('5', 'Numpad5', opts(), NORMAL)).toBe('default');
    expect(np('+', 'NumpadAdd', opts(), NORMAL)).toBe('default');
    expect(np('Enter', 'NumpadEnter', opts(), NORMAL)).toBe('default');
  });

  it('disableAppKeypad ignores application keypad mode', () => {
    expect(np('5', 'Numpad5', opts({ disableAppKeypad: true }))).toBe('default');
  });

  it('xterm-style operators', () => {
    for (const m of ['xterm', 'xterm-r6'] as FunctionKeyMode[]) {
      const o = opts({ functionKeys: m });
      expect(np('+', 'NumpadAdd', o)).toBe(`${ESC}Ok`);
      expect(np('+', 'NumpadAdd', o, APP, { shiftKey: true })).toBe(`${ESC}Ol`);
      expect(np('/', 'NumpadDivide', o)).toBe(`${ESC}Oo`);
      expect(np('*', 'NumpadMultiply', o)).toBe(`${ESC}Oj`);
      expect(np('-', 'NumpadSubtract', o)).toBe(`${ESC}Om`);
    }
  });

  it('ESC[n~ / Linux: PF keys in application mode, + = l (Shift m)', () => {
    for (const m of ['esc-n', 'linux'] as FunctionKeyMode[]) {
      const o = opts({ functionKeys: m });
      expect(np('NumLock', 'NumLock', o)).toBe(`${ESC}OP`);
      expect(np('/', 'NumpadDivide', o)).toBe(`${ESC}OQ`);
      expect(np('*', 'NumpadMultiply', o)).toBe(`${ESC}OR`);
      expect(np('-', 'NumpadSubtract', o)).toBe(`${ESC}OS`);
      expect(np('+', 'NumpadAdd', o)).toBe(`${ESC}Ol`);
      expect(np('+', 'NumpadAdd', o, APP, { shiftKey: true })).toBe(`${ESC}Om`);
      expect(np('/', 'NumpadDivide', o, NORMAL)).toBe('default');
    }
  });

  it('VT400: keypad top row is always PF1..PF4', () => {
    const o = opts({ functionKeys: 'vt400' });
    expect(np('NumLock', 'NumLock', o, NORMAL)).toBe(`${ESC}OP`);
    expect(np('/', 'NumpadDivide', o, NORMAL)).toBe(`${ESC}OQ`);
    expect(np('*', 'NumpadMultiply', o, NORMAL)).toBe(`${ESC}OR`);
    expect(np('-', 'NumpadSubtract', o, NORMAL)).toBe(`${ESC}OS`);
    expect(np('5', 'Numpad5', o, NORMAL)).toBe('default');
  });

  it('SCO / VT100+: no operator mapping, digits mapped in application mode', () => {
    for (const m of ['sco', 'vt100plus'] as FunctionKeyMode[]) {
      const o = opts({ functionKeys: m });
      expect(np('/', 'NumpadDivide', o)).toBe('default');
      expect(np('+', 'NumpadAdd', o)).toBe(`${ESC}Ol`);
      expect(np('1', 'Numpad1', o)).toBe(`${ESC}Oq`);
    }
  });
});

describe('Backspace', () => {
  it('^? (default): Backspace DEL, Shift/Ctrl+Backspace ^H', () => {
    expect(out(key('Backspace'))).toBe('\x7f');
    expect(out(key('Backspace', { shiftKey: true }))).toBe('\x08');
    expect(out(key('Backspace', { ctrlKey: true }))).toBe('\x08');
    expect(out(key('Backspace', { altKey: true }))).toBe(`${ESC}\x7f`);
  });

  it('^H: Backspace ^H, Shift/Ctrl+Backspace DEL', () => {
    const o = opts({ backspace: 'ctrl-h' });
    expect(out(key('Backspace'), o)).toBe('\x08');
    expect(out(key('Backspace', { shiftKey: true }), o)).toBe('\x7f');
    expect(out(key('Backspace', { ctrlKey: true }), o)).toBe('\x7f');
  });
});

describe('Ctrl+Alt / AltGr', () => {
  it('Ctrl+Alt = AltGr (default): characters typed natively', () => {
    expect(out(key('@', { ctrlKey: true, altKey: true }, 'KeyQ'))).toBe('default');
    expect(out(key('q', { ctrlKey: true, altKey: true }))).toBe('default');
  });

  it('Ctrl+Alt different from AltGr: ESC + control character', () => {
    const o = opts({ ctrlAltIsAltGr: false });
    expect(out(key('a', { ctrlKey: true, altKey: true }), o)).toBe(`${ESC}\x01`);
    expect(out(key('z', { ctrlKey: true, altKey: true }), o)).toBe(`${ESC}\x1a`);
    expect(out(key('[', { ctrlKey: true, altKey: true }, 'BracketLeft'), o)).toBe(`${ESC}${ESC}`);
    // A real AltGr still types its character.
    expect(out(key('@', { ctrlKey: true, altKey: true, altGraph: true }, 'KeyQ'), o)).toBe('default');
  });

  it('macOS: Option types its character unless it is Meta', () => {
    const optionTypes = (ev: KeyEventLike) => translateKey(ev, opts(), NORMAL, { optionTypes: true }).kind;
    expect(optionTypes(key('@', { altKey: true }, 'KeyL'))).toBe('default'); // Option+L on a German layout
    expect(optionTypes(key('™', { altKey: true }, 'Digit2'))).toBe('default');
    // Non-character keys keep PuTTY's Alt handling.
    expect(translateKey(key('Backspace', { altKey: true }), opts(), NORMAL, { optionTypes: true })).toEqual({ kind: 'send', data: `${ESC}\x7f` });
    // Option as Meta: ESC + the key, as Alt elsewhere (xterm.js sends it).
    expect(out(key('b', { altKey: true }))).toBe('default');
  });

  it('controlChar mapping', () => {
    expect(controlChar('KeyC', 'c')).toBe('\x03');
    expect(controlChar('Space', ' ')).toBe('\x00');
    expect(controlChar('Backslash', '\\')).toBe('\x1c');
    expect(controlChar('BracketRight', ']')).toBe('\x1d');
    expect(controlChar('Digit6', '6')).toBe('\x1e');
    expect(controlChar('Slash', '/')).toBe('\x1f');
    expect(controlChar('Digit8', '8')).toBe('\x7f');
    expect(controlChar('Period', '.')).toBeNull();
  });
});

describe('PuTTY local actions', () => {
  it('Shift+Insert pastes, Ctrl+Insert copies', () => {
    expect(translateKey(key('Insert', { shiftKey: true }), opts(), NORMAL)).toEqual({ kind: 'paste' });
    expect(translateKey(key('Insert', { ctrlKey: true }), opts(), NORMAL)).toEqual({ kind: 'copy' });
  });

  it('Shift+PageUp/PageDown scroll pages, Ctrl+PageUp/PageDown scroll lines', () => {
    expect(translateKey(key('PageUp', { shiftKey: true }), opts(), NORMAL)).toEqual({ kind: 'scroll', pages: -1 });
    expect(translateKey(key('PageDown', { shiftKey: true }), opts(), NORMAL)).toEqual({ kind: 'scroll', pages: 1 });
    expect(translateKey(key('PageUp', { ctrlKey: true }), opts(), NORMAL)).toEqual({ kind: 'scroll', lines: -1 });
    expect(translateKey(key('PageDown', { ctrlKey: true }), opts(), NORMAL)).toEqual({ kind: 'scroll', lines: 1 });
  });

  it('Ctrl+Break sends the break special, Pause sends ^Z', () => {
    expect(translateKey(key('Cancel', { ctrlKey: true }, 'Pause'), opts(), NORMAL)).toEqual({ kind: 'special', id: 'break' });
    expect(out(key('Pause'))).toBe('\x1a');
  });

  it('ordinary keys are left to xterm.js', () => {
    expect(out(key('a'))).toBe('default');
    expect(out(key('Enter', {}, 'Enter'))).toBe('default');
    expect(out(key('Tab', { shiftKey: true }))).toBe('default');
    expect(out(key('c', { ctrlKey: true }))).toBe('default');
    expect(out(key('v', { metaKey: true }))).toBe('default');
  });
});
