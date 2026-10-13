/**
 * Pure helpers for the session editor: section catalogue, validation (session, tunnels), protocol switching,
 * host-string parsing and the settings patch used when saving "Default Settings".
 * No React / bridge imports so it can be unit tested in node.
 */
import { DEFAULT_PORTS, newId, normalizeFolder } from '@shared/defaults';
import type { AppearanceOverrides, PortForward, Protocol, SessionConfig } from '@shared/types';

/* -------------------------------------------------------------------------- */
/* Sections                                                                   */
/* -------------------------------------------------------------------------- */

export type SectionId =
  | 'session'
  | 'connection'
  | 'proxy'
  | 'ssh'
  | 'ssh.auth'
  | 'ssh.tunnels'
  | 'ssh.x11'
  | 'ssh.algorithms'
  | 'telnet'
  | 'rlogin'
  | 'serial'
  | 'terminal'
  | 'keyboard'
  | 'appearance'
  | 'translation'
  | 'logging';

export interface SectionDef {
  id: SectionId;
  label: string;
  group: 'General' | 'Protocol' | 'Terminal';
  depth: 0 | 1;
  /** Protocols for which the section is relevant (undefined = all). */
  protocols?: readonly Protocol[];
}

const NETWORK: readonly Protocol[] = ['ssh', 'telnet', 'rlogin', 'raw'];

export const SESSION_SECTIONS: readonly SectionDef[] = [
  { id: 'session', label: 'Session', group: 'General', depth: 0 },
  { id: 'connection', label: 'Connection', group: 'General', depth: 0, protocols: NETWORK },
  { id: 'proxy', label: 'Proxy', group: 'General', depth: 0, protocols: NETWORK },
  { id: 'ssh', label: 'SSH', group: 'Protocol', depth: 0, protocols: ['ssh'] },
  { id: 'ssh.auth', label: 'Authentication', group: 'Protocol', depth: 1, protocols: ['ssh'] },
  { id: 'ssh.tunnels', label: 'Tunnels', group: 'Protocol', depth: 1, protocols: ['ssh'] },
  { id: 'ssh.x11', label: 'X11', group: 'Protocol', depth: 1, protocols: ['ssh'] },
  { id: 'ssh.algorithms', label: 'Algorithms', group: 'Protocol', depth: 1, protocols: ['ssh'] },
  { id: 'telnet', label: 'Telnet', group: 'Protocol', depth: 0, protocols: ['telnet'] },
  { id: 'rlogin', label: 'Rlogin', group: 'Protocol', depth: 0, protocols: ['rlogin'] },
  { id: 'serial', label: 'Serial', group: 'Protocol', depth: 0, protocols: ['serial'] },
  { id: 'terminal', label: 'Terminal', group: 'Terminal', depth: 0 },
  { id: 'keyboard', label: 'Keyboard', group: 'Terminal', depth: 0 },
  { id: 'appearance', label: 'Appearance', group: 'Terminal', depth: 0 },
  { id: 'translation', label: 'Translation', group: 'Terminal', depth: 0 },
  { id: 'logging', label: 'Logging', group: 'Terminal', depth: 0 }
];

/** Sections shown for a protocol. The "Default Settings" template shows everything (it applies to all protocols). */
export function visibleSections(protocol: Protocol, showAll = false): SectionDef[] {
  return SESSION_SECTIONS.filter((s) => showAll || !s.protocols || s.protocols.includes(protocol));
}

export function isSectionId(v: string | undefined): v is SectionId {
  return !!v && SESSION_SECTIONS.some((s) => s.id === v);
}

/* -------------------------------------------------------------------------- */
/* Basic validators                                                           */
/* -------------------------------------------------------------------------- */

export function isValidPort(n: unknown): boolean {
  return typeof n === 'number' && Number.isInteger(n) && n >= 1 && n <= 65535;
}

