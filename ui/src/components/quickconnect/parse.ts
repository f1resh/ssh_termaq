/**
 * Quick-connect target parser (pure; unit-tested). Accepted forms:
 *
 *   host                         web-01, 10.0.0.5, ::1, fe80::1%eth0
 *   user@host[:port]             deploy@web-01:2222   (the last '@' separates user and host)
 *   [v6]:port                    [::1]:2222
 *   proto://[user[:pw]@]host[:port][/...]   ssh://root@h:22  (password part is ignored, path ignored)
 *   sftp://[user@]host[:port][/path]   scp://...   open the file manager (SSH) in that folder
 *   serial://COM3[?baud=115200&data=8&parity=n&stop=1&flow=xonxoff]   also serial://COM3:9600, COM3
 *   proto host [port]            telnet router 2323, ssh deploy@web-01 2222
 *   CLI flags                    -ssh/-telnet/-rlogin/-raw/-serial, -P/-p <port>, -l <user>
 */
import { DEFAULT_PORTS } from '@shared/defaults';
import { osTerms } from '@shared/platform';
import type { FileProtocol, FlowControl, Parity, Protocol, SerialOptions, SessionConfig } from '@shared/types';

export const PROTOCOLS: Protocol[] = ['ssh', 'telnet', 'rlogin', 'raw', 'serial'];

export interface QuickConnectTarget {
  protocol: Protocol;
  /** Host name / address (IPv6 without brackets). For serial: the line, e.g. COM3. */
  host: string;
  /** Explicit port; undefined => protocol default. */
  port?: number;
  username?: string;
  /** Serial line settings given in the input (serial only). */
  serial?: Partial<SerialOptions>;
  /** sftp:// / scp:// URLs: open the file manager with this protocol. */
  files?: { protocol: FileProtocol; path?: string };
}

export type QuickConnectParse = { ok: true; target: QuickConnectTarget } | { ok: false; error: string };

const isProtocol = (s: string): s is Protocol => (PROTOCOLS as string[]).includes(s.toLowerCase());
const SERIAL_LINE = /^(?:COM\d{1,3}(?::\d+)?|\\\\\.\\COM\d{1,3}|\/dev\/\S+)$/i;
const BAUD_RATES = new Set([110, 300, 600, 1200, 2400, 4800, 9600, 14400, 19200, 28800, 38400, 57600, 115200, 128000, 230400, 256000, 460800, 921600]);

function parsePort(raw: string): number | null {
  if (!/^\d{1,5}$/.test(raw)) return null;
  const n = Number(raw);
  return n >= 1 && n <= 65535 ? n : null;
}

