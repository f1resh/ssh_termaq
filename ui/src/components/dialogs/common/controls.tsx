/**
 * Form building blocks used by the dialogs (built on the shared primitives in components/ui).
 * All styling lives in dialogs.css and uses theme CSS variables only.
 */
import {
  useEffect,
  useId,
  useLayoutEffect,
  useMemo,
  useRef,
  useState,
  type CSSProperties,
  type KeyboardEvent as ReactKeyboardEvent,
  type ReactNode
} from 'react';
import { createPortal } from 'react-dom';
import { AlertTriangle, Check, ChevronDown, Copy, FolderOpen, Info, Loader2, OctagonAlert, Plus, X } from 'lucide-react';
import { Button, IconButton, NumberInput, Switch, TextInput, cx } from '@/components/ui';
import { api } from '@/bridge/api';
import { useBuffered, useCloseGuard } from './hooks';

/* -------------------------------------------------------------------------- */
/* Layout                                                                     */
/* -------------------------------------------------------------------------- */

export function Section({
  title,
  description,
  actions,
  children,
  className
}: {
  title?: ReactNode;
  description?: ReactNode;
  actions?: ReactNode;
  children: ReactNode;
  className?: string;
}) {
  return (
    <section className={cx('dlg-section', className)}>
      {(title || actions) && (
        <header className="dlg-section__header">
          <div className="dlg-section__titles">
            {title && <h3 className="dlg-section__title">{title}</h3>}
            {description && <p className="dlg-section__desc">{description}</p>}
          </div>
          {actions && <div className="dlg-section__actions">{actions}</div>}
        </header>
      )}
      <div className="dlg-section__body">{children}</div>
    </section>
  );
}

/** Responsive grid of fields. `cols` = max columns. */
export function FieldGrid({ cols = 2, children, className }: { cols?: 1 | 2 | 3 | 4; children: ReactNode; className?: string }) {
  return <div className={cx('dlg-grid', `dlg-grid--${cols}`, className)}>{children}</div>;
}

/** Field wrapper with label/hint/error (like ui Field, but supports spanning grid columns). */
export function FormField({
  label,
  hint,
  error,
  warning,
  span,
  children,
  className,
  htmlFor
}: {
  label?: ReactNode;
  hint?: ReactNode;
  error?: ReactNode;
  warning?: ReactNode;
  span?: number | 'all';
  children: ReactNode;
  className?: string;
  htmlFor?: string;
}) {
  const style: CSSProperties | undefined = span ? { gridColumn: span === 'all' ? '1 / -1' : `span ${span}` } : undefined;
  return (
    <div className={cx('field', 'dlg-field', className)} style={style}>
      {label && (
        <label className="field__label" htmlFor={htmlFor}>
          {label}
        </label>
      )}
      {children}
      {error ? (
        <span className="field__error" role="alert">
          {error}
        </span>
      ) : warning ? (
        <span className="dlg-field__warning">{warning}</span>
      ) : hint ? (
        <span className="field__hint">{hint}</span>
      ) : null}
    </div>
  );
}

/** Settings-style row: label + description on the left, control on the right. */
export function SettingRow({
  label,
  description,
  children,
  className,
  disabled
}: {
  label: ReactNode;
  description?: ReactNode;
  children: ReactNode;
  className?: string;
  disabled?: boolean;
}) {
  return (
    <div className={cx('dlg-row', disabled && 'dlg-row--disabled', className)}>
      <div className="dlg-row__text">
        <div className="dlg-row__label">{label}</div>
        {description && <div className="dlg-row__desc">{description}</div>}
      </div>
      <div className="dlg-row__control">{children}</div>
    </div>
  );
}

export function ToggleRow({
  label,
  description,
  checked,
  onChange,
  disabled
}: {
  label: ReactNode;
  description?: ReactNode;
  checked: boolean;
  onChange: (v: boolean) => void;
  disabled?: boolean;
}) {
  return (
    <SettingRow label={label} description={description} disabled={disabled}>
      <Switch checked={checked} onChange={onChange} disabled={disabled} />
    </SettingRow>
  );
}

