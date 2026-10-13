/**
 * Command palette (Ctrl+Shift+P): fuzzy search over saved sessions and commands.
 * Prefix ">" = commands only, "@" = sessions only.
 */
import { useEffect, useMemo, useRef, useState } from 'react';
import { createPortal } from 'react-dom';
import { Server, TerminalSquare } from 'lucide-react';
import { connectSavedSession } from '@/commands/actions';
import { COMMANDS, recentCommands, runCommand } from '@/commands/commands';
import { shortcutLabel } from '@/commands/keybindings';
import { cx, Kbd } from '@/components/ui';
import { useSessions } from '@/store/data';
import { useUi } from '@/store/ui';
import { sessionLabel } from '@shared/defaults';
import type { CommandId } from '@shared/types';
import './palette.css';

interface Item {
  key: string;
  kind: 'session' | 'command';
  label: string;
  detail: string;
  shortcut?: string;
  run: () => void;
  score: number;
  match: number[];
}

/** Subsequence fuzzy match; returns score (higher = better) and matched indices, or null. */
function fuzzy(query: string, text: string): { score: number; match: number[] } | null {
  if (!query) return { score: 0, match: [] };
  const q = query.toLowerCase();
  const t = text.toLowerCase();
  const direct = t.indexOf(q);
  if (direct >= 0) {
    return { score: 1000 - direct * 2 - (t.length - q.length) * 0.1, match: [...Array(q.length)].map((_, i) => direct + i) };
  }
  const match: number[] = [];
  let ti = 0;
  let score = 0;
  let prev = -2;
  for (const ch of q) {
    const i = t.indexOf(ch, ti);
    if (i < 0) return null;
    score += i === prev + 1 ? 8 : i === 0 || /[\s/@._-]/.test(t[i - 1]) ? 6 : 1;
    match.push(i);
    prev = i;
    ti = i + 1;
  }
  return { score: score - t.length * 0.05, match };
}

function Highlight({ text, match }: { text: string; match: number[] }) {
  if (!match.length) return <>{text}</>;
  const set = new Set(match);
  return (
    <>
      {[...text].map((c, i) =>
        set.has(i) ? (
          <mark key={i} className="palette__hl">
            {c}
          </mark>
        ) : (
          <span key={i}>{c}</span>
        )
      )}
    </>
  );
}

export function CommandPalette() {
  const open = useUi((s) => s.paletteOpen);
  const mode = useUi((s) => s.paletteMode);
  if (!open) return null;
  return <PaletteInner initialMode={mode} />;
}

function PaletteInner({ initialMode }: { initialMode: 'all' | 'sessions' | 'commands' }) {
  const sessions = useSessions((s) => s.sessions);
  const [query, setQuery] = useState(initialMode === 'commands' ? '>' : initialMode === 'sessions' ? '@' : '');
  const [index, setIndex] = useState(0);
  const inputRef = useRef<HTMLInputElement>(null);
  const listRef = useRef<HTMLDivElement>(null);
  const close = () => useUi.getState().setPalette(false);

  useEffect(() => inputRef.current?.focus(), []);

  const items = useMemo<Item[]>(() => {
    let q = query.trim();
    let showSessions = true;
    let showCommands = true;
    if (q.startsWith('>')) {
      showSessions = false;
      q = q.slice(1).trim();
    } else if (q.startsWith('@')) {
      showCommands = false;
      q = q.slice(1).trim();
    }
    const out: Item[] = [];
    if (showSessions) {
      for (const s of sessions) {
        const label = s.name || sessionLabel(s);
        const detail = [s.folder, s.protocol === 'serial' ? s.serial.line : `${s.username ? s.username + '@' : ''}${s.host}`]
          .filter(Boolean)
          .join(' · ');
        const m = fuzzy(q, label) ?? (q ? fuzzy(q, `${label} ${detail}`) : null);
        if (!m) continue;
        const recency = s.lastUsedAt ? Math.min(50, (s.lastUsedAt / 1e12) * 10) : 0;
        out.push({
          key: 's:' + s.id,
          kind: 'session',
          label,
          detail,
          run: () => connectSavedSession(s.id),
          score: m.score + (q ? 0 : recency) + 5,
          match: m.match.filter((i) => i < label.length)
        });
      }
    }
    if (showCommands) {
      const recent = recentCommands();
      for (const c of COMMANDS) {
        const label = `${c.category}: ${c.label}`;
        const m = fuzzy(q, label);
        if (!m) continue;
        const r = recent.indexOf(c.id as CommandId);
        out.push({
          key: 'c:' + c.id,
          kind: 'command',
          label,
          detail: '',
          shortcut: shortcutLabel(c.id as CommandId),
          run: () => runCommand(c.id as CommandId),
          score: m.score + (r >= 0 ? 20 - r : 0),
          match: m.match
        });
      }
    }
    out.sort((a, b) => b.score - a.score || a.label.localeCompare(b.label));
    return out.slice(0, 200);
  }, [query, sessions]);

  useEffect(() => setIndex(0), [query]);
  useEffect(() => {
    listRef.current?.querySelector(`[data-index="${index}"]`)?.scrollIntoView({ block: 'nearest' });
  }, [index]);

  const choose = (it: Item | undefined) => {
    if (!it) return;
    close();
    // Let the palette unmount before running (commands may open dialogs / move focus).
    setTimeout(it.run, 0);
  };

  return createPortal(
    <div className="palette-overlay" onMouseDown={(e) => e.target === e.currentTarget && close()}>
      <div className="palette" role="dialog" aria-label="Command palette">
        <input
          ref={inputRef}
          className="palette__input"
          placeholder="Search sessions and commands  ( > commands  ·  @ sessions )"
          value={query}
          spellCheck={false}
          onChange={(e) => setQuery(e.target.value)}
          onKeyDown={(e) => {
            if (e.key === 'Escape') {
              e.preventDefault();
              close();
            } else if (e.key === 'ArrowDown') {
              e.preventDefault();
              setIndex((i) => Math.min(items.length - 1, i + 1));
            } else if (e.key === 'ArrowUp') {
              e.preventDefault();
              setIndex((i) => Math.max(0, i - 1));
            } else if (e.key === 'PageDown') {
              e.preventDefault();
              setIndex((i) => Math.min(items.length - 1, i + 10));
            } else if (e.key === 'PageUp') {
              e.preventDefault();
              setIndex((i) => Math.max(0, i - 10));
            } else if (e.key === 'Enter') {
              e.preventDefault();
              choose(items[index]);
            }
          }}
        />
        <div ref={listRef} className="palette__list" role="listbox">
          {items.length === 0 && <div className="palette__empty">No matches</div>}
          {items.map((it, i) => (
            <div
              key={it.key}
              data-index={i}
              role="option"
              aria-selected={i === index}
              className={cx('palette__item', i === index && 'palette__item--active')}
              onMouseMove={() => i !== index && setIndex(i)}
              onClick={() => choose(it)}
            >
              <span className="palette__icon">{it.kind === 'session' ? <Server size={14} /> : <TerminalSquare size={14} />}</span>
              <span className="palette__label">
                <Highlight text={it.label} match={it.match} />
              </span>
              {it.detail && <span className="palette__detail">{it.detail}</span>}
              {it.shortcut && <Kbd>{it.shortcut}</Kbd>}
            </div>
          ))}
        </div>
      </div>
    </div>,
    document.body
  );
}
