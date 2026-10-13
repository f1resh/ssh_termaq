/**
 * Small pure helpers used by the terminal pane (kept DOM-free so they can be unit-tested in node).
 */

/**
 * PuTTY's ^C notation for strings such as the answerback: ^A..^Z / ^@..^_ are control characters, ^? is DEL
 * and ^~ is a literal caret. Any other caret is kept as-is.
 */
export function decodeCaretNotation(s: string): string {
  let out = '';
  for (let i = 0; i < s.length; i++) {
    const c = s[i];
    if (c !== '^' || i + 1 >= s.length) {
      out += c;
      continue;
    }
    const n = s[i + 1];
    const code = n.toUpperCase().charCodeAt(0);
    if (n === '~') out += '^';
    else if (n === '?') out += '\x7f';
    else if (code >= 0x40 && code <= 0x5f) out += String.fromCharCode(code & 0x1f);
    else {
      out += c;
      continue;
    }
    i++;
  }
  return out;
}

/** "Implicit LF in every CR": every CR received also moves to the next line. */
export function implicitLfInCr(data: string): string {
  return data.includes('\r') ? data.replace(/\r/g, '\r\n') : data;
}

/** Number of ENQ (^E) characters in a chunk of output (each one triggers the answerback). */
export function countEnq(data: string): number {
  let n = 0;
  for (let i = data.indexOf('\x05'); i >= 0; i = data.indexOf('\x05', i + 1)) n++;
  return n;
}

/**
 * True when data emitted by xterm.js is an automatic terminal report (mouse event, focus event, device status /
 * attribute or window report, DCS / OSC reply) rather than user input. Reports go only to the pane's own
 * connection, never to the other panes of a broadcasting tab.
 */
const REPORT_RE =
  // eslint-disable-next-line no-control-regex
  /^(?:\x1b\[<\d+;\d+;\d+[Mm]|\x1b\[M[\s\S]{3}|\x1b\[[IO]|\x1b\[\??[\d;]*[Rnc]|\x1b\[[>=][\d;]*c|\x1b\[[\d;]*t|\x1b\[\??[\d;]*\$y|\x1bP[\s\S]*?\x1b\\|\x1b\][\s\S]*?(?:\x07|\x1b\\))+$/;

export function isTerminalReport(data: string): boolean {
  return data.length > 2 && data.charCodeAt(0) === 0x1b && REPORT_RE.test(data);
}

/** Whether pasted text spans more than one line (a single trailing newline does not count). */
export function isMultiline(text: string): boolean {
  return /[\r\n]/.test(text.replace(/(\r\n|\r|\n)$/, ''));
}

/** Preview of the first lines of a paste for the multi-line warning. */
export function pastePreview(text: string, maxLines = 5, maxCols = 120): { preview: string; lines: number } {
  const all = text.replace(/(\r\n|\r|\n)$/, '').split(/\r\n|\r|\n/);
  const shown = all.slice(0, maxLines).map((l) => (l.length > maxCols ? l.slice(0, maxCols - 1) + '…' : l));
  if (all.length > maxLines) shown.push(`… (${all.length - maxLines} more line${all.length - maxLines === 1 ? '' : 's'})`);
  return { preview: shown.join('\n'), lines: all.length };
}

/**
 * PuTTY bell overload management: `count` bells within `windowMs` silence the bell until `quietMs` pass without
 * a bell. Any keypress re-enables it.
 */
export class BellLimiter {
  private times: number[] = [];
  private overloaded = false;
  private last = -Infinity;

  constructor(
    private readonly count = 5,
    private readonly windowMs = 2000,
    private readonly quietMs = 5000
  ) {}

  /** Records a bell at `now`; returns true when it should be signalled. */
  ring(now: number): boolean {
    if (this.overloaded && now - this.last >= this.quietMs) {
      this.overloaded = false;
      this.times = [];
    }
    this.last = now;
    this.times = this.times.filter((t) => now - t < this.windowMs);
    this.times.push(now);
    if (!this.overloaded && this.times.length >= this.count) this.overloaded = true;
    return !this.overloaded;
  }

  reset(): void {
    this.overloaded = false;
    this.times = [];
  }

  get isOverloaded(): boolean {
    return this.overloaded;
  }
}

/** Exponential reconnect backoff: 2s, 4s, 8s, 16s, then 30s. */
export function reconnectDelayMs(attempt: number): number {
  return Math.min(30000, 2000 * 2 ** Math.max(0, attempt));
}

/** Text of the whole buffer (scrollback + screen), joining soft-wrapped lines, trailing blank lines removed. */
export function bufferToText(
  length: number,
  getLine: (i: number) => { isWrapped: boolean; translateToString(trimRight?: boolean): string } | undefined
): string {
  const lines: string[] = [];
  for (let i = 0; i < length; i++) {
    const line = getLine(i);
    if (!line) continue;
    const text = line.translateToString(true);
    if (line.isWrapped && lines.length) lines[lines.length - 1] += text;
    else lines.push(text);
  }
  while (lines.length && lines[lines.length - 1] === '') lines.pop();
  return lines.join('\r\n');
}

/** Mix two #rrggbb colours (t = weight of b); falls back to `a` for non-hex input. */
export function mixHex(a: string, b: string, t: number): string {
  const pa = /^#([0-9a-f]{6})/i.exec(a.trim());
  const pb = /^#([0-9a-f]{6})/i.exec(b.trim());
  if (!pa || !pb) return pa ? `#${pa[1]}` : '#808080';
  const ca = parseInt(pa[1], 16);
  const cb = parseInt(pb[1], 16);
  const ch = (v: number, s: number) => (v >> s) & 0xff;
  const m = (s: number) => Math.round(ch(ca, s) * (1 - t) + ch(cb, s) * t);
  return '#' + [16, 8, 0].map((s) => m(s).toString(16).padStart(2, '0')).join('');
}

/** #rrggbb form of a colour when it is hex (#rgb / #rrggbb / #rrggbbaa); null otherwise. */
export function toHex6(c: string): string | null {
  const m = /^#([0-9a-f]{3}|[0-9a-f]{6}|[0-9a-f]{8})$/i.exec(c.trim());
  if (!m) return null;
  const h = m[1].length === 3 ? m[1].split('').map((x) => x + x).join('') : m[1].slice(0, 6);
  return `#${h.toLowerCase()}`;
}
