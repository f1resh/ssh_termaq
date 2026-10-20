import { describe, expect, it } from 'vitest';
import { createDefaultSession } from '@shared/defaults';
import { applyQuickConnect, formatQuickConnect, parseQuickConnect, type QuickConnectTarget } from './parse';

const ok = (input: string, defaultProtocol?: Parameters<typeof parseQuickConnect>[1]): QuickConnectTarget => {
  const r = parseQuickConnect(input, defaultProtocol);
  if (!r.ok) throw new Error(`expected ok for "${input}", got: ${r.error}`);
  return r.target;
};
const err = (input: string): string => {
  const r = parseQuickConnect(input);
  if (r.ok) throw new Error(`expected error for "${input}"`);
  return r.error;
};

describe('parseQuickConnect', () => {
  it('parses a bare host with the default protocol', () => {
    expect(ok('web-01')).toEqual({ protocol: 'ssh', host: 'web-01' });
    expect(ok('  10.0.0.5  ')).toEqual({ protocol: 'ssh', host: '10.0.0.5' });
    expect(ok('router', 'telnet')).toEqual({ protocol: 'telnet', host: 'router' });
  });

  it('parses user@host:port', () => {
    expect(ok('deploy@web-01:2222')).toEqual({ protocol: 'ssh', host: 'web-01', port: 2222, username: 'deploy' });
    expect(ok('root@10.1.2.3')).toEqual({ protocol: 'ssh', host: '10.1.2.3', username: 'root' });
    expect(ok('host:22')).toEqual({ protocol: 'ssh', host: 'host', port: 22 });
  });

  it('uses the last @ to split user and host', () => {
    expect(ok('me@corp.com@jump')).toEqual({ protocol: 'ssh', host: 'jump', username: 'me@corp.com' });
  });

  it('parses URL forms and ignores passwords and paths', () => {
    expect(ok('ssh://root:secret@h.example:2200/')).toEqual({ protocol: 'ssh', host: 'h.example', port: 2200, username: 'root' });
    expect(ok('telnet://192.168.1.1')).toEqual({ protocol: 'telnet', host: '192.168.1.1' });
    expect(ok('rlogin://bob@old-box')).toEqual({ protocol: 'rlogin', host: 'old-box', username: 'bob' });
    expect(ok('raw://127.0.0.1:7000')).toEqual({ protocol: 'raw', host: '127.0.0.1', port: 7000 });
    expect(ok('SSH://Host')).toEqual({ protocol: 'ssh', host: 'Host' });
  });

  it('parses IPv6 addresses', () => {
    expect(ok('[::1]:2222')).toEqual({ protocol: 'ssh', host: '::1', port: 2222 });
    expect(ok('[fe80::1]')).toEqual({ protocol: 'ssh', host: 'fe80::1' });
    expect(ok('::1')).toEqual({ protocol: 'ssh', host: '::1' });
    expect(ok('admin@[2001:db8::5]:22')).toEqual({ protocol: 'ssh', host: '2001:db8::5', port: 22, username: 'admin' });
    expect(ok('ssh://[::1]:2022')).toEqual({ protocol: 'ssh', host: '::1', port: 2022 });
  });

  it('parses serial targets', () => {
    expect(ok('serial://COM3?baud=115200')).toEqual({ protocol: 'serial', host: 'COM3', serial: { baudRate: 115200 } });
    expect(ok('serial://com4:9600')).toEqual({ protocol: 'serial', host: 'COM4', serial: { baudRate: 9600 } });
    expect(ok('serial://COM1?baud=57600&data=7&parity=e&stop=2&flow=rtscts')).toEqual({
      protocol: 'serial',
      host: 'COM1',
      serial: { baudRate: 57600, dataBits: 7, parity: 'even', stopBits: 2, flowControl: 'rtscts' }
    });
    expect(ok('COM3')).toEqual({ protocol: 'serial', host: 'COM3', serial: {} });
    expect(ok('serial COM5 38400')).toEqual({ protocol: 'serial', host: 'COM5', serial: { baudRate: 38400 } });
    expect(ok('-serial COM2')).toEqual({ protocol: 'serial', host: 'COM2', serial: {} });
    expect(ok('/dev/ttyUSB0', 'serial')).toEqual({ protocol: 'serial', host: '/dev/ttyUSB0', serial: {} });
  });

  it('parses "protocol host port" style', () => {
    expect(ok('telnet host 2323')).toEqual({ protocol: 'telnet', host: 'host', port: 2323 });
    expect(ok('ssh deploy@web-01 2222')).toEqual({ protocol: 'ssh', host: 'web-01', port: 2222, username: 'deploy' });
    expect(ok('raw 10.0.0.1 9100')).toEqual({ protocol: 'raw', host: '10.0.0.1', port: 9100 });
    expect(ok('host 2200')).toEqual({ protocol: 'ssh', host: 'host', port: 2200 });
  });

  it('understands PuTTY/OpenSSH style flags', () => {
    expect(ok('-telnet -P 2323 box')).toEqual({ protocol: 'telnet', host: 'box', port: 2323 });
    expect(ok('box -p 2200 -l admin')).toEqual({ protocol: 'ssh', host: 'box', port: 2200, username: 'admin' });
  });

  it('rejects invalid input with a message', () => {
    expect(err('')).toMatch(/host/i);
    expect(err('host:99999')).toMatch(/port/i);
    expect(err('host:abc')).toMatch(/port/i);
    expect(err('gopher://x')).toMatch(/protocol/i);
    expect(err('[::1')).toMatch(/\]/);
    expect(err('raw://host')).toMatch(/port/i);
    expect(err('a b c')).toMatch(/Unexpected/);
    expect(err('host -x')).toMatch(/option/i);
    expect(err('serial://COM3?baud=fast')).toMatch(/baud/i);
    expect(err('serial://COM3?parity=z')).toMatch(/parity/i);
    expect(err('user@')).toMatch(/host/i);
    expect(err('host:22 23')).toMatch(/twice/);
  });
});

