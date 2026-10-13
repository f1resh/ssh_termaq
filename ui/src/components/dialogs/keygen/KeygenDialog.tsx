/**
 * Key generator / converter (PuTTYgen equivalent): generate Ed25519 / RSA / ECDSA key pairs and save them as
 * OpenSSH or PuTTY .ppk files, or load an existing key to inspect it and convert between the two formats.
 * Private key material only ever lives in component state and is handed straight to the save dialog.
 */
import { useId, useMemo, useState, type ReactNode } from 'react';
import { FileKey2, FolderOpen, KeyRound, Save, Sparkles } from 'lucide-react';
import { api } from '@/bridge/api';
import { Button, Modal, TextArea, TextInput, toast } from '@/components/ui';
import type { KeyInfo, KeyType, KeygenResult } from '@shared/types';
import {
  Badge,
  Callout,
  CopyButton,
  FieldGrid,
  FormField,
  PathInput,
  Section,
  SegmentedControl,
  Spinner,
  type SegmentOption
} from '../common/controls';
import { errorMessage, useStableCallback } from '../common/hooks';
import { defaultKeyComment, defaultKeyFileName, isPassphraseError, keyBitsFor, KEY_BITS, type KeygenMode } from './keygenModel';

const TYPE_OPTIONS: readonly SegmentOption<KeyType>[] = [
  { value: 'ed25519', label: 'Ed25519', title: 'Recommended: small, fast and secure' },
  { value: 'rsa', label: 'RSA', title: 'Widest compatibility with old servers' },
  { value: 'ecdsa', label: 'ECDSA', title: 'NIST curves' }
];

const MODE_OPTIONS: readonly SegmentOption<KeygenMode>[] = [
  { value: 'generate', label: 'Generate new key', icon: <Sparkles size={14} /> },
  { value: 'load', label: 'Load existing key', icon: <FolderOpen size={14} /> }
];

const PRIVATE_FILTERS = {
  openssh: [
    { name: 'OpenSSH private key', extensions: ['*'] },
    { name: 'Key files (*.key, *.pem)', extensions: ['key', 'pem'] }
  ],
  ppk: [
    { name: 'PuTTY private key (*.ppk)', extensions: ['ppk'] },
    { name: 'All files', extensions: ['*'] }
  ],
  pub: [
    { name: 'Public key (*.pub)', extensions: ['pub'] },
    { name: 'All files', extensions: ['*'] }
  ]
};

const LOAD_FILTERS = [
  { name: 'Private keys (*.ppk, *.pem, *.key, id_*)', extensions: ['ppk', 'pem', 'key'] },
  { name: 'All files', extensions: ['*'] }
];

const FORMAT_LABELS: Record<KeyInfo['format'], string> = {
  openssh: 'OpenSSH',
  pem: 'PEM',
  ppk2: 'PuTTY PPK v2',
  ppk3: 'PuTTY PPK v3',
  unknown: 'Unknown'
};

async function saveText(content: string, title: string, defaultPath: string, filters: { name: string; extensions: string[] }[]): Promise<void> {
  try {
    const written = await api.dialog.saveTextFile(content, { title, defaultPath, filters });
    if (written) toast.success('Saved', written);
  } catch (e) {
    toast.error('Could not save the file', errorMessage(e));
  }
}

function PublicKeyBlock({ publicKey, fingerprint, keyType, comment, badges }: { publicKey: string; fingerprint: string; keyType: string; comment?: string; badges?: ReactNode }) {
  return (
    <div className="keygen-result">
      <div className="keygen-result__head">
        <KeyRound size={16} />
        <span className="mono-text">{keyType}</span>
        {badges}
        <span className="keygen-result__spacer" />
        <CopyButton text={publicKey} label="Copy public key" withText />
      </div>
      <FormField label="Public key for pasting into authorized_keys">
        <TextArea className="mono-input keygen-result__pub" value={publicKey} onChange={() => undefined} readOnly rows={3} onFocus={(e) => e.currentTarget.select()} />
      </FormField>
      <dl className="keygen-result__facts">
        <dt>Fingerprint</dt>
        <dd className="mono-text">{fingerprint}</dd>
        {comment !== undefined && (
          <>
            <dt>Comment</dt>
            <dd>{comment || <span className="muted">(none)</span>}</dd>
          </>
        )}
      </dl>
    </div>
  );
}

/* -------------------------------------------------------------------------- */

