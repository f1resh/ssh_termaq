/**
 * Session editor: Session, Connection and Proxy sections.
 */
import { useMemo } from 'react';
import { Cable, Lock, LogIn, Network, Plus, Trash2, Usb } from 'lucide-react';
import { Button, IconButton, NumberInput, Select, TextArea, TextInput } from '@/components/ui';
import { DEFAULT_PORTS, normalizeFolder, sessionLabel } from '@shared/defaults';
import type { AddressFamily, Protocol, ProxyType, SessionView, TriState } from '@shared/types';
import {
  Callout,
  ColorSwatches,
  ComboInput,
  FieldGrid,
  FormField,
  Section,
  SegmentedControl,
  ToggleRow,
  type ComboOption,
  type SegmentOption
} from '../../common/controls';
import { useEditor } from '../editorContext';
import { fallbackSessionName, splitHostInput, switchProtocol } from '../sessionModel';
import { BaudRateInput, SerialLineInput } from './serialInputs';

const OPEN_AS_OPTIONS: readonly SegmentOption<SessionView>[] = [
  { value: 'terminal', label: 'Terminal' },
  { value: 'files', label: 'File manager', title: 'Opens the dual-pane file manager (SFTP / SCP) instead of a terminal' }
];

export const PROTOCOL_OPTIONS: readonly SegmentOption<Protocol>[] = [
  { value: 'ssh', label: 'SSH', icon: <Lock size={14} /> },
  { value: 'telnet', label: 'Telnet', icon: <Network size={14} /> },
  { value: 'rlogin', label: 'Rlogin', icon: <LogIn size={14} /> },
  { value: 'raw', label: 'Raw', icon: <Cable size={14} /> },
  { value: 'serial', label: 'Serial', icon: <Usb size={14} /> }
];

const TRISTATE_OPTIONS: readonly SegmentOption<TriState>[] = [
  { value: 'auto', label: 'Auto' },
  { value: 'on', label: 'On' },
  { value: 'off', label: 'Off' }
];

const FAMILY_OPTIONS: readonly SegmentOption<AddressFamily>[] = [
  { value: 'auto', label: 'Auto' },
  { value: 'ipv4', label: 'IPv4' },
  { value: 'ipv6', label: 'IPv6' }
];

/* -------------------------------------------------------------------------- */

