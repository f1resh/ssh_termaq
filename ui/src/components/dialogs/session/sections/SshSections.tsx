/**
 * Session editor: SSH, SSH › Authentication, Tunnels, X11 and Algorithms sections.
 */
import { useEffect, useMemo, useRef, useState, type DragEvent, type KeyboardEvent as ReactKeyboardEvent } from 'react';
import {
  ArrowDown,
  ArrowUp,
  GripVertical,
  KeyRound,
  Pencil,
  Plus,
  RotateCcw,
  Trash2,
  X
} from 'lucide-react';
import { api } from '@/bridge/api';
import { Button, IconButton, NumberInput, Switch, TextArea, TextInput, cx } from '@/components/ui';
import { createPortForward } from '@shared/defaults';
import { SSH_CIPHERS, SSH_HOSTKEYS, SSH_KEX, SSH_MACS, WEAK_ALGORITHMS } from '@shared/algorithms';
import type { AddressFamily, AgentKind, FileProtocol, ForwardType, KeyInfo, PortForward } from '@shared/types';
import {
  Badge,
  Callout,
  FieldGrid,
  FormField,
  PathInput,
  Section,
  SegmentedControl,
  SettingRow,
  Spinner,
  ToggleRow,
  type SegmentOption
} from '../../common/controls';
import { errorMessage } from '../../common/hooks';
import { useEditor } from '../editorContext';
import { algoRows, moveRow, valueForRows, type AlgoRow } from '../algoModel';
import { describeTunnel, stableStringify, validateTunnels, type TunnelErrors } from '../sessionModel';

/* -------------------------------------------------------------------------- */
/* SSH                                                                        */
/* -------------------------------------------------------------------------- */

export function SshSection() {
  const { draft, update, warning } = useEditor();
  const s = draft.ssh;
  return (
    <>
      <Section title="Remote command" description="Run this command instead of starting an interactive shell.">
        <TextArea
          className="mono-input"
          rows={3}
          value={s.remoteCommand}
          placeholder="(start a shell)"
          onChange={(v) => update((d) => void (d.ssh.remoteCommand = v))}
        />
      </Section>
      <Section title="Session">
        <div className="dlg-rows">
          <ToggleRow
            label="Don't start a shell or command at all"
            description="Useful for connections that only carry port forwardings (ssh -N)."
            checked={s.noShell}
            onChange={(v) => update((d) => void (d.ssh.noShell = v))}
          />
          <ToggleRow
            label="Allocate a pseudo-terminal"
            description="Turn off for commands that expect raw, non-interactive I/O."
            checked={s.allocatePty}
            onChange={(v) => update((d) => void (d.ssh.allocatePty = v))}
          />
          <ToggleRow
            label="Enable compression"
            description="Can speed up slow links; costs CPU on fast ones."
            checked={s.compression}
            onChange={(v) => update((d) => void (d.ssh.compression = v))}
          />
        </div>
        {warning('ssh.noShell') && <Callout tone="warning">{warning('ssh.noShell')}</Callout>}
      </Section>
      <Section title="File manager">
        <FormField label="File protocol" hint="Auto uses SFTP and falls back to SCP for servers without SFTP (routers, BusyBox, Dropbear).">
          <SegmentedControl
            ariaLabel="File protocol"
            value={s.fileProtocol ?? 'auto'}
            options={FILE_PROTOCOL_OPTIONS}
            onChange={(v) => update((d) => void (d.ssh.fileProtocol = v))}
          />
        </FormField>
        <FormField label="Initial remote folder" hint="Leave empty to start in the home folder">
          <TextInput
            className="mono-input"
            value={s.sftpStartPath}
            placeholder="~"
            onChange={(v) => update((d) => void (d.ssh.sftpStartPath = v))}
          />
        </FormField>
        <FormField label="Initial local folder" hint="Leave empty to start where the file manager was last used">
          <TextInput
            className="mono-input"
            value={s.localStartPath ?? ''}
            placeholder="C:\Users\…"
            onChange={(v) => update((d) => void (d.ssh.localStartPath = v))}
          />
        </FormField>
      </Section>
    </>
  );
}

