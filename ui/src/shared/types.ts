/**
 * Shared domain types used by the main process, the preload bridge and the renderer.
 * This file is the central contract of the application — keep it free of runtime
 * dependencies (types + tiny constants only).
 */

export type Protocol = 'ssh' | 'telnet' | 'rlogin' | 'raw' | 'serial';
export type TriState = 'auto' | 'on' | 'off';
export type AddressFamily = 'auto' | 'ipv4' | 'ipv6';

export type DeepPartial<T> = T extends (infer U)[]
  ? U[]
  : T extends object
    ? { [K in keyof T]?: DeepPartial<T[K]> }
    : T;

export interface EnvVar {
  name: string;
  value: string;
}

/* ------------------------------------------------------------------------ */
/* Port forwarding                                                           */
/* ------------------------------------------------------------------------ */

export type ForwardType = 'local' | 'remote' | 'dynamic';

export interface PortForward {
  id: string;
  type: ForwardType;
  enabled: boolean;
  /** Address to bind the listening side to. '' => localhost (or all interfaces when "accept all" is on). */
  bindAddress: string;
  sourcePort: number;
  /** Destination host (unused for dynamic). */
  destHost: string;
  /** Destination port (unused for dynamic). */
  destPort: number;
  addressFamily: AddressFamily;
  description: string;
}

/* ------------------------------------------------------------------------ */
/* Proxy                                                                     */
/* ------------------------------------------------------------------------ */

export type ProxyType = 'none' | 'socks4' | 'socks5' | 'http' | 'telnet' | 'command' | 'ssh';

export interface ProxyConfig {
  type: ProxyType;
  host: string;
  port: number;
  username: string;
  /** Encrypted at rest by the main-process store; plain text in memory/IPC. */
  password: string;
  /** Comma/space separated host patterns that bypass the proxy (supports * wildcards and CIDR-less IP prefixes). */
  excludeHosts: string;
  /** Use the proxy even for localhost / 127.0.0.0/8 / ::1 targets. */
  proxyLocalhost: boolean;
  /** Do DNS name lookup at proxy end: auto (yes for socks4a/socks5/http), on, off. */
  dnsAtProxy: TriState;
  /** PuTTY-style telnet proxy command template. Supports %host %port %user %pass %proxyhost %proxyport %% and \n \r \t \\ \xHH escapes. */
  telnetCommand: string;
  /** Local proxy command (stdin/stdout used as the transport), same substitutions as telnetCommand. */
  localCommand: string;
  /** For type 'ssh': id of a saved session used as the jump host. '' => use host/port/username above. */
  jumpSessionId: string;
}

/* ------------------------------------------------------------------------ */
/* Per-protocol options                                                      */
/* ------------------------------------------------------------------------ */

export type AgentKind = 'auto' | 'pageant' | 'openssh';

export interface SshAuthOptions {
  /** Private key file path: OpenSSH (any format ssh2 understands) or PuTTY .ppk (v2 / v3). */
  privateKeyPath: string;
  /** Try authenticating with an SSH agent (Pageant / Windows OpenSSH agent). */
  tryAgent: boolean;
  agent: AgentKind;
  agentForwarding: boolean;
  keyboardInteractive: boolean;
  password: boolean;
  /** Display the server's pre-auth banner in the terminal. */
  showBanner: boolean;
}

export interface SshOptions {
  remoteCommand: string;
  /** Do not start a shell or command at all (useful for tunnels only). */
  noShell: boolean;
  allocatePty: boolean;
  compression: boolean;
  /** ssh2 algorithm names in preference order; empty array => library defaults. */
  kex: string[];
  ciphers: string[];
  hostKeyAlgorithms: string[];
  macs: string[];
  auth: SshAuthOptions;
  x11: {
    enabled: boolean;
    /** X display location, e.g. 'localhost:0' or ':0'. */
    display: string;
  };
  tunnels: PortForward[];
  /** Local ports accept connections from other hosts (bind 0.0.0.0 when bindAddress empty). */
  localPortsAcceptAll: boolean;
  /** Remote ports do the same (bind 0.0.0.0 on server side when bindAddress empty). */
  remotePortsAcceptAll: boolean;
  /** Initial remote directory of the file manager ('' => home). */
  sftpStartPath: string;
  /** File manager protocol: 'auto' tries SFTP and falls back to SCP. */
  fileProtocol: FileProtocol;
  /** Initial local folder of the file manager ('' => last used, else Documents). */
  localStartPath: string;
}

