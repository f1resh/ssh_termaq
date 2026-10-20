#!/bin/bash
# Builds SSHTermaq for macOS (web UI + native Release, per architecture) and assembles what is handed out:
#
#   dist/SSHTermaq-<ver>-mac-<arch>/          SSHTermaq.app, README.txt, THIRD-PARTY-NOTICES.txt
#   dist/SSHTermaq-<ver>-mac-<arch>.zip       that folder (ditto, so the app's signature survives)
#   dist/SSHTermaq-<ver>-mac-<arch>.dSYM.zip  the debug symbols (to read crash reports), not for users
#   dist/SSHTermaq-<ver>-mac-<arch>.dmg       with --dmg: a disk image of the app, an Applications link and the texts
#
# Usage: scripts/macos/build.sh [--arch arm64|x64|all] [--tests] [--sign [--no-notarize]] [--dmg] [--skip-ui]
#                               [--no-zip] [--no-prompt]
#   --arch        which builds (default: all; x64 runs under Rosetta on Apple silicon)
#   --tests       run the UI and native test suites first (the other architecture's under Rosetta when it can)
#   --sign        sign with your Developer ID and notarize (scripts/macos/sign.sh; checked before building); with
#                 --dmg the disk images too
#   --no-notarize with --sign: sign only
#   --dmg         also make a disk image per architecture
#   --skip-ui     reuse ui/dist
#   --no-prompt   do not wait for Enter at the version check (also when there is no terminal)
# Starts by showing the version (set in CMakeLists.txt) and the build id, and waits for Enter so the version can be
# changed first. Without --sign the apps are signed ad hoc: they run on this Mac; on others macOS refuses them.
# Needs scripts/macos/bootstrap.sh run once.
set -euo pipefail

root="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$root"
export PATH="$root/.tools/bin:$PATH"

arch=all
tests=0
skip_ui=0
zip=1
prompt=1
sign=0
sign_flags=()
dmg=0
while [ $# -gt 0 ]; do
  case "$1" in
    --arch) arch="${2:-}"; shift 2 ;;
    --tests) tests=1; shift ;;
    --skip-ui) skip_ui=1; shift ;;
    --no-zip) zip=0; shift ;;
    --no-prompt) prompt=0; shift ;;
    --sign) sign=1; shift ;;
    --no-notarize) sign_flags=(--no-notarize); shift ;;
    --dmg) dmg=1; shift ;;
    -h|--help) sed -n '2,21p' "$0"; exit 0 ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
  esac
done
case "$arch" in
  arm64) archs=(arm64) ;;
  x64) archs=(x64) ;;
  all) archs=(arm64 x64) ;;
  *) echo "--arch must be arm64, x64 or all" >&2; exit 2 ;;
esac

step() { printf '\n== %s\n' "$*"; }
die() { echo "error: $*" >&2; exit 1; }

[ "$(uname -s)" = Darwin ] || die "this script is for macOS"
command -v cmake >/dev/null && command -v npm >/dev/null || die "run scripts/macos/bootstrap.sh first"

# ---- version --------------------------------------------------------------------------------------------------------
# Set only in CMakeLists.txt; the build number (commit count) and the commit come from git, as in cmake/BuildInfo.cmake.
build_id() {
  version="$(sed -nE 's/^project\(SSHTermaq VERSION ([0-9]+\.[0-9]+\.[0-9]+)[ )].*/\1/p' CMakeLists.txt | head -1)"
  [ -n "$version" ] || die "CMakeLists.txt: project(SSHTermaq VERSION x.y.z ...) not found"
  number="$(git rev-list --count HEAD 2>/dev/null || echo 0)"
  commit="$(git rev-parse --short=7 HEAD 2>/dev/null || echo unknown)"
  dirty=0
  if [ -n "$(git --no-optional-locks status --porcelain 2>/dev/null)" ]; then dirty=1; commit="$commit-dirty"; fi
}
build_id
echo "SSHTermaq $version (build $number, $commit)"
if [ "$prompt" = 1 ] && [ -t 0 ]; then
  echo "The version is set in CMakeLists.txt: project(SSHTermaq VERSION $version ...)."
  echo "If this release needs a new version, change it there now (x.y.z) and save the file."
  [ "$dirty" = 1 ] && echo 'There are uncommitted changes: the build will be marked "-dirty".'
  read -r -p "Press Enter to build, or Ctrl+C to stop " _
  build_id  # the version may just have been changed
  echo "Building SSHTermaq $version (build $number, $commit)"