/* -------------------------------------------------------------------------- */
/* SSH › Authentication                                                       */
/* -------------------------------------------------------------------------- */

const FILE_PROTOCOL_OPTIONS: readonly SegmentOption<FileProtocol>[] = [
  { value: 'auto', label: 'Auto' },
  { value: 'sftp', label: 'SFTP' },
  { value: 'scp', label: 'SCP', title: 'Legacy SCP with shell commands for browsing; no resume' }
];

const AGENT_OPTIONS: readonly SegmentOption<AgentKind>[] = [
  { value: 'auto', label: 'Auto', title: 'Pageant if running, otherwise the Windows OpenSSH agent' },
  { value: 'pageant', label: 'Pageant' },
  { value: 'openssh', label: 'OpenSSH agent' }
];

const KEY_FILTERS = [
  { name: 'PuTTY private key (*.ppk)', extensions: ['ppk'] },
  { name: 'OpenSSH / PEM private key', extensions: ['pem', 'key', 'id_rsa', 'id_ed25519', 'id_ecdsa'] },
  { name: 'All files', extensions: ['*'] }
];

const FORMAT_LABELS: Record<KeyInfo['format'], string> = {
  openssh: 'OpenSSH',
  pem: 'PEM',
  ppk2: 'PuTTY PPK v2',
  ppk3: 'PuTTY PPK v3',
  unknown: 'Unknown format'
};

/** Inspects a key file (debounced) and reports its public information. */
function useKeyInfo(path: string): { info: KeyInfo | null; error: string | null; loading: boolean } {
  const [state, setState] = useState<{ info: KeyInfo | null; error: string | null; loading: boolean }>({
    info: null,
    error: null,
    loading: false
  });
  useEffect(() => {
    const p = path.trim();
    if (!p) {
      setState({ info: null, error: null, loading: false });
      return;
    }
    let alive = true;
    setState((s) => ({ ...s, loading: true }));
    const t = setTimeout(() => {
      api.keys
        .inspect(p)
        .then((info) => alive && setState({ info, error: null, loading: false }))
        .catch((e: unknown) => alive && setState({ info: null, error: errorMessage(e), loading: false }));
    }, 350);
    return () => {
      alive = false;
      clearTimeout(t);
    };
  }, [path]);
  return state;
}

function KeyInfoCard({ path }: { path: string }) {
  const { info, error, loading } = useKeyInfo(path);
  if (!path.trim()) return null;
  if (loading && !info) return <Spinner size={14} label="Reading key…" />;
  if (error) return <Callout tone="danger" title="Cannot read this key file">{error}</Callout>;
  if (!info) return null;
  return (
    <div className="keyinfo">
      <div className="keyinfo__icon">
        <KeyRound size={18} />
      </div>
      <div className="keyinfo__body">
        <div className="keyinfo__head">
          <span className="mono-text">{info.keyType || 'unknown key type'}</span>
          <Badge>{FORMAT_LABELS[info.format]}</Badge>
          {info.encrypted ? <Badge tone="success">Passphrase protected</Badge> : <Badge tone="warning">No passphrase</Badge>}
        </div>
        <div className="keyinfo__fp mono-text">{info.fingerprint}</div>
        {info.comment && <div className="keyinfo__comment">{info.comment}</div>}
      </div>
    </div>
  );
}