export type FileProtocol = 'auto' | 'sftp' | 'scp';

export interface TelnetOptions {
  /** Passive negotiation: wait for the server to start option negotiation. */
  passive: boolean;
  /** Keyboard sends telnet special commands (Backspace => EC, Ctrl-C => IP, ...). */
  keyboardSendsSpecials: boolean;
  /** Return key sends telnet NEWLINE (CR LF) instead of ^M (CR NUL). */
  returnSendsNewline: boolean;
}

export interface RloginOptions {
  localUsername: string;
}

export type Parity = 'none' | 'odd' | 'even' | 'mark' | 'space';
export type FlowControl = 'none' | 'xonxoff' | 'rtscts' | 'dsrdtr';

export interface SerialOptions {
  /** e.g. 'COM1' */
  line: string;
  baudRate: number;
  dataBits: 5 | 6 | 7 | 8;
  stopBits: 1 | 1.5 | 2;
  parity: Parity;
  flowControl: FlowControl;
}

/* ------------------------------------------------------------------------ */
/* Terminal behaviour                                                        */
/* ------------------------------------------------------------------------ */

export type BellMode = 'none' | 'system' | 'visual' | 'sound';
export type CloseOnExit = 'always' | 'never' | 'clean';

export interface TerminalOptions {
  autoWrap: boolean;
  localEcho: TriState;
  localLineEditing: TriState;
  /** Implicit CR in every LF. */
  implicitCrInLf: boolean;
  /** Implicit LF in every CR. */
  implicitLfInCr: boolean;
  /** Answerback string sent in response to ^E (ENQ). */
  answerback: string;
  bell: BellMode;
  bellSoundPath: string;
  /** 0 => use global setting. */
  scrollbackLines: number;
  allowRemoteTitle: boolean;
  /** Fixed tab title ('' => use session name / remote title). */
  windowTitle: string;
  closeOnExit: CloseOnExit;
}

/** PuTTY "Function keys and keypad" modes. 'xterm' = modern xterm (xterm.js native, PuTTY's "Xterm 216+"). */
export type FunctionKeyMode = 'xterm' | 'esc-n' | 'linux' | 'xterm-r6' | 'vt400' | 'vt100plus' | 'sco';

export interface KeyboardOptions {
  /** 'del' => Backspace sends ^? (0x7F, PuTTY default); 'ctrl-h' => sends ^H (0x08). */
  backspace: 'del' | 'ctrl-h';
  /** 'xterm' => ESC[H / ESC[F (xterm.js native); 'standard' => ESC[1~ / ESC[4~ (PuTTY default); 'rxvt' => ESC[H / ESC[Ow. */
  homeEnd: 'xterm' | 'standard' | 'rxvt';
  functionKeys: FunctionKeyMode;
  disableAppCursorKeys: boolean;
  disableAppKeypad: boolean;
  /** Treat Ctrl+Alt as AltGr (do not send ESC-prefixed control chars). */
  ctrlAltIsAltGr: boolean;
}

export type CursorStyle = 'block' | 'underline' | 'bar';

/** Per-session overrides of global terminal appearance. Undefined/'' => inherit global setting. */
export interface AppearanceOverrides {
  fontFamily?: string;
  fontSize?: number;
  cursorStyle?: CursorStyle;
  cursorBlink?: boolean;
  /** Color scheme id ('' => global default). */
  colorScheme?: string;
  /** Explicit colours (e.g. imported from PuTTY "Colours" panel). Takes precedence over colorScheme. */
  customScheme?: ColorScheme;
  boldIsBright?: boolean;
}

export interface TranslationOptions {
  /** iconv-lite encoding name, e.g. 'UTF-8', 'ISO-8859-1', 'windows-1252', 'KOI8-R', 'CP437', 'Shift_JIS'. */
  charset: string;
  cjkAmbiguousWide: boolean;
}

export type LogMode = 'off' | 'printable' | 'all' | 'ssh-debug';

