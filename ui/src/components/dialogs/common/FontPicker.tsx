/**
 * Font family combo box: curated monospace suggestions plus installed fonts (Local Font Access API, loaded
 * lazily on first open because it may need a permission prompt). Options render in their own typeface.
 */
import { useEffect, useMemo, useState } from 'react';
import { ComboInput, type ComboOption } from './controls';
import { MONO_FONT_SUGGESTIONS, canQueryLocalFonts, familyToCss, primaryFamily, queryInstalledFonts } from './fonts';

export function FontPicker({
  value,
  onChange,
  placeholder,
  id
}: {
  /** CSS font-family list. */
  value: string;
  onChange: (cssFontFamily: string) => void;
  placeholder?: string;
  id?: string;
}) {
  const [installed, setInstalled] = useState<string[] | null>(null);
  const [text, setText] = useState<string | null>(null);

  const options = useMemo<ComboOption[]>(() => {
    const curated: ComboOption[] = MONO_FONT_SUGGESTIONS.map((f) => ({ value: f, group: 'Monospace' }));
    const curatedSet = new Set<string>(MONO_FONT_SUGGESTIONS);
    const rest: ComboOption[] = (installed ?? []).filter((f) => !curatedSet.has(f)).map((f) => ({ value: f, group: 'Installed fonts' }));
    return [...curated, ...rest];
  }, [installed]);

  // Drop the typed text when the value is changed from outside (reset, undo, ...).
  useEffect(() => {
    setText((t) => (t !== null && familyToCss(t) !== value ? null : t));
  }, [value]);

  // Show the friendly primary family while the user is not typing.
  const display = text ?? primaryFamily(value);

  return (
    <ComboInput
      id={id}
      value={display}
      placeholder={placeholder}
      options={options}
      filter={text !== null}
      onOpen={() => {
        if (installed === null && canQueryLocalFonts()) void queryInstalledFonts().then(setInstalled);
      }}
      onChange={(v) => {
        setText(v);
        onChange(familyToCss(v));
      }}
      renderOption={(o) => (
        <span className="font-option">
          <span className="font-option__name" style={{ fontFamily: `'${o.value}', var(--font-ui)` }}>
            {o.value}
          </span>
          <span className="font-option__sample" style={{ fontFamily: `'${o.value}', monospace` }}>
            {'{ } 0O 1lI ->'}
          </span>
        </span>
      )}
      emptyText={canQueryLocalFonts() ? 'No matching fonts' : 'Type any installed font name'}
    />
  );
}
