/**
 * Shown when no tab is open: quick connect, recent sessions, PuTTY import call-to-action, shortcuts.
 */
import { useEffect, useMemo, useState } from 'react';
import { ArrowRight, Download, Plus, Server, Settings } from 'lucide-react';
import { api } from '@/bridge/api';
import { runCommand } from '@/commands/commands';
import { shortcutLabel } from '@/commands/keybindings';
import { applyQuickConnect, parseQuickConnect } from '@/components/quickconnect/parse';
import { connectSavedSession, connectSession, createAdHocSession, newSessionDraft, reportError } from '@/commands/actions';
import { Button, Kbd } from '@/components/ui';
import { useSessions } from '@/store/data';
import { useUi } from '@/store/ui';
import { sessionLabel } from '@shared/defaults';
import type { CommandId } from '@shared/types';
import { Logo } from './Logo';
import './welcome.css';

const TIPS: [CommandId, string][] = [
  ['app.commandPalette', 'Command palette'],
  ['app.quickConnect', 'Quick connect'],
  ['tab.new', 'New tab'],
  ['pane.splitRight', 'Split right'],
  ['term.find', 'Find in terminal'],
  ['app.settings', 'Settings']
];

export function Welcome() {
  const sessions = useSessions((s) => s.sessions);
  const [value, setValue] = useState('');
  const [error, setError] = useState('');
  const [puttyCount, setPuttyCount] = useState(0);

  useEffect(() => {
    let alive = true;
    api.importer
      .scanPuttyRegistry()
      .then((p) => alive && setPuttyCount(p?.candidates?.length ?? 0))
      .catch(() => undefined);
    return () => {
      alive = false;
    };
  }, []);

  const recent = useMemo(
    () =>
      [...sessions]
        .sort((a, b) => (b.lastUsedAt || 0) - (a.lastUsedAt || 0) || a.name.localeCompare(b.name))
        .slice(0, 8),
    [sessions]
  );

  const connect = () => {
    const parsed = parseQuickConnect(value);
    if (!parsed.ok) {
      setError(parsed.error);
      return;
    }
    try {
      const t = parsed.target;
      connectSession(applyQuickConnect(createAdHocSession(), t), 'tab', undefined, t.files ? { view: 'files', remotePath: t.files.path } : { view: 'terminal' });
      setValue('');
    } catch (e) {
      reportError('Could not connect', e);
    }
  };

  return (
    <div className="welcome">
      <div className="welcome__inner">
        <div className="welcome__hero">
          <Logo size={56} />
          <div>
            <h1>SSHTermaq</h1>
            <p className="welcome__slogan">Your ultimate destination</p>
            <p>SSH, Telnet, Rlogin, Raw and Serial — with tabs, splits and your PuTTY sessions.</p>
          </div>
        </div>

        <form
          className="welcome__connect"
          onSubmit={(e) => {
            e.preventDefault();
            connect();
          }}
        >
          <input
            className="input welcome__input"
            placeholder="user@host[:port]  ·  telnet://host  ·  serial://COM3?baud=115200"
            value={value}
            spellCheck={false}
            autoFocus
            onChange={(e) => {
              setValue(e.target.value);
              setError('');
            }}
          />
          <Button variant="primary" type="submit" icon={<ArrowRight size={15} />} disabled={!value.trim()}>
            Connect
          </Button>
        </form>
        {error && <div className="welcome__error">{error}</div>}

        <div className="welcome__actions">
          <Button icon={<Plus size={15} />} onClick={() => newSessionDraft()}>
            New session
          </Button>
          {puttyCount > 0 && (
            <Button icon={<Download size={15} />} onClick={() => useUi.getState().openDialog({ type: 'import', source: 'putty-registry' })}>
              Import {puttyCount} PuTTY session{puttyCount === 1 ? '' : 's'}
            </Button>
          )}
          <Button variant="ghost" icon={<Settings size={15} />} onClick={() => runCommand('app.settings')}>
            Settings
          </Button>
        </div>

        {recent.length > 0 && (
          <section className="welcome__section">
            <h2>Recent sessions</h2>
            <div className="welcome__grid">
              {recent.map((s) => (
                <button key={s.id} className="welcome__card" onClick={() => connectSavedSession(s.id)} title={`Connect to ${sessionLabel(s)}`}>
                  <span className="welcome__card-icon" style={s.color ? { color: s.color } : undefined}>
                    <Server size={16} />
                  </span>
                  <span className="welcome__card-text">
                    <span className="welcome__card-name">{s.name || sessionLabel(s)}</span>
                    <span className="welcome__card-host">
                      {s.protocol === 'serial' ? s.serial.line : `${s.username ? s.username + '@' : ''}${s.host}`}
                      <span className="welcome__proto">{s.protocol}</span>
                    </span>
                  </span>
                </button>
              ))}
            </div>
          </section>
        )}

        <section className="welcome__section welcome__tips">
          {TIPS.map(([id, label]) => {
            const sc = shortcutLabel(id);
            return sc ? (
              <div key={id} className="welcome__tip">
                <span>{label}</span>
                <Kbd>{sc}</Kbd>
              </div>
            ) : null;
          })}
        </section>
      </div>
    </div>
  );
}