export interface LoggingOptions {
  mode: LogMode;
  /**
   * Log file path template. PuTTY placeholders: &Y year, &M month, &D day, &T time (HHMMSS),
   * &H host, &P port, &S session name, && literal &. Relative paths resolve against the global log directory.
   */
  path: string;
  ifExists: 'overwrite' | 'append' | 'ask';
  flush: boolean;
  /** Write a header line with date/host on open. */
  header: boolean;
}

/* ------------------------------------------------------------------------ */
/* Session                                                                   */
/* ------------------------------------------------------------------------ */

export interface ConnectionOptions {
  /** Seconds between keepalives; 0 = off. */
  keepaliveSeconds: number;
  tcpNoDelay: boolean;
  tcpKeepalive: boolean;
  addressFamily: AddressFamily;
  connectTimeoutSeconds: number;
  /** Hostname used for host-key lookup instead of `host` (PuTTY "Logical name of remote host"). */
  logicalHostname: string;
  /** TERM value sent to the server. */
  terminalType: string;
  /** Terminal speed string, e.g. '38400,38400'. */
  terminalSpeed: string;
  environment: EnvVar[];
}

export interface SessionConfig {
  id: string;
  name: string;
  /** '/'-separated folder path, '' for root. */
  folder: string;
  /** Accent colour for tab / sidebar ('' => none). */
  color: string;
  notes: string;
  protocol: Protocol;
  host: string;
  port: number;
  /** Auto-login username ('' => prompt). */
  username: string;
  /** What opening the session shows (SSH only): a terminal or the file manager. */
  openAs: SessionView;
  createdAt: number;
  updatedAt: number;
  lastUsedAt: number;
  connection: ConnectionOptions;
  proxy: ProxyConfig;
  ssh: SshOptions;
  telnet: TelnetOptions;
  rlogin: RloginOptions;
  serial: SerialOptions;
  terminal: TerminalOptions;
  keyboard: KeyboardOptions;
  appearance: AppearanceOverrides;
  translation: TranslationOptions;
  logging: LoggingOptions;
}

export type SessionView = 'terminal' | 'files';

/* ------------------------------------------------------------------------ */
/* Themes                                                                    */
/* ------------------------------------------------------------------------ */

export interface ColorScheme {
  id: string;
  name: string;
  variant: 'dark' | 'light';
  builtIn?: boolean;
  foreground: string;
  background: string;
  cursor: string;
  cursorAccent: string;
  selectionBackground: string;
  selectionForeground?: string;
  /** Optional distinct colour for bold default foreground (PuTTY "Default Bold Foreground"). */
  boldForeground?: string;
  black: string;
  red: string;
  green: string;
  yellow: string;
  blue: string;
  magenta: string;
  cyan: string;
  white: string;
  brightBlack: string;
  brightRed: string;
  brightGreen: string;
  brightYellow: string;
  brightBlue: string;
  brightMagenta: string;
  brightCyan: string;
  brightWhite: string;
}

/** Keys map 1:1 to CSS custom properties: `bgElevated` => `--bg-elevated`. */
export interface AppThemeColors {
  bg: string;
  bgElevated: string;
  bgSunken: string;
  surface: string;
  surfaceHover: string;
  surfaceActive: string;
  border: string;
  borderStrong: string;
  text: string;
  textMuted: string;
  textFaint: string;
  accent: string;
  accentHover: string;
  accentFg: string;
  danger: string;
  warning: string;
  success: string;
  info: string;
  titlebar: string;
  titlebarFg: string;
  sidebar: string;
  tabActive: string;
  tabHover: string;
  overlay: string;
  scrollbar: string;
  scrollbarHover: string;
}

export interface AppTheme {
  id: string;
  name: string;
  variant: 'dark' | 'light';
  colors: AppThemeColors;
  /** Terminal scheme used when the global terminal scheme is 'auto'. */
  defaultScheme: string;
}

/* ------------------------------------------------------------------------ */
/* Settings                                                                  */
/* ------------------------------------------------------------------------ */

