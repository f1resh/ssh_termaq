/**
 * Keybinding parsing/matching. Format: modifiers in the order Ctrl+Alt+Shift+Meta, then the key:
 * A-Z, 0-9, F1-F24, Tab, Enter, Escape, Space, Backspace, Delete, Insert, Home, End, PageUp, PageDown,
 * ArrowUp/Down/Left/Right, and unshifted punctuation from KeyboardEvent.code: = - , . / ; ' [ ] \ `
 * (Placeholder — implemented by the shell.)
 */
import type { CommandId } from '@shared/types';

/** Normalized chord string for a keyboard event, e.g. "Ctrl+Shift+T". */
export function chordOf(_e: KeyboardEvent): string {
  return '';
}

/** The command bound to this event, if any. */
export function matchKeybinding(_e: KeyboardEvent): CommandId | null {
  return null;
}
