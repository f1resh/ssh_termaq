/**
 * Global context menu. Call `showContextMenu(x, y, items)` from anywhere; render <ContextMenuHost/> once.
 * Supports separators, headers, disabled items, shortcuts, danger styling, one level of submenus and
 * keyboard navigation (arrows / Enter / Escape).
 */
import { useEffect, useLayoutEffect, useRef, useState, type ReactNode } from 'react';
import { createPortal } from 'react-dom';
import { ChevronRight } from 'lucide-react';
import { create } from 'zustand';

export type MenuItem =
  | {
      type?: 'item';
      label: string;
      icon?: ReactNode;
      shortcut?: string;
      disabled?: boolean;
      danger?: boolean;
      checked?: boolean;
      onClick?: () => void;
      submenu?: MenuItem[];
    }
  | { type: 'separator' }
  | { type: 'header'; label: string };

interface MenuState {
  menu: { x: number; y: number; items: MenuItem[] } | null;
  open(x: number, y: number, items: MenuItem[]): void;
  close(): void;
}

const useMenu = create<MenuState>((set) => ({
  menu: null,
  open: (x, y, items) => set({ menu: { x, y, items } }),
  close: () => set({ menu: null })
}));

export function showContextMenu(x: number, y: number, items: MenuItem[]): void {
  useMenu.getState().open(x, y, items);
}

function MenuList({
  items,
  x,
  y,
  onDone,
  autoFocus
}: {
  items: MenuItem[];
  x: number;
  y: number;
  onDone: () => void;
  autoFocus?: boolean;
}) {
  const ref = useRef<HTMLDivElement>(null);
  const [pos, setPos] = useState({ left: x, top: y });
  const [active, setActive] = useState(-1);
  const [sub, setSub] = useState<{ index: number; x: number; y: number } | null>(null);

  useLayoutEffect(() => {
    const el = ref.current;
    if (!el) return;
    const r = el.getBoundingClientRect();
    const left = Math.max(4, Math.min(x, window.innerWidth - r.width - 4));
    const top = Math.max(4, Math.min(y, window.innerHeight - r.height - 4));
    setPos({ left, top });
    if (autoFocus) el.focus();
  }, [x, y, autoFocus]);

  const actionable = items
    .map((it, i) => ({ it, i }))
    .filter(({ it }) => (it.type ?? 'item') === 'item' && !(it as { disabled?: boolean }).disabled);

  const activate = (i: number, el?: HTMLElement | null) => {
    const it = items[i];
    if (!it || (it.type ?? 'item') !== 'item') return;
    const item = it as Extract<MenuItem, { label: string; type?: 'item' }>;
    if (item.disabled) return;
    if (item.submenu) {
      const r = el?.getBoundingClientRect();
      setSub({ index: i, x: r ? r.right - 2 : pos.left + 200, y: r ? r.top - 4 : pos.top });
      return;
    }
    onDone();
    item.onClick?.();
  };

  return (
    <div
      ref={ref}
      className="ctx-menu"
      style={pos}
      tabIndex={-1}
      role="menu"
      onKeyDown={(e) => {
        const idx = actionable.findIndex((a) => a.i === active);
        if (e.key === 'ArrowDown') {
          e.preventDefault();
          setActive(actionable[(idx + 1) % actionable.length]?.i ?? -1);
        } else if (e.key === 'ArrowUp') {
          e.preventDefault();
          setActive(actionable[(idx - 1 + actionable.length) % actionable.length]?.i ?? -1);
        } else if (e.key === 'Enter' || e.key === 'ArrowRight') {
          e.preventDefault();
          if (active >= 0) activate(active, ref.current?.querySelector<HTMLElement>(`[data-index="${active}"]`));
        } else if (e.key === 'ArrowLeft' || e.key === 'Escape') {
          e.preventDefault();
          e.stopPropagation();
          onDone();
        }
      }}
    >
      {items.map((it, i) => {
        if (it.type === 'separator') return <div key={i} className="ctx-menu__sep" />;
        if (it.type === 'header')
          return (
            <div key={i} className="ctx-menu__header">
              {it.label}
            </div>
          );
        return (
          <button
            key={i}
            data-index={i}
            role="menuitem"
            className={'ctx-menu__item' + (it.danger ? ' ctx-menu__item--danger' : '')}
            data-active={active === i}
            disabled={it.disabled}
            onMouseEnter={(e) => {
              setActive(i);
              if (it.submenu) {
                const r = e.currentTarget.getBoundingClientRect();
                setSub({ index: i, x: r.right - 2, y: r.top - 4 });
              } else setSub(null);
            }}
            onClick={(e) => activate(i, e.currentTarget)}
          >
            <span className="ctx-menu__icon">{it.checked ? '✓' : it.icon}</span>
            <span className="ctx-menu__label">{it.label}</span>
            {it.shortcut && <span className="ctx-menu__shortcut">{it.shortcut}</span>}
            {it.submenu && <ChevronRight size={14} />}
          </button>
        );
      })}
      {sub && (items[sub.index] as { submenu?: MenuItem[] }).submenu && (
        <MenuList
          items={(items[sub.index] as { submenu: MenuItem[] }).submenu}
          x={sub.x}
          y={sub.y}
          onDone={onDone}
        />
      )}
    </div>
  );
}

export function ContextMenuHost() {
  const menu = useMenu((s) => s.menu);
  const close = useMenu((s) => s.close);

  useEffect(() => {
    if (!menu) return;
    const onDown = (e: MouseEvent) => {
      if (!(e.target as HTMLElement).closest('.ctx-menu')) close();
    };
    const onBlur = () => close();
    window.addEventListener('mousedown', onDown, true);
    window.addEventListener('blur', onBlur);
    window.addEventListener('resize', onBlur);
    return () => {
      window.removeEventListener('mousedown', onDown, true);
      window.removeEventListener('blur', onBlur);
      window.removeEventListener('resize', onBlur);
    };
  }, [menu, close]);

  if (!menu) return null;
  return createPortal(<MenuList items={menu.items} x={menu.x} y={menu.y} onDone={close} autoFocus />, document.body);
}
