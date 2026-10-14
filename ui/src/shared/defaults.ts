import type { AppSettings, CommandId, PortForward, Protocol, SessionConfig } from './types';

export const APP_NAME = 'SSH Crawler';

export const DEFAULT_PORTS: Record<Protocol, number> = {
  ssh: 22,
  telnet: 23,
  rlogin: 513,
  raw: 0,
  serial: 0
};

export function newId(): string {
  return globalThis.crypto.randomUUID();
}

export const DEFAULT_FONT_FAMILY = "'Cascadia Mono', 'Cascadia Code', Consolas, 'Courier New', monospace";

/** Builds a complete session with sensible modern defaults. */
export function createDefaultSession(overrides: Partial<SessionConfig> = {}): SessionConfig {
  const now = Date.now();
  const base: SessionConfig = {
    id: newId(),
    name: '',
    folder: '',
    color: '',
    notes: '',
    protocol: 'ssh',
    host: '',
    port: 22,
    username: '',
    openAs: 'terminal',
    createdAt: now,
    updatedAt: now,
    lastUsedAt: 0,
    connection: {
      keepaliveSeconds: 30,
      tcpNoDelay: true,
      tcpKeepalive: false,
      addressFamily: 'auto',
      connectTimeoutSeconds: 20,
      logicalHostname: '',
      terminalType: 'xterm-256color',
      terminalSpeed: '38400,38400',
      environment: []
    },
    proxy: {
      type: 'none',
      host: '',
      port: 1080,
      username: '',
      password: '',
      excludeHosts: '',
      proxyLocalhost: false,
      dnsAtProxy: 'auto',
      telnetCommand: 'connect %host %port\\n',
      localCommand: '',
      jumpSessionId: ''
    },
    ssh: {
      remoteCommand: '',
      noShell: false,
      allocatePty: true,
      compression: false,
      kex: [],
      ciphers: [],
      hostKeyAlgorithms: [],
      macs: [],
      auth: {
        privateKeyPath: '',
        tryAgent: true,
        agent: 'auto',
        agentForwarding: false,
        keyboardInteractive: true,
        password: true,
        showBanner: true
      },
      x11: { enabled: false, display: 'localhost:0' },
      tunnels: [],
      localPortsAcceptAll: false,
      remotePortsAcceptAll: false,
      sftpStartPath: '',
      fileProtocol: 'auto',
      localStartPath: ''
    },
    telnet: {
      passive: false,
      keyboardSendsSpecials: false,
      returnSendsNewline: true
    },
    rlogin: { localUsername: '' },
    serial: {
      line: 'COM1',
      baudRate: 9600,
      dataBits: 8,
      stopBits: 1,
      parity: 'none',
      flowControl: 'xonxoff'
    },
    terminal: {
      autoWrap: true,
      localEcho: 'auto',
      localLineEditing: 'auto',
      implicitCrInLf: false,
      implicitLfInCr: false,
      answerback: 'SSHCrawler',
      bell: 'visual',
      bellSoundPath: '',
      scrollbackLines: 0,
      allowRemoteTitle: true,
      windowTitle: '',
      closeOnExit: 'never'
    },
    keyboard: {
      backspace: 'del',
      homeEnd: 'xterm',
      functionKeys: 'xterm',
      disableAppCursorKeys: false,
      disableAppKeypad: false,
      ctrlAltIsAltGr: true
    },
    appearance: {},
    translation: { charset: 'UTF-8', cjkAmbiguousWide: false },
    logging: {
      mode: 'off',
      path: '&S-&Y&M&D-&T.log',
      ifExists: 'append',
      flush: true,
      header: true
    }
  };
  return deepMerge(base, overrides as Record<string, unknown>) as SessionConfig;
}

export function createPortForward(overrides: Partial<PortForward> = {}): PortForward {
  return {
    id: newId(),
    type: 'local',
    enabled: true,
    bindAddress: '',
    sourcePort: 0,
    destHost: 'localhost',
    destPort: 0,
    addressFamily: 'auto',
    description: '',
    ...overrides
  };
}

export const DEFAULT_KEYBINDINGS: Record<CommandId, string> = {
  'app.commandPalette': 'Ctrl+Shift+P',
  'app.quickConnect': 'Ctrl+Shift+O',
  'app.newSession': 'Ctrl+Shift+N',
  'app.settings': 'Ctrl+,',
  'app.toggleSidebar': 'Ctrl+Shift+B',
  'app.importPutty': '',
  'app.fullscreen': 'F11',
  'app.keygen': '',
  'tab.new': 'Ctrl+Shift+T',
  'tab.close': 'Ctrl+Shift+W',
  'tab.next': 'Ctrl+Tab',
  'tab.prev': 'Ctrl+Shift+Tab',
  'tab.duplicate': 'Ctrl+Shift+D',
  'tab.rename': '',
  'tab.goto1': 'Ctrl+Alt+1',
  'tab.goto2': 'Ctrl+Alt+2',
  'tab.goto3': 'Ctrl+Alt+3',
  'tab.goto4': 'Ctrl+Alt+4',
  'tab.goto5': 'Ctrl+Alt+5',
  'tab.goto6': 'Ctrl+Alt+6',
  'tab.goto7': 'Ctrl+Alt+7',
  'tab.goto8': 'Ctrl+Alt+8',
  'tab.goto9': 'Ctrl+Alt+9',
  'pane.splitRight': 'Alt+Shift+=',
  'pane.splitDown': 'Alt+Shift+-',
  'pane.close': 'Alt+Shift+W',
  'pane.focusNext': 'Alt+Shift+ArrowRight',
  'pane.focusPrev': 'Alt+Shift+ArrowLeft',
  'pane.restart': '',
  'pane.toggleBroadcast': '',
  'pane.sftp': 'Ctrl+Shift+S',
  'pane.eventLog': '',
  'pane.changeSettings': '',
  'term.copy': 'Ctrl+Shift+C',
  'term.paste': 'Ctrl+Shift+V',
  'term.copyAll': '',
  'term.selectAll': 'Ctrl+Shift+A',
  'term.find': 'Ctrl+Shift+F',
  'term.clearScrollback': '',
  'term.reset': '',
  'term.zoomIn': 'Ctrl+=',
  'term.zoomOut': 'Ctrl+-',
  'term.zoomReset': 'Ctrl+0'
};

