/**
 * Built-in terminal colour schemes. Values follow each scheme's canonical definition (PuTTY's compiled-in
 * defaults, Windows Terminal's bundled schemes, and the upstream projects for the editor themes).
 *
 * `ansi` is the 16-colour palette in the usual order: black, red, green, yellow, blue, magenta, cyan,
 * white, then the bright variants.
 */
import type { ColorScheme } from '@shared/types';

type Palette16 = [
  string, string, string, string, string, string, string, string,
  string, string, string, string, string, string, string, string
];

interface SchemeDef {
  id: string;
  name: string;
  variant: 'dark' | 'light';
  foreground: string;
  background: string;
  cursor: string;
  cursorAccent?: string;
  selectionBackground: string;
  selectionForeground?: string;
  boldForeground?: string;
  ansi: Palette16;
}

function scheme(d: SchemeDef): ColorScheme {
  const [black, red, green, yellow, blue, magenta, cyan, white] = d.ansi;
  const [brightBlack, brightRed, brightGreen, brightYellow, brightBlue, brightMagenta, brightCyan, brightWhite] = d.ansi.slice(8);
  const s: ColorScheme = {
    id: d.id,
    name: d.name,
    variant: d.variant,
    builtIn: true,
    foreground: d.foreground,
    background: d.background,
    cursor: d.cursor,
    cursorAccent: d.cursorAccent ?? d.background,
    selectionBackground: d.selectionBackground,
    black,
    red,
    green,
    yellow,
    blue,
    magenta,
    cyan,
    white,
    brightBlack,
    brightRed,
    brightGreen,
    brightYellow,
    brightBlue,
    brightMagenta,
    brightCyan,
    brightWhite
  };
  if (d.selectionForeground) s.selectionForeground = d.selectionForeground;
  if (d.boldForeground) s.boldForeground = d.boldForeground;
  return s;
}

/** Tango palette shared by Tango Dark/Light and the classic GNOME/Ubuntu terminal. */
const TANGO: Palette16 = [
  '#000000', '#cc0000', '#4e9a06', '#c4a000', '#3465a4', '#75507b', '#06989a', '#d3d7cf',
  '#555753', '#ef2929', '#8ae234', '#fce94f', '#729fcf', '#ad7fa8', '#34e2e2', '#eeeeec'
];

/** Campbell (Windows Terminal / Windows console default since 2017). */
const CAMPBELL: Palette16 = [
  '#0c0c0c', '#c50f1f', '#13a10e', '#c19c00', '#0037da', '#881798', '#3a96dd', '#cccccc',
  '#767676', '#e74856', '#16c60c', '#f9f1a5', '#3b78ff', '#b4009e', '#61d6d6', '#f2f2f2'
];

/** Solarized accent palette (canonical 16-colour terminal mapping). */
const SOLARIZED: Palette16 = [
  '#073642', '#dc322f', '#859900', '#b58900', '#268bd2', '#d33682', '#2aa198', '#eee8d5',
  '#002b36', '#cb4b16', '#586e75', '#657b83', '#839496', '#6c71c4', '#93a1a1', '#fdf6e3'
];