function SavedPasswordSection() {
  const { draft, isNew, passwordChange, setPasswordChange } = useEditor();
  const [stored, setStored] = useState<boolean | null>(isNew ? false : null);
  const [editing, setEditing] = useState(false);
  const [value, setValue] = useState('');
  useEffect(() => {
    if (isNew) return;
    let alive = true;
    api.credentials
      .hasPassword(draft.id)
      .then((v) => alive && setStored(!!v))
      .catch(() => alive && setStored(false));
    return () => {
      alive = false;
    };
  }, [draft.id, isNew]);

  const effective = passwordChange ? passwordChange.action === 'set' : stored;
  const status =
    passwordChange?.action === 'set'
      ? 'A new password will be saved'
      : passwordChange?.action === 'clear'
        ? 'The saved password will be removed'
        : stored === null
          ? 'Checking…'
          : stored
            ? 'A password is saved for this session'
            : 'No password saved';

  return (
    <Section
      title="Saved password"
      description="Encrypted with Windows DPAPI for your user account and used automatically when the server asks for a password."
    >
      <div className="dlg-rows">
        <SettingRow
          label={
            <span className="se-pw-status">
              <span className={cx('se-pw-dot', effective && 'se-pw-dot--on')} aria-hidden />
              {status}
            </span>
          }
        >
          {passwordChange && (
            <Button size="sm" variant="ghost" icon={<RotateCcw size={13} />} onClick={() => setPasswordChange(null)}>
              Undo
            </Button>
          )}
          {!editing && (
            <Button size="sm" onClick={() => setEditing(true)}>
              {effective ? 'Change…' : 'Set password…'}
            </Button>
          )}
          {effective && !editing && (
            <Button size="sm" variant="danger" onClick={() => setPasswordChange(stored ? { action: 'clear', value: '' } : null)}>
              Clear
            </Button>
          )}
        </SettingRow>
        {editing && (
          <div className="dlg-row se-pw-edit">
            <TextInput
              type="password"
              autoComplete="new-password"
              value={value}
              placeholder="Password"
              onChange={setValue}
              aria-label="Password to save"
              autoFocus
              onKeyDown={(e) => {
                if (e.key === 'Enter' && value) {
                  e.preventDefault();
                  setPasswordChange({ action: 'set', value });
                  setValue('');
                  setEditing(false);
                }
              }}
            />
            <Button size="sm" onClick={() => (setEditing(false), setValue(''))}>
              Cancel
            </Button>
            <Button
              size="sm"
              variant="primary"
              disabled={!value}
              onClick={() => {
                setPasswordChange({ action: 'set', value });
                setValue('');
                setEditing(false);
              }}
            >
              Use password
            </Button>
          </div>
        )}
      </div>
    </Section>
  );
}

