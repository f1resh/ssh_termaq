/**
 * Command registry: every user-invokable action (palette, keybindings, menus). (Placeholder — implemented by the shell.)
 */
import type { CommandId } from '@shared/types';

export interface CommandInfo {
  id: CommandId;
  label: string;
  category: 'App' | 'Tab' | 'Pane' | 'Terminal';
}

export const COMMANDS: CommandInfo[] = [];

/** Run a command against the current focus (active tab / pane). */
export function runCommand(id: CommandId): void {
  console.debug('runCommand (placeholder)', id);
}
