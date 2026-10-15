import { describe, expect, it } from 'vitest';
import { formatBuildId, formatBuildTime } from './buildInfo';

describe('About: version and build id', () => {
  it('shows the build number with the commit, marking uncommitted changes', () => {
    expect(formatBuildId({ build: 10, commit: '35de5d8' })).toBe('10 (35de5d8)');
    expect(formatBuildId({ build: 10, commit: '35de5d8-dirty' })).toBe('10 (35de5d8-dirty)');
    expect(formatBuildId({ build: 0, commit: 'unknown' })).toBe('0 (unknown)');
  });

  it('shows nothing when the host sent no build id', () => {
    expect(formatBuildId({ build: 0, commit: '' })).toBeNull();
  });

  it('shows the build time in local time, and nothing for a missing or invalid time', () => {
    expect(formatBuildTime('2026-10-15T10:16:49Z', { locale: 'en-GB', timeZone: 'UTC' })).toBe('15 Oct 2026, 10:16');
    expect(formatBuildTime('2026-10-15T10:16:49Z', { locale: 'en-GB', timeZone: 'Asia/Tokyo' })).toBe('15 Oct 2026, 19:16');
    expect(formatBuildTime('')).toBeNull();
    expect(formatBuildTime(undefined)).toBeNull();
    expect(formatBuildTime('not a date')).toBeNull();
  });
});