export function AuthSection() {
  const { draft, update, isDefault } = useEditor();
  const a = draft.ssh.auth;
  return (
    <>
      <Section title="Private key" description="PuTTY .ppk (v2 / v3) or OpenSSH / PEM private key file.">
        <PathInput
          value={a.privateKeyPath}
          placeholder="(none)"
          onChange={(v) => update((d) => void (d.ssh.auth.privateKeyPath = v))}
          onBrowse={() => api.dialog.openFile({ title: 'Choose a private key', defaultPath: a.privateKeyPath || undefined, filters: KEY_FILTERS })}
          extra={
            a.privateKeyPath ? (
              <IconButton label="Clear key file" onClick={() => update((d) => void (d.ssh.auth.privateKeyPath = ''))}>
                <X size={14} />
              </IconButton>
            ) : undefined
          }
        />
        <KeyInfoCard path={a.privateKeyPath} />
      </Section>

      <Section title="Agent">
        <div className="dlg-rows">
          <ToggleRow
            label="Try authentication using an SSH agent"
            description="Keys held by Pageant or the Windows OpenSSH agent are offered first."
            checked={a.tryAgent}
            onChange={(v) => update((d) => void (d.ssh.auth.tryAgent = v))}
          />
          <SettingRow label="Agent" description="Which agent to talk to." disabled={!a.tryAgent && !a.agentForwarding}>
            <SegmentedControl
              size="sm"
              value={a.agent}
              options={AGENT_OPTIONS.map((o) => ({ ...o, disabled: !a.tryAgent && !a.agentForwarding }))}
              onChange={(v) => update((d) => void (d.ssh.auth.agent = v))}
              ariaLabel="Agent"
            />
          </SettingRow>
          <ToggleRow
            label="Allow agent forwarding"
            description="Lets the server use your local agent to authenticate onward connections. Only enable for servers you trust."
            checked={a.agentForwarding}
            onChange={(v) => update((d) => void (d.ssh.auth.agentForwarding = v))}
          />
        </div>
      </Section>

      <Section title="Methods">
        <div className="dlg-rows">
          <ToggleRow
            label="Keyboard-interactive"
            description="Challenge/response prompts, one-time passwords, 2FA."
            checked={a.keyboardInteractive}
            onChange={(v) => update((d) => void (d.ssh.auth.keyboardInteractive = v))}
          />
          <ToggleRow
            label="Password"
            checked={a.password}
            onChange={(v) => update((d) => void (d.ssh.auth.password = v))}
          />
          <ToggleRow
            label="Show the server's pre-authentication banner"
            checked={a.showBanner}
            onChange={(v) => update((d) => void (d.ssh.auth.showBanner = v))}
          />
        </div>
        {!a.tryAgent && !a.privateKeyPath && !a.keyboardInteractive && !a.password && (
          <Callout tone="warning">All authentication methods are disabled; the connection cannot log in.</Callout>
        )}
      </Section>

      {!isDefault && <SavedPasswordSection />}
    </>
  );
}

/* -------------------------------------------------------------------------- */
/* SSH › Tunnels                                                              */
/* -------------------------------------------------------------------------- */

const FORWARD_TYPE_OPTIONS: readonly SegmentOption<ForwardType>[] = [
  { value: 'local', label: 'Local', title: 'Listen locally, connect from the server (ssh -L)' },
  { value: 'remote', label: 'Remote', title: 'Listen on the server, connect from this PC (ssh -R)' },
  { value: 'dynamic', label: 'Dynamic', title: 'Local SOCKS proxy (ssh -D)' }
];

const FAMILY_OPTIONS: readonly SegmentOption<AddressFamily>[] = [
  { value: 'auto', label: 'Auto' },
  { value: 'ipv4', label: 'IPv4' },
  { value: 'ipv6', label: 'IPv6' }
];

const TYPE_HELP: Record<ForwardType, string> = {
  local: 'Connections to the source port on this PC are forwarded through the server to the destination.',
  remote: 'The server listens on the source port and forwards connections back to the destination as seen from this PC.',
  dynamic: 'A SOCKS 4/5 proxy on the source port; the destination is chosen by each client application.'
};

const TYPE_LETTER: Record<ForwardType, string> = { local: 'L', remote: 'R', dynamic: 'D' };

interface TunnelEdit {
  value: PortForward;
  isNew: boolean;
  errors: TunnelErrors | null;
}

