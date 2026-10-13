/**
 * SSH host key verification (global prompt kind 'hostkey').
 *  - unknown : no stored key for host:port -> Accept & Save / Connect Once / Cancel
 *  - mismatch: stored key differs (possible MITM) -> Cancel (default) / Update key & connect / Connect once
 * Responds via usePrompts.respond with choice 'accept-save' | 'accept-once' | 'reject'.
 */
import { useState } from 'react';
import { ShieldAlert, ShieldQuestion } from 'lucide-react';
import { Button, Checkbox, Modal } from '@/components/ui';
import type { PromptRequest } from '@shared/types';
import { Callout, CopyButton } from './common/controls';
import { useInitialFocus, useStableCallback } from './common/hooks';

export type HostKeyPrompt = Extract<PromptRequest, { kind: 'hostkey' }>;
export type HostKeyChoice = 'accept-save' | 'accept-once' | 'reject';

export interface HostKeyDialogProps {
  prompt: HostKeyPrompt;
  onChoice: (choice: HostKeyChoice) => void;
}

function hostLabel(host: string, port: number): string {
  const h = host.includes(':') && !host.startsWith('[') ? `[${host}]` : host;
  return port && port !== 22 ? `${h}:${port}` : h;
}

function Fingerprint({ label, value }: { label: string; value: string }) {
  if (!value) return null;
  return (
    <div className="hostkey__fp">
      <span className="hostkey__fp-label">{label}</span>
      <code className="hostkey__fp-value">{value}</code>
      <CopyButton text={value} label={`Copy ${label} fingerprint`} />
    </div>
  );
}

export function HostKeyDialog({ prompt, onChoice }: HostKeyDialogProps) {
  const mismatch = prompt.status === 'mismatch';
  const [confirmed, setConfirmed] = useState(false);
  const reject = useStableCallback(() => onChoice('reject'));
  const cancelRef = useInitialFocus<HTMLButtonElement>(mismatch);
  const target = hostLabel(prompt.host, prompt.port);

  const keyBlock = (
    <div className="hostkey__key">
      <div className="hostkey__meta">
        <span>
          <span className="muted">Host</span> <strong className="mono-text">{target}</strong>
        </span>
        <span>
          <span className="muted">Key type</span> <strong className="mono-text">{prompt.keyType}</strong>
        </span>
      </div>
      <Fingerprint label="SHA256" value={prompt.fingerprint.replace(/^SHA256:/, '')} />
      <Fingerprint label="MD5" value={prompt.fingerprintMd5.replace(/^MD5:/, '')} />
    </div>
  );

  if (!mismatch) {
    return (
      <Modal
        open
        dismissable={false}
        title="Unknown host key"
        icon={<ShieldQuestion size={20} className="dlg-title-icon dlg-title-icon--warning" />}
        onClose={reject}
        width={580}
        footer={
          <>
            <Button onClick={reject}>Cancel</Button>
            <Button onClick={() => onChoice('accept-once')}>Connect Once</Button>
            <Button variant="primary" onClick={() => onChoice('accept-save')}>
              Accept &amp; Save
            </Button>
          </>
        }
      >
        <div className="hostkey">
          <p>
            There is no stored host key for <strong className="mono-text">{target}</strong>, so there is no guarantee that this server is
            the computer you think it is.
          </p>
          {keyBlock}
          <p className="hostkey__help">
            If you trust this host, choose <strong>Accept &amp; Save</strong> to remember its key and continue. <strong>Connect Once</strong>{' '}
            continues without remembering the key. If you do not trust this host, choose <strong>Cancel</strong> to abandon the
            connection.
          </p>
        </div>
      </Modal>
    );
  }

  return (
    <Modal
      open
      dismissable={false}
      className="hostkey-modal--danger"
      title="Host key changed: potential security breach"
      icon={<ShieldAlert size={20} className="dlg-title-icon dlg-title-icon--danger" />}
      onClose={reject}
      width={620}
      footer={
        <>
          <Button variant="ghost" className="dlg-footer__left" onClick={() => onChoice('accept-once')}>
            Connect once
          </Button>
          <Button variant="danger" disabled={!confirmed} onClick={() => onChoice('accept-save')}>
            Update key &amp; connect
          </Button>
          <Button ref={cancelRef} variant="primary" onClick={reject}>
            Cancel
          </Button>
        </>
      }
    >
      <div className="hostkey">
        <Callout tone="danger" title="The server's host key does not match the key stored for it">
          Either the server administrator has replaced the host key, or you are connecting to a different computer that is pretending
          to be the server (a man-in-the-middle attack), which may be trying to capture your password or the data you send.
        </Callout>
        <div className="hostkey__subhead">New key presented by the server</div>
        {keyBlock}
        {prompt.previous.length > 0 && (
          <>
            <div className="hostkey__subhead">Previously stored {prompt.previous.length === 1 ? 'key' : 'keys'}</div>
            <ul className="hostkey__previous">
              {prompt.previous.map((fp) => (
                <li key={fp}>
                  <code>{fp}</code>
                </li>
              ))}
            </ul>
          </>
        )}
        <p className="hostkey__help">
          If you were expecting this change, verify the new fingerprint with the server administrator through a trusted channel before
          updating the stored key. If you were not expecting it, cancel the connection. <strong>Connect once</strong> carries on without
          changing the stored key.
        </p>
        <Checkbox
          className="hostkey__confirm"
          checked={confirmed}
          onChange={setConfirmed}
          label="I have verified the new fingerprint and want to replace the stored key"
        />
      </div>
    </Modal>
  );
}
