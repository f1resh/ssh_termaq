/**
 * Find-in-terminal overlay (search addon): incremental search, match count, prev/next, case / word / regex.
 */
import { useEffect, useRef, useState } from 'react';
import { CaseSensitive, ChevronDown, ChevronUp, Regex, WholeWord, X } from 'lucide-react';
import { IconButton } from '@/components/ui';
import type { FindOptions, TermSession } from './termSession';

export function FindBar({ session, onClose }: { session: TermSession; onClose: () => void }) {
  const [query, setQuery] = useState('');
  const [opts, setOpts] = useState<FindOptions>({ caseSensitive: false, wholeWord: false, regex: false });
  const [result, setResult] = useState<{ resultIndex: number; resultCount: number } | null>(null);
  const inputRef = useRef<HTMLInputElement>(null);

  useEffect(() => session.onSearchResults(setResult), [session]);
  useEffect(() => {
    inputRef.current?.focus();
    inputRef.current?.select();
    const off = session.on('find', () => {
      inputRef.current?.focus();
      inputRef.current?.select();
    });
    return off;
  }, [session]);

  useEffect(() => {
    if (!query) {
      session.clearSearch();
      setResult(null);
      return;
    }
    session.find(query, opts, 'next', true);
  }, [query, opts, session]);

  const close = () => {
    session.clearSearch();
    onClose();
    session.focus();
  };
  const step = (dir: 'next' | 'prev') => query && session.find(query, opts, dir);
  const toggle = (k: keyof FindOptions) => setOpts((o) => ({ ...o, [k]: !o[k] }));

  const count = result ? (result.resultCount > 0 ? `${result.resultIndex + 1} of ${result.resultCount}` : 'No results') : '';

  return (
    <div
      className="find-bar"
      onKeyDown={(e) => {
        e.stopPropagation();
        if (e.key === 'Escape') {
          e.preventDefault();
          close();
        } else if (e.key === 'Enter') {
          e.preventDefault();
          step(e.shiftKey ? 'prev' : 'next');
        }
      }}
    >
      <input
        ref={inputRef}
        className="input find-bar__input"
        placeholder="Find"
        value={query}
        spellCheck={false}
        onChange={(e) => setQuery(e.target.value)}
      />
      <span className="find-bar__count">{count}</span>
      <IconButton size="sm" label="Match case" pressed={opts.caseSensitive} onClick={() => toggle('caseSensitive')}>
        <CaseSensitive size={14} />
      </IconButton>
      <IconButton size="sm" label="Whole word" pressed={opts.wholeWord} onClick={() => toggle('wholeWord')}>
        <WholeWord size={14} />
      </IconButton>
      <IconButton size="sm" label="Regular expression" pressed={opts.regex} onClick={() => toggle('regex')}>
        <Regex size={14} />
      </IconButton>
      <span className="find-bar__sep" />
      <IconButton size="sm" label="Previous (Shift+Enter)" onClick={() => step('prev')}>
        <ChevronUp size={14} />
      </IconButton>
      <IconButton size="sm" label="Next (Enter)" onClick={() => step('next')}>
        <ChevronDown size={14} />
      </IconButton>
      <IconButton size="sm" label="Close (Esc)" onClick={close}>
        <X size={14} />
      </IconButton>
    </div>
  );
}
