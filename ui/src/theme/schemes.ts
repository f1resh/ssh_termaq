/**
 * Built-in terminal colour schemes. (Expanded by the shell implementation.)
 */
import type { ColorScheme } from '@shared/types';

export const BUILTIN_SCHEMES: ColorScheme[] = [
  {
    id: 'one-dark',
    name: 'One Dark',
    variant: 'dark',
    builtIn: true,
    foreground: '#abb2bf',
    background: '#1e2127',
    cursor: '#528bff',
    cursorAccent: '#1e2127',
    selectionBackground: '#3e4451',
    black: '#1e2127',
    red: '#e06c75',
    green: '#98c379',
    yellow: '#d19a66',
    blue: '#61afef',
    magenta: '#c678dd',
    cyan: '#56b6c2',
    white: '#abb2bf',
    brightBlack: '#5c6370',
    brightRed: '#e06c75',
    brightGreen: '#98c379',
    brightYellow: '#e5c07b',
    brightBlue: '#61afef',
    brightMagenta: '#c678dd',
    brightCyan: '#56b6c2',
    brightWhite: '#ffffff'
  },
  {
    id: 'one-light',
    name: 'One Light',
    variant: 'light',
    builtIn: true,
    foreground: '#383a42',
    background: '#fafafa',
    cursor: '#526fff',
    cursorAccent: '#fafafa',
    selectionBackground: '#d7dae0',
    black: '#383a42',
    red: '#e45649',
    green: '#50a14f',
    yellow: '#c18401',
    blue: '#0184bc',
    magenta: '#a626a4',
    cyan: '#0997b3',
    white: '#a0a1a7',
    brightBlack: '#696c77',
    brightRed: '#e45649',
    brightGreen: '#50a14f',
    brightYellow: '#c18401',
    brightBlue: '#0184bc',
    brightMagenta: '#a626a4',
    brightCyan: '#0997b3',
    brightWhite: '#ffffff'
  }
];
