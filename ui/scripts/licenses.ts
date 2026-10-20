/**
 * Vite plugin: the licenses of the npm packages that end up in the built UI, as dist/licenses.txt.
 *
 * Every package with code in the bundle is listed once, with its version, its license id and the full text of its
 * LICENSE file (MIT, ISC and the like require the copyright notice and the license text to travel with the code).
 * scripts/third-party-notices.mjs puts the file into the THIRD-PARTY-NOTICES.txt of the release packages; it is also
 * embedded with the rest of ui/dist.
 */
import { existsSync, readdirSync, readFileSync } from 'node:fs';
import { join } from 'node:path';
import type { Plugin } from 'vite';

interface PackageInfo {
  name: string;
  version: string;
  license: string;
  text: string;
}

/** The package folder of a module path inside node_modules ("…/node_modules/@xterm/xterm/lib/x.js" -> its root). */
function packageRoot(id: string): string | null {
  const path = id.split('?')[0].replace(/\\/g, '/');
  const at = path.lastIndexOf('/node_modules/');
  if (at < 0) return null;
  const rest = path.slice(at + '/node_modules/'.length).split('/');
  const depth = rest[0].startsWith('@') ? 2 : 1;
  if (rest.length <= depth) return null;
  return path.slice(0, at + '/node_modules/'.length) + rest.slice(0, depth).join('/');
}

function readPackage(root: string): PackageInfo {
  const pkg = JSON.parse(readFileSync(join(root, 'package.json'), 'utf8')) as { name: string; version: string; license?: string };
  const file = readdirSync(root).find((f) => /^licen[cs]e(\.(md|txt))?$/i.test(f));
  return {
    name: pkg.name,
    version: pkg.version,
    license: pkg.license ?? 'see the package',
    text: file ? readFileSync(join(root, file), 'utf8').trim() : ''
  };
}

export function licenses(): Plugin {
  return {
    name: 'sshtermaq-licenses',
    apply: 'build',
    generateBundle(_options, bundle) {
      const roots = new Set<string>();
      for (const chunk of Object.values(bundle)) {
        if (chunk.type !== 'chunk') continue;
        for (const [id, info] of Object.entries(chunk.modules)) {
          if (info.renderedLength === 0) continue; // tree-shaken away entirely
          const root = packageRoot(id);
          if (root && existsSync(join(root, 'package.json'))) roots.add(root);
        }
      }
      const packages = [...roots].map(readPackage).sort((a, b) => a.name.localeCompare(b.name));
      const missing = packages.filter((p) => !p.text).map((p) => p.name);
      if (missing.length) this.warn(`no LICENSE file in: ${missing.join(', ')}`);
      const line = '-'.repeat(100);
      const text = [
        `npm packages in the SSHTermaq user interface (${packages.length}):`,
        '',
        ...packages.map((p) => `  ${p.name} ${p.version} (${p.license})`),
        '',
        ...packages.flatMap((p) => [line, `${p.name} ${p.version} (${p.license})`, line, '', p.text || `License: ${p.license}`, ''])
      ].join('\n');
      this.emitFile({ type: 'asset', fileName: 'licenses.txt', source: text });
    }
  };
}