const IPV4 = /^(25[0-5]|2[0-4]\d|1\d\d|[1-9]?\d)(\.(25[0-5]|2[0-4]\d|1\d\d|[1-9]?\d)){3}$/;
const HOSTNAME = /^(?=.{1,253}$)[A-Za-z0-9_]([A-Za-z0-9_-]{0,62})(\.[A-Za-z0-9_]([A-Za-z0-9_-]{0,62}))*\.?$/;

export function isIPv4(s: string): boolean {
  return IPV4.test(s);
}

/** Loose IPv6 check (hex groups, '::' compression, optional zone id / embedded IPv4). */
export function isIPv6(s: string): boolean {
  const v = s.replace(/^\[|\]$/g, '').replace(/%[\w.-]+$/, '');
  if (!v.includes(':')) return false;
  if ((v.match(/::/g) ?? []).length > 1) return false;
  const parts = v.split(':');
  if (parts.length > 8) return false;
  return parts.every((p, i) => p === '' || /^[0-9a-fA-F]{1,4}$/.test(p) || (i === parts.length - 1 && IPV4.test(p)));
}

/** Host name, IPv4 or IPv6 literal (brackets allowed). */
export function isValidHost(s: string): boolean {
  const v = s.trim();
  if (!v || /\s/.test(v)) return false;
  return isIPv4(v) || isIPv6(v) || HOSTNAME.test(v);
}

/** Bind address: '' (default), '*', host name or IP literal. */
export function isValidBindAddress(s: string): boolean {
  const v = s.trim();
  return v === '' || v === '*' || isValidHost(v);
}

/* -------------------------------------------------------------------------- */
/* Host input parsing                                                         */
/* -------------------------------------------------------------------------- */

export interface ParsedHost {
  host: string;
  username?: string;
  port?: number;
}

/**
 * Splits "user@host:port", "host:port", "[v6]:port" and plain hosts. A bare IPv6 literal is returned as host.
 * Only a trailing port that is a valid port number is split off.
 */
export function splitHostInput(raw: string): ParsedHost {
  let s = raw.trim();
  const out: ParsedHost = { host: s };
  const at = s.lastIndexOf('@');
  if (at > 0) {
    out.username = s.slice(0, at);
    s = s.slice(at + 1);
  }
  const bracket = /^\[([^\]]+)\](?::(\d+))?$/.exec(s);
  if (bracket) {
    out.host = bracket[1];
    if (bracket[2] && isValidPort(Number(bracket[2]))) out.port = Number(bracket[2]);
    return out;
  }
  const colons = (s.match(/:/g) ?? []).length;
  if (colons === 1) {
    const [h, p] = s.split(':');
    if (/^\d+$/.test(p) && isValidPort(Number(p))) {
      out.host = h;
      out.port = Number(p);
      return out;
    }
  }
  out.host = s;
  return out;
}

/* -------------------------------------------------------------------------- */
/* Tunnels                                                                    */
/* -------------------------------------------------------------------------- */

export type TunnelField = 'sourcePort' | 'destHost' | 'destPort' | 'bindAddress';
export type TunnelErrors = Partial<Record<TunnelField, string>>;

export function validatePortForward(t: PortForward): TunnelErrors {
  const e: TunnelErrors = {};
  if (!isValidPort(t.sourcePort)) e.sourcePort = 'Source port must be 1–65535';
  if (!isValidBindAddress(t.bindAddress)) e.bindAddress = 'Invalid bind address';
  if (t.type !== 'dynamic') {
    if (!t.destHost.trim()) e.destHost = 'Destination host is required';
    else if (!isValidHost(t.destHost)) e.destHost = 'Invalid destination host';
    if (!isValidPort(t.destPort)) e.destPort = 'Destination port must be 1–65535';
  }
  return e;
}

/** Which side listens: local & dynamic forwardings listen locally, remote ones on the server. */
function listenSide(t: PortForward): 'local' | 'remote' {
  return t.type === 'remote' ? 'remote' : 'local';
}