export function SessionSection() {
  const { draft, update, fieldError, warning, isDefault, folders } = useEditor();
  const network = draft.protocol !== 'serial';
  const folderOptions = useMemo<ComboOption[]>(
    () => [{ value: '', label: '(root)' }, ...folders.map((f) => ({ value: f }))],
    [folders]
  );
  const folderNorm = normalizeFolder(draft.folder);
  const newFolder = folderNorm !== '' && !folders.includes(folderNorm);

  return (
    <>
      {isDefault && (
        <Callout tone="info" title="Default Settings">
          New sessions start from these settings. Saved sessions are not changed.
        </Callout>
      )}

      {!isDefault && (
        <Section title="Identity">
          <FieldGrid cols={2}>
            <FormField label="Name" error={fieldError('name')} warning={warning('name')} htmlFor="se-name">
              <TextInput
                id="se-name"
                value={draft.name}
                placeholder={fallbackSessionName(draft)}
                onChange={(v) => update((d) => void (d.name = v))}
                invalid={!!fieldError('name')}
              />
            </FormField>
            <FormField label="Folder" hint={newFolder ? `Folder "${folderNorm}" will be created` : 'Use "/" for nested folders'} htmlFor="se-folder">
              <ComboInput
                id="se-folder"
                value={draft.folder}
                options={folderOptions}
                placeholder="(root)"
                onChange={(v) => update((d) => void (d.folder = v))}
              />
            </FormField>
            <FormField label="Colour" span="all">
              <ColorSwatches value={draft.color} onChange={(c) => update((d) => void (d.color = c))} />
            </FormField>
          </FieldGrid>
        </Section>
      )}

      <Section title="Connection">
        <FormField label="Protocol">
          <SegmentedControl
            ariaLabel="Protocol"
            value={draft.protocol}
            options={PROTOCOL_OPTIONS}
            onChange={(p) => update((d) => Object.assign(d, switchProtocol(d, p)))}
          />
        </FormField>
        {network ? (
          <FieldGrid cols={4} className="se-hostgrid">
            <FormField label="Host name or IP address" error={fieldError('host')} span={3} htmlFor="se-host">
              <TextInput
                id="se-host"
                value={draft.host}
                placeholder={isDefault ? '(none)' : 'server.example.com'}
                invalid={!!fieldError('host')}
                onChange={(v) => update((d) => void (d.host = v))}
                onBlur={() => {
                  // Accept "user@host:port" pasted into the host field.
                  const parsed = splitHostInput(draft.host);
                  if (parsed.host === draft.host && parsed.username === undefined && parsed.port === undefined) return;
                  update((d) => {
                    d.host = parsed.host;
                    if (parsed.username !== undefined && !d.username) d.username = parsed.username;
                    if (parsed.port !== undefined) d.port = parsed.port;
                  });
                }}
              />
            </FormField>
            <FormField label="Port" error={fieldError('port')} htmlFor="se-port">
              <NumberInput
                id="se-port"
                value={draft.port}
                min={0}
                max={65535}
                onChange={(n) => update((d) => void (d.port = n))}
                aria-invalid={!!fieldError('port') || undefined}
                placeholder={String(DEFAULT_PORTS[draft.protocol] || '')}
              />
            </FormField>
            {draft.protocol !== 'raw' && (
              <FormField
                label={draft.protocol === 'rlogin' ? 'Remote username' : 'Username'}
                hint={draft.protocol === 'ssh' ? 'Leave empty to be asked when connecting' : 'Sent automatically when logging in'}
                span={2}
                htmlFor="se-user"
              >
                <TextInput id="se-user" value={draft.username} placeholder="(prompt)" onChange={(v) => update((d) => void (d.username = v))} />
              </FormField>
            )}
          </FieldGrid>
        ) : (
          <FieldGrid cols={2}>
            <FormField label="Serial line" error={fieldError('serial.line')} htmlFor="se-line">
              <SerialLineInput id="se-line" />
            </FormField>
            <FormField label="Speed (baud)" error={fieldError('serial.baudRate')}>
              <BaudRateInput />
            </FormField>
          </FieldGrid>
        )}
        {draft.protocol === 'ssh' && (
          <FormField label="Open as" hint="What opening this session shows. Both are always available from its context menu.">
            <SegmentedControl
              ariaLabel="Open as"
              value={draft.openAs ?? 'terminal'}
              options={OPEN_AS_OPTIONS}
              onChange={(v) => update((d) => void (d.openAs = v))}
            />
          </FormField>
        )}
      </Section>

      {!isDefault && (
        <Section title="Notes">
          <TextArea
            value={draft.notes}
            onChange={(v) => update((d) => void (d.notes = v))}
            placeholder="Anything worth remembering about this host…"
            rows={3}
          />
        </Section>
      )}
    </>
  );
}

/* -------------------------------------------------------------------------- */

const TERM_TYPES: ComboOption[] = [
  { value: 'xterm-256color', hint: 'Modern default' },
  { value: 'xterm' },
  { value: 'xterm-color' },
  { value: 'screen-256color' },
  { value: 'tmux-256color' },
  { value: 'linux' },
  { value: 'vt100' },
  { value: 'vt220' },
  { value: 'ansi' },
  { value: 'putty' },
  { value: 'dumb' }
];

