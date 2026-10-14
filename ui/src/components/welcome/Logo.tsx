/**
 * SSHTerm logo: a rounded tile with a terminal prompt and a trail of connected nodes.
 * Pure SVG so it scales crisply; colours come from the accent token with a fixed violet partner.
 */
import { useId } from 'react';

export function Logo({ size = 64, className }: { size?: number; className?: string }) {
  const gid = useId().replace(/:/g, '');
  return (
    <svg className={className} width={size} height={size} viewBox="0 0 64 64" role="img" aria-label="SSHTerm">
      <defs>
        <linearGradient id={`lg-${gid}`} x1="0" y1="0" x2="1" y2="1">
          <stop offset="0" stopColor="var(--accent)" />
          <stop offset="1" stopColor="color-mix(in srgb, var(--accent) 35%, #8b5cf6)" />
        </linearGradient>
        <linearGradient id={`sh-${gid}`} x1="0" y1="0" x2="0" y2="1">
          <stop offset="0" stopColor="#ffffff" stopOpacity="0.28" />
          <stop offset="0.55" stopColor="#ffffff" stopOpacity="0" />
        </linearGradient>
      </defs>
      <rect x="2" y="2" width="60" height="60" rx="15" fill={`url(#lg-${gid})`} />
      <rect x="2" y="2" width="60" height="60" rx="15" fill={`url(#sh-${gid})`} />
      <path d="M15 22 L26 31 L15 40" fill="none" stroke="#fff" strokeWidth="5" strokeLinecap="round" strokeLinejoin="round" />
      <rect x="30" y="37" width="17" height="5" rx="2.5" fill="#fff" />
      <circle cx="44" cy="19" r="3" fill="#fff" opacity="0.95" />
      <circle cx="51" cy="26" r="2.2" fill="#fff" opacity="0.7" />
      <circle cx="37" cy="23" r="1.8" fill="#fff" opacity="0.55" />
      <path d="M37 23 L44 19 L51 26" fill="none" stroke="#fff" strokeOpacity="0.55" strokeWidth="1.4" />
    </svg>
  );
}