/* -------------------------------------------------------------------------- */
/* Small display elements                                                     */
/* -------------------------------------------------------------------------- */

export type Tone = 'neutral' | 'accent' | 'success' | 'warning' | 'danger' | 'info';

export function Badge({ tone = 'neutral', children, title, className }: { tone?: Tone; children: ReactNode; title?: string; className?: string }) {
  return (
    <span className={cx('dlg-badge', `dlg-badge--${tone}`, className)} title={title}>
      {children}
    </span>
  );
}

export function Callout({
  tone = 'info',
  title,
  children,
  icon,
  className,
  actions
}: {
  tone?: 'info' | 'warning' | 'danger' | 'success';
  title?: ReactNode;
  children?: ReactNode;
  icon?: ReactNode;
  className?: string;
  actions?: ReactNode;
}) {
  const ico =
    icon ??
    (tone === 'danger' ? (
      <OctagonAlert size={16} />
    ) : tone === 'warning' ? (
      <AlertTriangle size={16} />
    ) : tone === 'success' ? (
      <Check size={16} />
    ) : (
      <Info size={16} />
    ));
  return (
    <div className={cx('dlg-callout', `dlg-callout--${tone}`, className)} role={tone === 'danger' ? 'alert' : undefined}>
      <span className="dlg-callout__icon">{ico}</span>
      <div className="dlg-callout__body">
        {title && <div className="dlg-callout__title">{title}</div>}
        {children && <div className="dlg-callout__text">{children}</div>}
      </div>
      {actions && <div className="dlg-callout__actions">{actions}</div>}
    </div>
  );
}

export function Spinner({ size = 16, label }: { size?: number; label?: string }) {
  return (
    <span className="dlg-spinner" role="status" aria-label={label ?? 'Loading'}>
      <Loader2 size={size} />
      {label && <span>{label}</span>}
    </span>
  );
}

export function EmptyState({ icon, title, children }: { icon?: ReactNode; title: ReactNode; children?: ReactNode }) {
  return (
    <div className="dlg-empty">
      {icon && <div className="dlg-empty__icon">{icon}</div>}
      <div className="dlg-empty__title">{title}</div>
      {children && <div className="dlg-empty__text">{children}</div>}
    </div>
  );
}

/** Copies text to the clipboard (via the host) and briefly shows a check mark. */
export function CopyButton({ text, label = 'Copy', size = 'sm', withText }: { text: string; label?: string; size?: 'sm' | 'md'; withText?: boolean }) {
  const [copied, setCopied] = useState(false);
  useEffect(() => {
    if (!copied) return;
    const t = setTimeout(() => setCopied(false), 1400);
    return () => clearTimeout(t);
  }, [copied]);
  const copy = () => {
    api.app.writeClipboard(text);
    setCopied(true);
  };
  if (withText)
    return (
      <Button size={size} icon={copied ? <Check size={14} /> : <Copy size={14} />} onClick={copy} disabled={!text}>
        {copied ? 'Copied' : label}
      </Button>
    );
  return (
    <IconButton label={copied ? 'Copied' : label} size={size} onClick={copy} disabled={!text}>
      {copied ? <Check size={14} /> : <Copy size={14} />}
    </IconButton>
  );
}

/* -------------------------------------------------------------------------- */
/* Segmented control                                                          */
/* -------------------------------------------------------------------------- */

export interface SegmentOption<T extends string | number> {
  value: T;
  label: ReactNode;
  icon?: ReactNode;
  title?: string;
  disabled?: boolean;
}