function TunnelEditor({
  edit,
  onChange,
  onCancel,
  onCommit,
  acceptAll
}: {
  edit: TunnelEdit;
  onChange: (t: PortForward) => void;
  onCancel: () => void;
  onCommit: () => void;
  acceptAll: boolean;
}) {
  const t = edit.value;
  const e = edit.errors ?? {};
  const set = <K extends keyof PortForward>(k: K, v: PortForward[K]) => onChange({ ...t, [k]: v });
  const onKeyDown = (ev: ReactKeyboardEvent) => {
    if (ev.key === 'Enter' && (ev.target as HTMLElement).tagName === 'INPUT') {
      ev.preventDefault();
      onCommit();
    }
  };
  return (
    <div className="tunnel-editor" onKeyDown={onKeyDown}>
      <div className="tunnel-editor__top">
        <SegmentedControl value={t.type} options={FORWARD_TYPE_OPTIONS} onChange={(v) => set('type', v)} ariaLabel="Forwarding type" />
        <span className="tunnel-editor__help">{TYPE_HELP[t.type]}</span>
      </div>
      <FieldGrid cols={4}>
        <FormField label="Bind address" error={e.bindAddress} hint={acceptAll ? 'Empty = all interfaces' : 'Empty = localhost'}>
          <TextInput className="mono-input" value={t.bindAddress} placeholder={acceptAll ? '0.0.0.0' : 'localhost'} invalid={!!e.bindAddress} onChange={(v) => set('bindAddress', v)} />
        </FormField>
        <FormField label="Source port" error={e.sourcePort}>
          <NumberInput
            value={t.sourcePort || NaN}
            min={1}
            max={65535}
            placeholder="8080"
            aria-invalid={!!e.sourcePort || undefined}
            onChange={(n) => set('sourcePort', n)}
            autoFocus
          />
        </FormField>
        {t.type !== 'dynamic' ? (
          <>
            <FormField label="Destination host" error={e.destHost}>
              <TextInput className="mono-input" value={t.destHost} placeholder="localhost" invalid={!!e.destHost} onChange={(v) => set('destHost', v)} />
            </FormField>
            <FormField label="Destination port" error={e.destPort}>
              <NumberInput
                value={t.destPort || NaN}
                min={1}
                max={65535}
                placeholder="80"
                aria-invalid={!!e.destPort || undefined}
                onChange={(n) => set('destPort', n)}
              />
            </FormField>
          </>
        ) : (
          <div className="tunnel-editor__socks" style={{ gridColumn: 'span 2' }}>
            Applications connect to this port using SOCKS 4, 4A or 5.
          </div>
        )}
        <FormField label="Description" span={2}>
          <TextInput value={t.description} placeholder="(optional)" onChange={(v) => set('description', v)} />
        </FormField>
        <FormField label="IP version" span={2}>
          <SegmentedControl size="sm" value={t.addressFamily} options={FAMILY_OPTIONS} onChange={(v) => set('addressFamily', v)} />
        </FormField>
      </FieldGrid>
      <div className="tunnel-editor__actions">
        <Switch checked={t.enabled} onChange={(v) => set('enabled', v)} label="Enabled" />
        <span className="tunnel-editor__summary mono-text">{describeTunnel(t)}</span>
        <Button size="sm" onClick={onCancel}>
          Cancel
        </Button>
        <Button size="sm" variant="primary" onClick={onCommit}>
          {edit.isNew ? 'Add forwarding' : 'Apply'}
        </Button>
      </div>
    </div>
  );
}

