/**
 * Formatting of the version and build id shown in About (AppInfo from app:info; see native/src/app/BuildInfo.h).
 */
import type { AppInfo } from '@shared/types';

/** "10 (35de5d8)" or "10 (35de5d8-dirty)"; null when the host sent no build id. */
export function formatBuildId(info: Pick<AppInfo, 'build' | 'commit'>): string | null {
  if (!info.commit) return null;
  return `${info.build ?? 0} (${info.commit})`;
}

/** Build (link) time in local time, e.g. "15 Oct 2026, 13:16"; null when unknown or invalid. */
export function formatBuildTime(iso: string | undefined, opts: { locale?: string; timeZone?: string } = {}): string | null {
  if (!iso) return null;
  const d = new Date(iso);
  if (Number.isNaN(d.getTime())) return null;
  return d.toLocaleString(opts.locale, { dateStyle: 'medium', timeStyle: 'short', timeZone: opts.timeZone });
}
