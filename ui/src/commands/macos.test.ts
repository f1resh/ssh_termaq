import { describe, expect, it } from 'vitest';
import { MAC_KEYBINDINGS, PC_KEYBINDINGS } from '@shared/defaults';
import { detectPlatform } from '@shared/platform';
import type { CommandId } from '@shared/types';
import { formatChord, normalizeChord } from './chords';
import { buildMenuBar } from './menubar';

describe('platform', () => {
  it('knows the host by its message channel, else by the browser', () => {
    expect(detectPlatform({ webkit: { messageHandlers: { st: {} } } })).toBe('darwin');
    expect(detectPlatform({ chrome: { webview: {} }, navigator: { platform: 'MacIntel' } })).toBe('win32');
    expect(detectPlatform({ navigator: { platform: 'MacIntel', userAgent: '' } })).toBe('darwin');
    expect(detectPlatform({ navigator: { platform: 'Win32', userAgent: 'Mozilla/5.0 (Windows NT 10.0)' } })).toBe('win32');
    expect(detectPlatform({})).toBe('win32');
  });
});

describe('shortcut labels', () => {
  it('macOS: the Mac symbols in its order', () => {
    expect(formatChord('Shift+Meta+P', true)).toBe('⇧⌘P');
    expect(formatChord('Ctrl+Meta+F', true)).toBe('⌃⌘F');
    expect(formatChord('Alt+Meta+ArrowRight', true)).toBe('⌥⌘→');
    expect(formatChord('Meta+,', true)).toBe('⌘,');
    expect(formatChord('F11', true)).toBe('F11');
  });

  it('Windows: unchanged', () => {
    expect(formatChord('Ctrl+Shift+T', false)).toBe('Ctrl+Shift+T');
    expect(formatChord('Meta+ArrowLeft', false)).toBe('Win+←');
  });
});

describe('default key bindings', () => {
  for (const [name, table] of [
    ['Windows', PC_KEYBINDINGS],
    ['macOS', MAC_KEYBINDINGS]
  ] as const) {
    it(`${name}: every command, written canonically, no chord twice`, () => {
      expect(Object.keys(table).sort()).toEqual(Object.keys(PC_KEYBINDINGS).sort());
      const chords = Object.values(table).filter(Boolean);
      for (const c of chords) expect(normalizeChord(c)).toBe(c);
      expect(new Set(chords).size).toBe(chords.length);
    });
  }

  it('macOS: Command for the app, Control left to the terminal', () => {
    expect(MAC_KEYBINDINGS['tab.new']).toBe('Meta+T');
    expect(MAC_KEYBINDINGS['term.copy']).toBe('Meta+C');
    for (const c of Object.values(MAC_KEYBINDINGS).filter(Boolean)) expect(c).toContain('Meta');
  });
});

describe('menu bar', () => {
  const state = (over: Partial<Parameters<typeof buildMenuBar>[0]> = {}) => ({
    shortcut: (id: CommandId) => MAC_KEYBINDINGS[id],
    sidebarShown: true,
    hasPane: false,
    ...over
  });
  const find = (menus: ReturnType<typeof buildMenuBar>, command: string) =>
    menus.flatMap((m) => m.items).find((i) => 'command' in i && i.command === command) as
      | { command: string; label: string; shortcut?: string; enabled?: boolean }
      | undefined;

  it('About and Settings open the application menu, then File, View and Terminal', () => {
    const menus = buildMenuBar(state());
    expect(menus.map((m) => m.title)).toEqual(['app', 'File', 'View', 'Terminal']);
    expect(menus[0].items[0]).toEqual({ command: 'app.about', label: 'About SSHTermaq' });
    expect(find(menus, 'app.settings')?.shortcut).toBe('Meta+,');
  });

  it('follows the state: sidebar label, commands that need a pane', () => {
    expect(find(buildMenuBar(state()), 'app.toggleSidebar')?.label).toBe('Hide Sidebar');
    expect(find(buildMenuBar(state({ sidebarShown: false })), 'app.toggleSidebar')?.label).toBe('Show Sidebar');
    expect(find(buildMenuBar(state()), 'tab.close')?.enabled).toBe(false);
    expect(find(buildMenuBar(state({ hasPane: true })), 'tab.close')?.enabled).toBe(true);
    expect(find(buildMenuBar(state()), 'app.newSession')?.enabled).toBe(true);
  });
});
