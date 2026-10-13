/**
 * Pure helpers for the key generator dialog (unit tested).
 */
import type { KeyType } from '@shared/types';

export type KeygenMode = 'generate' | 'load';

/** Allowed sizes: RSA modulus bits / ECDSA curve sizes. */
export const KEY_BITS: Record<Exclude<KeyType, 'ed25519'>, number[]> = {
  rsa: [2048, 3072, 4096],
  ecdsa: [256, 384, 521]
};

/** Default size for a key type (RSA 3072 like current PuTTYgen / OpenSSH, ECDSA P-256). */
export function keyBitsFor(type: KeyType): number {
  return type === 'rsa' ? 3072 : type === 'ecdsa' ? 256 : 256;
}

/** PuTTYgen-style default comment, e.g. "ed25519-key-20261013". */
export function defaultKeyComment(type: KeyType, date = new Date()): string {
  const pad = (n: number) => String(n).padStart(2, '0');
  return `${type}-key-${date.getFullYear()}${pad(date.getMonth() + 1)}${pad(date.getDate())}`;
}

/** OpenSSH conventional file name for a key type ("id_ed25519", "id_rsa", "id_ecdsa"). */
export function defaultKeyFileName(type: KeyType): string {
  return `id_${type}`;
}

/** Whether a host error message means a (correct) passphrase is needed. */
export function isPassphraseError(message: string): boolean {
  return /pass ?phrase|encrypted|decrypt/i.test(message);
}