describe('applyQuickConnect', () => {
  const base = createDefaultSession({ username: 'template-user', port: 22 });

  it('fills host/port/user and keeps template defaults', () => {
    const s = applyQuickConnect(base, ok('telnet://router'));
    expect(s.protocol).toBe('telnet');
    expect(s.host).toBe('router');
    expect(s.port).toBe(23);
    expect(s.username).toBe('template-user');
    expect(applyQuickConnect(base, ok('me@h:2200'))).toMatchObject({ host: 'h', port: 2200, username: 'me' });
  });

  it('configures serial lines', () => {
    const s = applyQuickConnect(base, ok('serial://COM7?baud=9600'));
    expect(s.protocol).toBe('serial');
    expect(s.serial.line).toBe('COM7');
    expect(s.serial.baudRate).toBe(9600);
    expect(s.serial.dataBits).toBe(base.serial.dataBits);
  });

  it('does not mutate the base session', () => {
    applyQuickConnect(base, ok('x@y:1'));
    expect(base.host).toBe('');
  });
});

describe('formatQuickConnect', () => {
  it('round-trips', () => {
    for (const s of ['deploy@web-01:2222', 'telnet://router', '[::1]:2222', 'serial://COM3?baud=115200', 'raw://h:7000']) {
      expect(formatQuickConnect(ok(s))).toBe(s);
    }
  });
});

describe('file manager URLs', () => {
  it('parses sftp:// and scp:// with a folder', () => {
    const t = ok('sftp://deploy@files.example.test:2200/var/www/my%20site');
    expect(t.protocol).toBe('ssh');
    expect(t.host).toBe('files.example.test');
    expect(t.port).toBe(2200);
    expect(t.username).toBe('deploy');
    expect(t.files).toEqual({ protocol: 'sftp', path: '/var/www/my site' });
    expect(ok('scp://router').files).toEqual({ protocol: 'scp', path: undefined });
  });

  it('sets the file protocol on the session', () => {
    const s = applyQuickConnect(createDefaultSession(), ok('scp://root@nas'));
    expect(s.protocol).toBe('ssh');
    expect(s.ssh.fileProtocol).toBe('scp');
    expect(s.username).toBe('root');
  });

  it('round-trips', () => {
    for (const s of ['sftp://deploy@web-01:2222/srv', 'scp://router']) expect(formatQuickConnect(ok(s))).toBe(s);
  });
});