export type CommandId =
  | 'app.commandPalette'
  | 'app.quickConnect'
  | 'app.newSession'
  | 'app.settings'
  | 'app.toggleSidebar'
  | 'app.importPutty'
  | 'app.fullscreen'
  | 'app.keygen'
  | 'tab.new'
  | 'tab.close'
  | 'tab.next'
  | 'tab.prev'
  | 'tab.duplicate'
  | 'tab.rename'
  | 'tab.goto1'
  | 'tab.goto2'
  | 'tab.goto3'
  | 'tab.goto4'
  | 'tab.goto5'
  | 'tab.goto6'
  | 'tab.goto7'
  | 'tab.goto8'
  | 'tab.goto9'
  | 'pane.splitRight'
  | 'pane.splitDown'
  | 'pane.close'
  | 'pane.focusNext'
  | 'pane.focusPrev'
  | 'pane.restart'
  | 'pane.toggleBroadcast'
  | 'pane.sftp'
  | 'pane.eventLog'
  | 'pane.changeSettings'
  | 'term.copy'
  | 'term.paste'
  | 'term.copyAll'
  | 'term.selectAll'
  | 'term.find'
  | 'term.clearScrollback'
  | 'term.reset'
  | 'term.zoomIn'
  | 'term.zoomOut'
  | 'term.zoomReset';

export type BackgroundMaterial = 'none' | 'mica' | 'acrylic';

export interface AppSettings {
  version: 1;
  appearance: {
    /** App theme id, or 'system' to follow Windows light/dark. */
    theme: string;
    /** Accent override ('' => theme default). */
    accentColor: string;
    backgroundMaterial: BackgroundMaterial;
    showSidebar: boolean;
    sidebarWidth: number;
    /** UI zoom factor (1 = 100%). */
    uiScale: number;
  };
  terminal: {
    fontFamily: string;
    fontSize: number;
    lineHeight: number;
    letterSpacing: number;
    fontWeight: 'normal' | 'bold' | '300' | '400' | '500' | '600';
    fontWeightBold: 'normal' | 'bold' | '500' | '600' | '700' | '800';
    cursorStyle: CursorStyle;
    cursorBlink: boolean;
    /** Scheme id or 'auto' (use app theme's default scheme). */
    colorScheme: string;
    scrollback: number;
    renderer: 'webgl' | 'dom';
    drawBoldTextInBrightColors: boolean;
    minimumContrastRatio: number;
    padding: number;
  };
  behavior: {
    copyOnSelect: boolean;
    rightClick: 'paste' | 'menu';
    middleClickPaste: boolean;
    wordSeparators: string;
    warnOnMultilinePaste: boolean;
    confirmCloseConnected: boolean;
    restoreTabs: boolean;
    reconnect: 'off' | 'prompt' | 'auto';
    scrollOnOutput: boolean;
    scrollOnKeystroke: boolean;
    /** What double-click on a sidebar session does. */
    sessionDoubleClick: 'tab' | 'split';
  };
  keybindings: Partial<Record<CommandId, string>>;
  customSchemes: ColorScheme[];
  /** Template for new sessions (PuTTY "Default Settings"). */
  defaultSession: SessionConfig;
  logging: {
    /** Directory used for relative log paths ('' => <userData>/logs). */
    directory: string;
  };
  files: {
    /** Editor program for "Edit" ('' => the system editor for the file type, else Notepad). */
    editor: string;
    /** What happens when a transfer target already exists. */
    defaultConflict: ConflictPolicy;
    showHidden: boolean;
    confirmDelete: boolean;
    /** Local deletes go to the Recycle Bin. */
    deleteToRecycleBin: boolean;
    preserveTimestamps: boolean;
    /** Parallel transfers per connection (1-6). */
    parallelTransfers: number;
  };
}

/* ------------------------------------------------------------------------ */
/* Known hosts / credentials                                                 */
/* ------------------------------------------------------------------------ */

export interface KnownHost {
  id: string;
  host: string;
  port: number;
  /** SSH key type, e.g. 'ssh-ed25519', 'ssh-rsa', 'ecdsa-sha2-nistp256'. */
  keyType: string;
  /** Base64 of the SSH wire-format public key blob. */
  key: string;
  /** 'SHA256:...' (OpenSSH style, unpadded base64). */
  fingerprint: string;
  addedAt: number;
  source: 'user' | 'putty-import' | 'openssh-import';
  comment: string;
}