function validHost(h: string): boolean {
  return h.length > 0 && !/[\s/\\?#]/.test(h);
}

/** Splits "user@host:port" / "[v6]:port" / "v6" into parts (no protocol prefix). */
function parseAuthority(s: string): { username?: string; host: string; port?: number } | string {
  let rest = s;
  let username: string | undefined;
  const at = rest.lastIndexOf('@');
  if (at >= 0) {
    username = rest.slice(0, at);
    rest = rest.slice(at + 1);
    // "user:password@host" — the password is never used from here.
    const colon = username.indexOf(':');
    if (colon >= 0) username = username.slice(0, colon);
    if (!username) username = undefined;
  }
  let host = rest;
  let port: number | undefined;
  if (rest.startsWith('[')) {
    const close = rest.indexOf(']');
    if (close < 0) return 'Missing "]" after IPv6 address';
    host = rest.slice(1, close);
    const after = rest.slice(close + 1);
    if (after) {
      if (!after.startsWith(':')) return `Unexpected "${after}" after address`;
      const p = parsePort(after.slice(1));
      if (p === null) return `Invalid port "${after.slice(1)}"`;
      port = p;
    }
  } else {
    const colons = rest.split(':').length - 1;
    if (colons === 1) {
      const idx = rest.indexOf(':');
      host = rest.slice(0, idx);
      const p = parsePort(rest.slice(idx + 1));
      if (p === null) return `Invalid port "${rest.slice(idx + 1)}"`;
      port = p;
    }
    // 2+ colons without brackets: a bare IPv6 address, no port.
  }
  if (!validHost(host)) return host ? `Invalid host "${host}"` : 'Enter a host name or address';
  return { username, host, port };
}

const PARITY: Record<string, Parity> = { n: 'none', none: 'none', o: 'odd', odd: 'odd', e: 'even', even: 'even', m: 'mark', mark: 'mark', s: 'space', space: 'space' };
const FLOW: Record<string, FlowControl> = {
  n: 'none',
  none: 'none',
  x: 'xonxoff',
  xon: 'xonxoff',
  xonxoff: 'xonxoff',
  r: 'rtscts',
  rts: 'rtscts',
  rtscts: 'rtscts',
  d: 'dsrdtr',
  dsr: 'dsrdtr',
  dsrdtr: 'dsrdtr'
};

function parseSerial(spec: string): QuickConnectTarget | string {
  const [path, query = ''] = spec.split('?', 2);
  let line = path.replace(/\/+$/, '');
  const serial: Partial<SerialOptions> = {};
  // COM3:115200 shorthand
  const m = /^(.*?):(\d+)$/.exec(line);
  if (m && !line.startsWith('/dev/')) {
    line = m[1];
    serial.baudRate = Number(m[2]);
  }
  if (!line) return `Enter a serial line, e.g. ${osTerms.serialLine}`;
  if (/^com\d+$/i.test(line)) line = line.toUpperCase();
  for (const part of query.split('&').filter(Boolean)) {
    const [k, v = ''] = part.split('=', 2).map((x) => decodeURIComponent(x).trim());
    const key = k.toLowerCase();
    const val = v.toLowerCase();
    if (key === 'baud' || key === 'speed' || key === 'baudrate') {
      const n = Number(val);
      if (!Number.isInteger(n) || n <= 0) return `Invalid baud rate "${v}"`;
      serial.baudRate = n;
    } else if (key === 'data' || key === 'databits') {
      const n = Number(val);
      if (![5, 6, 7, 8].includes(n)) return `Invalid data bits "${v}"`;
      serial.dataBits = n as SerialOptions['dataBits'];
    } else if (key === 'stop' || key === 'stopbits') {
      const n = Number(val);
      if (![1, 1.5, 2].includes(n)) return `Invalid stop bits "${v}"`;
      serial.stopBits = n as SerialOptions['stopBits'];
    } else if (key === 'parity') {
      if (!PARITY[val]) return `Invalid parity "${v}"`;
      serial.parity = PARITY[val];
    } else if (key === 'flow' || key === 'flowcontrol') {
      if (!FLOW[val]) return `Invalid flow control "${v}"`;
      serial.flowControl = FLOW[val];
    } else return `Unknown serial option "${k}"`;
  }
  if (serial.baudRate !== undefined && !BAUD_RATES.has(serial.baudRate) && (serial.baudRate < 50 || serial.baudRate > 4_000_000)) {
    return `Invalid baud rate "${serial.baudRate}"`;
  }
  return { protocol: 'serial', host: line, serial };
}

export function parseQuickConnect(input: string, defaultProtocol: Protocol = 'ssh'): QuickConnectParse {
  const text = input.trim();
  if (!text) return { ok: false, error: 'Enter a host name or address' };
  const fail = (error: string): QuickConnectParse => ({ ok: false, error });

  // URL form
  const url = /^([a-z]+):\/\/(.*)$/i.exec(text);
  if (url) {
    const proto = url[1].toLowerCase();
    if (proto === 'sftp' || proto === 'scp') {
      const slash = url[2].search(/[/?#]/);
      const authority = slash < 0 ? url[2] : url[2].slice(0, slash);
      const rest = slash < 0 ? '' : url[2].slice(slash).replace(/[?#].*$/, '');
      const a = parseAuthority(decodeSafe(authority));
      if (typeof a === 'string') return fail(a);
      const path = rest.startsWith('/') && rest.length > 1 ? decodeSafe(rest) : undefined;
      return finish({ protocol: 'ssh', ...a, files: { protocol: proto, path } });
    }
    if (!isProtocol(proto)) return fail(`Unknown protocol "${url[1]}"`);
    if (proto === 'serial') {
      const r = parseSerial(url[2]);
      return typeof r === 'string' ? fail(r) : { ok: true, target: r };
    }
    const authority = url[2].replace(/[/?#].*$/, '');
    const a = parseAuthority(authority);
    if (typeof a === 'string') return fail(a);
    return finish({ protocol: proto, ...a });
  }

  // Token form: [proto] target [port] with optional CLI flags.
  const tokens = text.split(/\s+/);
  let protocol: Protocol | undefined;
  let port: number | undefined;
  let username: string | undefined;
  const positional: string[] = [];
  for (let i = 0; i < tokens.length; i++) {
    const t = tokens[i];
    const flag = /^-(ssh|telnet|rlogin|raw|serial)$/i.exec(t);
    if (flag) {
      protocol = flag[1].toLowerCase() as Protocol;
    } else if (t === '-P' || t === '-p') {
      const p = parsePort(tokens[++i] ?? '');
      if (p === null) return fail(`Invalid port "${tokens[i] ?? ''}"`);
      port = p;
    } else if (t === '-l') {
      username = tokens[++i];
      if (!username) return fail('Missing user name after -l');
    } else if (t.startsWith('-') && t.length > 1) {
      return fail(`Unknown option "${t}"`);
    } else positional.push(t);
  }
  if (positional.length && isProtocol(positional[0]) && positional.length > 1) {
    protocol = positional.shift()!.toLowerCase() as Protocol;
  }
  if (!positional.length) return fail('Enter a host name or address');
  if (positional.length > 2) return fail(`Unexpected "${positional.slice(2).join(' ')}"`);
  const [target, portToken] = positional;

  if (protocol === 'serial' || (!protocol && (defaultProtocol === 'serial' || SERIAL_LINE.test(target)))) {
    const r = parseSerial(target);
    if (typeof r === 'string') return fail(r);
    if (portToken) {
      const n = Number(portToken);
      if (!Number.isInteger(n) || n <= 0) return fail(`Invalid baud rate "${portToken}"`);
      r.serial = { ...r.serial, baudRate: n };
    }
    return { ok: true, target: r };
  }

  const a = parseAuthority(target);
  if (typeof a === 'string') return fail(a);
  if (portToken) {
    const p = parsePort(portToken);
    if (p === null) return fail(`Invalid port "${portToken}"`);
    if (a.port !== undefined && a.port !== p) return fail('Port given twice');
    a.port = p;
  }
  if (port !== undefined) a.port = port;
  if (username !== undefined) a.username = username;
  return finish({ protocol: protocol ?? defaultProtocol, ...a });
}

function decodeSafe(s: string): string {
  try {
    return decodeURIComponent(s);
  } catch {
    return s;
  }
}

function finish(t: QuickConnectTarget): QuickConnectParse {
  if (t.protocol === 'raw' && !t.port) return { ok: false, error: 'Raw connections need a port, e.g. host:7000' };
  if (t.username === undefined) delete t.username;
  if (t.port === undefined) delete t.port;
  if (t.files === undefined) delete t.files;
  return { ok: true, target: t };
}

/** Applies a parsed target to a base session (normally a copy of "Default Settings"). */
export function applyQuickConnect(base: SessionConfig, t: QuickConnectTarget): SessionConfig {
  const s = structuredClone(base);
  s.protocol = t.protocol;
  if (t.protocol === 'serial') {
    s.host = '';
    s.port = 0;
    s.serial = { ...s.serial, ...t.serial, line: t.host };
    return s;
  }
  s.host = t.host;
  s.port = t.port ?? (DEFAULT_PORTS[t.protocol] || s.port);
  if (t.username !== undefined) s.username = t.username;
  if (t.files) s.ssh = { ...s.ssh, fileProtocol: t.files.protocol };
  return s;
}

/** Canonical text for a target (history entries, previews). */
export function formatQuickConnect(t: QuickConnectTarget): string {
  if (t.protocol === 'serial') {
    const q: string[] = [];
    if (t.serial?.baudRate) q.push(`baud=${t.serial.baudRate}`);
    if (t.serial?.dataBits) q.push(`data=${t.serial.dataBits}`);
    if (t.serial?.parity) q.push(`parity=${t.serial.parity}`);
    if (t.serial?.stopBits) q.push(`stop=${t.serial.stopBits}`);
    if (t.serial?.flowControl) q.push(`flow=${t.serial.flowControl}`);
    return `serial://${t.host}${q.length ? '?' + q.join('&') : ''}`;
  }
  const host = t.host.includes(':') ? `[${t.host}]` : t.host;
  const user = t.username ? `${t.username}@` : '';
  const port = t.port && t.port !== DEFAULT_PORTS[t.protocol] ? `:${t.port}` : '';
  if (t.files) return `${t.files.protocol}://${user}${host}${port}${t.files.path ?? ''}`;
  const scheme = t.protocol === 'ssh' ? '' : `${t.protocol}://`;
  return `${scheme}${user}${host}${port}`;
}
