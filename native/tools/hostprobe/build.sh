#!/bin/bash
# Builds the B3 host probe (HostProbe.mm) into build/hostprobe/HostProbe.app for this Mac's architecture.
# Usage: native/tools/hostprobe/build.sh [arm64|x86_64]
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../../.." && pwd)"
arch="${1:-$(uname -m)}"
app="$root/build/hostprobe/HostProbe.app"
mkdir -p "$app/Contents/MacOS"
cat > "$app/Contents/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>CFBundleIdentifier</key><string>dev.sshtermaq.hostprobe</string>
  <key>CFBundleName</key><string>HostProbe</string>
  <key>CFBundleExecutable</key><string>HostProbe</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>LSMinimumSystemVersion</key><string>12.0</string>
  <key>NSHighResolutionCapable</key><true/>
</dict></plist>
EOF
clang++ -std=c++20 -fobjc-arc -O2 -arch "$arch" -mmacosx-version-min=12.0 -Wall -Wextra -Wno-unused-parameter \
  -DHOSTPROBE_DIR="\"$here\"" "$here/HostProbe.mm" -framework AppKit -framework WebKit \
  -o "$app/Contents/MacOS/HostProbe"
codesign -s - --force "$app" >/dev/null 2>&1 || true
echo "$app"