export type HostKeyCheck = 'match' | 'mismatch' | 'unknown';

/* ------------------------------------------------------------------------ */
/* Connections                                                               */
/* ------------------------------------------------------------------------ */

export type ConnStatus = 'connecting' | 'authenticating' | 'connected' | 'disconnected' | 'error';

export interface ConnOpenRequest {
  /** Unique id chosen by the renderer (one per connection attempt). */
  connId: string;
  session: SessionConfig;
  cols: number;
  rows: number;
  widthPx: number;
  heightPx: number;
  /** One-time password (e.g. from the command line -pw); not persisted. */
  password?: string;
}

export interface ConnStatusEvent {
  connId: string;
  status: ConnStatus;
  message?: string;
  /** Present when status is 'disconnected'. */
  clean?: boolean;
  exitCode?: number;
  exitSignal?: string;
  /** Present once connected, informational (e.g. 'SSH-2.0-OpenSSH_9.6, aes256-gcm@openssh.com'). */
  serverInfo?: string;
}

export type LogLevel = 'info' | 'warn' | 'error' | 'debug';

export interface EventLogEntry {
  time: number;
  level: LogLevel;
  message: string;
}

export interface SpecialCommandDef {
  /** e.g. 'break', 'eof', 'ignore', 'rekey', 'signal:INT', 'telnet:ayt' */
  id: string;
  label: string;
  group?: string;
}

/* ------------------------------------------------------------------------ */
/* Prompts (main -> renderer -> main)                                        */
/* ------------------------------------------------------------------------ */

export type PromptInput =
  | { kind: 'username'; title: string; message?: string }
  | { kind: 'password'; title: string; message?: string; username?: string; allowSave: boolean }
  | {
      kind: 'kbdint';
      title: string;
      name?: string;
      instructions?: string;
      prompts: { prompt: string; echo: boolean }[];
      allowSave: boolean;
    }
  | { kind: 'passphrase'; title: string; keyPath: string; message?: string }
  | {
      kind: 'hostkey';
      host: string;
      port: number;
      keyType: string;
      fingerprint: string;
      fingerprintMd5: string;
      status: 'unknown' | 'mismatch';
      /** Fingerprints of the previously stored keys for this host (mismatch only). */
      previous: string[];
    }
  | { kind: 'confirm'; title: string; message: string; buttons: string[]; danger?: boolean };

export type PromptRequest = PromptInput & { id: string; connId: string };

export interface PromptResponse {
  cancelled: boolean;
  /** Answers in prompt order (username, password, kbdint answers, passphrase). */
  values?: string[];
  /** User ticked "remember" (password save). */
  remember?: boolean;
  /** hostkey: 'accept-save' | 'accept-once' | 'reject'. confirm: the chosen button label. */
  choice?: string;
}

/* ------------------------------------------------------------------------ */
/* Import / export                                                           */
/* ------------------------------------------------------------------------ */

/** 'json' = our own JSON backup (matches the C++ ImportPreview::source values). */
export type ImportSource = 'putty-registry' | 'putty-regfile' | 'ssh-config' | 'json';

export interface ImportCandidate {
  /** Stable key within the preview (e.g. original PuTTY session name). */
  key: string;
  name: string;
  source: ImportSource;
  session: SessionConfig;
  warnings: string[];
  /** A saved session with the same name+folder already exists. */
  exists: boolean;
}

export interface ImportPreview {
  source: ImportSource;
  candidates: ImportCandidate[];
  hostKeys: KnownHost[];
  /** PuTTY "Default Settings" mapped to a session, if present. */
  defaultSettings?: SessionConfig;
  errors: string[];
  /** Human readable origin (registry path / file path). */
  origin: string;
}

export interface ImportRequest {
  candidates: ImportCandidate[];
  hostKeys: KnownHost[];
  conflict: 'skip' | 'overwrite' | 'rename';
  /** Folder to put imported sessions under ('' => root). Candidate folders are nested beneath it. */
  targetFolder: string;
  /** Replace our default-session template with PuTTY's "Default Settings". */
  defaultSettings?: SessionConfig;
}

export interface ImportResult {
  imported: number;
  overwritten: number;
  skipped: number;
  hostKeysImported: number;
  errors: string[];
}