export const BUILTIN_SCHEMES: ColorScheme[] = [
  scheme({
    id: 'putty',
    name: 'PuTTY Classic',
    variant: 'dark',
    foreground: '#bbbbbb',
    background: '#000000',
    boldForeground: '#ffffff',
    cursor: '#00ff00',
    cursorAccent: '#000000',
    selectionBackground: '#bbbbbb',
    selectionForeground: '#000000',
    ansi: [
      '#000000', '#bb0000', '#00bb00', '#bbbb00', '#0000bb', '#bb00bb', '#00bbbb', '#bbbbbb',
      '#555555', '#ff5555', '#55ff55', '#ffff55', '#5555ff', '#ff55ff', '#55ffff', '#ffffff'
    ]
  }),
  scheme({
    id: 'campbell',
    name: 'Campbell',
    variant: 'dark',
    foreground: '#cccccc',
    background: '#0c0c0c',
    cursor: '#ffffff',
    selectionBackground: '#ffffff50',
    ansi: CAMPBELL
  }),
  scheme({
    id: 'campbell-powershell',
    name: 'Campbell PowerShell',
    variant: 'dark',
    foreground: '#cccccc',
    background: '#012456',
    cursor: '#ffffff',
    selectionBackground: '#ffffff50',
    ansi: CAMPBELL
  }),
  scheme({
    id: 'one-dark',
    name: 'One Dark',
    variant: 'dark',
    foreground: '#abb2bf',
    background: '#1e2127',
    cursor: '#528bff',
    selectionBackground: '#3e4451',
    ansi: [
      '#1e2127', '#e06c75', '#98c379', '#d19a66', '#61afef', '#c678dd', '#56b6c2', '#abb2bf',
      '#5c6370', '#e06c75', '#98c379', '#e5c07b', '#61afef', '#c678dd', '#56b6c2', '#ffffff'
    ]
  }),
  scheme({
    id: 'one-light',
    name: 'One Light',
    variant: 'light',
    foreground: '#383a42',
    background: '#fafafa',
    cursor: '#526fff',
    selectionBackground: '#d7dae0',
    ansi: [
      '#383a42', '#e45649', '#50a14f', '#c18401', '#0184bc', '#a626a4', '#0997b3', '#a0a1a7',
      '#696c77', '#e45649', '#50a14f', '#c18401', '#0184bc', '#a626a4', '#0997b3', '#ffffff'
    ]
  }),
  scheme({
    id: 'one-half-dark',
    name: 'One Half Dark',
    variant: 'dark',
    foreground: '#dcdfe4',
    background: '#282c34',
    cursor: '#a3b3cc',
    selectionBackground: '#474e5d',
    ansi: [
      '#282c34', '#e06c75', '#98c379', '#e5c07b', '#61afef', '#c678dd', '#56b6c2', '#dcdfe4',
      '#5a6374', '#e06c75', '#98c379', '#e5c07b', '#61afef', '#c678dd', '#56b6c2', '#dcdfe4'
    ]
  }),
  scheme({
    id: 'one-half-light',
    name: 'One Half Light',
    variant: 'light',
    foreground: '#383a42',
    background: '#fafafa',
    cursor: '#4f525d',
    selectionBackground: '#bfceff',
    ansi: [
      '#383a42', '#e45649', '#50a14f', '#c18301', '#0184bc', '#a626a4', '#0997b3', '#fafafa',
      '#4f525d', '#df6c75', '#98c379', '#e4c07a', '#61afef', '#c577dd', '#56b5c1', '#ffffff'
    ]
  }),
  scheme({
    id: 'dracula',
    name: 'Dracula',
    variant: 'dark',
    foreground: '#f8f8f2',
    background: '#282a36',
    cursor: '#f8f8f2',
    selectionBackground: '#44475a',
    ansi: [
      '#21222c', '#ff5555', '#50fa7b', '#f1fa8c', '#bd93f9', '#ff79c6', '#8be9fd', '#f8f8f2',
      '#6272a4', '#ff6e6e', '#69ff94', '#ffffa5', '#d6acff', '#ff92df', '#a4ffff', '#ffffff'
    ]
  }),
  scheme({
    id: 'solarized-dark',
    name: 'Solarized Dark',
    variant: 'dark',
    foreground: '#839496',
    background: '#002b36',
    cursor: '#93a1a1',
    selectionBackground: '#073642',
    selectionForeground: '#93a1a1',
    ansi: SOLARIZED
  }),
  scheme({
    id: 'solarized-light',
    name: 'Solarized Light',
    variant: 'light',
    foreground: '#657b83',
    background: '#fdf6e3',
    cursor: '#586e75',
    selectionBackground: '#eee8d5',
    selectionForeground: '#586e75',
    ansi: SOLARIZED
  }),
  scheme({
    id: 'nord',
    name: 'Nord',
    variant: 'dark',
    foreground: '#d8dee9',
    background: '#2e3440',
    cursor: '#d8dee9',
    selectionBackground: '#434c5e',
    ansi: [
      '#3b4252', '#bf616a', '#a3be8c', '#ebcb8b', '#81a1c1', '#b48ead', '#88c0d0', '#e5e9f0',
      '#4c566a', '#bf616a', '#a3be8c', '#ebcb8b', '#81a1c1', '#b48ead', '#8fbcbb', '#eceff4'
    ]
  }),
  scheme({
    id: 'gruvbox-dark',
    name: 'Gruvbox Dark',
    variant: 'dark',
    foreground: '#ebdbb2',
    background: '#282828',
    cursor: '#ebdbb2',
    selectionBackground: '#504945',
    ansi: [
      '#282828', '#cc241d', '#98971a', '#d79921', '#458588', '#b16286', '#689d6a', '#a89984',
      '#928374', '#fb4934', '#b8bb26', '#fabd2f', '#83a598', '#d3869b', '#8ec07c', '#ebdbb2'
    ]
  }),
  scheme({
    id: 'gruvbox-light',
    name: 'Gruvbox Light',
    variant: 'light',
    foreground: '#3c3836',
    background: '#fbf1c7',
    cursor: '#3c3836',
    selectionBackground: '#d5c4a1',
    ansi: [
      '#fbf1c7', '#cc241d', '#98971a', '#d79921', '#458588', '#b16286', '#689d6a', '#7c6f64',
      '#928374', '#9d0006', '#79740e', '#b57614', '#076678', '#8f3f71', '#427b58', '#3c3836'
    ]
  }),
  scheme({
    id: 'monokai',
    name: 'Monokai',
    variant: 'dark',
    foreground: '#f8f8f2',
    background: '#272822',
    cursor: '#f8f8f0',
    selectionBackground: '#49483e',
    ansi: [
      '#272822', '#f92672', '#a6e22e', '#f4bf75', '#66d9ef', '#ae81ff', '#a1efe4', '#f8f8f2',
      '#75715e', '#f92672', '#a6e22e', '#f4bf75', '#66d9ef', '#ae81ff', '#a1efe4', '#f9f8f5'
    ]
  }),
  scheme({
    id: 'tokyo-night',
    name: 'Tokyo Night',
    variant: 'dark',
    foreground: '#c0caf5',
    background: '#1a1b26',
    cursor: '#c0caf5',
    selectionBackground: '#283457',
    ansi: [
      '#15161e', '#f7768e', '#9ece6a', '#e0af68', '#7aa2f7', '#bb9af7', '#7dcfff', '#a9b1d6',
      '#414868', '#f7768e', '#9ece6a', '#e0af68', '#7aa2f7', '#bb9af7', '#7dcfff', '#c0caf5'
    ]
  }),
  scheme({
    id: 'catppuccin-mocha',
    name: 'Catppuccin Mocha',
    variant: 'dark',
    foreground: '#cdd6f4',
    background: '#1e1e2e',
    cursor: '#f5e0dc',
    cursorAccent: '#1e1e2e',
    selectionBackground: '#585b70',
    ansi: [
      '#45475a', '#f38ba8', '#a6e3a1', '#f9e2af', '#89b4fa', '#f5c2e7', '#94e2d5', '#bac2de',
      '#585b70', '#f38ba8', '#a6e3a1', '#f9e2af', '#89b4fa', '#f5c2e7', '#94e2d5', '#a6adc8'
    ]
  }),
  scheme({
    id: 'catppuccin-latte',
    name: 'Catppuccin Latte',
    variant: 'light',
    foreground: '#4c4f69',
    background: '#eff1f5',
    cursor: '#dc8a78',
    cursorAccent: '#eff1f5',
    selectionBackground: '#acb0be',
    ansi: [
      '#5c5f77', '#d20f39', '#40a02b', '#df8e1d', '#1e66f5', '#ea76cb', '#179299', '#acb0be',
      '#6c6f85', '#d20f39', '#40a02b', '#df8e1d', '#1e66f5', '#ea76cb', '#179299', '#bcc0cc'
    ]
  }),
  scheme({
    id: 'github-dark',
    name: 'GitHub Dark',
    variant: 'dark',
    foreground: '#e6edf3',
    background: '#0d1117',
    cursor: '#2f81f7',
    selectionBackground: '#264f78',
    ansi: [
      '#484f58', '#ff7b72', '#3fb950', '#d29922', '#58a6ff', '#bc8cff', '#39c5cf', '#b1bac4',
      '#6e7681', '#ffa198', '#56d364', '#e3b341', '#79c0ff', '#d2a8ff', '#56d4dd', '#ffffff'
    ]
  }),
  scheme({
    id: 'github-light',
    name: 'GitHub Light',
    variant: 'light',
    foreground: '#1f2328',
    background: '#ffffff',
    cursor: '#0969da',
    selectionBackground: '#add6ff',
    ansi: [
      '#24292f', '#cf222e', '#116329', '#4d2d00', '#0969da', '#8250df', '#1b7c83', '#6e7781',
      '#57606a', '#a40e26', '#1a7f37', '#633c01', '#218bff', '#a475f9', '#3192aa', '#8c959f'
    ]
  }),
  scheme({
    id: 'tango-dark',
    name: 'Tango Dark',
    variant: 'dark',
    foreground: '#d3d7cf',
    background: '#2e3436',
    cursor: '#d3d7cf',
    selectionBackground: '#555753',
    ansi: TANGO
  }),
  scheme({
    id: 'tango-light',
    name: 'Tango Light',
    variant: 'light',
    foreground: '#2e3436',
    background: '#eeeeec',
    cursor: '#2e3436',
    selectionBackground: '#babdb6',
    ansi: TANGO
  }),
  scheme({
    id: 'material',
    name: 'Material',
    variant: 'dark',
    foreground: '#eeffff',
    background: '#263238',
    cursor: '#ffcc00',
    selectionBackground: '#425b67',
    ansi: [
      '#000000', '#e53935', '#91b859', '#ffb62c', '#6182b8', '#ff5370', '#39adb5', '#a0a0a0',
      '#4e4e4e', '#ff5370', '#c3e88d', '#ffcb6b', '#82aaff', '#f07178', '#89ddff', '#ffffff'
    ]
  }),
  scheme({
    id: 'ayu-dark',
    name: 'Ayu Dark',
    variant: 'dark',
    foreground: '#b3b1ad',
    background: '#0a0e14',
    cursor: '#e6b450',
    selectionBackground: '#273747',
    ansi: [
      '#01060e', '#ea6c73', '#91b362', '#f9af4f', '#53bdfa', '#fae994', '#90e1c6', '#c7c7c7',
      '#686868', '#f07178', '#c2d94c', '#ffb454', '#59c2ff', '#ffee99', '#95e6cb', '#ffffff'
    ]
  }),
  scheme({
    id: 'night-owl',
    name: 'Night Owl',
    variant: 'dark',
    foreground: '#d6deeb',
    background: '#011627',
    cursor: '#80a4c2',
    selectionBackground: '#1d3b53',
    ansi: [
      '#011627', '#ef5350', '#22da6e', '#addb67', '#82aaff', '#c792ea', '#21c7a8', '#ffffff',
      '#575656', '#ef5350', '#22da6e', '#ffeb95', '#82aaff', '#c792ea', '#7fdbca', '#ffffff'
    ]
  }),
  scheme({
    id: 'zenburn',
    name: 'Zenburn',
    variant: 'dark',
    foreground: '#dcdccc',
    background: '#3f3f3f',
    cursor: '#73635a',
    cursorAccent: '#dcdccc',
    selectionBackground: '#21322f',
    ansi: [
      '#4d4d4d', '#705050', '#60b48a', '#f0dfaf', '#506070', '#dc8cc3', '#8cd0d3', '#dcdccc',
      '#709080', '#dca3a3', '#c3bf9f', '#e0cf9f', '#94bff3', '#ec93d3', '#93e0e3', '#ffffff'
    ]
  }),
  scheme({
    id: 'ubuntu',
    name: 'Ubuntu',
    variant: 'dark',
    foreground: '#eeeeec',
    background: '#300a24',
    cursor: '#bbbbbb',
    selectionBackground: '#b5d5ff60',
    ansi: [
      '#171421', '#c01c28', '#26a269', '#a2734c', '#12488b', '#a347ba', '#2aa1b3', '#d0cfcc',
      '#5e5c64', '#f66151', '#33da7a', '#e9ad0c', '#2a7bde', '#c061cb', '#33c7de', '#ffffff'
    ]
  }),
  scheme({
    id: 'retro-green',
    name: 'Retro Green',
    variant: 'dark',
    foreground: '#33ff66',
    background: '#050b06',
    boldForeground: '#9dffb5',
    cursor: '#33ff66',
    cursorAccent: '#050b06',
    selectionBackground: '#1a5c2a',
    selectionForeground: '#b6ffc8',
    ansi: [
      '#050b06', '#1f9e3c', '#2bd352', '#5ee67d', '#178a32', '#24b546', '#3ff06a', '#7dff9d',
      '#0f3d1a', '#33cc59', '#44ff74', '#88ffa6', '#26a646', '#39d963', '#66ff8c', '#c8ffd6'
    ]
  }),
  scheme({
    id: 'midnight',
    name: 'Midnight',
    variant: 'dark',
    foreground: '#c8d3e6',
    background: '#0b0e14',
    cursor: '#7c9cff',
    selectionBackground: '#25335a',
    ansi: [
      '#1b2130', '#ff6b78', '#4ad295', '#f5c26b', '#6f9bff', '#c08cff', '#4fd1e0', '#c8d3e6',
      '#4a5570', '#ff8a95', '#6fe3ad', '#ffd78f', '#97b6ff', '#d3adff', '#7fe3ef', '#f2f6fc'
    ]
  }),
  scheme({
    id: 'vintage',
    name: 'Vintage (Windows console)',
    variant: 'dark',
    foreground: '#c0c0c0',
    background: '#000000',
    cursor: '#ffffff',
    selectionBackground: '#ffffff50',
    ansi: [
      '#000000', '#800000', '#008000', '#808000', '#000080', '#800080', '#008080', '#c0c0c0',
      '#808080', '#ff0000', '#00ff00', '#ffff00', '#0000ff', '#ff00ff', '#00ffff', '#ffffff'
    ]
  })
];

export function findScheme(id: string, extra: ColorScheme[] = []): ColorScheme | undefined {
  return extra.find((s) => s.id === id) ?? BUILTIN_SCHEMES.find((s) => s.id === id);
}
