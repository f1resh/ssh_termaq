/**
 * In-pane authentication prompt card: username / password / keyboard-interactive / key passphrase.
 */
import { useEffect, useRef, useState } from 'react';
import { Eye, EyeOff, KeyRound, Lock, User } from 'lucide-react';
import { Button, Checkbox } from '@/components/ui';
import { usePrompts } from '@/store/data';
import type { PromptRequest } from '@shared/types';

interface Field {
  label: string;
  secret: boolean;
}

function fieldsOf(p: PromptRequest): Field[] {
  switch (p.kind) {
    case 'username':
      return [{ label: 'Username', secret: false }];
    case 'password':
      return [{ label: p.username ? `Password for ${p.username}` : 'Password', secret: true }];
    case 'passphrase':
      return [{ label: 'Passphrase', secret: true }];
    case 'kbdint':
      return p.prompts.map((q) => ({ label: q.prompt.trim().replace(/:$/, '') || 'Response', secret: !q.echo }));
    default:
      return [];
  }
}

/** `focusSignal`: a changed value focuses the first field again (the pane was asked for focus, e.g. by a tab click). */
export function PromptOverlay({ prompt, host, focusSignal = 0 }: { prompt: PromptRequest; host: string; focusSignal?: number }) {
  const respond = usePrompts((s) => s.respond);
  const fields = fieldsOf(prompt);
  const [values, setValues] = useState<string[]>(() => fields.map(() => ''));
  const [reveal, setReveal] = useState(false);
  const [remember, setRemember] = useState(false);
  const firstRef = useRef<HTMLInputElement>(null);

  useEffect(() => {
    setValues(fieldsOf(prompt).map(() => ''));
    setRemember(false);
    const t = setTimeout(() => firstRef.current?.focus(), 0);
    return () => clearTimeout(t);
  }, [prompt.id]); // eslint-disable-line react-hooks/exhaustive-deps

  useEffect(() => {
    if (focusSignal) firstRef.current?.focus();
  }, [focusSignal]);

  const allowSave = (prompt.kind === 'password' || prompt.kind === 'kbdint') && prompt.allowSave;
  const submit = () => respond(prompt.id, { cancelled: false, values, remember: allowSave ? remember : false });
  const cancel = () => respond(prompt.id, { cancelled: true });

  const Icon = prompt.kind === 'username' ? User : prompt.kind === 'passphrase' ? KeyRound : Lock;
  const message = 'message' in prompt ? prompt.message : undefined;

  return (
    <div className="term-overlay term-overlay--center term-overlay--dim">
      <form
        className="prompt-card"
        onSubmit={(e) => {
          e.preventDefault();
          submit();
        }}
        onKeyDown={(e) => {
          if (e.key === 'Escape') {
            e.preventDefault();
            e.stopPropagation();
            cancel();
          }
          e.stopPropagation();
        }}
      >
        <div className="prompt-card__head">
          <span className="prompt-card__icon">
            <Icon size={16} />
          </span>
          <div>
            <div className="prompt-card__title">{('title' in prompt && prompt.title) || 'Authentication required'}</div>
            <div className="prompt-card__host">{host}</div>
          </div>
        </div>
        {prompt.kind === 'kbdint' && (prompt.name || prompt.instructions) && (
          <div className="prompt-card__info">
            {prompt.name && <b>{prompt.name}</b>}
            {prompt.instructions && <pre>{prompt.instructions}</pre>}
          </div>
        )}
        {prompt.kind === 'passphrase' && <div className="prompt-card__info mono">{prompt.keyPath}</div>}
        {message && <div className="prompt-card__info">{message}</div>}

        {fields.map((f, i) => (
          <label key={i} className="prompt-card__field">
            <span>{f.label}</span>
            <div className="prompt-card__input">
              <input
                ref={i === 0 ? firstRef : undefined}
                className="input"
                type={f.secret && !reveal ? 'password' : 'text'}
                autoComplete="off"
                spellCheck={false}
                value={values[i] ?? ''}
                onChange={(e) => setValues((v) => v.map((x, j) => (j === i ? e.target.value : x)))}
              />
              {f.secret && (
                <button
                  type="button"
                  className="icon-btn icon-btn--sm"
                  aria-label={reveal ? 'Hide' : 'Show'}
                  title={reveal ? 'Hide' : 'Show'}
                  onClick={() => setReveal((r) => !r)}
                >
                  {reveal ? <EyeOff size={14} /> : <Eye size={14} />}
                </button>
              )}
            </div>
          </label>
        ))}

        <div className="prompt-card__foot">
          {allowSave ? <Checkbox checked={remember} onChange={setRemember} label="Remember password" /> : <span />}
          <div className="prompt-card__buttons">
            <Button size="sm" onClick={cancel}>
              Cancel
            </Button>
            <Button size="sm" variant="primary" type="submit">
              {prompt.kind === 'username' ? 'Continue' : 'Log in'}
            </Button>
          </div>
        </div>
      </form>
    </div>
  );
}
