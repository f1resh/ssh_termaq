/**
 * Small presentational helpers shared by the tab strip, sidebar, palette, welcome screen and status bar:
 * protocol icons, connection status dots/labels and the "user@host:port" subtitle.
 */
import type { ReactNode } from 'react';
import { Cable, Network, Plug, Terminal, Usb, type LucideIcon } from 'lucide-react';
import { DEFAULT_PORTS } from '@shared/defaults';
import type { Protocol, SessionConfig } from '@shared/types';
import type { PaneStatus } from '@/store/workspace';
import { cx } from '@/components/ui';
import './sessionVisuals.css';

export const PROTOCOL_ICONS: Record<Protocol, LucideIcon> = {
  ssh: Terminal,
  telnet: Network,
  rlogin: Cable,
  raw: Plug,
  serial: Usb
};

export const PROTOCOL_LABELS: Record<Protocol, string> = {
  ssh: 'SSH',
  telnet: 'Telnet',
  rlogin: 'Rlogin',
  raw: 'Raw',
  serial: 'Serial'
};

export function ProtocolIcon({ protocol, size = 14, className }: { protocol: Protocol; size?: number; className?: string }) {
  const Icon = PROTOCOL_ICONS[protocol] ?? Terminal;
  return <Icon size={size} className={className} strokeWidth={1.75} aria-hidden />;
}

/** "user@host:port" (port only when non-default) or the serial line + speed. */
export function sessionSubtitle(s: Pick<SessionConfig, 'protocol' | 'host' | 'port' | 'username' | 'serial'>): string {
  if (s.protocol === 'serial') return `${s.serial.line || 'COM?'} · ${s.serial.baudRate} baud`;
  const user = s.username ? `${s.username}@` : '';
  const port = s.port && s.port !== DEFAULT_PORTS[s.protocol] ? `:${s.port}` : '';
  const host = s.host.includes(':') && !s.host.startsWith('[') ? `[${s.host}]` : s.host;
  return `${user}${host || '(no host)'}${port}`;
}

export const STATUS_LABELS: Record<PaneStatus, string> = {
  idle: 'Not connected',
  connecting: 'Connecting',
  authenticating: 'Authenticating',
  connected: 'Connected',
  disconnected: 'Disconnected',
  error: 'Error'
};

export function StatusDot({ status, className, title }: { status: PaneStatus; className?: string; title?: string }) {
  return <span className={cx('status-dot', `status-dot--${status}`, className)} title={title ?? STATUS_LABELS[status]} aria-label={STATUS_LABELS[status]} />;
}

/** Swatches offered for tab / session colours. */
export const COLOR_SWATCHES: { name: string; value: string }[] = [
  { name: 'Red', value: '#e5534b' },
  { name: 'Orange', value: '#e8913a' },
  { name: 'Yellow', value: '#d4b13d' },
  { name: 'Green', value: '#3fb950' },
  { name: 'Teal', value: '#2bb3a3' },
  { name: 'Blue', value: '#4c8dff' },
  { name: 'Purple', value: '#a371f7' },
  { name: 'Pink', value: '#e0569c' }
];

export function Swatch({ color, size = 12 }: { color?: string; size?: number }) {
  return (
    <span
      className={cx('swatch', !color && 'swatch--none')}
      style={{ width: size, height: size, background: color || undefined }}
      aria-hidden
    />
  );
}

/** Wraps case-insensitive occurrences of any token in <mark>. */
export function Highlight({ text, tokens }: { text: string; tokens: string[] }) {
  const toks = tokens.map((t) => t.toLowerCase()).filter(Boolean);
  if (!toks.length || !text) return <>{text}</>;
  const lower = text.toLowerCase();
  const marks = new Array<boolean>(text.length).fill(false);
  for (const t of toks) {
    for (let i = lower.indexOf(t); i >= 0; i = lower.indexOf(t, i + 1)) marks.fill(true, i, i + t.length);
  }
  return <>{renderMarks(text, marks)}</>;
}

/** Renders text with the characters at `positions` highlighted (fuzzy matches). */
export function HighlightPositions({ text, positions }: { text: string; positions: number[] }) {
  if (!positions.length) return <>{text}</>;
  const marks = new Array<boolean>(text.length).fill(false);
  for (const p of positions) if (p >= 0 && p < text.length) marks[p] = true;
  return <>{renderMarks(text, marks)}</>;
}

function renderMarks(text: string, marks: boolean[]) {
  const out: ReactNode[] = [];
  let i = 0;
  while (i < text.length) {
    const m = marks[i];
    let j = i;
    while (j < text.length && marks[j] === m) j++;
    const part = text.slice(i, j);
    out.push(m ? <mark key={i} className="hl">{part}</mark> : <span key={i}>{part}</span>);
    i = j;
  }
  return out;
}