function GeneratePane() {
  const [type, setType] = useState<KeyType>('ed25519');
  const [bits, setBits] = useState<number>(keyBitsFor('rsa'));
  const [comment, setComment] = useState(() => defaultKeyComment('ed25519'));
  const [commentEdited, setCommentEdited] = useState(false);
  const [pass, setPass] = useState('');
  const [pass2, setPass2] = useState('');
  const [busy, setBusy] = useState(false);
  const [result, setResult] = useState<(KeygenResult & { type: KeyType; bits?: number; comment: string }) | null>(null);
  const [error, setError] = useState<string | null>(null);
  const passId = useId();

  const mismatch = pass2.length > 0 && pass !== pass2;
  const effBits = type === 'ed25519' ? undefined : KEY_BITS[type].includes(bits) ? bits : keyBitsFor(type);

  const changeType = (t: KeyType) => {
    setType(t);
    if (t !== 'ed25519') setBits(keyBitsFor(t));
    if (!commentEdited) setComment(defaultKeyComment(t));
  };

  const generate = async () => {
    if (pass !== pass2) return;
    setBusy(true);
    setError(null);
    setResult(null);
    try {
      const r = await api.keys.generate({ type, bits: effBits, comment: comment.trim(), passphrase: pass });
      setResult({ ...r, type, bits: effBits, comment: comment.trim() });
    } catch (e) {
      setError(errorMessage(e));
    } finally {
      setBusy(false);
    }
  };

  const base = result ? defaultKeyFileName(result.type) : '';
  return (
    <>
      <Section title="Key parameters">
        <FieldGrid cols={2}>
          <FormField label="Key type">
            <SegmentedControl value={type} options={TYPE_OPTIONS} onChange={changeType} ariaLabel="Key type" fill />
          </FormField>
          <FormField label={type === 'rsa' ? 'Key size (bits)' : type === 'ecdsa' ? 'Curve' : 'Key size'}>
            {type === 'ed25519' ? (
              <div className="keygen-fixed">256 bits (fixed)</div>
            ) : (
              <SegmentedControl
                value={effBits ?? 0}
                options={KEY_BITS[type].map((b) => ({ value: b, label: type === 'ecdsa' ? `P-${b}` : String(b) }))}
                onChange={setBits}
                ariaLabel="Key size"
                fill
              />
            )}
          </FormField>
          <FormField label="Comment" span="all" hint="Shown by agents and appended to the public key">
            <TextInput
              value={comment}
              onChange={(v) => {
                setComment(v);
                setCommentEdited(true);
              }}
            />
          </FormField>
          <FormField label="Passphrase" htmlFor={passId}>
            <TextInput id={passId} type="password" autoComplete="new-password" value={pass} onChange={setPass} placeholder="(none)" />
          </FormField>
          <FormField label="Confirm passphrase" error={mismatch ? 'Passphrases do not match' : undefined}>
            <TextInput type="password" autoComplete="new-password" value={pass2} onChange={setPass2} invalid={mismatch} placeholder="(none)" />
          </FormField>
        </FieldGrid>
        {!pass && <Callout tone="warning">Without a passphrase, anyone who gets hold of the private key file can use it.</Callout>}
        <div className="keygen-actions">
          <Button variant="primary" icon={busy ? undefined : <Sparkles size={14} />} onClick={() => void generate()} disabled={busy || mismatch || (pass !== '' && pass2 === '')}>
            {busy ? 'Generating…' : result ? 'Generate another' : 'Generate'}
          </Button>
          {busy && <Spinner size={14} label={type === 'rsa' ? 'Finding primes, this can take a few seconds…' : 'Generating…'} />}
          {pass !== '' && pass2 === '' && !busy && <span className="muted">Confirm the passphrase to continue.</span>}
        </div>
        {error && <Callout tone="danger" title="Key generation failed">{error}</Callout>}
      </Section>

      {result && (
        <Section title="Your new key">
          <PublicKeyBlock
            publicKey={result.publicKeyOpenSsh}
            fingerprint={result.fingerprint}
            keyType={result.publicKeyOpenSsh.split(' ')[0]}
            badges={result.bits ? <Badge>{result.type === 'ecdsa' ? `P-${result.bits}` : `${result.bits} bits`}</Badge> : undefined}
          />
          <div className="keygen-save">
            <Button icon={<Save size={14} />} onClick={() => void saveText(result.privateKeyOpenSsh, 'Save OpenSSH private key', base, PRIVATE_FILTERS.openssh)}>
              Save private key (OpenSSH)
            </Button>
            <Button icon={<Save size={14} />} onClick={() => void saveText(result.privateKeyPpk, 'Save PuTTY private key', `${base}.ppk`, PRIVATE_FILTERS.ppk)}>
              Save PuTTY .ppk
            </Button>
            <Button icon={<FileKey2 size={14} />} onClick={() => void saveText(result.publicKeyOpenSsh + '\n', 'Save public key', `${base}.pub`, PRIVATE_FILTERS.pub)}>
              Save public key
            </Button>
          </div>
        </Section>
      )}
    </>
  );
}

/* -------------------------------------------------------------------------- */