export function TunnelsSection() {
  const { draft, update, fieldError } = useEditor();
  const tunnels = draft.ssh.tunnels;
  const [edit, setEdit] = useState<TunnelEdit | null>(null);

  const startAdd = () => setEdit({ value: createPortForward({ sourcePort: 0, destHost: 'localhost' }), isNew: true, errors: null });
  const startEdit = (t: PortForward) => setEdit({ value: structuredClone(t), isNew: false, errors: null });

  const commit = () => {
    if (!edit) return;
    const list = edit.isNew ? [...tunnels, edit.value] : tunnels.map((t) => (t.id === edit.value.id ? edit.value : t));
    const errs = validateTunnels(list)[edit.value.id];
    if (errs && Object.keys(errs).length) {
      setEdit({ ...edit, errors: errs });
      return;
    }
    const value = { ...edit.value, bindAddress: edit.value.bindAddress.trim(), destHost: edit.value.destHost.trim() };
    update((d) => {
      if (edit.isNew) d.ssh.tunnels.push(value);
      else d.ssh.tunnels = d.ssh.tunnels.map((t) => (t.id === value.id ? value : t));
    });
    setEdit(null);
  };

  const rowError = (t: PortForward) =>
    fieldError(`tunnel:${t.id}:sourcePort`) ??
    fieldError(`tunnel:${t.id}:destHost`) ??
    fieldError(`tunnel:${t.id}:destPort`) ??
    fieldError(`tunnel:${t.id}:bindAddress`);

  const editor = (acceptAll: boolean) =>
    edit && (
      <TunnelEditor
        edit={edit}
        acceptAll={acceptAll}
        onChange={(value) => setEdit({ ...edit, value, errors: edit.errors ? validateTunnels(edit.isNew ? [...tunnels, value] : tunnels.map((t) => (t.id === value.id ? value : t)))[value.id] ?? {} : null })}
        onCancel={() => setEdit(null)}
        onCommit={commit}
      />
    );

  const acceptAllFor = (t: PortForward) => (t.type === 'remote' ? draft.ssh.remotePortsAcceptAll : draft.ssh.localPortsAcceptAll);

  return (
    <>
      <Section
        title="Port forwardings"
        description="Forwardings are started when the connection is established."
        actions={
          <Button size="sm" variant="primary" icon={<Plus size={14} />} onClick={startAdd} disabled={!!edit}>
            Add forwarding
          </Button>
        }
      >
        <div className="dlg-table tunnels">
          <div className="dlg-table__head tunnels__grid" role="row">
            <span>On</span>
            <span>Type</span>
            <span>Listen</span>
            <span>Destination</span>
            <span>Description</span>
            <span />
          </div>
          {tunnels.length === 0 && !edit?.isNew && (
            <div className="dlg-table__empty">No port forwardings. Add one to reach services behind the server or expose local ones.</div>
          )}
          {tunnels.map((t) => {
            if (edit && !edit.isNew && edit.value.id === t.id) return <div key={t.id}>{editor(acceptAllFor(edit.value))}</div>;
            const err = rowError(t);
            const listen = `${t.bindAddress || (acceptAllFor(t) ? '*' : 'localhost')}:${t.sourcePort || '?'}`;
            const dest = t.type === 'dynamic' ? 'SOCKS proxy' : `${t.destHost.includes(':') ? `[${t.destHost}]` : t.destHost || '?'}:${t.destPort || '?'}`;
            return (
              <div key={t.id}>
                <div
                  className={cx('dlg-table__row tunnels__grid', !t.enabled && 'dlg-table__row--muted', err && 'tunnels__row--error')}
                  onDoubleClick={() => !edit && startEdit(t)}
                >
                  <Switch
                    checked={t.enabled}
                    onChange={(v) => update((d) => void (d.ssh.tunnels.find((x) => x.id === t.id)!.enabled = v))}
                    title={t.enabled ? 'Disable' : 'Enable'}
                  />
                  <span>
                    <Badge tone={t.type === 'local' ? 'accent' : t.type === 'remote' ? 'info' : 'success'} title={t.type}>
                      {TYPE_LETTER[t.type]} · {t.type}
                    </Badge>
                  </span>
                  <span className="dlg-table__cell mono-text">{listen}</span>
                  <span className="dlg-table__cell mono-text">{dest}</span>
                  <span className="dlg-table__cell muted" title={t.description}>
                    {t.addressFamily !== 'auto' && <Badge className="tunnels__family">{t.addressFamily === 'ipv4' ? 'IPv4' : 'IPv6'}</Badge>}
                    {t.description}
                  </span>
                  <span className="dlg-table__actions">
                    <IconButton size="sm" label="Edit forwarding" onClick={() => startEdit(t)} disabled={!!edit}>
                      <Pencil size={13} />
                    </IconButton>
                    <IconButton
                      size="sm"
                      label="Delete forwarding"
                      onClick={() => update((d) => void (d.ssh.tunnels = d.ssh.tunnels.filter((x) => x.id !== t.id)))}
                    >
                      <Trash2 size={13} />
                    </IconButton>
                  </span>
                </div>
                {err && <div className="tunnels__error field__error">{err}</div>}
              </div>
            );
          })}
          {edit?.isNew && editor(acceptAllFor(edit.value))}
        </div>
      </Section>

      <Section title="Listening">
        <div className="dlg-rows">
          <ToggleRow
            label="Local ports accept connections from other hosts"
            description="Local and dynamic forwardings without a bind address listen on all interfaces instead of localhost."
            checked={draft.ssh.localPortsAcceptAll}
            onChange={(v) => update((d) => void (d.ssh.localPortsAcceptAll = v))}
          />
          <ToggleRow
            label="Remote ports do the same"
            description="Ask the server to listen on all its interfaces (requires GatewayPorts on OpenSSH servers)."
            checked={draft.ssh.remotePortsAcceptAll}
            onChange={(v) => update((d) => void (d.ssh.remotePortsAcceptAll = v))}
          />
        </div>
      </Section>
    </>
  );
}

