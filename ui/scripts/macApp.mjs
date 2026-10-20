/**
 * The macOS side of app.mjs: SSHTermaq.app (a Debug build) started with --automation <socket> and driven over that
 * Unix socket (native/src/platform/mac/ui/AutomationMac.h) - WKWebView has no CDP for Playwright.
 *
 * `page` offers the part of Playwright's Page that steps use: evaluate, waitForSelector, waitForTimeout, click,
 * keyboard.type / keyboard.press (real AppKit key presses: Command keys reach the menu bar), screenshot, and
 * on('console' | 'pageerror'). Selectors are CSS selectors. macOS only: sheet({ check, press }) reads (and answers) a
 * native sheet on the window - an alert or a file panel - as { texts, buttons } or null.
 */
import { spawn } from 'node:child_process';
import { connect } from 'node:net';

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

/** One JSON object per line each way; answers matched by id. */
function openConnection(socketPath, onEvent) {
  return new Promise((resolve, reject) => {
    const socket = connect(socketPath);
    const pending = new Map();
    let nextId = 1;
    let buffer = '';
    socket.setEncoding('utf8');
    socket.on('data', (chunk) => {
      buffer += chunk;
      let nl;
      while ((nl = buffer.indexOf('\n')) >= 0) {
        const msg = JSON.parse(buffer.slice(0, nl));
        buffer = buffer.slice(nl + 1);
        if (msg.event) {
          onEvent(msg);
          continue;
        }
        const p = pending.get(msg.id);
        if (!p) continue;
        pending.delete(msg.id);
        if (msg.ok) p.resolve(msg.result);
        else p.reject(new Error(msg.error));
      }
    });
    socket.on('close', () => {
      for (const p of pending.values()) p.reject(new Error('the app closed the automation socket'));
      pending.clear();
    });
    socket.once('error', reject);
    socket.once('connect', () =>
      resolve({
        call(request) {
          const id = nextId++;
          return new Promise((res, rej) => {
            pending.set(id, { resolve: res, reject: rej });
            socket.write(JSON.stringify({ id, ...request }) + '\n');
          });
        },
        end: () => socket.end()
      })
    );
  });
}

class MacPage {
  constructor() {
    this.listeners = { console: [], pageerror: [] };
    this.keyboard = {
      type: (text) => this.conn.call({ cmd: 'type', text }),
      press: (key) => this.conn.call({ cmd: 'press', key })
    };
  }

  /** Console messages as Playwright's ConsoleMessage (type(), text()); errors as Error. */
  dispatch(event) {
    if (event.type === 'pageerror') for (const cb of this.listeners.pageerror) cb(new Error(event.text));
    else for (const cb of this.listeners.console) cb({ type: () => event.type, text: () => event.text });
  }

  on(event, cb) {
    (this.listeners[event] ??= []).push(cb);
  }

  /** fn(arg) in the page, as Playwright's page.evaluate (arg and result go through JSON). */
  evaluate(fn, arg) {
    const source = typeof fn === 'function' ? fn.toString() : `() => (${fn})`;
    return this.conn.call({ cmd: 'eval', fn: source, arg: JSON.stringify(arg === undefined ? null : arg) });
  }

  waitForTimeout(ms) {
    return sleep(ms);
  }

  /** Waits until `selector` matches an element; state 'visible' (default) also needs it to have a size. */
  async waitForSelector(selector, { timeout = 30000, state = 'visible' } = {}) {
    const deadline = Date.now() + timeout;
    for (;;) {
      const found = await this.evaluate(
        ([s, visible]) => {
          const el = document.querySelector(s);
          if (!el) return false;
          if (!visible) return true;
          const r = el.getBoundingClientRect();
          return r.width > 0 && r.height > 0;
        },
        [selector, state === 'visible']
      );
      if (found) return;
      if (Date.now() > deadline) throw new Error(`waitForSelector timed out: ${selector}`);
      await sleep(100);
    }
  }

  /** A real click in the middle of the element. */
  async click(selector) {
    await this.waitForSelector(selector);
    const at = await this.evaluate((s) => {
      const el = document.querySelector(s);
      el.scrollIntoView({ block: 'nearest', inline: 'nearest' });
      const r = el.getBoundingClientRect();
      return { x: r.left + r.width / 2, y: r.top + r.height / 2 };
    }, selector);
    await this.conn.call({ cmd: 'click', x: at.x, y: at.y });
  }

  async screenshot({ path }) {
    await this.conn.call({ cmd: 'screenshot', path });
  }

  sheet({ check, press } = {}) {
    return this.conn.call({ cmd: 'sheet', check, press });
  }
}

/**
 * Starts the app and connects. Returns { page, bridge, close }. bridge(ch, ...args) is a raw bridge invoke from the
 * page, as on Windows.
 */
export async function openMacApp({ exe, dataDir, extra, socketPath, onExit }) {
  const child = spawn(exe, ['--data-dir', dataDir, '--automation', socketPath, ...extra], { stdio: 'ignore' });
  let exited = false;
  child.on('exit', (code) => {
    exited = true;
    onExit?.(code);
  });
  const page = new MacPage();
  for (let i = 0; i < 60 && !page.conn; i++) {
    if (exited) throw new Error('app exited before its automation socket was available (a Debug build is needed)');
    try {
      page.conn = await openConnection(socketPath, (e) => page.dispatch(e));
    } catch {
      await sleep(500);
    }
  }
  if (!page.conn) throw new Error('could not connect to the automation socket');

  const bridge = (ch, ...a) =>
    page.evaluate(
      ([ch, a]) =>
        new Promise((resolve, reject) => {
          // Answers arrive in the batches window.__stReceive gets (src/bridge/api.ts); watch them once.
          if (!window.__stAutoWaiters) {
            window.__stAutoWaiters = new Map();
            const original = window.__stReceive;
            window.__stReceive = (batch) => {
              original(batch);
              for (const m of JSON.parse(batch)) {
                const waiter = m && m.t === 'res' && window.__stAutoWaiters.get(m.id);
                if (waiter) {
                  window.__stAutoWaiters.delete(m.id);
                  waiter(m);
                }
              }
            };
          }
          const id = 1e9 + Math.floor(Math.random() * 1e6);
          window.__stAutoWaiters.set(id, (m) => (m.ok ? resolve(m.result) : reject(new Error(m.error))));
          window.webkit.messageHandlers.st.postMessage(JSON.stringify({ t: 'req', id, ch, args: a }));
        }),
      [ch, a]
    );

  return {
    page,
    bridge,
    async close() {
      page.conn.end();
      if (!exited) child.kill();
    }
  };
}
