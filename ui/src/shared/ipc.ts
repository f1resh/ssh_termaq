/**
 * Bridge contract between the UI (React, running in WebView2) and the native C++ host.
 *
 * The UI uses `api: Api` from src/bridge/api.ts. Every method is listed in API_SCHEMA with its kind:
 *   - 'invoke' : request/response (returns a Promise)
 *   - 'send'   : fire-and-forget message to the host
 *   - 'event'  : host -> UI push; the UI subscribes with a callback and gets an unsubscribe fn
 *   - 'local'  : implemented in the UI bridge itself (special transport)
 * The channel name is always `${namespace}:${method}`.
 *
 * Wire format (JSON via window.chrome.webview.postMessage / ICoreWebView2::PostWebMessageAsJson):
 *   UI -> host  invoke : {"t":"req","id":<number>,"ch":"sessions:list","args":[...]}
 *   host -> UI  result : {"t":"res","id":<number>,"ok":true,"result":<any>}
 *                        {"t":"res","id":<number>,"ok":false,"error":"<message>"}
 *   UI -> host  send   : {"t":"send","ch":"conn:write","args":[...]}
 *   host -> UI  event  : {"t":"evt","ch":"conn:onData","args":[...]}
 *   UI -> host  files  : chrome.webview.postMessageWithAdditionalObjects(
 *                          {"t":"req","id":<n>,"ch":"files:pathsForFiles","args":[]}, FileList)
 *                        host answers with a normal "res" whose result is string[] of absolute paths.
 * `args` is always the positional argument array of the Api method (callbacks excluded).
 * Optional trailing arguments may be omitted or null. Methods returning void resolve with null.
 */
import type {
  AppInfo,
  AppSettings,
  BackgroundMaterial,
  ConflictResolution,
  ConnOpenRequest,
  ConnStatusEvent,
  DeepPartial,
  DragResult,
  EditEvent,
  EditOptions,
  EventLogEntry,
  FileDialogOptions,
  FindEvent,
  FindRequest,
  ImportPreview,
  ImportRequest,
  ImportResult,
  KeygenRequest,
  KeygenResult,
  KeyInfo,
  KnownHost,
  LocalEntry,
  OpenRequest,
  PromptRequest,
  PromptResponse,
  SerialPortInfo,
  SessionConfig,
  SftpEntry,
  SftpOpenOptions,
  SftpOpenResult,
  SpecialCommandDef,
  SyncItem,
  SyncRequest,
  TransferConflict,
  TransferOptions,
  TransferProgress
} from './types';

export type Unsubscribe = () => void;

