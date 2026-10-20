#!/usr/bin/env node
/**
 * Build the UI (mock backend) into an isolated folder, serve it, open it in headless Microsoft Edge and take a
 * screenshot. Lets several people/agents preview concurrently without clobbering each other.
 *
 *   node scripts/shot.mjs --name shell --out ../artifacts/shell.png [--steps steps.mjs] [--width 1400 --height 900]
 *                         [--no-build] [--theme dark|light] [--keep-storage]
 *
 * --steps: an ES module whose default export is `async (page) => { ... }` run before the screenshot
 *          (click, type, page.evaluate(...) etc.). Further screenshots can be taken inside it.
 * The mock backend persists to localStorage; each run starts clean unless --keep-storage is passed.
 */
import { spawnSync } from 'node:child_process';
import { createReadStream, existsSync, mkdirSync, statSync } from 'node:fs';
import { createServer } from 'node:http';
import { dirname, extname, join, resolve } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { chromium } from 'playwright-core';

const here = dirname(fileURLToPath(import.meta.url));
const uiRoot = resolve(here, '..');

const args = process.argv.slice(2);
const opt = (name, def) => {
  const i = args.indexOf(`--${name}`);
  return i >= 0 && args[i + 1] && !args[i + 1].startsWith('--') ? args[i + 1] : def;
};
const flag = (name) => args.includes(`--${name}`);

const name = opt('name', 'default');
const out = resolve(process.cwd(), opt('out', `../artifacts/${name}.png`));
const width = Number(opt('width', 1400));
const height = Number(opt('height', 900));
const steps = opt('steps');
const theme = opt('theme');
const outDir = join(uiRoot, '.preview', name);

if (!flag('no-build')) {
  const r = spawnSync('npx', ['vite', 'build', '--outDir', outDir, '--emptyOutDir', '--logLevel', 'warn'], {
    cwd: uiRoot,
    stdio: 'inherit',
    shell: true
  });
  if (r.status !== 0) process.exit(r.status ?? 1);
}

const types = {
  '.html': 'text/html',
  '.js': 'text/javascript',
  '.css': 'text/css',
  '.svg': 'image/svg+xml',
  '.png': 'image/png',
  '.woff2': 'font/woff2',
  '.json': 'application/json',
  '.map': 'application/json'
};
const server = createServer((req, res) => {
  const url = new URL(req.url ?? '/', 'http://x');
  let p = join(outDir, decodeURIComponent(url.pathname));
  if (!existsSync(p) || statSync(p).isDirectory()) p = join(outDir, 'index.html');
  res.writeHead(200, { 'content-type': types[extname(p)] ?? 'application/octet-stream' });
  createReadStream(p).pipe(res);
});
await new Promise((r) => server.listen(0, '127.0.0.1', r));
const port = server.address().port;

const browser = await chromium.launch({ channel: 'msedge', headless: true });
try {
  const context = await browser.newContext({
    viewport: { width, height },
    colorScheme: theme === 'light' ? 'light' : 'dark',
    deviceScaleFactor: 1
  });
  const page = await context.newPage();
  const logs = [];
  page.on('console', (m) => logs.push(`[${m.type()}] ${m.text()}`));
  page.on('pageerror', (e) => logs.push(`[pageerror] ${e.message}`));
  await page.goto(`http://127.0.0.1:${port}/`);
  if (!flag('keep-storage')) {
    await page.evaluate(() => localStorage.clear());
    await page.reload();
  }
  await page.waitForTimeout(600);
  if (steps) {
    const mod = await import(pathToFileURL(resolve(process.cwd(), steps)).href);
    await mod.default(page, { out, port });
  }
  mkdirSync(dirname(out), { recursive: true });
  await page.screenshot({ path: out });
  console.log(`screenshot: ${out}`);
  const errors = logs.filter((l) => l.startsWith('[error]') || l.startsWith('[pageerror]'));
  if (errors.length) console.log('console errors:\n' + errors.join('\n'));
} finally {
  await browser.close();
  server.close();
}
