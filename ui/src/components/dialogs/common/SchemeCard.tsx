/**
 * Selectable colour-scheme card (mini preview + name) used by the session editor's Appearance section and
 * the settings scheme galleries.
 */
import type { ReactNode } from 'react';
import { Check } from 'lucide-react';
import { cx } from '@/components/ui';
import type { ColorScheme } from '@shared/types';
import { SchemeMiniPreview } from './SchemePreview';

export function SchemeCard({
  scheme,
  label,
  active,
  onSelect,
  badge,
  placeholder,
  title
}: {
  /** Scheme to preview; omit to show `placeholder` instead. */
  scheme?: ColorScheme;
  label: ReactNode;
  active: boolean;
  onSelect: () => void;
  badge?: ReactNode;
  placeholder?: ReactNode;
  title?: string;
}) {
  return (
    <button
      type="button"
      role="radio"
      aria-checked={active}
      className={cx('scheme-card', active && 'scheme-card--active')}
      onClick={onSelect}
      title={title}
    >
      {scheme ? <SchemeMiniPreview scheme={scheme} /> : <div className="scheme-card__placeholder">{placeholder}</div>}
      <span className="scheme-card__name">
        <span>{label}</span>
        {badge}
        {active && <Check size={13} />}
      </span>
    </button>
  );
}
