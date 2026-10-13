/**
 * SSH algorithm names understood by ssh2 (client side), in a sensible default preference order.
 * Used by the session editor (reorderable lists) and by the PuTTY importer.
 * Availability at runtime depends on the crypto backend (Electron = BoringSSL), the SSH transport
 * filters these against `require('ssh2').utils` / supported lists before passing them on.
 */
export const SSH_KEX = [
  'curve25519-sha256',
  'curve25519-sha256@libssh.org',
  'ecdh-sha2-nistp256',
  'ecdh-sha2-nistp384',
  'ecdh-sha2-nistp521',
  'diffie-hellman-group-exchange-sha256',
  'diffie-hellman-group14-sha256',
  'diffie-hellman-group15-sha512',
  'diffie-hellman-group16-sha512',
  'diffie-hellman-group17-sha512',
  'diffie-hellman-group18-sha512',
  'diffie-hellman-group-exchange-sha1',
  'diffie-hellman-group14-sha1',
  'diffie-hellman-group1-sha1'
] as const;

export const SSH_CIPHERS = [
  'chacha20-poly1305@openssh.com',
  'aes128-gcm@openssh.com',
  'aes256-gcm@openssh.com',
  'aes128-ctr',
  'aes192-ctr',
  'aes256-ctr',
  'aes256-cbc',
  'aes192-cbc',
  'aes128-cbc',
  '3des-cbc',
  'blowfish-cbc',
  'cast128-cbc',
  'arcfour256',
  'arcfour128',
  'arcfour'
] as const;

export const SSH_HOSTKEYS = [
  'ssh-ed25519',
  'ecdsa-sha2-nistp256',
  'ecdsa-sha2-nistp384',
  'ecdsa-sha2-nistp521',
  'rsa-sha2-512',
  'rsa-sha2-256',
  'ssh-rsa',
  'ssh-dss'
] as const;

export const SSH_MACS = [
  'hmac-sha2-256-etm@openssh.com',
  'hmac-sha2-512-etm@openssh.com',
  'hmac-sha1-etm@openssh.com',
  'hmac-sha2-256',
  'hmac-sha2-512',
  'hmac-sha1',
  'hmac-md5',
  'hmac-sha2-256-96',
  'hmac-sha2-512-96',
  'hmac-ripemd160',
  'hmac-sha1-96',
  'hmac-md5-96'
] as const;

/** Algorithms considered weak; the editor shows them below a "warn below here" divider like PuTTY. */
export const WEAK_ALGORITHMS = new Set<string>([
  'diffie-hellman-group1-sha1',
  'diffie-hellman-group14-sha1',
  'diffie-hellman-group-exchange-sha1',
  '3des-cbc',
  'blowfish-cbc',
  'cast128-cbc',
  'arcfour256',
  'arcfour128',
  'arcfour',
  'ssh-dss',
  'hmac-md5',
  'hmac-md5-96',
  'hmac-sha1-96'
]);

/** Charsets offered in the Translation panel (iconv-lite names). */
export const CHARSETS = [
  'UTF-8',
  'ISO-8859-1',
  'ISO-8859-2',
  'ISO-8859-3',
  'ISO-8859-4',
  'ISO-8859-5',
  'ISO-8859-6',
  'ISO-8859-7',
  'ISO-8859-8',
  'ISO-8859-9',
  'ISO-8859-10',
  'ISO-8859-11',
  'ISO-8859-13',
  'ISO-8859-14',
  'ISO-8859-15',
  'ISO-8859-16',
  'windows-1250',
  'windows-1251',
  'windows-1252',
  'windows-1253',
  'windows-1254',
  'windows-1255',
  'windows-1256',
  'windows-1257',
  'windows-1258',
  'KOI8-R',
  'KOI8-U',
  'CP437',
  'CP850',
  'CP852',
  'CP866',
  'Shift_JIS',
  'EUC-JP',
  'GBK',
  'GB18030',
  'Big5',
  'EUC-KR',
  'macintosh'
] as const;

export const BAUD_RATES = [300, 1200, 2400, 4800, 9600, 19200, 38400, 57600, 115200, 230400, 460800, 921600] as const;