fi

# The certificate and the notary profile, before the long build.
[ "$sign" = 1 ] && "$root/scripts/macos/sign.sh" --check ${sign_flags[@]+"${sign_flags[@]}"}

host="$(uname -m)"  # arm64 or x86_64
can_run() {         # can this Mac run the tests of an architecture?
  case "$1" in
    arm64) [ "$host" = arm64 ] ;;
    x64) [ "$host" = x86_64 ] || arch -x86_64 /usr/bin/true 2>/dev/null ;;
  esac
}

# ---- UI ---------------------------------------------------------------------------------------------------------------
if [ "$skip_ui" = 0 ]; then
  step "UI"
  (
    cd ui
    [ -d node_modules ] || npm ci --no-audit --no-fund
    [ "$tests" = 1 ] && npm test
    npm run build
  )
fi
[ -f ui/dist/index.html ] || die "ui/dist is missing (run without --skip-ui)"

# ---- per architecture --------------------------------------------------------------------------------------------------
made=()
for a in "${archs[@]}"; do
  machine=$([ "$a" = x64 ] && echo x86_64 || echo arm64)
  step "Native $a"
  cmake --preset "mac-$a" >/dev/null
  cmake --build --preset "mac-$a-release" --target SSHTermaq
  if [ "$tests" = 1 ]; then
    if can_run "$a"; then
      cmake --build --preset "mac-$a-release"
      runner=()
      [ "$a" = x64 ] && [ "$host" = arm64 ] && runner=(arch -x86_64)
      ${runner[@]+"${runner[@]}"} ctest --test-dir "build/mac-$a" -C Release --output-on-failure || die "$a tests failed"
    else
      echo "($a tests skipped: this Mac cannot run $a code)"
    fi
  fi

  name="SSHTermaq-$version-mac-$a"
  dist="dist/$name"
  app="$dist/SSHTermaq.app"
  step "Assemble $name"
  rm -rf "$dist" "dist/$name.zip" "dist/$name.dSYM.zip" "dist/$name.dmg"
  mkdir -p "$dist"
  ditto "build/mac-$a/Release/SSHTermaq.app" "$app"
  exe="$app/Contents/MacOS/SSHTermaq"

  # Debug symbols first (from the unstripped binary), then strip local symbols from what is handed out.
  dsymutil "$exe" -o "dist/$name.dSYM"
  strip -S -x "$exe"
  strip -x "$app"/Contents/Frameworks/*.dylib

  # Ad-hoc signature over the whole bundle, inside out (stripping invalidated the linker's).
  codesign --force --sign - --timestamp=none "$app"/Contents/Frameworks/*.dylib
  codesign --force --sign - --timestamp=none "$app"

  # Checks: architecture, what the executable and libssh load (macOS itself and the bundled libssh only), signature.
  for f in "$exe" "$app"/Contents/Frameworks/*.dylib; do
    [ "$(lipo -archs "$f")" = "$machine" ] || die "$f: not $machine only ($(lipo -archs "$f"))"
    bad="$(otool -L "$f" | tail -n +2 | awk '{print $1}' | grep -vE '^(/System/Library/|/usr/lib/|@rpath/libssh\.)' || true)"
    [ -z "$bad" ] || die "$f loads libraries from outside macOS and the app: $bad"
  done
  codesign --verify --strict --deep "$app" || die "the signature does not verify"
  /usr/libexec/PlistBuddy -c 'Print :CFBundleShortVersionString' "$app/Contents/Info.plist" | grep -qx "$version" ||
    die "Info.plist does not carry version $version (re-run cmake --preset mac-$a)"

  # Developer ID and notarization (the ad-hoc signature above made the checks possible without it).
  [ "$sign" = 1 ] && "$root/scripts/macos/sign.sh" ${sign_flags[@]+"${sign_flags[@]}"} "$app"

  title="SSHTermaq $version (build $number, $commit)"
  cat > "$dist/README.txt" <<EOF
$title
$(printf '=%.0s' $(seq 1 ${#title}))

Modern tabbed SSH / Telnet / Rlogin / Raw / Serial client for macOS ($machine) with PuTTY session import.

Move SSHTermaq.app to Applications and open it. Requires macOS 12 or later. Data is stored in
~/Library/Application Support/SSHTermaq.

PuTTY-compatible command line:  /Applications/SSHTermaq.app/Contents/MacOS/SSHTermaq [-ssh|-telnet|-rlogin|-raw|-serial]
  [user@]host [-P port] [-l user] [-pw password] [-i key] [-load "session"] [-L/-R/-D spec] [-A] [-X] [-C] ...
ssh://, sftp://, scp:// and telnet:// links open in SSHTermaq.
EOF
  # Versions, libssh's source and the full license texts: the vcpkg ports' copyright files and the npm packages in
  # the UI bundle (ui/dist/licenses.txt, written by `npm run build`).
  [ -f ui/dist/licenses.txt ] || die "ui/dist/licenses.txt is missing: build the UI again (run without --skip-ui)"
  node scripts/third-party-notices.mjs --platform macos --vcpkg "vcpkg_installed/mac-$a/$a-osx-st" \
    --ui ui/dist/licenses.txt --out "$dist/THIRD-PARTY-NOTICES.txt"

  if [ "$zip" = 1 ]; then
    ditto -c -k --sequesterRsrc --keepParent "$dist" "dist/$name.zip"
    ditto -c -k --keepParent "dist/$name.dSYM" "dist/$name.dSYM.zip"
  fi
  rm -rf "dist/$name.dSYM"

  # The disk image users drag the app out of: the app, a link to Applications and the two texts.
  if [ "$dmg" = 1 ]; then
    stage="$(mktemp -d)"
    mkdir "$stage/SSHTermaq"
    ditto "$app" "$stage/SSHTermaq/SSHTermaq.app"
    ln -s /Applications "$stage/SSHTermaq/Applications"
    cp "$dist/README.txt" "$dist/THIRD-PARTY-NOTICES.txt" "$stage/SSHTermaq/"
    hdiutil create -volname SSHTermaq -srcfolder "$stage/SSHTermaq" -fs HFS+ -format UDZO -ov "dist/$name.dmg" >/dev/null
    rm -rf "$stage"
    hdiutil verify -quiet "dist/$name.dmg" || die "dist/$name.dmg does not verify"
    [ "$sign" = 1 ] && "$root/scripts/macos/sign.sh" ${sign_flags[@]+"${sign_flags[@]}"} "dist/$name.dmg"
  fi
  made+=("$name")
done

step "Done"
for name in "${made[@]}"; do
  echo "dist/$name/SSHTermaq.app ($(du -sh "dist/$name/SSHTermaq.app" | cut -f1))"
  [ "$zip" = 1 ] && echo "dist/$name.zip ($(du -h "dist/$name.zip" | cut -f1)), dist/$name.dSYM.zip"
  [ "$dmg" = 1 ] && echo "dist/$name.dmg ($(du -h "dist/$name.dmg" | cut -f1))"
done
if [ "$sign" = 0 ]; then echo "Signed ad hoc: for other Macs, build with --sign (Developer ID and notarization)."; fi
