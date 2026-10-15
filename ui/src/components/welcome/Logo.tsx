/**
 * SSHTermaq logo: a neon tube on black — a terminal window whose corner grows the tail of a "Q", with a pink ">_"
 * prompt inside (terminus ad quem: the end point toward which). Pure SVG: the glow is a blurred copy of the strokes,
 * under a solid tube and a light core, so it reads at 18 px in the title bar and at 56 px on the welcome screen.
 * The .exe icon is the same drawing, rendered by native/res/tools/make_icon.py.
 */
import { useId } from 'react';

const TILE = '#08050c';
const FRAME = '#e040fb'; // neon magenta
const FRAME_CORE = '#ffe4ff';
const PROMPT = '#ff3db8'; // neon pink
const PROMPT_CORE = '#ffe0f2';

/** Window outline and the Q's tail. */
const frame = (
  <>
    <rect x="22" y="24" width="72" height="68" rx="18" />
    <line x1="88" y1="86" x2="104" y2="102" />
  </>
);

/** The ">_" prompt. */
const prompt = (
  <>
    <path d="M38 46 L50 57 L38 68" />
    <line x1="57" y1="69" x2="74" y2="69" />
  </>
);

export function Logo({ size = 64, className }: { size?: number; className?: string }) {
  const glow = `glow-${useId().replace(/:/g, '')}`;
  return (
    <svg className={className} width={size} height={size} viewBox="0 0 120 120" role="img" aria-label="SSHTermaq">
      <defs>
        <filter id={glow} x="-25%" y="-25%" width="150%" height="150%">
          <feGaussianBlur stdDeviation="5" />
        </filter>
      </defs>
      <rect width="120" height="120" rx="26" fill={TILE} />
      <g fill="none" strokeLinecap="round" strokeLinejoin="round">
        <g filter={`url(#${glow})`} strokeWidth="10" opacity="0.85">
          <g stroke={FRAME}>{frame}</g>
          <g stroke={PROMPT}>{prompt}</g>
        </g>
        <g strokeWidth="6.5" opacity="0.85">
          <g stroke={FRAME}>{frame}</g>
          <g stroke={PROMPT}>{prompt}</g>
        </g>
        <g strokeWidth="2.8">
          <g stroke={FRAME_CORE}>{frame}</g>
          <g stroke={PROMPT_CORE}>{prompt}</g>
        </g>
      </g>
    </svg>
  );
}