/* ------------------------------------------------------------------------ */
/* SFTP                                                                      */
/* ------------------------------------------------------------------------ */

export interface SftpEntry {
  name: string;
  path: string;
  type: 'file' | 'dir' | 'link' | 'other';
  size: number;
  /** ms since epoch */
  mtime: number;
  mode: number;
  /** e.g. 'drwxr-xr-x' */
  permissions: string;
  uid: number;
  gid: number;
  /** For symlinks: whether the target is a directory (best effort). */
  linkTargetIsDir?: boolean;
}

export type RemoteProtocol = 'sftp' | 'scp';

export interface RemoteCapabilities {
  /** Interrupted transfers can continue where they stopped. */
  resume: boolean;
  /** Times are precise to the second (SCP over plain `ls` may only know minutes). */
  preciseTimes: boolean;
}

export interface SftpOpenOptions {
  fileProtocol?: FileProtocol;
  /** Parallel transfers for this connection (1-6, default 2). */
  parallelTransfers?: number;
}

export interface SftpOpenResult {
  sftpId: string;
  home: string;
  protocol: RemoteProtocol;
  capabilities: RemoteCapabilities;
}

/** 'delete' only appears for synchronize deletes. */
export type TransferDirection = 'upload' | 'download' | 'delete';
export type TransferState = 'queued' | 'running' | 'done' | 'error' | 'cancelled';

export interface TransferProgress {
  transferId: string;
  sftpId: string;
  direction: TransferDirection;
  name: string;
  localPath: string;
  remotePath: string;
  transferred: number;
  total: number;
  state: TransferState;
  error?: string;
  /** The source is deleted once copied. */
  move?: boolean;
  /** Folder transfers: files finished / total, file being copied now. */
  filesDone?: number;
  filesTotal?: number;
  currentFile?: string;
  /** Files left alone because of the conflict policy. */
  skipped?: number;
  /** What started the transfer. */
  origin?: 'transfer' | 'edit' | 'sync' | 'drag' | 'open';
}

/** What happens when the target of a transfer already exists. */
export type ConflictPolicy = 'ask' | 'overwrite' | 'newer' | 'skip' | 'resume' | 'rename';

export interface TransferOptions {
  /** Default 'ask'. */
  conflict?: ConflictPolicy;
  /** Delete the source after it was copied. */
  move?: boolean;
  /** Default true. */
  preserveTimestamps?: boolean;
  /** Local deletes (synchronize) go to the Recycle Bin. Default true. */
  deleteToRecycleBin?: boolean;
}

export interface FileStat {
  path: string;
  size: number;
  /** ms since epoch */
  mtime: number;
}

export interface TransferConflict {
  conflictId: string;
  transferId: string;
  sftpId: string;
  direction: TransferDirection;
  name: string;
  source: FileStat;
  target: FileStat;
  /** The target is a shorter copy of the source that can be continued. */
  canResume: boolean;
  /** 'exists': the target exists; 'changed': (edit) the server copy changed since it was opened. */
  reason: 'exists' | 'changed';
  /** More files belong to this request: offer "apply to all". */
  multiple: boolean;
}

export interface ConflictResolution {
  /** 'newer' = overwrite only when the source is newer. */
  action: 'overwrite' | 'newer' | 'skip' | 'resume' | 'rename' | 'cancel';
  /** For 'rename': the new target name ('' => automatic "name (1).ext"). */
  newName?: string;
  /** Apply this answer to the remaining conflicts of the same request. */
  applyToAll?: boolean;
}

export interface FindRequest {
  root: string;
  /** Glob on the file name: `*`, `?`, `[abc]`; several patterns separated by ';'. */
  pattern: string;
  caseSensitive?: boolean;
  includeHidden?: boolean;
  /** Default 10000. */
  maxResults?: number;
}

export interface FindEvent {
  findId: string;
  sftpId: string;
  /** New matches since the previous event. */
  results: SftpEntry[];
  scannedDirs: number;
  /** The folder being searched now. */
  currentDir?: string;
  done: boolean;
  truncated?: boolean;
  error?: string;
}

export type SyncDirection = 'upload' | 'download' | 'both';