export function createDefaultSettings(): AppSettings {
  return {
    version: 1,
    appearance: {
      theme: 'system',
      accentColor: '',
      backgroundMaterial: 'none',
      showSidebar: true,
      sidebarWidth: 260,
      uiScale: 1
    },
    terminal: {
      fontFamily: DEFAULT_FONT_FAMILY,
      fontSize: 14,
      lineHeight: 1.2,
      letterSpacing: 0,
      fontWeight: 'normal',
      fontWeightBold: 'bold',
      cursorStyle: 'block',
      cursorBlink: true,
      colorScheme: 'auto',
      scrollback: 10000,
      renderer: 'webgl',
      drawBoldTextInBrightColors: true,
      minimumContrastRatio: 1,
      padding: 6
    },
    behavior: {
      copyOnSelect: true,
      rightClick: 'paste',
      middleClickPaste: false,
      wordSeparators: ' ()[]{}\'"`,;:|<>',
      warnOnMultilinePaste: true,
      confirmCloseConnected: true,
      restoreTabs: false,
      reconnect: 'prompt',
      scrollOnOutput: false,
      scrollOnKeystroke: true,
      sessionDoubleClick: 'tab'
    },
    keybindings: { ...DEFAULT_KEYBINDINGS },
    customSchemes: [],
    defaultSession: createDefaultSession({ id: 'default', name: 'Default Settings', createdAt: 0, updatedAt: 0 }),
    logging: { directory: '' },
    files: {
      editor: '',
      defaultConflict: 'ask',
      showHidden: false,
      confirmDelete: true,
      deleteToRecycleBin: true,
      preserveTimestamps: true,
      parallelTransfers: 2
    }
  };
}

/* ------------------------------------------------------------------------ */
/* Merging / normalisation                                                   */
/* ------------------------------------------------------------------------ */

function isPlainObject(v: unknown): v is Record<string, unknown> {
  return typeof v === 'object' && v !== null && !Array.isArray(v);
}

/**
 * Deep merge `patch` into a copy of `base`. Objects merge recursively, arrays and primitives replace.
 * `undefined` values in the patch are ignored.
 */
export function deepMerge<T>(base: T, patch: unknown): T {
  if (!isPlainObject(patch)) return (patch === undefined ? structuredClone(base) : patch) as T;
  const out: Record<string, unknown> = isPlainObject(base) ? { ...(structuredClone(base) as Record<string, unknown>) } : {};
  for (const [k, v] of Object.entries(patch)) {
    if (v === undefined) continue;
    const cur = out[k];
    out[k] = isPlainObject(v) && isPlainObject(cur) ? deepMerge(cur, v) : structuredClone(v);
  }
  return out as T;
}

/**
 * Fill any missing fields of a (possibly older / partial) stored session from the template.
 * Guarantees a complete SessionConfig with an id.
 */
export function normalizeSession(raw: unknown, template?: SessionConfig): SessionConfig {
  const base = template ? structuredClone(template) : createDefaultSession();
  const merged = deepMerge(base, raw) as SessionConfig;
  if (!merged.id || (template && merged.id === template.id && !(isPlainObject(raw) && raw.id))) merged.id = newId();
  if (typeof merged.port !== 'number' || !Number.isFinite(merged.port)) merged.port = DEFAULT_PORTS[merged.protocol] ?? 22;
  merged.folder = normalizeFolder(merged.folder);
  return merged;
}

export function normalizeSettings(raw: unknown): AppSettings {
  const merged = deepMerge(createDefaultSettings(), raw);
  merged.version = 1;
  merged.defaultSession = normalizeSession(merged.defaultSession, createDefaultSession({ id: 'default', name: 'Default Settings' }));
  merged.defaultSession.id = 'default';
  merged.keybindings = { ...DEFAULT_KEYBINDINGS, ...merged.keybindings };
  return merged;
}

/** Trim slashes/whitespace, collapse duplicate separators. '' = root. */
export function normalizeFolder(folder: string | undefined | null): string {
  if (!folder) return '';
  return folder
    .split('/')
    .map((p) => p.trim())
    .filter(Boolean)
    .join('/');
}

/** Human-friendly label for a session (name, else user@host:port). */
export function sessionLabel(s: Pick<SessionConfig, 'name' | 'host' | 'port' | 'username' | 'protocol' | 'serial'>): string {
  if (s.name) return s.name;
  if (s.protocol === 'serial') return s.serial.line;
  const user = s.username ? `${s.username}@` : '';
  const port = s.port && s.port !== DEFAULT_PORTS[s.protocol] ? `:${s.port}` : '';
  return `${user}${s.host}${port}`;
}
