/**
 * The file lists' shortcuts that use modifiers (the file manager's panes, the SFTP panel), per OS: Windows uses Ctrl
 * as Explorer does; macOS uses Command and Finder's keys - ⌘⌫ deletes, ⌘↑ goes up, ⌘↓ opens, ⌘[ / ⌘] go back /
 * forward, ⇧⌘N makes a folder, ⇧⌘G (or ⌘L) types a path. A focused list owns these chords (data-own-keys): the app's
 * global shortcuts with the same chord (⌘F find in terminal, ⌘D split, ⌘A select all) leave them to it. Plain keys
 * (arrows, Enter, Backspace, Delete, F2 - F8, Tab, type-ahead) are the same on both and stay in the lists' handlers.
 */
import { chordOf, formatChord, type KeyLike } from '@/commands/chords';
import { isMac } from '@shared/platform';

export type ListAction =
  | 'refresh'
  | 'find'
  | 'bookmark'
  | 'path'
  | 'selectAll'
  | 'up'
  | 'open'
  | 'back'
  | 'forward'
  | 'newFolder'
  | 'delete';

type ListKeys = Partial<Record<ListAction, string[]>>;

const WIN_KEYS: ListKeys = {
  refresh: ['Ctrl+R'],
  find: ['Ctrl+F'],
  bookmark: ['Ctrl+D'],
  path: ['Ctrl+L'],
  selectAll: ['Ctrl+A']
};

const MAC_KEYS: ListKeys = {
  refresh: ['Meta+R'],
  find: ['Meta+F'],
  bookmark: ['Meta+D'],
  path: ['Meta+L', 'Shift+Meta+G'],
  selectAll: ['Meta+A'],
  up: ['Meta+ArrowUp'],
  open: ['Meta+ArrowDown'],
  back: ['Meta+['],
  forward: ['Meta+]'],
  newFolder: ['Shift+Meta+N'],
  delete: ['Meta+Backspace']
};

const keysFor = (mac: boolean) => (mac ? MAC_KEYS : WIN_KEYS);

/** The action of a key event in a file list, when it is one of these chords. */
export function listAction(e: KeyLike, mac = isMac): ListAction | null {
  const chord = chordOf(e);
  if (!chord) return null;
  for (const [action, chords] of Object.entries(keysFor(mac))) if (chords?.includes(chord)) return action as ListAction;
  return null;
}

/** The shortcut shown for an action: its first chord, else `fallback` (a plain key such as "F7"). */
export function listShortcut(action: ListAction, fallback = '', mac = isMac): string {
  const chord = keysFor(mac)[action]?.[0];
  return chord ? formatChord(chord, mac) : fallback;
}

/** The value of a file list's data-own-keys: the chords of the actions it handles itself (default: all). */
export function ownKeys(actions?: readonly ListAction[], mac = isMac): string {
  return Object.entries(keysFor(mac))
    .filter(([action]) => !actions || actions.includes(action as ListAction))
    .flatMap(([, chords]) => chords ?? [])
    .join(' ');
}