export interface Api {
  sessions: {
    list(): Promise<SessionConfig[]>;
    /** Insert or update (by id). Returns the stored, normalized session. */
    save(session: SessionConfig): Promise<SessionConfig>;
    /** Bulk insert/update (moves between folders, imports). */
    saveMany(sessions: SessionConfig[]): Promise<void>;
    delete(ids: string[]): Promise<void>;
    /** All folder paths (explicit + implied by sessions), sorted. */
    folders(): Promise<string[]>;
    createFolder(path: string): Promise<void>;
    /** Renames a folder and re-parents all sessions/sub-folders beneath it. */
    renameFolder(oldPath: string, newPath: string): Promise<void>;
    /** deleteSessions=false moves contained sessions to the parent folder. */
    deleteFolder(path: string, deleteSessions: boolean): Promise<void>;
    /** Fired after any change to sessions or folders (from any source). */
    onChanged(cb: () => void): Unsubscribe;
  };
  credentials: {
    hasPassword(sessionId: string): Promise<boolean>;
    setPassword(sessionId: string, password: string): Promise<void>;
    clearPassword(sessionId: string): Promise<void>;
  };
  settings: {
    get(): Promise<AppSettings>;
    /** Deep-merge patch (arrays replace). Returns the new settings and broadcasts onChanged. */
    update(patch: DeepPartial<AppSettings>): Promise<AppSettings>;
    onChanged(cb: (settings: AppSettings) => void): Unsubscribe;
  };
  conn: {
    /** Starts a connection attempt. Resolves once started; progress arrives via onStatus/onData. */
    open(req: ConnOpenRequest): Promise<void>;
    write(connId: string, data: string, binary?: boolean): void;
    resize(connId: string, cols: number, rows: number, widthPx: number, heightPx: number): void;
    close(connId: string): void;
    special(connId: string, commandId: string): void;
    specials(connId: string): Promise<SpecialCommandDef[]>;
    eventLog(connId: string): Promise<EventLogEntry[]>;
    /** Decoded terminal output (already converted from the session charset). */
    onData(cb: (connId: string, data: string) => void): Unsubscribe;
    onStatus(cb: (event: ConnStatusEvent) => void): Unsubscribe;
    onEventLog(cb: (connId: string, entry: EventLogEntry) => void): Unsubscribe;
  };
  prompts: {
    onPrompt(cb: (prompt: PromptRequest) => void): Unsubscribe;
    /** Main withdrew a pending prompt (connection closed / timed out). */
    onCancel(cb: (promptId: string) => void): Unsubscribe;
    respond(promptId: string, response: PromptResponse): void;
  };
  knownHosts: {
    list(): Promise<KnownHost[]>;
    remove(ids: string[]): Promise<void>;
    /** Import an OpenSSH known_hosts file (dialog when path omitted). Returns number imported. */
    importOpenSsh(path?: string): Promise<number>;
    onChanged(cb: () => void): Unsubscribe;
  };
  importer: {
    scanPuttyRegistry(): Promise<ImportPreview>;
    /** Shows an open dialog when path is omitted; null when cancelled. */
    parsePuttyRegFile(path?: string): Promise<ImportPreview | null>;
    /** Defaults to ~/.ssh/config. */
    scanSshConfig(path?: string): Promise<ImportPreview>;
    /** Our own JSON backup format. Dialog when path omitted; null when cancelled. */
    parseJsonFile(path?: string): Promise<ImportPreview | null>;
    apply(req: ImportRequest): Promise<ImportResult>;
    /** Shows a save dialog. Returns the written path or null when cancelled. */
    exportPuttyRegFile(sessionIds: string[]): Promise<string | null>;
    exportJsonFile(sessionIds: string[]): Promise<string | null>;
  };
  /**
   * Remote files of a connected SSH connection, over SFTP or (fallback / per session) SCP. The namespace keeps its
   * historical name; `open` reports which protocol is in use.
   */
  sftp: {
    open(connId: string, opts?: SftpOpenOptions): Promise<SftpOpenResult>;
    list(sftpId: string, path: string): Promise<SftpEntry[]>;
    /** null when the path does not exist. Follows symlinks. */
    stat(sftpId: string, path: string): Promise<SftpEntry | null>;
    realpath(sftpId: string, path: string): Promise<string>;
    mkdir(sftpId: string, path: string): Promise<void>;
    rename(sftpId: string, from: string, to: string): Promise<void>;
    remove(sftpId: string, path: string, recursive: boolean): Promise<void>;
    chmod(sftpId: string, path: string, mode: number): Promise<void>;
    /** localDir omitted => folder picker. Returns transfer ids ([] when cancelled). */
    download(sftpId: string, remotePaths: string[], localDir?: string, opts?: TransferOptions): Promise<string[]>;
    /** localPaths omitted => file picker. Directories upload recursively. */
    upload(sftpId: string, remoteDir: string, localPaths?: string[], opts?: TransferOptions): Promise<string[]>;
    cancelTransfer(transferId: string): void;
    /** Download to a temp file and open it with the default application (executables are only revealed). */
    openRemote(sftpId: string, remotePath: string): Promise<void>;
    close(sftpId: string): void;
    onProgress(cb: (p: TransferProgress) => void): Unsubscribe;
    /** A transfer with conflict policy 'ask' hit an existing target and waits for resolveConflict. */
    onConflict(cb: (c: TransferConflict) => void): Unsubscribe;
    resolveConflict(conflictId: string, resolution: ConflictResolution): void;
    /** Starts a recursive name search; results arrive via onFind. Returns the find id. */
    find(sftpId: string, req: FindRequest): Promise<string>;
    cancelFind(findId: string): void;
    onFind(cb: (e: FindEvent) => void): Unsubscribe;
    /** Compares a local and a remote folder tree. */
    syncCompare(sftpId: string, req: SyncRequest): Promise<SyncItem[]>;
    /** Queues the chosen items (deletes run in the queue too). Returns transfer ids. */
    syncApply(sftpId: string, req: SyncRequest, items: SyncItem[], opts?: TransferOptions): Promise<string[]>;
    /** Downloads to a temp file, opens an editor and uploads every saved change. Returns the edit id. */
    edit(sftpId: string, remotePath: string, opts?: EditOptions): Promise<string>;
    stopEdit(editId: string): void;
    onEdit(cb: (e: EditEvent) => void): Unsubscribe;
    /** Native drag of remote items (virtual files for Explorer). Resolves when the drag ends. */
    startDrag(sftpId: string, remotePaths: string[]): Promise<DragResult>;
  };
  /** Local disks of this PC (the left pane of the file manager). */
  local: {
    /** '' => "This PC": known folders and drives. */
    list(path: string): Promise<LocalEntry[]>;
    /** null when the path does not exist. */
    stat(path: string): Promise<LocalEntry | null>;
    /** The user's Documents folder. */
    home(): Promise<string>;
    mkdir(path: string): Promise<void>;
    rename(from: string, to: string): Promise<void>;
    remove(paths: string[], toRecycleBin: boolean): Promise<void>;
    /** Copies or moves into targetDir with the Windows progress UI. */
    copy(paths: string[], targetDir: string, move: boolean): Promise<void>;
    open(path: string): Promise<void>;
    reveal(path: string): Promise<void>;
    /** Watches one folder (not recursive); onChanged fires (debounced) after changes. Returns the watch id. */
    watch(path: string): Promise<string>;
    unwatch(watchId: string): void;
    onChanged(cb: (watchId: string) => void): Unsubscribe;
    /** Native drag of local files (to Explorer or back into this window). Resolves when the drag ends. */
    startDrag(paths: string[]): Promise<DragResult>;
  };
  keys: {
    generate(req: KeygenRequest): Promise<KeygenResult>;
    inspect(path: string, passphrase?: string): Promise<KeyInfo>;
    /** Returns converted key file content. */
    convert(path: string, passphrase: string, to: 'openssh' | 'ppk'): Promise<string>;
  };
  serial: {
    listPorts(): Promise<SerialPortInfo[]>;
  };
  dialog: {
    openFile(opts?: FileDialogOptions): Promise<string | null>;
    openFiles(opts?: FileDialogOptions): Promise<string[]>;
    openDirectory(opts?: FileDialogOptions): Promise<string | null>;
    saveFile(opts?: FileDialogOptions): Promise<string | null>;
    /** Save dialog + write. Returns written path or null. */
    saveTextFile(content: string, opts?: FileDialogOptions): Promise<string | null>;
  };
  app: {
    info(): Promise<AppInfo>;
    openExternal(url: string): Promise<void>;
    openPath(path: string): Promise<void>;
    showItemInFolder(path: string): void;
    setBackgroundMaterial(material: BackgroundMaterial): void;
    /** Tells the host which theme variant is active (dark title bar / window frame / native dialogs). */
    setThemeSource(source: 'system' | 'light' | 'dark'): void;
    systemPrefersDark(): Promise<boolean>;
    onSystemThemeChanged(cb: (dark: boolean) => void): Unsubscribe;
    toggleFullscreen(): void;
    onFullscreenChanged(cb: (fullscreen: boolean) => void): Unsubscribe;
    setUiScale(factor: number): void;
    /** Drains open requests queued from CLI args / second instances. */
    takeOpenRequests(): Promise<OpenRequest[]>;
    onOpenRequest(cb: (req: OpenRequest) => void): Unsubscribe;
    getState<T = unknown>(key: string): Promise<T | undefined>;
    setState(key: string, value: unknown): Promise<void>;
    readClipboard(): Promise<string>;
    writeClipboard(text: string): void;
    /** System bell. */
    beep(): void;
    /** data: URL of a local (audio) file, null if unreadable. */
    readFileDataUrl(path: string): Promise<string | null>;
    logDirectory(): Promise<string>;
    /** Main asks the renderer whether it may close (connected sessions). Renderer answers via confirmClose. */
    onCloseRequested(cb: () => void): Unsubscribe;
    confirmClose(allow: boolean): void;
    quit(): void;
  };
  /** Native window controls (the UI draws its own title bar with tabs + caption buttons). */
  window: {
    minimize(): void;
    toggleMaximize(): void;
    close(): void;
    isMaximized(): Promise<boolean>;
    onMaximizedChanged(cb: (maximized: boolean) => void): Unsubscribe;
    /** Window activation (to dim the title bar when inactive). */
    onFocusChanged(cb: (focused: boolean) => void): Unsubscribe;
    setTitle(title: string): void;
    /** Flash the taskbar button (bell while inactive). */
    flash(): void;
  };
  /** Special transport: postMessageWithAdditionalObjects (see wire format above). */
  files: {
    pathsForFiles(files: File[] | FileList): Promise<string[]>;
  };
}

