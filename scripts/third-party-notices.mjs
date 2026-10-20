#!/usr/bin/env node
/**
 * Writes THIRD-PARTY-NOTICES.txt for a release package (scripts/windows/build.ps1, scripts/macos/build.sh):
 * the components with their versions and licenses, where libssh's source code is (LGPL-2.1), and the full license
 * texts - the vcpkg ports' `copyright` files and ui/dist/licenses.txt (the npm packages in the UI bundle, written by
 * ui/scripts/licenses.ts during `npm run build`).
 *
 *   node scripts/third-party-notices.mjs --platform windows|macos --vcpkg <vcpkg_installed/<triplet>>
 *                                        --ui <ui/dist/licenses.txt> --out <file>
 */
import { existsSync, readFileSync, writeFileSync } from 'node:fs';
import { join } from 'node:path';

const args = process.argv.slice(2);
const opt = (name) => {
  const i = args.indexOf(`--${name}`);
  if (i < 0 || args[i + 1] === undefined) throw new Error(`missing --${name}`);
  return args[i + 1];
};
const platform = opt('platform');
const vcpkg = opt('vcpkg');
const uiLicenses = opt('ui');
const out = opt('out');
if (platform !== 'windows' && platform !== 'macos') throw new Error('--platform must be windows or macos');

const libsshFile = platform === 'windows' ? 'ssh.dll' : 'SSHTermaq.app/Contents/Frameworks/libssh.*.dylib';
const mainFile = platform === 'windows' ? 'SSHTermaq.exe' : 'SSHTermaq';
/** The vcpkg ports in the package: port, name, license, home page, how it is linked. */
const ports = [
  ['libssh', 'libssh', 'LGPL-2.1', 'https://www.libssh.org/', `dynamically linked: ${libsshFile}`],
  ['openssl', 'OpenSSL', 'Apache-2.0', 'https://www.openssl.org/', `statically linked into ${mainFile} and libssh`],
  ['zlib', 'zlib', 'Zlib', 'https://zlib.net/', 'statically linked into libssh'],
  ['nlohmann-json', 'nlohmann/json', 'MIT', 'https://github.com/nlohmann/json', 'header-only'],
  ...(platform === 'windows'
    ? [['webview2', 'Microsoft WebView2 SDK', 'BSD-3-Clause', 'https://aka.ms/webview2', 'static WebView2 loader']]
    : [])
];

const read = (path) => {
  if (!existsSync(path)) throw new Error(`not found: ${path}`);
  return readFileSync(path, 'utf8').replace(/\r\n/g, '\n').replace(/^(\s*\n)+/, '').trimEnd();
};
/** A port's SPDX document (written by vcpkg): its version, the vcpkg commit of the port, its source download. */
function spdx(port) {
  const doc = JSON.parse(read(join(vcpkg, 'share', port, 'vcpkg.spdx.json')));
  const pkgs = doc.packages ?? [];
  const main = pkgs.find((p) => p.name === port) ?? pkgs[0] ?? {};
  const commit = /github\.com\/microsoft\/vcpkg@([0-9a-f]+)/.exec(main.downloadLocation ?? '')?.[1];
  const sources = pkgs.map((p) => p.downloadLocation).filter((u) => /^https?:/.test(u ?? ''));
  return { version: (main.versionInfo ?? '?').replace(/#\d+$/, ''), commit, sources };
}

const info = Object.fromEntries(ports.map(([port]) => [port, spdx(port)]));
const line = '='.repeat(100);
const dash = '-'.repeat(100);
const pad = (s, n) => s + ' '.repeat(Math.max(1, n - s.length));

const libssh = info.libssh;
const text = [
  'SSHTermaq includes the following third-party software. Their copyright notices and license texts follow below.',
  '',
  ...ports.map(([port, name, license, home, how]) => `${pad(`${name} ${info[port].version}`, 36)}${pad(license, 14)}${pad(home, 36)}(${how})`),
  `${pad('npm packages of the user interface', 36)}${pad('MIT, ISC', 14)}(listed below, inside ${mainFile})`,
  '',
  line,
  'libssh and the GNU Lesser General Public License 2.1',
  line,
  '',
  `libssh is a separate library (${libsshFile}), used under the LGPL 2.1. You may replace it with your own`,
  'build of a compatible libssh version.',
  '',
  `The library in this package is libssh ${libssh.version}, built by vcpkg from:`,
  ...libssh.sources.map((u) => `  ${u}`),
  ...(libssh.commit
    ? [
        'with the patches and build options of the vcpkg port at',
        `  https://github.com/microsoft/vcpkg/tree/${libssh.commit}/ports/libssh`
      ]
    : []),
  '',
  ...ports.flatMap(([port, name]) => [line, `${name} ${info[port].version}`, line, '', read(join(vcpkg, 'share', port, 'copyright')), '']),
  line,
  'User interface',
  line,
  '',
  read(uiLicenses),
  ''
].join('\n');

writeFileSync(out, platform === 'windows' ? text.replace(/\n/g, '\r\n') : text);
console.log(`Wrote ${out} (${ports.length} libraries, ${Math.round(text.length / 1024)} KB)`);