export function SegmentedControl<T extends string | number>({
  value,
  options,
  onChange,
  size = 'md',
  ariaLabel,
  fill
}: {
  value: T;
  options: readonly SegmentOption<T>[];
  onChange: (v: T) => void;
  size?: 'sm' | 'md';
  ariaLabel?: string;
  /** Stretch to the container width. */
  fill?: boolean;
}) {
  const refs = useRef<(HTMLButtonElement | null)[]>([]);
  const onKey = (e: ReactKeyboardEvent, i: number) => {
    if (e.key !== 'ArrowRight' && e.key !== 'ArrowLeft') return;
    e.preventDefault();
    const dir = e.key === 'ArrowRight' ? 1 : -1;
    for (let k = 1; k <= options.length; k++) {
      const j = (i + dir * k + options.length) % options.length;
      if (!options[j].disabled) {
        onChange(options[j].value);
        refs.current[j]?.focus();
        break;
      }
    }
  };
  return (
    <div className={cx('dlg-seg', size === 'sm' && 'dlg-seg--sm', fill && 'dlg-seg--fill')} role="radiogroup" aria-label={ariaLabel}>
      {options.map((o, i) => {
        const active = o.value === value;
        return (
          <button
            key={String(o.value)}
            ref={(el) => {
              refs.current[i] = el;
            }}
            type="button"
            role="radio"
            aria-checked={active}
            tabIndex={active ? 0 : -1}
            className={cx('dlg-seg__item', active && 'dlg-seg__item--active')}
            title={o.title}
            disabled={o.disabled}
            onClick={() => onChange(o.value)}
            onKeyDown={(e) => onKey(e, i)}
          >
            {o.icon}
            {o.label && <span>{o.label}</span>}
          </button>
        );
      })}
    </div>
  );
}

/* -------------------------------------------------------------------------- */
/* Colour swatches                                                            */
/* -------------------------------------------------------------------------- */

/** Accent / tag colours offered as swatches (data values, not UI styling). */
export const SWATCH_COLORS = [
  '#e5534b',
  '#f0883e',
  '#d4a72c',
  '#3fb950',
  '#2ea0a0',
  '#4c8dff',
  '#7c6cf2',
  '#bc4fd8',
  '#e05d9c',
  '#8b949e'
] as const;

