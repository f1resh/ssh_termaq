/**
 * Quick connect: "[protocol://][user@]host[:port]" (see ./parse.ts for every accepted form) -> opens an
 * ad-hoc session built from "Default Settings". Keeps a short history of recent targets in host state.
 */
import { useEffect, useMemo, useRef, useState } from 'react';
import { ArrowRight, Clock, Save, X, Zap } from 'lucide-react';
import { api } from '@/bridge/api';
import { Button, Kbd, Modal, cx } from '@/components/ui';
import { connectSession, createAdHocSession } from '@/commands/actions';
import { useUi } from '@/store/ui';
import type { Protocol } from '@shared/types';
import { PROTOCOL_LABELS, ProtocolIcon, sessionSubtitle } from '@/components/layout/sessionVisuals';
import { PROTOCOLS, applyQuickConnect, formatQuickConnect, parseQuickConnect, type QuickConnectTarget } from './parse';
import './quickconnect.css';

export interface QuickConnectDialogProps {
  initial?: string;
  onClose: () => void;
}

const HISTORY_KEY = 'quickConnect.history';
const HISTORY_MAX = 8;

export async function loadQuickConnectHistory(): Promise<string[]> {
  try {
    const h = await api.app.getState<string[]>(HISTORY_KEY);
    return Array.isArray(h) ? h.filter((x) => typeof x === 'string').slice(0, HISTORY_MAX) : [];
  } catch {
    return [];
  }
}

export async function rememberQuickConnect(text: string): Promise<void> {
  const prev = await loadQuickConnectHistory();
  const next = [text, ...prev.filter((x) => x !== text)].slice(0, HISTORY_MAX);
  await api.app.setState(HISTORY_KEY, next).catch(() => undefined);
}

/** Opens a parsed target in a new tab and records it in the history. */
export function quickConnectTo(target: QuickConnectTarget): void {
  const session = applyQuickConnect(createAdHocSession(), target);
  connectSession(session, 'tab', undefined, target.files ? { view: 'files', remotePath: target.files.path } : { view: 'terminal' });
  void rememberQuickConnect(formatQuickConnect(target));
}

export function QuickConnectDialog({ initial, onClose }: QuickConnectDialogProps) {
  const [text, setText] = useState(initial ?? '');
  const [protocol, setProtocol] = useState<Protocol>('ssh');
  const [history, setHistory] = useState<string[]>([]);
  const [touched, setTouched] = useState(false);
  const inputRef = useRef<HTMLInputElement>(null);

  useEffect(() => {
    void loadQuickConnectHistory().then(setHistory);
  }, []);

  useEffect(() => {
    const t = setTimeout(() => {
      inputRef.current?.focus();
      inputRef.current?.select();
    }, 10);
    return () => clearTimeout(t);
  }, []);

  const parsed = useMemo(() => parseQuickConnect(text, protocol), [text, protocol]);
  const effectiveProtocol = parsed.ok ? parsed.target.protocol : protocol;
  const preview = useMemo(() => (parsed.ok ? applyQuickConnect(createAdHocSession(), parsed.target) : null), [parsed]);

  const connect = (input = text) => {
    const r = parseQuickConnect(input, protocol);
    if (!r.ok) {
      setTouched(true);
      inputRef.current?.focus();
      return;
    }
    onClose();
    quickConnectTo(r.target);
  };

  const saveAsSession = () => {
    if (!parsed.ok) {
      setTouched(true);
      return;
    }
    const draft = applyQuickConnect(createAdHocSession(), parsed.target);
    draft.name = parsed.target.protocol === 'serial' ? parsed.target.host : parsed.target.host;
    useUi.getState().openDialog({ type: 'sessionEditor', draft });
  };

  const removeHistory = (h: string) => {
    const next = history.filter((x) => x !== h);
    setHistory(next);
    void api.app.setState(HISTORY_KEY, next).catch(() => undefined);
  };

  const placeholder: Record<Protocol, string> = {
    ssh: 'user@host:port',
    telnet: 'host:port',
    rlogin: 'user@host',
    raw: 'host:port',
    serial: 'COM3 or serial://COM3?baud=115200'
  };

  return (
    <Modal
      open
      title="Quick connect"
      icon={<Zap size={16} className="qc__title-icon" />}
      onClose={onClose}
      width={560}
      className="qc"
      footer={
        <>
          <Button variant="ghost" icon={<Save size={14} />} onClick={saveAsSession} disabled={!parsed.ok} className="qc__save">
            Save as session…
          </Button>
          <Button onClick={onClose}>Cancel</Button>
          <Button variant="primary" onClick={() => connect()} disabled={!text.trim()}>
            Connect
          </Button>
        </>
      }
    >
      <div className="qc__protocols" role="radiogroup" aria-label="Protocol">
        {PROTOCOLS.map((p) => (
          <button
            key={p}
            role="radio"
            aria-checked={effectiveProtocol === p}
            className={cx('qc__proto', effectiveProtocol === p && 'qc__proto--on')}
            onClick={() => {
              setProtocol(p);
              inputRef.current?.focus();
            }}
          >
            <ProtocolIcon protocol={p} size={14} />
            {PROTOCOL_LABELS[p]}
          </button>
        ))}
      </div>

      <div className={cx('qc__field', touched && !parsed.ok && text.trim() && 'qc__field--invalid')}>
        <ProtocolIcon protocol={effectiveProtocol} size={18} className="qc__field-icon" />
        <input
          ref={inputRef}
          className="qc__input"
          value={text}
          spellCheck={false}
          autoComplete="off"
          placeholder={placeholder[protocol]}
          aria-label="Connection target"
          aria-invalid={touched && !parsed.ok}
          onChange={(e) => {
            setText(e.target.value);
            setTouched(false);
          }}
          onKeyDown={(e) => {
            if (e.key === 'Enter') {
              e.preventDefault();
              connect();
            }
          }}
        />
        <Kbd>Enter</Kbd>
      </div>

      <div className="qc__status" aria-live="polite">
        {!text.trim() ? (
          <span className="qc__hint">
            Examples: <code>deploy@web-01:2222</code> <code>telnet://router</code> <code>[::1]:2222</code> <code>serial://COM3?baud=115200</code>
          </span>
        ) : parsed.ok && preview ? (
          <span className="qc__preview">
            <ArrowRight size={13} />
            {PROTOCOL_LABELS[preview.protocol]} · {sessionSubtitle(preview)}
            {preview.protocol !== 'serial' && !preview.username && <span className="qc__muted"> · user name asked on connect</span>}
          </span>
        ) : (
          <span className={cx('qc__error', touched && 'qc__error--strong')}>{parsed.ok ? '' : parsed.error}</span>
        )}
      </div>

      {history.length > 0 && (
        <div className="qc__history">
          <div className="qc__history-title">Recent</div>
          {history.map((h) => {
            const r = parseQuickConnect(h);
            return (
              <div key={h} className="qc__history-row">
                <button className="qc__history-item" onClick={() => connect(h)} title={`Connect to ${h}`}>
                  {r.ok ? <ProtocolIcon protocol={r.target.protocol} size={14} /> : <Clock size={14} />}
                  <span className="qc__history-text">{h}</span>
                </button>
                <button className="qc__history-remove" aria-label={`Remove ${h} from history`} title="Remove" onClick={() => removeHistory(h)}>
                  <X size={13} />
                </button>
              </div>
            );
          })}
        </div>
      )}
    </Modal>
  );
}