export function ConnectionSection() {
  const { draft, update, fieldError, warning, isDefault } = useEditor();
  const c = draft.connection;
  const showEnv = isDefault || draft.protocol === 'ssh' || draft.protocol === 'telnet';
  const showTerm = isDefault || draft.protocol !== 'raw';

  return (
    <>
      <Section title="Network">
        <FieldGrid cols={2}>
          <FormField
            label="Keepalive interval"
            hint="Seconds between keepalive packets, 0 = off"
            error={fieldError('connection.keepaliveSeconds')}
          >
            <div className="dlg-suffixed">
              <NumberInput value={c.keepaliveSeconds} min={0} max={86400} onChange={(n) => update((d) => void (d.connection.keepaliveSeconds = n))} />
              <span className="dlg-suffixed__suffix">s</span>
            </div>
          </FormField>
          <FormField label="Connect timeout" hint="0 = no timeout" error={fieldError('connection.connectTimeoutSeconds')}>
            <div className="dlg-suffixed">
              <NumberInput
                value={c.connectTimeoutSeconds}
                min={0}
                max={3600}
                onChange={(n) => update((d) => void (d.connection.connectTimeoutSeconds = n))}
              />
              <span className="dlg-suffixed__suffix">s</span>
            </div>
          </FormField>
          <FormField label="Internet protocol version">
            <SegmentedControl value={c.addressFamily} options={FAMILY_OPTIONS} onChange={(v) => update((d) => void (d.connection.addressFamily = v))} />
          </FormField>
        </FieldGrid>
        <div className="dlg-rows">
          <ToggleRow
            label="Disable Nagle's algorithm (TCP_NODELAY)"
            description="Send keystrokes immediately instead of batching small packets."
            checked={c.tcpNoDelay}
            onChange={(v) => update((d) => void (d.connection.tcpNoDelay = v))}
          />
          <ToggleRow
            label="Enable TCP keepalives (SO_KEEPALIVE)"
            description="Let the operating system detect dead connections."
            checked={c.tcpKeepalive}
            onChange={(v) => update((d) => void (d.connection.tcpKeepalive = v))}
          />
        </div>
      </Section>

      <Section title="Identity">
        <FieldGrid cols={2}>
          <FormField label="Logical host name" hint="Used instead of the host name when checking host keys" span="all">
            <TextInput
              value={c.logicalHostname}
              placeholder={draft.host || '(same as host)'}
              onChange={(v) => update((d) => void (d.connection.logicalHostname = v))}
            />
          </FormField>
          {showTerm && (
            <>
              <FormField label="Terminal type (TERM)">
                <ComboInput mono value={c.terminalType} options={TERM_TYPES} onChange={(v) => update((d) => void (d.connection.terminalType = v))} />
              </FormField>
              <FormField label="Terminal speed" hint="Reported to the server, e.g. 38400,38400">
                <TextInput className="mono-input" value={c.terminalSpeed} onChange={(v) => update((d) => void (d.connection.terminalSpeed = v))} />
              </FormField>
            </>
          )}
        </FieldGrid>
      </Section>

      {showEnv && (
        <Section
          title="Environment variables"
          description="Sent to the server when the session starts (SSH: may require AcceptEnv on the server; Telnet: NEW-ENVIRON)."
          actions={
            <Button
              size="sm"
              icon={<Plus size={14} />}
              onClick={() => update((d) => void d.connection.environment.push({ name: '', value: '' }))}
            >
              Add variable
            </Button>
          }
        >
          {c.environment.length === 0 ? (
            <div className="dlg-muted-box">No environment variables.</div>
          ) : (
            <div className="env-table" role="table">
              <div className="env-table__head" role="row">
                <span role="columnheader">Name</span>
                <span role="columnheader">Value</span>
                <span />
              </div>
              {c.environment.map((v, i) => {
                const err = fieldError(`env:${i}`);
                const warn = warning(`env:${i}`);
                return (
                  <div className="env-table__row" role="row" key={i}>
                    <div>
                      <TextInput
                        className="mono-input"
                        value={v.name}
                        placeholder="NAME"
                        invalid={!!err}
                        onChange={(val) => update((d) => void (d.connection.environment[i].name = val))}
                        aria-label="Variable name"
                      />
                      {(err || warn) && <span className={err ? 'field__error' : 'dlg-field__warning'}>{err ?? warn}</span>}
                    </div>
                    <TextInput
                      className="mono-input"
                      value={v.value}
                      placeholder="value"
                      onChange={(val) => update((d) => void (d.connection.environment[i].value = val))}
                      aria-label="Variable value"
                    />
                    <IconButton label="Remove variable" onClick={() => update((d) => void d.connection.environment.splice(i, 1))}>
                      <Trash2 size={14} />
                    </IconButton>
                  </div>
                );
              })}
            </div>
          )}
        </Section>
      )}
    </>
  );
}

