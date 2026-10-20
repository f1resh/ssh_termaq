# Building SSHTermaq on macOS

From a Mac with nothing installed to a running debug build, the tests and a release package. Windows: see
[BUILDING.md](BUILDING.md). For the architecture and module layout see [ARCHITECTURE.md](ARCHITECTURE.md).

SSHTermaq for macOS is two builds, one per processor: **arm64** (Apple silicon) and **x64** (Intel). Each runs on
macOS 12 or later. A Mac of either kind builds both; an Apple silicon Mac runs the Intel build's tests under Rosetta.

## 1. What you need

- A recent macOS to build (this project is built on macOS 26; the apps themselves run on 12 and later).
- The Xcode command line tools: `xcode-select --install` (the full Xcode works too). Git, curl and perl come with
  macOS.
- About 3 GB of disk space for the tools, the dependencies of both architectures and the builds.
- For the Intel build's tests on Apple silicon: Rosetta (`softwareupdate --install-rosetta`).

No Homebrew is needed: everything else is put inside the repository.

## 2. Set up the tools and dependencies (once)

```bash
scripts/macos/bootstrap.sh
```

It puts into the repository (all ignored by git):

| Where | What |
|---|---|
| `.tools/vcpkg` | vcpkg at the `builtin-baseline` of `vcpkg.json` |
| `.tools/node` | Node.js 22 (checked against its SHA-256) |
| `.tools/bin` | `cmake`, `ctest`, `ninja` (fetched by vcpkg), `node`, `npm`, `npx` |
| `vcpkg_installed/mac-arm64`, `vcpkg_installed/mac-x64` | libssh, OpenSSL, zlib, nlohmann-json, Catch2 per architecture |

The dependencies take a while the first time (OpenSSL twice); vcpkg keeps built packages in `~/.cache/vcpkg`, so later
runs are quick. `--arch arm64` (or `x64`) sets up one architecture only. Running it again skips what is done.

Each shell that builds needs the tools on its `PATH`:

```bash
export PATH="$PWD/.tools/bin:$PATH"
```

## 3. Build the UI

```bash
cd ui && npm ci && npm run build && cd ..
```

The native build embeds `ui/dist` into the executable (two `__TEXT` sections), so build the UI before the app and again
after changing it.

## 4. Build and run the app

```bash
cmake --preset mac-arm64
```

```bash
cmake --build --preset mac-arm64-debug
```

```bash
./build/mac-arm64/Debug/SSHTermaq.app/Contents/MacOS/SSHTermaq --data-dir /tmp/st-dev
```

`--data-dir` keeps your real data (`~/Library/Application Support/SSHTermaq`) out of it. The Intel build is the same
with `mac-x64` (it runs under Rosetta here). Debug builds also answer on an automation socket for the end-to-end
scripts (`ui/scripts/app.mjs`, [TESTING.md](TESTING.md)).

