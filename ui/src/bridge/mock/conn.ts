/**
 * Mock connections: a tiny fake shell that echoes input, so terminal UI can be exercised in a browser.
 */
import type { ConnOpenRequest, EventLogEntry, SpecialCommandDef } from '@shared/types';
import { sessionLabel } from '@shared/defaults';
import type { MockContext } from './index';

interface MockConn {
  req: ConnOpenRequest;
  line: string;
  log: EventLogEntry[];
  closed: boolean;
}

const conns = new Map<string, MockConn>();

export function registerConnMocks(ctx: MockContext): void {
  const log = (c: MockConn, message: string) => {
    const e: EventLogEntry = { time: Date.now(), level: 'info', message };
    c.log.push(e);
    ctx.emit('conn:onEventLog', c.req.connId, e);
  };
  const out = (c: MockConn, s: string) => ctx.emit('conn:onData', c.req.connId, s);
  const prompt = () => '\x1b[1;32mmock\x1b[0m@\x1b[1;34m' + 'demo' + '\x1b[0m:~$ ';

  ctx.handle('conn:open', (req: ConnOpenRequest) => {
    const c: MockConn = { req, line: '', log: [], closed: false };
    conns.set(req.connId, c);
    const s = req.session;
    ctx.emit('conn:onStatus', { connId: req.connId, status: 'connecting', message: `Connecting to ${sessionLabel(s)}` });
    log(c, `Looking up host "${s.host}" (mock)`);
    setTimeout(() => {
      if (c.closed) return;
      ctx.emit('conn:onStatus', { connId: req.connId, status: 'authenticating' });
      log(c, 'Authenticating (mock)');
    }, 150);
    setTimeout(() => {
      if (c.closed) return;
      ctx.emit('conn:onStatus', {
        connId: req.connId,
        status: 'connected',
        serverInfo: 'SSH-2.0-MockSSH_1.0, chacha20-poly1305@openssh.com'
      });
      log(c, 'Connected (mock)');
      out(
        c,
        `Welcome to the \x1b[1mSSH Crawler\x1b[0m mock shell (${s.protocol} ${sessionLabel(s)}).\r\n` +
          `Type \x1b[33mhelp\x1b[0m, \x1b[33mcolors\x1b[0m, \x1b[33mexit\x1b[0m.\r\n\r\n` +
          prompt()
      );
    }, 400);
  });

  ctx.on('conn:write', (connId: string, data: string) => {
    const c = conns.get(connId);
    if (!c || c.closed) return;
    for (const ch of data) {
      if (ch === '\r') {
        const cmd = c.line.trim();
        c.line = '';
        out(c, '\r\n');
        if (cmd === 'exit') {
          c.closed = true;
          out(c, 'logout\r\n');
          ctx.emit('conn:onStatus', { connId, status: 'disconnected', clean: true, exitCode: 0, message: 'Session ended' });
          return;
        }
        if (cmd === 'help') out(c, 'Commands: help, colors, ls, date, exit\r\n');
        else if (cmd === 'colors') {
          let s = '';
          for (let i = 0; i < 16; i++) s += `\x1b[48;5;${i}m  ${String(i).padStart(2)}  \x1b[0m` + (i === 7 ? '\r\n' : '');
          out(c, s + '\r\n');
        } else if (cmd === 'ls') out(c, '\x1b[1;34mdocuments\x1b[0m  \x1b[1;34mprojects\x1b[0m  notes.txt  \x1b[1;32mrun.sh\x1b[0m\r\n');
        else if (cmd === 'date') out(c, new Date().toString() + '\r\n');
        else if (cmd) out(c, `${cmd}: command not found\r\n`);
        out(c, prompt());
      } else if (ch === '\x7f' || ch === '\b') {
        if (c.line.length) {
          c.line = c.line.slice(0, -1);
          out(c, '\b \b');
        }
      } else if (ch === '\x03') {
        c.line = '';
        out(c, '^C\r\n' + prompt());
      } else if (ch >= ' ') {
        c.line += ch;
        out(c, ch);
      }
    }
  });

  ctx.on('conn:resize', () => undefined);
  ctx.on('conn:close', (connId: string) => {
    const c = conns.get(connId);
    if (!c || c.closed) return;
    c.closed = true;
    ctx.emit('conn:onStatus', { connId, status: 'disconnected', clean: true, message: 'Connection closed by user' });
  });
  ctx.on('conn:special', (connId: string, id: string) => {
    const c = conns.get(connId);
    if (c) log(c, `Special command: ${id}`);
  });
  ctx.handle('conn:specials', (): SpecialCommandDef[] => [
    { id: 'break', label: 'Break' },
    { id: 'eof', label: 'EOF' },
    { id: 'ignore', label: 'IGNORE message' },
    { id: 'signal:INT', label: 'SIGINT', group: 'Signals' },
    { id: 'signal:TERM', label: 'SIGTERM', group: 'Signals' },
    { id: 'signal:KILL', label: 'SIGKILL', group: 'Signals' }
  ]);
  ctx.handle('conn:eventLog', (connId: string) => conns.get(connId)?.log ?? []);
  ctx.on('prompts:respond', () => undefined);
}
