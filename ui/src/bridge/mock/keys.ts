/**
 * Mock handlers for the `keys` namespace (owner: ui-dialogs): fake key generation, inspection and conversion
 * so the keygen dialog and the session editor's key info work in the browser preview.
 *
 * Inspection is driven by the file name: `*.ppk` -> PPK v3 (v2 when the name contains "v2"), `*.pem` -> PEM,
 * anything else -> OpenSSH. Names containing "enc" are passphrase protected (old-style encrypted PEM needs the
 * passphrase even to read the public key; the mock passphrase is anything except "wrong"), names containing
 * "missing" fail as if the file did not exist.
 */
import type { KeyInfo, KeygenRequest, KeygenResult } from '@shared/types';
import type { MockContext } from './index';

const B64 = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';

function randomB64(len: number): string {
  const bytes = new Uint8Array(len);
  globalThis.crypto.getRandomValues(bytes);
  let out = '';
  for (const b of bytes) out += B64[b & 63];
  return out;
}

/** Deterministic pseudo base64 derived from a string (stable fingerprints per mock file). */
function hashB64(seed: string, len: number): string {
  let h = 2166136261;
  let out = '';
  for (let i = 0; out.length < len; i++) {
    h ^= seed.charCodeAt(i % Math.max(1, seed.length)) + i;
    h = Math.imul(h, 16777619) >>> 0;
    out += B64[h & 63];
  }
  return out;
}

function keyTypeName(type: KeygenRequest['type'], bits?: number): string {
  if (type === 'ed25519') return 'ssh-ed25519';
  if (type === 'rsa') return 'ssh-rsa';
  return `ecdsa-sha2-nistp${bits ?? 256}`;
}

function blobLength(type: KeygenRequest['type'], bits?: number): number {
  if (type === 'ed25519') return 68;
  if (type === 'rsa') return Math.round(((bits ?? 3072) / 8) * 1.37) + 28;
  return bits === 521 ? 172 : bits === 384 ? 136 : 104;
}

function wrap(s: string, width = 64): string {
  return s.replace(new RegExp(`(.{${width}})`, 'g'), '$1\n').trimEnd();
}

const delay = (ms: number) => new Promise((r) => setTimeout(r, ms));

function fileName(path: string): string {
  return path.split(/[\\/]/).pop() ?? path;
}

function inspectMock(path: string, passphrase?: string): KeyInfo {
  const name = fileName(path).toLowerCase();
  if (!path.trim() || name.includes('missing')) throw new Error(`Cannot open "${path}": the system cannot find the file specified.`);
  const ppk = name.endsWith('.ppk');
  const pem = name.endsWith('.pem');
  const encrypted = name.includes('enc');
  if (pem && encrypted) {
    if (!passphrase) throw new Error('This key is encrypted; a passphrase is required to read it.');
    if (passphrase === 'wrong') throw new Error('Wrong passphrase.');
  }
  const keyType = name.includes('rsa') ? 'ssh-rsa' : name.includes('ecdsa') ? 'ecdsa-sha2-nistp256' : 'ssh-ed25519';
  const blob = `AAAA${hashB64(path + keyType, keyType === 'ssh-rsa' ? 360 : keyType === 'ssh-ed25519' ? 64 : 100)}`;
  return {
    path,
    format: ppk ? (name.includes('v2') ? 'ppk2' : 'ppk3') : pem ? 'pem' : 'openssh',
    keyType,
    comment: pem ? '' : `${name.replace(/\.(ppk|pem|key)$/, '')}@workstation`,
    encrypted,
    fingerprint: `SHA256:${hashB64(path, 43)}`,
    publicKeyOpenSsh: `${keyType} ${blob} ${pem ? '' : name.replace(/\.(ppk|key)$/, '') + '@workstation'}`.trim()
  };
}

export function registerKeysMocks(ctx: MockContext): void {
  ctx.handle('keys:generate', async (req: KeygenRequest): Promise<KeygenResult> => {
    await delay(req.type === 'rsa' ? 300 + (req.bits ?? 3072) / 6 : 250);
    const kt = keyTypeName(req.type, req.bits);
    const pub = `AAAA${randomB64(blobLength(req.type, req.bits))}`;
    const comment = req.comment || 'mock-key';
    const enc = req.passphrase ? 'aes256-ctr' : 'none';
    const priv = randomB64(req.type === 'rsa' ? 1600 : 380);
    return {
      publicKeyOpenSsh: `${kt} ${pub} ${comment}`,
      fingerprint: `SHA256:${randomB64(43)}`,
      privateKeyOpenSsh: `-----BEGIN OPENSSH PRIVATE KEY-----\n${wrap(`b3BlbnNzaC1rZXktdjEAAAAA${enc === 'none' ? 'BG5vbmU' : 'CmFlczI1Ni1jdHI'}${priv}`, 70)}\n-----END OPENSSH PRIVATE KEY-----\n`,
      privateKeyPpk: [
        `PuTTY-User-Key-File-3: ${kt}`,
        `Encryption: ${req.passphrase ? 'aes256-cbc' : 'none'}`,
        `Comment: ${comment}`,
        `Public-Lines: ${wrap(pub).split('\n').length}`,
        wrap(pub),
        ...(req.passphrase ? ['Key-Derivation: Argon2id', 'Argon2-Memory: 8192', 'Argon2-Passes: 13', 'Argon2-Parallelism: 1', `Argon2-Salt: ${hashB64(pub, 32)}`] : []),
        `Private-Lines: ${wrap(priv).split('\n').length}`,
        wrap(priv),
        `Private-MAC: ${hashB64(priv, 64)}`,
        ''
      ].join('\n')
    };
  });

  ctx.handle('keys:inspect', async (path: string, passphrase?: string) => {
    await delay(60);
    return inspectMock(path, passphrase ?? undefined);
  });

  ctx.handle('keys:convert', async (path: string, passphrase: string, to: 'openssh' | 'ppk') => {
    await delay(120);
    const info = inspectMock(path, passphrase);
    if (info.encrypted && !passphrase) throw new Error('This key is encrypted; enter its passphrase to convert it.');
    if (info.encrypted && passphrase === 'wrong') throw new Error('Wrong passphrase.');
    const [kt, blob, ...rest] = info.publicKeyOpenSsh.split(' ');
    const comment = rest.join(' ');
    const priv = hashB64(path + 'private', kt === 'ssh-rsa' ? 1600 : 380);
    if (to === 'openssh') {
      return `-----BEGIN OPENSSH PRIVATE KEY-----\n${wrap(`b3BlbnNzaC1rZXktdjEAAAAA${priv}`, 70)}\n-----END OPENSSH PRIVATE KEY-----\n`;
    }
    return [
      `PuTTY-User-Key-File-3: ${kt}`,
      `Encryption: ${info.encrypted ? 'aes256-cbc' : 'none'}`,
      `Comment: ${comment}`,
      `Public-Lines: ${wrap(blob).split('\n').length}`,
      wrap(blob),
      `Private-Lines: ${wrap(priv).split('\n').length}`,
      wrap(priv),
      `Private-MAC: ${hashB64(priv, 64)}`,
      ''
    ].join('\n');
  });
}