The app is a normal bundle: `Contents/MacOS/SSHTermaq` (with the UI inside), `Contents/Frameworks/libssh.4.11.0.dylib`
(found through the executable's only rpath, `@executable_path/../Frameworks`), `Contents/Resources/app.icns` and
`Contents/Info.plist` (from `native/res/macos/Info.plist.in`; bundle id `io.github.f1resh.sshtermaq`).

## 5. Run the tests

```bash
ctest --preset mac-arm64 --output-on-failure
```

```bash
arch -x86_64 ctest --preset mac-x64 --output-on-failure
```

```bash
cd ui && npm test
```

The test presets run the Debug builds; what each suite covers is in [TESTING.md](TESTING.md).

## 6. Release package

```bash
scripts/macos/build.sh
```

It shows the version and build id and waits for Enter (as on Windows: the version is set in `CMakeLists.txt`, see
[BUILDING.md, section 9](BUILDING.md#9-versions-and-build-ids)), then builds the UI and, per architecture, the Release
app, and assembles in `dist/`:

| File | What |
|---|---|
| `SSHTermaq-<version>-mac-<arch>/` | `SSHTermaq.app`, `README.txt`, `THIRD-PARTY-NOTICES.txt` |
| `SSHTermaq-<version>-mac-<arch>.zip` | that folder, made with `ditto` so the app's signature survives |
| `SSHTermaq-<version>-mac-<arch>.dSYM.zip` | the debug symbols, to make crash reports readable; not for users |
| `SSHTermaq-<version>-mac-<arch>.dmg` | with `--dmg`: a disk image with the app, a link to Applications and the two texts |

On the way it moves the debug information into the dSYM, strips the app, signs it and checks it: the right
processor only, nothing loaded from outside macOS and the app (only `/System/Library`, `/usr/lib` and the bundled
libssh), a valid signature and the version in `Info.plist`.

Options: `--arch arm64` or `--arch x64` builds one, `--tests` runs the UI and native test suites first (Release),
`--sign` signs with your Developer ID and notarizes (section 7), `--sign --no-notarize` only signs, `--dmg` also makes
the disk images (made with macOS's `hdiutil`; signed and notarized too with `--sign`), `--skip-ui` reuses
`ui/dist`, `--no-zip` leaves the folders only, `--no-prompt` skips the version check (also skipped without a terminal).

Without `--sign` the apps are signed ad hoc: they run on the Mac that built them. On another Mac, macOS refuses an
ad-hoc app that came from the Internet ("damaged" or "cannot be opened"); a Developer ID signature and Apple's
notarization make it open normally.

## 7. Signing and notarization

With `--sign`, `build.sh` checks the certificate and the notary profile before building, then has
`scripts/macos/sign.sh` sign each app before zipping it: `Contents/Frameworks/libssh*.dylib` first, then the app, with
the hardened runtime and a secure timestamp; it sends the app to Apple's notary service, waits for it (a few minutes),
staples the ticket to the app (so it opens offline too) and checks it as Gatekeeper does. With `--dmg` it then does the
same for each disk image (a signature of its own, notarization, ticket), so that the download itself is accepted, not
only the app inside. `sign.sh` also works on its own, on apps and disk images:
`scripts/macos/sign.sh dist/SSHTermaq-<version>-mac-arm64/SSHTermaq.app`. A release with everything:
`scripts/macos/build.sh --tests --sign --dmg`.

Once per Mac:

1. **The certificate.** A "Developer ID Application" certificate with its private key in your login keychain; only
   your developer account's Account Holder can create one. In Xcode: Settings > Accounts > your team > Manage
   Certificates > + > Developer ID Application (or at developer.apple.com > Certificates, with a request made in
   Keychain Access). `security find-identity -v -p codesigning` lists it. An "Apple Development" certificate is for
   testing only and does not count.
2. **The configuration.** Copy `scripts/macos/signing.example.json` to `scripts/macos/signing.json` (not committed)
   and fill in:

   | Key | What |
   |---|---|
   | `teamId` | your Team ID (developer.apple.com > Account > Membership details) |
   | `appleId` | the Apple ID of the developer account (for the notary service) |
   | `identity` | empty: the only Developer ID Application certificate of `teamId`; else its name or SHA-1 |
   | `notaryProfile` | the name of the notary credentials in the keychain (`sshtermaq-notary`) |

3. **The notary credentials.** `scripts/macos/notary-setup.sh` stores them in your keychain under `notaryProfile`
   (`xcrun notarytool store-credentials`). It asks for an app-specific password for the Apple ID - make one at
   account.apple.com > Sign-In and Security > App-Specific Passwords - checks it with Apple and keeps it in the
   keychain only. On a build machine an App Store Connect API key works instead (`apiKey`, `apiKeyId`, `apiIssuer`
   in `signing.json`; no password).
4. **A check.** `scripts/macos/sign.sh --check` shows the certificate it will use and tests the notary credentials.

The first signing shows a keychain dialog ("codesign wants to access key ..."): Always Allow lets the next builds
sign without asking. Environment variables win over `signing.json` (for CI): `SSHTERMAQ_SIGN_IDENTITY`,
`SSHTERMAQ_NOTARY_PROFILE`, `SSHTERMAQ_SIGN_CONFIG`.

If Apple does not accept an app, `sign.sh` prints the notary log, which names each problem (for example a file
without the hardened runtime or a signature without a secure timestamp).

## 8. libssh and the LGPL

libssh is the one library that is not linked into the executable, so that it can be replaced (LGPL-2.1), as `ssh.dll`
can on Windows. To use your own build of libssh 0.11 or later (the same ABI, `libssh.4`):

1. Build it as a dynamic library for the app's processor (e.g. with this repository's vcpkg triplets, or libssh's own
   CMake build with `-DCMAKE_OSX_ARCHITECTURES=arm64` and `-DCMAKE_OSX_DEPLOYMENT_TARGET=12.0`).
2. Replace `SSHTermaq.app/Contents/Frameworks/libssh.4.11.0.dylib` with it, under that name.
3. Sign the app again, ad hoc: `codesign --force --deep --sign - SSHTermaq.app`. Changing a file inside a signed app
   invalidates its signature, and the Developer ID one cannot be made again outside this project; an ad-hoc
   signature lets the app run on your Mac.

## Troubleshooting

- **`cmake --preset` says the toolchain file or ninja is missing:** run `scripts/macos/bootstrap.sh`, and put
  `.tools/bin` on the `PATH`.
- **The Intel tests fail with "Bad CPU type in executable":** install Rosetta (`softwareupdate --install-rosetta`).
- **An ad-hoc app copied from another Mac "is damaged and can't be opened":** that is Gatekeeper's message for
  unsigned downloads. For a test build only: `xattr -dr com.apple.quarantine SSHTermaq.app`.
- **A Release app does not start and Console shows "Library not loaded: @rpath/libssh...":** the bundle lacks
  `Contents/Frameworks`; rebuild (the build copies libssh there after linking).
