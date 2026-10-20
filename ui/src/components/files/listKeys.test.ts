import { describe, expect, it } from 'vitest';
import { listAction, listShortcut, ownKeys } from './listKeys';

const key = (k: string, code: string, mods: Partial<Record<'ctrlKey' | 'altKey' | 'shiftKey' | 'metaKey', boolean>> = {}) => ({
  key: k,
  code,
  ctrlKey: false,
  altKey: false,
  shiftKey: false,
  metaKey: false,
  ...mods
});

describe('file list keys', () => {
  it('macOS: Command and Finder keys', () => {
    expect(listAction(key('Backspace', 'Backspace', { metaKey: true }), true)).toBe('delete');
    expect(listAction(key('ArrowUp', 'ArrowUp', { metaKey: true }), true)).toBe('up');
    expect(listAction(key('ArrowDown', 'ArrowDown', { metaKey: true }), true)).toBe('open');
    expect(listAction(key('[', 'BracketLeft', { metaKey: true }), true)).toBe('back');
    expect(listAction(key(']', 'BracketRight', { metaKey: true }), true)).toBe('forward');
    expect(listAction(key('N', 'KeyN', { metaKey: true, shiftKey: true }), true)).toBe('newFolder');
    expect(listAction(key('G', 'KeyG', { metaKey: true, shiftKey: true }), true)).toBe('path');
    expect(listAction(key('l', 'KeyL', { metaKey: true }), true)).toBe('path');
    expect(listAction(key('f', 'KeyF', { metaKey: true }), true)).toBe('find');
    expect(listAction(key('d', 'KeyD', { metaKey: true }), true)).toBe('bookmark');
    // Control stays out of it on macOS, and plain keys are the lists' own.
    expect(listAction(key('r', 'KeyR', { ctrlKey: true }), true)).toBeNull();
    expect(listAction(key('Backspace', 'Backspace'), true)).toBeNull();
  });
  it('Windows: Ctrl as before', () => {
    expect(listAction(key('r', 'KeyR', { ctrlKey: true }), false)).toBe('refresh');
    expect(listAction(key('a', 'KeyA', { ctrlKey: true }), false)).toBe('selectAll');
    expect(listAction(key('l', 'KeyL', { ctrlKey: true }), false)).toBe('path');
    expect(listAction(key('r', 'KeyR', { metaKey: true }), false)).toBeNull();
    expect(listAction(key('Backspace', 'Backspace', { metaKey: true }), false)).toBeNull();
  });
  it('labels the shortcuts per OS', () => {
    expect(listShortcut('delete', 'Del', true)).toBe('⌘⌫');
    expect(listShortcut('delete', 'Del', false)).toBe('Del');
    expect(listShortcut('newFolder', 'F7', true)).toBe('⇧⌘N');
    expect(listShortcut('newFolder', 'F7', false)).toBe('F7');
    expect(listShortcut('refresh', '', false)).toBe('Ctrl+R');
    expect(listShortcut('path', '', true)).toBe('⌘L');
  });
  it('names the chords a list keeps from the global shortcuts', () => {
    expect(ownKeys(['delete', 'up'], true)).toBe('Meta+ArrowUp Meta+Backspace');
    expect(ownKeys(undefined, true).split(' ')).toEqual(expect.arrayContaining(['Meta+F', 'Meta+D', 'Meta+A']));
    expect(ownKeys(undefined, false).split(' ')).toEqual(['Ctrl+R', 'Ctrl+F', 'Ctrl+D', 'Ctrl+L', 'Ctrl+A']);
  });
});
