#!/bin/bash
# Sets up everything needed to build SSHTermaq on macOS, inside the repository (no Homebrew needed):
#
#   .tools/vcpkg      vcpkg at the builtin-baseline of vcpkg.json, bootstrapped
#   .tools/node       Node.js (the version pinned below, SHA-256 checked)
#   .tools/bin        cmake, ctest, ninja (fetched by vcpkg), pkg-config, node, npm, npx
#   vcpkg_installed/mac-arm64, vcpkg_installed/mac-x64
#                     the C++ dependencies of vcpkg.json per architecture (triplets/{arm64,x64}-osx-st.cmake)
#
# Usage: scripts/macos/bootstrap.sh [--arch arm64|x64|all] [--no-deps]
# Then:  export PATH="$PWD/.tools/bin:$PATH"; cmake --preset mac-arm64
# Needs the Xcode command line tools (xcode-select --install); git, curl and perl come with macOS.
# Safe to run again: finished parts are skipped; vcpkg caches built dependencies in ~/.cache/vcpkg.
set -euo pipefail

NODE_VERSION=22.23.3

root="$(cd "$(dirname "$0")/../.." && pwd)"
tools="$root/.tools"
arch=all
deps=1
while [ $# -gt 0 ]; do
  case "$1" in
    --arch) arch="${2:-}"; shift 2 ;;
    --no-deps) deps=0; shift ;;
    -h|--help) sed -n '2,13p' "$0"; exit 0 ;;
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
xcode-select -p >/dev/null 2>&1 || die "the Xcode command line tools are missing: xcode-select --install"
for t in git curl tar perl make shasum; do command -v "$t" >/dev/null || die "$t not found"; done
mkdir -p "$tools/bin"

# ---- vcpkg ----------------------------------------------------------------------------------------
baseline="$(sed -n 's/.*"builtin-baseline": *"\([0-9a-f]*\)".*/\1/p' "$root/vcpkg.json")"
[ -n "$baseline" ] || die "builtin-baseline not found in vcpkg.json"
vcpkg_dir="$tools/vcpkg"
if [ ! -d "$vcpkg_dir/.git" ]; then
  step "Cloning vcpkg"
  git clone --filter=blob:none https://github.com/microsoft/vcpkg.git "$vcpkg_dir"
fi
if [ "$(git -C "$vcpkg_dir" rev-parse HEAD)" != "$baseline" ]; then
  step "Checking out vcpkg $baseline (the builtin-baseline of vcpkg.json)"
  git -C "$vcpkg_dir" cat-file -e "$baseline^{commit}" 2>/dev/null || git -C "$vcpkg_dir" fetch --quiet origin
  git -C "$vcpkg_dir" -c advice.detachedHead=false checkout --quiet "$baseline"
  rm -f "$vcpkg_dir/vcpkg"   # bootstrap the vcpkg tool that belongs to this checkout
fi
if [ ! -x "$vcpkg_dir/vcpkg" ]; then
  step "Bootstrapping vcpkg"
  "$vcpkg_dir/bootstrap-vcpkg.sh" -disableMetrics
fi
vcpkg="$vcpkg_dir/vcpkg"

# ---- cmake, ctest, ninja --------------------------------------------------------------------------
if [ ! -x "$tools/bin/cmake" ]; then
  step "Fetching cmake"
  cmake_path="$("$vcpkg" fetch cmake | tail -n 1)"
  [ -x "$cmake_path" ] || die "vcpkg fetch cmake failed"
  ln -sf "$cmake_path" "$tools/bin/cmake"
  ln -sf "$(dirname "$cmake_path")/ctest" "$tools/bin/ctest"
fi
if [ ! -x "$tools/bin/ninja" ]; then
  step "Fetching ninja"
  ninja_path="$("$vcpkg" fetch ninja | tail -n 1)"
  [ -x "$ninja_path" ] || die "vcpkg fetch ninja failed"
  ln -sf "$ninja_path" "$tools/bin/ninja"
fi

# ---- pkg-config -----------------------------------------------------------------------------------
# vcpkg needs one on macOS to check the packages' .pc files. Without Homebrew's, vcpkg builds pkgconf; that package's
# own check needs a pkg-config already, so it alone is checked with `true`.
if [ ! -x "$tools/bin/pkg-config" ]; then
  if command -v pkg-config >/dev/null; then
    ln -sf "$(command -v pkg-config)" "$tools/bin/pkg-config"
  else
    step "Building pkgconf (pkg-config)"
    case "$(uname -m)" in arm64) host_triplet=arm64-osx ;; *) host_triplet=x64-osx ;; esac
    (cd "$tools" && PKG_CONFIG=/usr/bin/true "$vcpkg" install "pkgconf:$host_triplet" --classic)
    ln -sf "$vcpkg_dir/installed/$host_triplet/tools/pkgconf/pkgconf" "$tools/bin/pkg-config"
  fi
fi
export PKG_CONFIG="$tools/bin/pkg-config"

# ---- Node.js --------------------------------------------------------------------------------------
node_dir="$tools/node"
if [ "$("$node_dir/bin/node" --version 2>/dev/null || true)" != "v$NODE_VERSION" ]; then
  step "Downloading Node.js $NODE_VERSION"
  case "$(uname -m)" in
    arm64) node_arch=arm64 ;;
    x86_64) node_arch=x64 ;;
    *) die "unsupported CPU $(uname -m)" ;;
  esac
  name="node-v$NODE_VERSION-darwin-$node_arch"
  tmp="$(mktemp -d)"
  curl -fsSL "https://nodejs.org/dist/v$NODE_VERSION/$name.tar.gz" -o "$tmp/$name.tar.gz"
  curl -fsSL "https://nodejs.org/dist/v$NODE_VERSION/SHASUMS256.txt" -o "$tmp/SHASUMS256.txt"
  (cd "$tmp" && grep " $name.tar.gz\$" SHASUMS256.txt | shasum -a 256 -c -) || die "Node.js checksum mismatch"
  rm -rf "$node_dir"
  mkdir -p "$node_dir"
  tar -xzf "$tmp/$name.tar.gz" -C "$node_dir" --strip-components 1
  rm -rf "$tmp"
fi
for t in node npm npx; do ln -sf "$node_dir/bin/$t" "$tools/bin/$t"; done

# ---- C++ dependencies -----------------------------------------------------------------------------
if [ "$deps" = 1 ]; then
  for a in "${archs[@]}"; do
    step "C++ dependencies for $a (the first build takes several minutes, mostly OpenSSL)"
    (cd "$root" && "$vcpkg" install --triplet "$a-osx-st" --overlay-triplets="$root/triplets" \
      --x-install-root="$root/vcpkg_installed/mac-$a")
  done
fi

step "Done"
echo "Add the tools to PATH:  export PATH=\"$tools/bin:\$PATH\""
echo "Then configure:         cmake --preset mac-arm64   (or mac-x64)"
