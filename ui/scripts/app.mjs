#!/usr/bin/env node
/**
 * Launch the real SSHTermaq, optionally run a steps module, take a screenshot and exit (the app is killed afterwards).
 *   Windows: SSHTermaq.exe (WebView2) with remote debugging; Playwright attaches over CDP.
 *   macOS:   SSHTermaq.app (a Debug build) with --automation; ./macApp.mjs drives it over a Unix socket and offers
 *            the steps a Page with the common Playwright calls (evaluate, waitForSelector, click, keyboard, screenshot).
 *
 *   node scripts/app.mjs [--exe <path>] [--data-dir <dir>] [--steps file.mjs]
 *                        [--out ../artifacts/app.png] [--keep-data] [--args "..."]
 *
 * --exe defaults to the Debug build: ../build/vs/Debug/SSHTermaq.exe, or on macOS
 * ../build/mac-<arch>/Debug/SSHTermaq.app/Contents/MacOS/SSHTermaq.
 * A fresh temporary data directory is used unless --data-dir is given (the user's real data is never touched).
 * The steps module default export receives (page, { out, exe, dataDir, bridge, platform }) where bridge(ch, ...args)
 * performs a raw bridge invoke from the page and platform is process.platform.
 */
import { spawn } from 'node:child_process';
import { mkdtempSync, mkdirSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const args = process.argv.slice(2);
const opt = (n, d) => {
  const i = args.indexOf(`--${n}`);
  return i >= 0 && args[i + 1] !== undefined ? args[i + 1] : d;
};
const flag = (n) => args.includes(`--${n}`);

const isMac = process.platform === 'darwin';
const defaultExe = isMac
  ? join(here, `../../build/mac-${process.arch === 'x64' ? 'x64' : 'arm64'}/Debug/SSHTermaq.app/Contents/MacOS/SSHTermaq`)
  : join(here, '../../build/vs/Debug/SSHTermaq.exe');
const exe = resolve(process.cwd(), opt('exe', defaultExe));
const out = resolve(process.cwd(), opt('out', join(here, '../../artifacts/app.png')));
const steps = opt('steps');
const port = Number(opt('port', 9333));
const tempData = !opt('data-dir');
const dataDir = opt('data-dir') ? resolve(opt('data-dir')) : mkdtempSync(join(tmpdir(), 'sshtermaq-e2e-'));
const extra = (opt('args', '') || '').split(' ').filter(Boolean);
const onExit = (code) => console.log(`app exited with code ${code}`);

/** Windows: WebView2 with remote debugging, Playwright over CDP. */
async function openWindowsApp() {
  const { chromium } = await import('playwright-core');
  const child = spawn(exe, ['--data-dir', dataDir, ...extra], {
    env: { ...process.env, WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS: `--remote-debugging-port=${port}` },
    stdio: 'ignore'
  });
  let exited = false;
  child.on('exit', (code) => {
    exited = true;
    onExit(code);
  });

  let browser;
  for (let i = 0; i < 60 && !browser; i++) {
    if (exited) throw new Error('app exited before CDP was available');
    try {
      browser = await chromium.connectOverCDP(`http://127.0.0.1:${port}`);
    } catch {
      await new Promise((r) => setTimeout(r, 500));
    }
  }
  const close = async () => {
    try {
      await browser?.close();
    } catch {
      /* ignore */
    }
    if (!exited) child.kill();
  };
  if (!browser) {
    await close();
    throw new Error('could not connect over CDP');
  }
  let page;
  for (let i = 0; i < 40 && !page; i++) {
    page = browser.contexts().flatMap((c) => c.pages()).find((p) => p.url().startsWith('https://app.sshtermaq.local'));
    if (!page) await new Promise((r) => setTimeout(r, 250));
  }
  if (!page) {
    const urls = browser.contexts().flatMap((c) => c.pages()).map((p) => p.url());
    await close();
    throw new Error('app page not found: ' + urls);
  }

  /** Raw bridge invoke from inside the page (same wire format as src/bridge/api.ts). */
  const bridge = (ch, ...a) =>
    page.evaluate(
      ([ch, a]) =>
        new Promise((resolve, reject) => {
          const id = 1e9 + Math.floor(Math.random() * 1e6);
          const onMsg = (ev) => {
            const m = typeof ev.data === 'string' ? JSON.parse(ev.data) : ev.data;
            if (m && m.t === 'res' && m.id === id) {
              window.chrome.webview.removeEventListener('message', onMsg);
              m.ok ? resolve(m.result) : reject(new Error(m.error));
            }
          };
          window.chrome.webview.addEventListener('message', onMsg);
          window.chrome.webview.postMessage({ t: 'req', id, ch, args: a });
        }),
      [ch, a]
    );
  return { page, bridge, close };
}

/** macOS: the automation socket (./macApp.mjs). */
async function openMac() {
  const { openMacApp } = await import('./macApp.mjs');
  return openMacApp({ exe, dataDir, extra, socketPath: join(tmpdir(), `sshtermaq-auto-${process.pid}.sock`), onExit });
}

let app;
let code = 0;
try {
  app = isMac ? await openMac() : await openWindowsApp();
  const { page, bridge } = app;
  const logs = [];
  page.on('console', (m) => logs.push(`[${m.type()}] ${m.text()}`));
  page.on('pageerror', (e) => logs.push(`[pageerror] ${e.message}`));
  await page.waitForSelector('.app', { timeout: 15000 });
  await page.waitForTimeout(800);

  if (steps) {
    const mod = await import(pathToFileURL(resolve(process.cwd(), steps)).href);
    await mod.default(page, { out, exe, dataDir, bridge, platform: process.platform });
  }
  mkdirSync(dirname(out), { recursive: true });
  await page.screenshot({ path: out });
  console.log(`screenshot: ${out}`);
  const errors = logs.filter((l) => l.startsWith('[error]') || l.startsWith('[pageerror]'));
  if (errors.length) console.log('console errors:\n' + errors.join('\n'));
} catch (e) {
  console.error('E2E failed:', e?.message ?? e);
  code = 1;
} finally {
  await app?.close();
  // WebView2 child processes release the profile asynchronously.
  for (let i = 0; i < 20 && tempData && !flag('keep-data'); i++) {
    await new Promise((r) => setTimeout(r, 500));
    try {
      rmSync(dataDir, { recursive: true, force: true });
      break;
    } catch {
      /* retry */
    }
  }
}
process.exit(code);