/** Per-tunnel errors including duplicate listeners (same side + bind address + port among enabled tunnels). */
export function validateTunnels(list: readonly PortForward[]): Record<string, TunnelErrors> {
  const out: Record<string, TunnelErrors> = {};
  const seen = new Map<string, string>();
  for (const t of list) {
    const e = validatePortForward(t);
    if (t.enabled && !e.sourcePort) {
      const key = `${listenSide(t)}|${t.bindAddress.trim().toLowerCase()}|${t.sourcePort}`;
      const other = seen.get(key);
      if (other) e.sourcePort = `Port ${t.sourcePort} is already used by another ${listenSide(t)} forwarding`;
      else seen.set(key, t.id);
    }
    if (Object.keys(e).length) out[t.id] = e;
  }
  return out;
}

/** PuTTY-style one-line summary, e.g. "L8080 → db:5432", "D1080", "R2222 → localhost:22". */
export function describeTunnel(t: PortForward): string {
  const letter = t.type === 'local' ? 'L' : t.type === 'remote' ? 'R' : 'D';
  const bind = t.bindAddress ? `${t.bindAddress}:` : '';
  const src = `${letter}${bind}${t.sourcePort || '?'}`;
  if (t.type === 'dynamic') return src;
  const host = t.destHost.includes(':') ? `[${t.destHost}]` : t.destHost;
  return `${src} → ${host || '?'}:${t.destPort || '?'}`;
}

/* -------------------------------------------------------------------------- */
/* Session validation                                                         */
/* -------------------------------------------------------------------------- */

export interface ValidationResult {
  /** Field path -> message. Paths: 'host', 'port', 'serial.line', 'proxy.host', 'tunnel:<id>:<field>', ... */
  errors: Record<string, string>;
  warnings: Record<string, string>;
}

export interface ValidationContext {
  /** Saved sessions (for duplicate-name warnings). */
  sessions: readonly Pick<SessionConfig, 'id' | 'name' | 'folder'>[];
  /** Validating the "Default Settings" template (host not required). */
  isDefault?: boolean;
}

const PROXY_NEEDS_ENDPOINT = new Set(['socks4', 'socks5', 'http', 'telnet']);