function LoadPane() {
  const [path, setPath] = useState('');
  const [passphrase, setPassphrase] = useState('');
  const [needPass, setNeedPass] = useState(false);
  const [busy, setBusy] = useState<false | 'load' | 'openssh' | 'ppk'>(false);
  const [info, setInfo] = useState<KeyInfo | null>(null);
  const [error, setError] = useState<string | null>(null);

  const load = async (p = path) => {
    if (!p.trim()) return;
    setBusy('load');
    setError(null);
    setInfo(null);
    try {
      const i = await api.keys.inspect(p.trim(), passphrase || undefined);
      setInfo(i);
      if (i.encrypted) setNeedPass(true);
    } catch (e) {
      const msg = errorMessage(e);
      if (isPassphraseError(msg)) setNeedPass(true);
      setError(msg);
    } finally {
      setBusy(false);
    }
  };

  const convert = async (to: 'openssh' | 'ppk') => {
    if (!info) return;
    setBusy(to);
    setError(null);
    try {
      const content = await api.keys.convert(info.path, passphrase, to);
      const stem = (info.path.split(/[\\/]/).pop() ?? 'key').replace(/\.(ppk|pem|key)$/i, '');
      await saveText(
        content,
        to === 'ppk' ? 'Save PuTTY private key' : 'Save OpenSSH private key',
        to === 'ppk' ? `${stem}.ppk` : stem,
        to === 'ppk' ? PRIVATE_FILTERS.ppk : PRIVATE_FILTERS.openssh
      );
    } catch (e) {
      const msg = errorMessage(e);
      if (isPassphraseError(msg)) setNeedPass(true);
      setError(msg);
    } finally {
      setBusy(false);
    }
  };

  return (
    <>
      <Section title="Key file" description="PuTTY .ppk (v2 / v3), OpenSSH or PEM private key.">
        <PathInput
          value={path}
          placeholder="C:\\Users\\you\\.ssh\\id_ed25519"
          onChange={(v) => {
            setPath(v);
            setInfo(null);
            setError(null);
          }}
          onBrowse={async () => {
            const p = await api.dialog.openFile({ title: 'Load private key', filters: LOAD_FILTERS });
            if (p) void load(p);
            return p;
          }}
          extra={
            <Button variant="primary" onClick={() => void load()} disabled={!path.trim() || !!busy}>
              {busy === 'load' ? 'Loading…' : 'Load'}
            </Button>
          }
        />
        {needPass && (
          <FormField label="Passphrase" hint={info ? 'Needed to convert the private key' : 'This key is encrypted'}>
            <TextInput
              type="password"
              autoComplete="off"
              value={passphrase}
              onChange={setPassphrase}
              onKeyDown={(e) => {
                if (e.key === 'Enter' && !info) void load();
              }}
            />
          </FormField>
        )}
        {error && <Callout tone="danger" title={isPassphraseError(error) ? 'Passphrase required' : 'Cannot use this key'}>{error}</Callout>}
      </Section>

      {info && (
        <Section title="Key details">
          <PublicKeyBlock
            publicKey={info.publicKeyOpenSsh}
            fingerprint={info.fingerprint}
            keyType={info.keyType}
            comment={info.comment}
            badges={
              <>
                <Badge>{FORMAT_LABELS[info.format]}</Badge>
                {info.encrypted ? <Badge tone="success">Passphrase protected</Badge> : <Badge tone="warning">No passphrase</Badge>}
              </>
            }
          />
          <div className="keygen-save">
            <Button icon={<Save size={14} />} onClick={() => void convert('ppk')} disabled={!!busy || (info.encrypted && !passphrase)}>
              {busy === 'ppk' ? 'Converting…' : 'Convert to .ppk…'}
            </Button>
            <Button icon={<Save size={14} />} onClick={() => void convert('openssh')} disabled={!!busy || (info.encrypted && !passphrase)}>
              {busy === 'openssh' ? 'Converting…' : 'Convert to OpenSSH…'}
            </Button>
            <Button
              icon={<FileKey2 size={14} />}
              onClick={() =>
                void saveText(info.publicKeyOpenSsh + '\n', 'Save public key', `${(info.path.split(/[\\/]/).pop() ?? 'key').replace(/\.(ppk|pem|key)$/i, '')}.pub`, PRIVATE_FILTERS.pub)
              }
            >
              Save public key
            </Button>
          </div>
          {info.encrypted && !passphrase && <div className="muted keygen-hint">Enter the passphrase above to convert this key.</div>}
        </Section>
      )}
    </>
  );
}

/* -------------------------------------------------------------------------- */

export function KeygenDialog({ onClose }: { onClose: () => void }) {
  const [mode, setMode] = useState<KeygenMode>('generate');
  const close = useStableCallback(onClose);
  // Keep both panes mounted so switching tabs does not lose a generated key.
  const panes = useMemo(
    () => ({
      generate: <GeneratePane />,
      load: <LoadPane />
    }),
    []
  );
  return (
    <Modal
      open
      title="Key generator"
      icon={<KeyRound size={18} className="dlg-title-icon" />}
      onClose={close}
      dismissable={false}
      width={680}
      height="min(720px, calc(100vh - 48px))"
      footer={
        <Button variant="primary" onClick={close}>
          Done
        </Button>
      }
    >
      <div className="keygen">
        <SegmentedControl value={mode} options={MODE_OPTIONS} onChange={setMode} ariaLabel="Mode" fill />
        <div hidden={mode !== 'generate'}>{panes.generate}</div>
        <div hidden={mode !== 'load'}>{panes.load}</div>
      </div>
    </Modal>
  );
}