/* -------------------------------------------------------------------------- */

const PROXY_TYPES: { value: ProxyType; label: string }[] = [
  { value: 'none', label: 'None (direct connection)' },
  { value: 'socks4', label: 'SOCKS 4 / 4a' },
  { value: 'socks5', label: 'SOCKS 5' },
  { value: 'http', label: 'HTTP CONNECT' },
  { value: 'telnet', label: 'Telnet' },
  { value: 'command', label: 'Local command' },
  { value: 'ssh', label: 'SSH jump host' }
];

const DEFAULT_PROXY_PORT: Partial<Record<ProxyType, number>> = { socks4: 1080, socks5: 1080, http: 3128, telnet: 23, ssh: 22 };

function PlaceholderHelp({ telnet }: { telnet?: boolean }) {
  return (
    <div className="placeholder-help">
      <div className="placeholder-help__title">Substitutions</div>
      <dl>
        <dt>%host</dt>
        <dd>destination host</dd>
        <dt>%port</dt>
        <dd>destination port</dd>
        <dt>%user</dt>
        <dd>proxy username</dd>
        <dt>%pass</dt>
        <dd>proxy password</dd>
        <dt>%proxyhost</dt>
        <dd>proxy host</dd>
        <dt>%proxyport</dt>
        <dd>proxy port</dd>
        <dt>%%</dt>
        <dd>a literal %</dd>
        {telnet && (
          <>
            <dt>\n \r \t</dt>
            <dd>newline, return, tab</dd>
            <dt>\xHH \\</dt>
            <dd>hex byte, backslash</dd>
          </>
        )}
      </dl>
    </div>
  );
}