/* -------------------------------------------------------------------------- */
/* SSH › X11                                                                  */
/* -------------------------------------------------------------------------- */

export function X11Section() {
  const { draft, update, fieldError } = useEditor();
  const x = draft.ssh.x11;
  return (
    <>
      <Section>
        <div className="dlg-rows">
          <ToggleRow
            label="Enable X11 forwarding"
            description="Remote X applications open windows on a local X server (e.g. VcXsrv, Xming, X410)."
            checked={x.enabled}
            onChange={(v) => update((d) => void (d.ssh.x11.enabled = v))}
          />
        </div>
      </Section>
      <Section title="X server">
        <FormField label="X display location" hint="e.g. localhost:0 or :0 — the display your local X server listens on" error={fieldError('ssh.x11.display')}>
          <TextInput
            className="mono-input"
            value={x.display}
            disabled={!x.enabled}
            placeholder="localhost:0"
            invalid={!!fieldError('ssh.x11.display')}
            onChange={(v) => update((d) => void (d.ssh.x11.display = v))}
          />
        </FormField>
      </Section>
    </>
  );
}

/* -------------------------------------------------------------------------- */
/* SSH › Algorithms                                                           */
/* -------------------------------------------------------------------------- */

function AlgoList({
  title,
  catalogue,
  value,
  onChange
}: {
  title: string;
  catalogue: readonly string[];
  value: string[];
  onChange: (v: string[]) => void;
}) {
  const [rows, setRows] = useState<AlgoRow[]>(() => algoRows(catalogue, value));
  const emitted = useRef(stableStringify(value));
  const [dragFrom, setDragFrom] = useState<number | null>(null);
  const [dragOver, setDragOver] = useState<number | null>(null);

  // Follow external changes (reset, protocol template) that did not originate here.
  useEffect(() => {
    const v = stableStringify(value);
    if (v !== emitted.current) {
      emitted.current = v;
      setRows(algoRows(catalogue, value));
    }
  }, [value, catalogue]);

  const commit = (next: AlgoRow[]) => {
    setRows(next);
    const v = valueForRows(catalogue, next);
    emitted.current = stableStringify(v);
    onChange(v);
  };

  const isDefault = value.length === 0;
  const enabledCount = rows.filter((r) => r.enabled).length;

  const onDrop = (e: DragEvent, to: number) => {
    e.preventDefault();
    if (dragFrom !== null && dragFrom !== to) commit(moveRow(rows, dragFrom, to));
    setDragFrom(null);
    setDragOver(null);
  };

  return (
    <div className="algo">
      <div className="algo__head">
        <span className="algo__title">{title}</span>
        {isDefault ? <Badge tone="success">Defaults</Badge> : <Badge tone="accent">Custom</Badge>}
        <Button
          size="sm"
          variant="ghost"
          icon={<RotateCcw size={13} />}
          disabled={isDefault}
          onClick={() => {
            emitted.current = stableStringify([]);
            setRows(algoRows(catalogue, []));
            onChange([]);
          }}
        >
          Use defaults
        </Button>
      </div>
      <ol className="algo__list" aria-label={title}>
        {rows.map((r, i) => (
          <li
            key={r.name}
            className={cx(
              'algo__row',
              !r.enabled && 'algo__row--off',
              dragOver === i && dragFrom !== null && dragFrom !== i && (dragFrom < i ? 'algo__row--drop-below' : 'algo__row--drop-above'),
              dragFrom === i && 'algo__row--dragging'
            )}
            draggable
            onDragStart={(e) => {
              e.dataTransfer.effectAllowed = 'move';
              e.dataTransfer.setData('text/plain', r.name);
              setDragFrom(i);
            }}
            onDragOver={(e) => {
              if (dragFrom === null) return;
              e.preventDefault();
              e.dataTransfer.dropEffect = 'move';
              if (dragOver !== i) setDragOver(i);
            }}
            onDragEnd={() => {
              setDragFrom(null);
              setDragOver(null);
            }}
            onDrop={(e) => onDrop(e, i)}
          >
            <GripVertical size={13} className="algo__grip" aria-hidden />
            <input
              type="checkbox"
              checked={r.enabled}
              aria-label={`Enable ${r.name}`}
              disabled={r.enabled && enabledCount === 1}
              onChange={(e) => commit(rows.map((x, j) => (j === i ? { ...x, enabled: e.target.checked } : x)))}
            />
            <span className="algo__name mono-text" title={r.name}>
              {r.name}
            </span>
            {r.weak && (
              <Badge tone="warning" title="Considered weak; enable only for legacy servers">
                weak
              </Badge>
            )}
            {!r.known && <Badge title="Not in the list of algorithms known to this version">unknown</Badge>}
            <span className="algo__move">
              <IconButton size="sm" label={`Move ${r.name} up`} disabled={i === 0} onClick={() => commit(moveRow(rows, i, i - 1))}>
                <ArrowUp size={12} />
              </IconButton>
              <IconButton
                size="sm"
                label={`Move ${r.name} down`}
                disabled={i === rows.length - 1}
                onClick={() => commit(moveRow(rows, i, i + 1))}
              >
                <ArrowDown size={12} />
              </IconButton>
            </span>
          </li>
        ))}
      </ol>
    </div>
  );
}