export function validateSession(s: SessionConfig, ctx: ValidationContext): ValidationResult {
  const errors: Record<string, string> = {};
  const warnings: Record<string, string> = {};
  const isDefault = !!ctx.isDefault;

  if (s.protocol === 'serial') {
    if (!s.serial.line.trim()) errors['serial.line'] = 'Serial line is required (e.g. COM1)';
    if (!Number.isInteger(s.serial.baudRate) || s.serial.baudRate <= 0) errors['serial.baudRate'] = 'Speed must be a positive number';
  } else {
    if (!isDefault) {
      if (!s.host.trim()) errors.host = 'Host name is required';
      else if (!isValidHost(s.host)) errors.host = 'Not a valid host name or IP address';
    } else if (s.host.trim() && !isValidHost(s.host)) {
      errors.host = 'Not a valid host name or IP address';
    }
    if (!isValidPort(s.port) && !(isDefault && s.port === 0)) errors.port = 'Port must be 1–65535';
  }

  if (s.protocol !== 'serial') {
    if (s.connection.keepaliveSeconds < 0) errors['connection.keepaliveSeconds'] = 'Must be 0 (off) or more';
    if (s.connection.connectTimeoutSeconds < 0) errors['connection.connectTimeoutSeconds'] = 'Must be 0 or more';
    const names = new Set<string>();
    s.connection.environment.forEach((v, i) => {
      const n = v.name.trim();
      if (!n) errors[`env:${i}`] = 'Variable name is required';
      else if (/[\s=]/.test(n)) errors[`env:${i}`] = 'Name must not contain spaces or "="';
      else if (names.has(n)) warnings[`env:${i}`] = 'Duplicate variable';
      names.add(n);
    });

    const p = s.proxy;
    if (PROXY_NEEDS_ENDPOINT.has(p.type)) {
      if (!p.host.trim()) errors['proxy.host'] = 'Proxy host is required';
      else if (!isValidHost(p.host)) errors['proxy.host'] = 'Not a valid host name or IP address';
      if (!isValidPort(p.port)) errors['proxy.port'] = 'Port must be 1–65535';
    } else if (p.type === 'command') {
      if (!p.localCommand.trim()) errors['proxy.localCommand'] = 'Local proxy command is required';
    } else if (p.type === 'ssh') {
      if (!p.jumpSessionId) {
        if (!p.host.trim()) errors['proxy.host'] = 'Choose a saved session or enter the jump host';
        else if (!isValidHost(p.host)) errors['proxy.host'] = 'Not a valid host name or IP address';
        if (!isValidPort(p.port)) errors['proxy.port'] = 'Port must be 1–65535';
      } else if (p.jumpSessionId === s.id) {
        errors['proxy.jumpSessionId'] = 'A session cannot be its own jump host';
      }
    }
  }

  if (s.protocol === 'ssh' || isDefault) {
    const tunnelErrors = validateTunnels(s.ssh.tunnels);
    for (const [id, e] of Object.entries(tunnelErrors)) {
      for (const [field, msg] of Object.entries(e)) errors[`tunnel:${id}:${field}`] = msg as string;
    }
    if (s.ssh.x11.enabled && !s.ssh.x11.display.trim()) errors['ssh.x11.display'] = 'X display location is required';
    if (s.ssh.noShell && s.ssh.tunnels.length === 0 && !s.ssh.remoteCommand) {
      warnings['ssh.noShell'] = 'With no shell and no tunnels the connection will do nothing';
    }
  }

  if (s.terminal.scrollbackLines < 0) errors['terminal.scrollbackLines'] = 'Must be 0 (use global) or more';
  if (s.terminal.bell === 'sound' && !s.terminal.bellSoundPath.trim()) warnings['terminal.bellSoundPath'] = 'No sound file selected';
  if (s.logging.mode !== 'off' && !s.logging.path.trim()) errors['logging.path'] = 'Log file name is required';

  if (!isDefault) {
    const name = s.name.trim();
    if (name && /[\\]/.test(name)) errors.name = 'Name must not contain "\\"';
    const folder = normalizeFolder(s.folder);
    if (name && ctx.sessions.some((o) => o.id !== s.id && o.name.trim().toLowerCase() === name.toLowerCase() && normalizeFolder(o.folder) === folder)) {
      warnings.name = `Another session named "${name}" already exists in ${folder ? `"${folder}"` : 'the root folder'}`;
    }
  }

  return { errors, warnings };
}

/** Section that displays a given validation field path. */
export function sectionOfField(path: string, protocol: Protocol): SectionId {
  if (path.startsWith('tunnel:')) return 'ssh.tunnels';
  if (path.startsWith('env:') || path.startsWith('connection.')) return 'connection';
  if (path.startsWith('proxy.')) return 'proxy';
  if (path.startsWith('ssh.x11')) return 'ssh.x11';
  if (path.startsWith('ssh.')) return 'ssh';
  if (path.startsWith('serial.')) return protocol === 'serial' ? 'session' : 'serial';
  if (path.startsWith('terminal.')) return 'terminal';
  if (path.startsWith('logging.')) return 'logging';
  return 'session';
}

export function sectionsWithErrors(errors: Record<string, string>, protocol: Protocol): Set<SectionId> {
  return new Set(Object.keys(errors).map((k) => sectionOfField(k, protocol)));
}

/* -------------------------------------------------------------------------- */
/* Session construction / transformation                                      */
/* -------------------------------------------------------------------------- */

/** Change protocol; the port follows the protocol default when it was still the old default (or unset). */
export function switchProtocol(s: SessionConfig, protocol: Protocol): SessionConfig {
  if (s.protocol === protocol) return s;
  const oldDefault = DEFAULT_PORTS[s.protocol];
  const keepPort = s.port && s.port !== oldDefault;
  return { ...s, protocol, port: keepPort ? s.port : DEFAULT_PORTS[protocol] };
}