export function ProxySection() {
  const { draft, update, fieldError, sessions } = useEditor();
  const p = draft.proxy;
  const endpoint = p.type === 'socks4' || p.type === 'socks5' || p.type === 'http' || p.type === 'telnet' || (p.type === 'ssh' && !p.jumpSessionId);
  const auth = p.type === 'socks5' || p.type === 'http' || p.type === 'telnet' || (p.type === 'ssh' && !p.jumpSessionId);

  const jumpOptions = useMemo(() => {
    const opts: { value: string; label: string }[] = [{ value: '', label: 'Custom host (enter below)' }];
    for (const s of sessions) {
      if (s.protocol !== 'ssh' || s.id === draft.id) continue;
      opts.push({ value: s.id, label: `${s.folder ? s.folder + ' / ' : ''}${sessionLabel(s)}${s.host && s.name ? `  (${s.host})` : ''}` });
    }
    if (p.jumpSessionId && !opts.some((o) => o.value === p.jumpSessionId)) opts.push({ value: p.jumpSessionId, label: '(missing session)' });
    return opts;
  }, [sessions, draft.id, p.jumpSessionId]);

  return (
    <>
      <Section title="Proxy type">
        <FormField label="Connect through">
          <Select<ProxyType>
            value={p.type}
            options={PROXY_TYPES}
            onChange={(t) =>
              update((d) => {
                const oldDefault = DEFAULT_PROXY_PORT[d.proxy.type];
                d.proxy.type = t;
                const nd = DEFAULT_PROXY_PORT[t];
                if (nd && (!d.proxy.port || d.proxy.port === oldDefault)) d.proxy.port = nd;
              })
            }
          />
        </FormField>
        {p.type === 'none' && <div className="dlg-muted-box">Connections are made directly to the destination host.</div>}
      </Section>

      {p.type === 'ssh' && (
        <Section title="Jump host" description="Tunnel this connection through another SSH server (like ssh -J).">
          <FormField label="Jump through saved session" error={fieldError('proxy.jumpSessionId')}>
            <Select value={p.jumpSessionId} options={jumpOptions} onChange={(v) => update((d) => void (d.proxy.jumpSessionId = v))} />
          </FormField>
        </Section>
      )}

      {endpoint && (
        <Section title={p.type === 'ssh' ? 'Jump host server' : 'Proxy server'}>
          <FieldGrid cols={4}>
            <FormField label="Host" error={fieldError('proxy.host')} span={3}>
              <TextInput value={p.host} invalid={!!fieldError('proxy.host')} placeholder="proxy.example.com" onChange={(v) => update((d) => void (d.proxy.host = v))} />
            </FormField>
            <FormField label="Port" error={fieldError('proxy.port')}>
              <NumberInput value={p.port} min={1} max={65535} onChange={(n) => update((d) => void (d.proxy.port = n))} />
            </FormField>
            {auth && (
              <>
                <FormField label="Username" span={2}>
                  <TextInput value={p.username} onChange={(v) => update((d) => void (d.proxy.username = v))} placeholder="(none)" />
                </FormField>
                <FormField label="Password" span={2} hint="Stored encrypted (Windows DPAPI)">
                  <TextInput type="password" value={p.password} onChange={(v) => update((d) => void (d.proxy.password = v))} placeholder="(none)" autoComplete="new-password" />
                </FormField>
              </>
            )}
          </FieldGrid>
        </Section>
      )}

      {p.type === 'telnet' && (
        <Section title="Telnet proxy command" description="Sent to the telnet proxy after connecting.">
          <div className="placeholder-layout">
            <TextArea className="mono-input" rows={3} value={p.telnetCommand} onChange={(v) => update((d) => void (d.proxy.telnetCommand = v))} />
            <PlaceholderHelp telnet />
          </div>
        </Section>
      )}

      {p.type === 'command' && (
        <Section title="Local proxy command" description="A program whose standard input/output is used as the connection (e.g. ncat, plink -nc).">
          <div className="placeholder-layout">
            <FormField error={fieldError('proxy.localCommand')}>
              <TextArea
                className="mono-input"
                rows={3}
                value={p.localCommand}
                placeholder="ncat --proxy proxy.example.com:8080 %host %port"
                onChange={(v) => update((d) => void (d.proxy.localCommand = v))}
              />
            </FormField>
            <PlaceholderHelp telnet />
          </div>
        </Section>
      )}

      {p.type !== 'none' && (
        <Section title="Options">
          <FormField label="Exclude hosts" hint="Comma or space separated patterns that bypass the proxy, e.g. *.local, 10.*">
            <TextInput className="mono-input" value={p.excludeHosts} onChange={(v) => update((d) => void (d.proxy.excludeHosts = v))} placeholder="(none)" />
          </FormField>
          <div className="dlg-rows">
            <ToggleRow
              label="Use the proxy for local addresses"
              description="Also proxy connections to localhost, 127.0.0.0/8 and ::1."
              checked={p.proxyLocalhost}
              onChange={(v) => update((d) => void (d.proxy.proxyLocalhost = v))}
            />
          </div>
          {p.type !== 'command' && (
            <FormField label="Resolve host names at the proxy" hint="Auto = yes for SOCKS 4a/5 and HTTP">
              <SegmentedControl value={p.dnsAtProxy} options={TRISTATE_OPTIONS} onChange={(v) => update((d) => void (d.proxy.dnsAtProxy = v))} />
            </FormField>
          )}
        </Section>
      )}
    </>
  );
}