export function AlgorithmsSection() {
  const { draft, update } = useEditor();
  const s = draft.ssh;
  const anyWeak = useMemo(
    () => [s.kex, s.ciphers, s.hostKeyAlgorithms, s.macs].some((list) => list.some((n) => WEAK_ALGORITHMS.has(n))),
    [s.kex, s.ciphers, s.hostKeyAlgorithms, s.macs]
  );
  return (
    <>
      <Callout tone="info">
        Algorithms are offered in the order shown; unticked ones are never used. Drag rows or use the arrows to reorder. “Defaults” uses the
        secure library defaults.
      </Callout>
      {anyWeak && (
        <Callout tone="warning" title="Weak algorithms enabled">
          Only enable weak algorithms when connecting to legacy equipment that supports nothing else.
        </Callout>
      )}
      <div className="algo-grid">
        <AlgoList title="Key exchange" catalogue={SSH_KEX} value={s.kex} onChange={(v) => update((d) => void (d.ssh.kex = v))} />
        <AlgoList title="Ciphers" catalogue={SSH_CIPHERS} value={s.ciphers} onChange={(v) => update((d) => void (d.ssh.ciphers = v))} />
        <AlgoList
          title="Host key types"
          catalogue={SSH_HOSTKEYS}
          value={s.hostKeyAlgorithms}
          onChange={(v) => update((d) => void (d.ssh.hostKeyAlgorithms = v))}
        />
        <AlgoList title="MACs" catalogue={SSH_MACS} value={s.macs} onChange={(v) => update((d) => void (d.ssh.macs = v))} />
      </div>
    </>
  );
}