/** New session from a template (e.g. settings.defaultSession): fresh id, timestamps, empty name. */
export function sessionFromTemplate(template: SessionConfig, overrides: Partial<SessionConfig> = {}): SessionConfig {
  const now = Date.now();
  const s = structuredClone(template);
  s.id = newId();
  s.name = '';
  s.createdAt = now;
  s.updatedAt = now;
  s.lastUsedAt = 0;
  // Tunnels need unique ids per session.
  s.ssh.tunnels = s.ssh.tunnels.map((t) => ({ ...t, id: newId() }));
  return { ...s, ...structuredClone(overrides), id: overrides.id ?? s.id };
}

/** Name used when the user leaves it empty. */
export function fallbackSessionName(s: SessionConfig): string {
  if (s.protocol === 'serial') return s.serial.line || 'Serial';
  const user = s.username ? `${s.username}@` : '';
  const port = s.port && s.port !== DEFAULT_PORTS[s.protocol] ? `:${s.port}` : '';
  return `${user}${s.host}${port}` || 'New session';
}

/** Prepares a draft for saving (trimmed fields, normalized folder, fallback name, timestamps). */
export function finalizeSession(s: SessionConfig, isDefault: boolean): SessionConfig {
  const out = structuredClone(s);
  out.host = out.host.trim();
  out.username = out.username.trim();
  out.folder = normalizeFolder(out.folder);
  out.name = out.name.trim();
  if (!isDefault && !out.name) out.name = fallbackSessionName(out);
  out.connection.environment = out.connection.environment.filter((v) => v.name.trim() || v.value);
  out.updatedAt = Date.now();
  return out;
}

const APPEARANCE_KEYS: readonly (keyof AppearanceOverrides)[] = [
  'fontFamily',
  'fontSize',
  'cursorStyle',
  'cursorBlink',
  'colorScheme',
  'customScheme',
  'boldIsBright'
];

/**
 * settings.update() deep-merges and ignores undefined, so removing an appearance override from the
 * "Default Settings" template must be sent as an explicit null (JSON merge-patch semantics: null deletes).
 */
export function defaultSessionPatch(prev: SessionConfig, next: SessionConfig): SessionConfig {
  const out = structuredClone(next) as SessionConfig & { appearance: Record<string, unknown> };
  for (const k of APPEARANCE_KEYS) {
    if (prev.appearance[k] !== undefined && prev.appearance[k] !== null && next.appearance[k] === undefined) {
      out.appearance[k] = null;
    }
  }
  return out;
}

/** Deterministic JSON (sorted keys) for dirty checking. */
export function stableStringify(v: unknown): string {
  return JSON.stringify(v, (_k, val: unknown) => {
    if (val && typeof val === 'object' && !Array.isArray(val)) {
      return Object.fromEntries(
        Object.entries(val as Record<string, unknown>)
          .filter(([, x]) => x !== undefined)
          .sort(([a], [b]) => (a < b ? -1 : a > b ? 1 : 0))
      );
    }
    return val;
  });
}

/* -------------------------------------------------------------------------- */
/* Log file name placeholders                                                 */
/* -------------------------------------------------------------------------- */

/**
 * Expands PuTTY log file placeholders: &Y year, &M month, &D day, &T time (HHMMSS), &H host, &P port,
 * &S session name, && literal '&'. Unknown sequences are kept verbatim.
 */
export function expandLogPath(template: string, v: { host: string; port: number; session: string; date: Date }): string {
  const pad = (n: number, w = 2) => String(n).padStart(w, '0');
  const d = v.date;
  const map: Record<string, string> = {
    Y: String(d.getFullYear()),
    M: pad(d.getMonth() + 1),
    D: pad(d.getDate()),
    T: `${pad(d.getHours())}${pad(d.getMinutes())}${pad(d.getSeconds())}`,
    H: v.host,
    P: String(v.port),
    S: v.session,
    '&': '&'
  };
  return template.replace(/&(.)/g, (m, c: string) => map[c.toUpperCase()] ?? map[c] ?? m);
}