export function ColorSwatches({
  value,
  onChange,
  colors = SWATCH_COLORS,
  allowNone = true,
  allowCustom = true,
  noneLabel = 'None'
}: {
  value: string;
  onChange: (v: string) => void;
  colors?: readonly string[];
  allowNone?: boolean;
  allowCustom?: boolean;
  noneLabel?: string;
}) {
  const custom = value && !colors.some((c) => c.toLowerCase() === value.toLowerCase());
  return (
    <div className="dlg-swatches" role="radiogroup">
      {allowNone && (
        <button
          type="button"
          role="radio"
          aria-checked={!value}
          className={cx('dlg-swatch', 'dlg-swatch--none', !value && 'dlg-swatch--active')}
          title={noneLabel}
          aria-label={noneLabel}
          onClick={() => onChange('')}
        >
          <X size={12} />
        </button>
      )}
      {colors.map((c) => {
        const active = value.toLowerCase() === c.toLowerCase();
        return (
          <button
            key={c}
            type="button"
            role="radio"
            aria-checked={active}
            aria-label={c}
            title={c}
            className={cx('dlg-swatch', active && 'dlg-swatch--active')}
            style={{ '--swatch': c } as CSSProperties}
            onClick={() => onChange(c)}
          >
            {active && <Check size={12} />}
          </button>
        );
      })}
      {allowCustom && (
        <label
          className={cx('dlg-swatch', 'dlg-swatch--custom', custom && 'dlg-swatch--active')}
          style={custom ? ({ '--swatch': value } as CSSProperties) : undefined}
          title="Custom colour"
        >
          <input
            type="color"
            className="sr-only"
            value={/^#[0-9a-f]{6}$/i.test(value) ? value : '#4c8dff'}
            onChange={(e) => onChange(e.target.value)}
            aria-label="Custom colour"
          />
          {custom ? <Check size={12} /> : <Plus size={12} />}
        </label>
      )}
    </div>
  );
}

/* -------------------------------------------------------------------------- */
/* Path input with Browse                                                     */
/* -------------------------------------------------------------------------- */

export function PathInput({
  value,
  onChange,
  onBrowse,
  placeholder,
  invalid,
  browseLabel = 'Browse…',
  id,
  extra
}: {
  value: string;
  onChange: (v: string) => void;
  onBrowse: () => Promise<string | null | undefined>;
  placeholder?: string;
  invalid?: boolean;
  browseLabel?: string;
  id?: string;
  extra?: ReactNode;
}) {
  const [busy, setBusy] = useState(false);
  return (
    <div className="dlg-path">
      <TextInput id={id} value={value} onChange={onChange} placeholder={placeholder} invalid={invalid} className="mono-input" />
      <Button
        icon={<FolderOpen size={14} />}
        disabled={busy}
        onClick={async () => {
          setBusy(true);
          try {
            const p = await onBrowse();
            if (p) onChange(p);
          } finally {
            setBusy(false);
          }
        }}
      >
        {browseLabel}
      </Button>
      {extra}
    </div>
  );
}

/* -------------------------------------------------------------------------- */
/* Combo box (free text + suggestion popover)                                 */
/* -------------------------------------------------------------------------- */

export interface ComboOption {
  value: string;
  label?: string;
  hint?: string;
  /** Group heading shown before the first option of a group. */
  group?: string;
}

export function ComboInput({
  value,
  onChange,
  options,
  placeholder,
  invalid,
  id,
  renderOption,
  onOpen,
  filter = true,
  className,
  mono,
  emptyText = 'No suggestions'
}: {
  value: string;
  onChange: (v: string) => void;
  options: readonly ComboOption[];
  placeholder?: string;
  invalid?: boolean;
  id?: string;
  renderOption?: (o: ComboOption) => ReactNode;
  /** Called when the popover opens (e.g. to lazily load suggestions). */
  onOpen?: () => void;
  /** Filter options by the typed text (default true). */
  filter?: boolean;
  className?: string;
  mono?: boolean;
  emptyText?: string;
}) {
  const [open, setOpen] = useState(false);
  const [query, setQuery] = useState<string | null>(null);
  const [active, setActive] = useState(-1);
  const wrapRef = useRef<HTMLDivElement>(null);
  const inputRef = useRef<HTMLInputElement>(null);
  const listRef = useRef<HTMLDivElement>(null);
  const [rect, setRect] = useState<DOMRect | null>(null);
  const listId = useId();

  const shown = useMemo(() => {
    const q = (query ?? '').trim().toLowerCase();
    if (!filter || !q) return options;
    return options.filter((o) => o.value.toLowerCase().includes(q) || (o.label ?? '').toLowerCase().includes(q));
  }, [options, query, filter]);

  const openList = () => {
    if (!open) {
      setOpen(true);
      setQuery(null);
      setActive(-1);
      onOpen?.();
    }
  };

  useLayoutEffect(() => {
    if (!open) return;
    const update = () => wrapRef.current && setRect(wrapRef.current.getBoundingClientRect());
    update();
    window.addEventListener('resize', update);
    window.addEventListener('scroll', update, true);
    return () => {
      window.removeEventListener('resize', update);
      window.removeEventListener('scroll', update, true);
    };
  }, [open]);

  useEffect(() => {
    if (!open) return;
    const onDown = (e: MouseEvent) => {
      const t = e.target as Node;
      if (wrapRef.current?.contains(t) || listRef.current?.contains(t)) return;
      setOpen(false);
    };
    window.addEventListener('mousedown', onDown, true);
    return () => window.removeEventListener('mousedown', onDown, true);
  }, [open]);

  useEffect(() => {
    if (active < 0) return;
    listRef.current?.querySelector<HTMLElement>(`[data-index="${active}"]`)?.scrollIntoView({ block: 'nearest' });
  }, [active]);

  const pick = (o: ComboOption) => {
    onChange(o.value);
    setOpen(false);
    inputRef.current?.focus();
  };

  const onKeyDown = (e: ReactKeyboardEvent<HTMLInputElement>) => {
    if (e.key === 'ArrowDown') {
      e.preventDefault();
      if (!open) openList();
      else setActive((a) => Math.min(shown.length - 1, a + 1));
    } else if (e.key === 'ArrowUp') {
      e.preventDefault();
      setActive((a) => Math.max(0, a - 1));
    } else if (e.key === 'Enter' && open && active >= 0 && shown[active]) {
      e.preventDefault();
      pick(shown[active]);
    }
  };
  // Escape (handled by the dialog's close guards) closes the popover instead of the dialog.
  useCloseGuard(open, () => setOpen(false));

  let lastGroup: string | undefined;
  return (
    <div className={cx('dlg-combo', className)} ref={wrapRef}>
      <input
        ref={inputRef}
        id={id}
        className={cx('input', 'dlg-combo__input', mono && 'mono-input')}
        value={value}
        placeholder={placeholder}
        aria-invalid={invalid || undefined}
        role="combobox"
        aria-expanded={open}
        aria-controls={listId}
        aria-autocomplete="list"
        spellCheck={false}
        autoComplete="off"
        onChange={(e) => {
          onChange(e.target.value);
          setQuery(e.target.value);
          setActive(-1);
          if (!open) {
            setOpen(true);
            onOpen?.();
          }
        }}
        onFocus={openList}
        onClick={openList}
        onKeyDown={onKeyDown}
      />
      <button
        type="button"
        className="dlg-combo__toggle"
        tabIndex={-1}
        aria-label="Show suggestions"
        onMouseDown={(e) => {
          e.preventDefault();
          if (open) setOpen(false);
          else {
            inputRef.current?.focus();
            openList();
          }
        }}
      >
        <ChevronDown size={14} />
      </button>
      {open &&
        rect &&
        createPortal(
          <div
            ref={listRef}
            id={listId}
            role="listbox"
            className="dlg-combo__list"
            style={{
              left: rect.left,
              width: Math.max(rect.width, 220),
              ...(window.innerHeight - rect.bottom < 240 && rect.top > 260
                ? { bottom: window.innerHeight - rect.top + 4 }
                : { top: rect.bottom + 4 })
            }}
          >
            {shown.length === 0 && <div className="dlg-combo__empty">{emptyText}</div>}
            {shown.map((o, i) => {
              const header = o.group && o.group !== lastGroup ? o.group : null;
              lastGroup = o.group;
              return (
                <div key={`${o.group ?? ''}:${o.value}`}>
                  {header && <div className="dlg-combo__group">{header}</div>}
                  <div
                    role="option"
                    data-index={i}
                    aria-selected={o.value === value}
                    className={cx('dlg-combo__option', i === active && 'dlg-combo__option--active', o.value === value && 'dlg-combo__option--selected')}
                    onMouseDown={(e) => {
                      e.preventDefault();
                      pick(o);
                    }}
                    onMouseEnter={() => setActive(i)}
                  >
                    {renderOption ? (
                      renderOption(o)
                    ) : (
                      <>
                        <span className="dlg-combo__label">{o.label ?? o.value}</span>
                        {o.hint && <span className="dlg-combo__hint">{o.hint}</span>}
                      </>
                    )}
                  </div>
                </div>
              );
            })}
          </div>,
          document.body
        )}
    </div>
  );
}

/* -------------------------------------------------------------------------- */
/* Buffered inputs for host-backed (async) values                             */
/* -------------------------------------------------------------------------- */

export function BufferedTextInput({
  value,
  onCommit,
  placeholder,
  className,
  mono,
  id,
  delay
}: {
  value: string;
  onCommit: (v: string) => void;
  placeholder?: string;
  className?: string;
  mono?: boolean;
  id?: string;
  delay?: number;
}) {
  const [local, setLocal, flush] = useBuffered(value, onCommit, delay);
  return (
    <TextInput
      id={id}
      value={local}
      onChange={setLocal}
      onBlur={flush}
      placeholder={placeholder}
      className={cx(className, mono && 'mono-input')}
    />
  );
}

export function BufferedNumberInput({
  value,
  onCommit,
  min,
  max,
  step,
  className,
  id,
  suffix
}: {
  value: number;
  onCommit: (v: number) => void;
  min?: number;
  max?: number;
  step?: number;
  className?: string;
  id?: string;
  suffix?: ReactNode;
}) {
  const clampCommit = (v: number) => {
    let n = v;
    if (min !== undefined) n = Math.max(min, n);
    if (max !== undefined) n = Math.min(max, n);
    onCommit(n);
  };
  const [local, setLocal, flush] = useBuffered(value, clampCommit, 400);
  const input = <NumberInput id={id} value={local} onChange={setLocal} min={min} max={max} step={step} className={className} onBlur={flush} />;
  if (!suffix) return input;
  return (
    <div className="dlg-suffixed">
      {input}
      <span className="dlg-suffixed__suffix">{suffix}</span>
    </div>
  );
}

/** Range slider with value label; commits buffered while dragging. */
export function Slider({
  value,
  onChange,
  min,
  max,
  step = 1,
  format = (v) => String(v),
  buffered = true,
  ariaLabel
}: {
  value: number;
  onChange: (v: number) => void;
  min: number;
  max: number;
  step?: number;
  format?: (v: number) => string;
  buffered?: boolean;
  ariaLabel?: string;
}) {
  const [local, setLocal, flush] = useBuffered(value, onChange, buffered ? 150 : 0);
  const pct = ((local - min) / (max - min)) * 100;
  return (
    <div className="dlg-slider">
      <input
        type="range"
        min={min}
        max={max}
        step={step}
        value={local}
        aria-label={ariaLabel}
        style={{ '--pct': `${pct}%` } as CSSProperties}
        onChange={(e) => setLocal(Number(e.target.value))}
        onPointerUp={flush}
        onKeyUp={flush}
      />
      <span className="dlg-slider__value">{format(local)}</span>
    </div>
  );
}

/* -------------------------------------------------------------------------- */
/* Left navigation for large dialogs                                          */
/* -------------------------------------------------------------------------- */

export interface NavItem {
  id: string;
  label: string;
  icon?: ReactNode;
  depth?: 0 | 1;
  /** Shows an error dot. */
  error?: boolean;
  /** Small trailing badge. */
  badge?: ReactNode;
  group?: string;
}

export function NavList({ items, active, onSelect, ariaLabel }: { items: NavItem[]; active: string; onSelect: (id: string) => void; ariaLabel?: string }) {
  const refs = useRef(new Map<string, HTMLButtonElement>());
  const onKey = (e: ReactKeyboardEvent, idx: number) => {
    if (e.key !== 'ArrowDown' && e.key !== 'ArrowUp') return;
    e.preventDefault();
    const next = items[(idx + (e.key === 'ArrowDown' ? 1 : -1) + items.length) % items.length];
    onSelect(next.id);
    refs.current.get(next.id)?.focus();
  };
  let lastGroup: string | undefined;
  return (
    <nav className="dlg-nav" aria-label={ariaLabel}>
      {items.map((it, idx) => {
        const header = it.group && it.group !== lastGroup ? it.group : null;
        lastGroup = it.group;
        return (
          <div key={it.id}>
            {header && <div className="dlg-nav__group">{header}</div>}
            <button
              ref={(el) => {
                if (el) refs.current.set(it.id, el);
                else refs.current.delete(it.id);
              }}
              type="button"
              className={cx('dlg-nav__item', it.depth === 1 && 'dlg-nav__item--child', it.id === active && 'dlg-nav__item--active')}
              aria-current={it.id === active ? 'page' : undefined}
              tabIndex={it.id === active ? 0 : -1}
              onClick={() => onSelect(it.id)}
              onKeyDown={(e) => onKey(e, idx)}
            >
              {it.icon && <span className="dlg-nav__icon">{it.icon}</span>}
              <span className="dlg-nav__label">{it.label}</span>
              {it.badge}
              {it.error && <span className="dlg-nav__error" aria-label="Has errors" />}
            </button>
          </div>
        );
      })}
    </nav>
  );
}