export interface SyncRequest {
  localDir: string;
  remoteDir: string;
  /** 'upload' makes the server match this PC, 'download' the other way, 'both' copies newer files each way. */
  direction: SyncDirection;
  compareTime: boolean;
  compareSize: boolean;
  /** Delete files that only exist on the target side (not used for 'both'). */
  deleteExtra: boolean;
  /** ';'-separated masks; a trailing '/' matches folders only, e.g. "*.tmp; .git/". */
  exclude: string;
  includeHidden: boolean;
}

export type SyncAction = 'upload' | 'download' | 'deleteLocal' | 'deleteRemote';

export interface SyncItem {
  /** '/'-separated path relative to both roots. */
  relPath: string;
  isDir: boolean;
  action: SyncAction;
  reason: 'new' | 'newer' | 'older' | 'size' | 'extra';
  local?: { size: number; mtime: number };
  remote?: { size: number; mtime: number };
}

export interface EditOptions {
  /** Editor program ('' => system editor, else Notepad). */
  editor?: string;
}

export interface EditEvent {
  editId: string;
  sftpId: string;
  remotePath: string;
  localPath: string;
  state: 'opened' | 'uploading' | 'uploaded' | 'error' | 'closed';
  error?: string;
}

/** End of a native drag started from a file pane. */
export interface DragResult {
  /** 'internal' = dropped back onto this window (the page decides where). */
  result: 'external' | 'internal' | 'cancelled';
  /** Internal drops: WebView client position in physical pixels. */
  x?: number;
  y?: number;
  /** Shift was held at the drop (move instead of copy). */
  move?: boolean;
}

/* ------------------------------------------------------------------------ */
/* Local files                                                               */
/* ------------------------------------------------------------------------ */

export interface LocalEntry {
  name: string;
  path: string;
  /** 'drive' and 'place' only appear in the "This PC" list (path ''). */
  type: 'file' | 'dir' | 'drive' | 'place';
  size: number;
  /** ms since epoch */
  mtime: number;
  hidden: boolean;
  system: boolean;
  readonly: boolean;
  /** Symbolic link or junction. */
  link: boolean;
  /** Drives: volume label; places: display name. */
  label?: string;
  freeBytes?: number;
  totalBytes?: number;
  icon?: 'desktop' | 'documents' | 'downloads' | 'home' | 'drive' | 'removable' | 'network' | 'cdrom';
}

/* ------------------------------------------------------------------------ */
/* Keys                                                                      */
/* ------------------------------------------------------------------------ */

export type KeyType = 'ed25519' | 'rsa' | 'ecdsa';

export interface KeygenRequest {
  type: KeyType;
  /** RSA: 2048/3072/4096; ECDSA: 256/384/521. */
  bits?: number;
  comment: string;
  passphrase: string;
}

export interface KeygenResult {
  privateKeyOpenSsh: string;
  privateKeyPpk: string;
  publicKeyOpenSsh: string;
  fingerprint: string;
}

export interface KeyInfo {
  path: string;
  format: 'openssh' | 'pem' | 'ppk2' | 'ppk3' | 'unknown';
  keyType: string;
  comment: string;
  encrypted: boolean;
  fingerprint: string;
  publicKeyOpenSsh: string;
}

/* ------------------------------------------------------------------------ */
/* App                                                                       */
/* ------------------------------------------------------------------------ */

export interface AppInfo {
  name: string;
  version: string;
  electron: string;
  chrome: string;
  node: string;
  platform: string;
  userDataDir: string;
  portable: boolean;
}

/** Request (from CLI args or a second app instance) to open a connection. */
export interface OpenRequest {
  session: SessionConfig;
  /** Saved session id when the request referenced a saved session (-load). */
  savedSessionId?: string;
  password?: string;
  /** 'files' for sftp:// and scp:// URLs (default: the session's openAs). */
  view?: SessionView;
  /** Remote folder from an sftp:// / scp:// URL. */
  remotePath?: string;
}

export interface SerialPortInfo {
  path: string;
  friendlyName?: string;
  manufacturer?: string;
}

export interface FileDialogOptions {
  title?: string;
  defaultPath?: string;
  filters?: { name: string; extensions: string[] }[];
}
