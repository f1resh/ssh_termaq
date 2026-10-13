/**
 * Static renderings of terminal colour schemes (styled spans, not xterm): a compact card preview for
 * pickers/galleries and a larger live sample for the settings Terminal panel.
 * Colours come from the scheme data itself, so inline styles are intended here.
 */
import type { CSSProperties, ReactNode } from 'react';
import type { ColorScheme, CursorStyle } from '@shared/types';

export const ANSI_KEYS = [
  'black',
  'red',
  'green',
  'yellow',
  'blue',
  'magenta',
  'cyan',
  'white',
  'brightBlack',
  'brightRed',
  'brightGreen',
  'brightYellow',
  'brightBlue',
  'brightMagenta',
  'brightCyan',
  'brightWhite'
] as const satisfies readonly (keyof ColorScheme)[];

export type AnsiKey = (typeof ANSI_KEYS)[number];

/** Compact preview used in scheme pickers. */
export function SchemeMiniPreview({ scheme, fontFamily }: { scheme: ColorScheme; fontFamily?: string }) {
  const c = (k: AnsiKey) => ({ color: scheme[k] });
  return (
    <div className="scheme-mini" style={{ background: scheme.background, color: scheme.foreground, fontFamily }} aria-hidden>
      <div>
        <span style={c('green')}>user@host</span>
        <span style={c('white')}>:</span>
        <span style={c('blue')}>~</span>$ ls
      </div>
      <div>
        <span style={c('blue')}>bin</span> <span style={c('green')}>run.sh</span> <span style={c('red')}>a.tgz</span>
      </div>
      <div>
        $ <span className="scheme-mini__cursor" style={{ background: scheme.cursor }} />
      </div>
      <div className="scheme-mini__palette">
        {ANSI_KEYS.slice(0, 8).map((k) => (
          <span key={k} style={{ background: scheme[k] }} />
        ))}
      </div>
    </div>
  );
}

export interface SampleFont {
  fontFamily: string;
  fontSize: number;
  lineHeight: number;
  letterSpacing: number;
  fontWeight: string;
  fontWeightBold: string;
  cursorStyle: CursorStyle;
  cursorBlink: boolean;
  boldIsBright: boolean;
  padding?: number;
}

/** Larger sample that reflects font + colour settings. */
export function TerminalSample({ scheme, font, height }: { scheme: ColorScheme; font: SampleFont; height?: number }) {
  const fg = (k: AnsiKey, bold = false): CSSProperties => {
    let key: AnsiKey = k;
    if (bold && font.boldIsBright && !k.startsWith('bright')) key = ('bright' + k[0].toUpperCase() + k.slice(1)) as AnsiKey;
    return { color: scheme[key], fontWeight: bold ? font.fontWeightBold : undefined };
  };
  const B = ({ k, children }: { k: AnsiKey; children: ReactNode }) => <span style={fg(k, true)}>{children}</span>;
  const N = ({ k, children }: { k: AnsiKey; children: ReactNode }) => <span style={fg(k)}>{children}</span>;
  const prompt = (
    <>
      <B k="green">deploy@web-01</B>
      <N k="white">:</N>
      <B k="blue">~/app</B>
      <span>$ </span>
    </>
  );
  const cursorStyle: CSSProperties =
    font.cursorStyle === 'block'
      ? { background: scheme.cursor, color: scheme.cursorAccent }
      : font.cursorStyle === 'underline'
        ? { boxShadow: `inset 0 -2px 0 ${scheme.cursor}` }
        : { boxShadow: `inset 2px 0 0 ${scheme.cursor}` };
  return (
    <div
      className="term-sample"
      style={{
        background: scheme.background,
        color: scheme.foreground,
        fontFamily: font.fontFamily,
        fontSize: font.fontSize,
        lineHeight: font.lineHeight,
        letterSpacing: font.letterSpacing,
        fontWeight: font.fontWeight,
        padding: Math.max(6, font.padding ?? 10),
        height
      }}
      aria-label="Terminal preview"
    >
      <div>
        {prompt}ls -la
      </div>
      <div>
        <N k="white">drwxr-xr-x 4 deploy deploy 4096 Oct 13 </N>
        <B k="blue">.</B>
      </div>
      <div>
        <N k="white">-rw-r--r-- 1 deploy deploy  220 Oct 13 </N>.bashrc
      </div>
      <div>
        <N k="white">-rwxr-xr-x 1 deploy deploy 8.1K Oct 13 </N>
        <B k="green">deploy.sh</B>
      </div>
      <div>
        <N k="white">-rw-r--r-- 1 deploy deploy  64M Oct 13 </N>
        <B k="red">backup.tar.gz</B>
      </div>
      <div>
        {prompt}git status <N k="brightBlack"># on main</N>
      </div>
      <div>
        {'  '}
        <N k="red">modified:   src/server.ts</N>
      </div>
      <div>
        {'  '}
        <N k="green">new file:   src/health.ts</N>
      </div>
      <div>
        <N k="yellow">warning:</N> <N k="cyan">2 files</N> <N k="magenta">changed</N>, <span style={{ fontWeight: font.fontWeightBold }}>bold text</span>
      </div>
      <div>
        {prompt}
        <span className={font.cursorBlink ? 'term-sample__cursor term-sample__cursor--blink' : 'term-sample__cursor'} style={cursorStyle}>
          {' '}
        </span>
      </div>
      <div className="term-sample__palette">
        {ANSI_KEYS.map((k) => (
          <span key={k} title={k} style={{ background: scheme[k] }} />
        ))}
      </div>
    </div>
  );
}