export type IpcKind = 'invoke' | 'send' | 'event' | 'local';

type Schema = { [NS in keyof Api]: { [M in keyof Api[NS]]: IpcKind } };

export const API_SCHEMA = {
  sessions: {
    list: 'invoke',
    save: 'invoke',
    saveMany: 'invoke',
    delete: 'invoke',
    folders: 'invoke',
    createFolder: 'invoke',
    renameFolder: 'invoke',
    deleteFolder: 'invoke',
    onChanged: 'event'
  },
  credentials: {
    hasPassword: 'invoke',
    setPassword: 'invoke',
    clearPassword: 'invoke'
  },
  settings: {
    get: 'invoke',
    update: 'invoke',
    onChanged: 'event'
  },
  conn: {
    open: 'invoke',
    write: 'send',
    resize: 'send',
    close: 'send',
    special: 'send',
    specials: 'invoke',
    eventLog: 'invoke',
    onData: 'event',
    onStatus: 'event',
    onEventLog: 'event'
  },
  prompts: {
    onPrompt: 'event',
    onCancel: 'event',
    respond: 'send'
  },
  knownHosts: {
    list: 'invoke',
    remove: 'invoke',
    importOpenSsh: 'invoke',
    onChanged: 'event'
  },
  importer: {
    scanPuttyRegistry: 'invoke',
    parsePuttyRegFile: 'invoke',
    scanSshConfig: 'invoke',
    parseJsonFile: 'invoke',
    apply: 'invoke',
    exportPuttyRegFile: 'invoke',
    exportJsonFile: 'invoke'
  },
  sftp: {
    open: 'invoke',
    list: 'invoke',
    stat: 'invoke',
    realpath: 'invoke',
    mkdir: 'invoke',
    rename: 'invoke',
    remove: 'invoke',
    chmod: 'invoke',
    download: 'invoke',
    upload: 'invoke',
    cancelTransfer: 'send',
    openRemote: 'invoke',
    close: 'send',
    onProgress: 'event',
    onConflict: 'event',
    resolveConflict: 'send',
    find: 'invoke',
    cancelFind: 'send',
    onFind: 'event',
    syncCompare: 'invoke',
    syncApply: 'invoke',
    edit: 'invoke',
    stopEdit: 'send',
    onEdit: 'event',
    startDrag: 'invoke'
  },
  local: {
    list: 'invoke',
    stat: 'invoke',
    home: 'invoke',
    mkdir: 'invoke',
    rename: 'invoke',
    remove: 'invoke',
    copy: 'invoke',
    open: 'invoke',
    reveal: 'invoke',
    watch: 'invoke',
    unwatch: 'send',
    onChanged: 'event',
    startDrag: 'invoke'
  },
  keys: {
    generate: 'invoke',
    inspect: 'invoke',
    convert: 'invoke'
  },
  serial: {
    listPorts: 'invoke'
  },
  dialog: {
    openFile: 'invoke',
    openFiles: 'invoke',
    openDirectory: 'invoke',
    saveFile: 'invoke',
    saveTextFile: 'invoke'
  },
  app: {
    info: 'invoke',
    openExternal: 'invoke',
    openPath: 'invoke',
    showItemInFolder: 'send',
    setBackgroundMaterial: 'send',
    setThemeSource: 'send',
    systemPrefersDark: 'invoke',
    onSystemThemeChanged: 'event',
    toggleFullscreen: 'send',
    onFullscreenChanged: 'event',
    setUiScale: 'send',
    takeOpenRequests: 'invoke',
    onOpenRequest: 'event',
    getState: 'invoke',
    setState: 'invoke',
    readClipboard: 'invoke',
    writeClipboard: 'send',
    beep: 'send',
    readFileDataUrl: 'invoke',
    logDirectory: 'invoke',
    onCloseRequested: 'event',
    confirmClose: 'send',
    quit: 'send'
  },
  window: {
    minimize: 'send',
    toggleMaximize: 'send',
    close: 'send',
    isMaximized: 'invoke',
    onMaximizedChanged: 'event',
    onFocusChanged: 'event',
    setTitle: 'send',
    flash: 'send'
  },
  files: {
    pathsForFiles: 'local'
  }
} as const satisfies Schema;

export function channelOf(ns: string, method: string): string {
  return `${ns}:${method}`;
}
