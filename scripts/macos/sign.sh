#!/bin/bash
# Signs SSHTermaq.app (or a .dmg holding it) with your Developer ID and has Apple notarize it, so that it opens on any
# Mac without warnings:
#
#   1. signs Contents/Frameworks/*.dylib, then the app (inside out), with the hardened runtime and a secure timestamp;
#      a .dmg gets a signature of its own
#   2. sends it to Apple's notary service and waits (a few minutes; on rejection it prints Apple's log)
#   3. staples the ticket to it, so it opens offline too, and checks it as Gatekeeper does
#
# Used on its own or by scripts/macos/build.sh --sign (apps before zipping, then the --dmg images). Configuration: scripts/macos/signing.json (not
# committed; start from scripts/macos/signing.example.json), environment variables winning over it:
#
#   "identity"       SSHTERMAQ_SIGN_IDENTITY    "Developer ID Application: Name (TEAMID)" or its SHA-1; empty: the
#                                               only Developer ID Application certificate in the keychain (of teamId)
#   "teamId"                                    checked against the signature
#   "notaryProfile"  SSHTERMAQ_NOTARY_PROFILE   the notarytool keychain profile (scripts/macos/notary-setup.sh)
#                    SSHTERMAQ_SIGN_CONFIG      another config file
#
# Usage: scripts/macos/sign.sh --check [--no-notarize]       show the identity and test the notary profile
#        scripts/macos/sign.sh [--no-notarize] SSHTermaq.app|SSHTermaq-<version>-mac-<arch>.dmg ...
set -euo pipefail

root="$(cd "$(dirname "$0")/../.." && pwd)"
config="${SSHTERMAQ_SIGN_CONFIG:-$root/scripts/macos/signing.json}"

die() { echo "error: $*" >&2; exit 1; }
value() { [ -f "$config" ] && plutil -extract "$1" raw -o - "$config" 2>/dev/null || true; }

check=0
notarize=1
apps=()
while [ $# -gt 0 ]; do
  case "$1" in
    --check) check=1; shift ;;
    --no-notarize) notarize=0; shift ;;
    -h|--help) sed -n '2,20p' "$0"; exit 0 ;;
    -*) die "unknown option: $1" ;;
    *) apps+=("$1"); shift ;;
  esac
done
[ "$check" = 1 ] || [ ${#apps[@]} -gt 0 ] || die "nothing to sign (scripts/macos/sign.sh --help)"
[ ! -f "$config" ] || plutil -convert xml1 -o /dev/null "$config" || die "$config is not valid JSON"

identity="${SSHTERMAQ_SIGN_IDENTITY:-$(value identity)}"
team_id="$(value teamId)"
profile="$(value notaryProfile)"
profile="${SSHTERMAQ_NOTARY_PROFILE:-${profile:-sshtermaq-notary}}"

# ---- the certificate ------------------------------------------------------------------------------------------------
# "  1) <SHA-1> "Developer ID Application: Name (TEAMID)"" lines of the valid code-signing identities.
candidates="$(security find-identity -v -p codesigning | sed -nE 's/^ *[0-9]+\) ([0-9A-F]{40}) "(Developer ID Application: .*)"$/\1 \2/p')"
if [ -n "$team_id" ]; then candidates="$(printf '%s\n' "$candidates" | grep -F "($team_id)" || true)"; fi
if [ -n "$identity" ]; then
  match="$(printf '%s\n' "$candidates" | grep -F -- "$identity" || true)"
  [ -n "$match" ] || die "no valid Developer ID Application identity \"$identity\" in the keychain (security find-identity -v -p codesigning)"
else
  match="$candidates"
  [ -n "$match" ] || die "no valid Developer ID Application certificate${team_id:+ of team $team_id} in the keychain: \
install it (developer.apple.com > Certificates, or Xcode > Settings > Accounts > Manage Certificates)"
fi
[ "$(printf '%s\n' "$match" | wc -l | tr -d ' ')" = 1 ] || { printf '%s\n' "$match" >&2; die "several identities match: set \"identity\" in $config"; }
hash="${match%% *}"
name="${match#* }"
echo "Signing identity: $name"

notary_ok() { xcrun notarytool history --keychain-profile "$profile" >/dev/null 2>&1; }
if [ "$notarize" = 1 ]; then
  notary_ok || die "the notary profile \"$profile\" does not work: run scripts/macos/notary-setup.sh"
  echo "Notary profile: $profile"
fi
[ "$check" = 1 ] && exit 0

# ---- sign, notarize, staple -------------------------------------------------------------------------------------------
sign() { codesign --force --timestamp --options runtime --sign "$hash" "$@"; }

check_team() {
  local signed_team
  signed_team="$(codesign -dv "$1" 2>&1 | sed -n 's/^TeamIdentifier=//p')"
  [ -z "$team_id" ] || [ "$signed_team" = "$team_id" ] || die "$1 is signed by team $signed_team, not $team_id"
}

# Sends `upload` (a zip of the app, or the disk image itself) to Apple, waits, and staples the ticket to `target`.
notarize_and_staple() {
  local target="$1" upload="$2" work status id
  work="$(mktemp -d)"
  echo "Sending to Apple's notary service (this takes a few minutes)..."
  xcrun notarytool submit "$upload" --keychain-profile "$profile" --wait --output-format json > "$work/result.json" || true
  status="$(plutil -extract status raw -o - "$work/result.json" 2>/dev/null || echo "no answer")"
  id="$(plutil -extract id raw -o - "$work/result.json" 2>/dev/null || true)"
  echo "Notarization: $status${id:+ ($id)}"
  if [ "$status" != Accepted ]; then
    cat "$work/result.json" >&2 || true
    [ -n "$id" ] && xcrun notarytool log "$id" --keychain-profile "$profile" >&2 || true
    rm -rf "$work"
    die "Apple did not accept $target"
  fi
  rm -rf "$work"
  xcrun stapler staple "$target"
  xcrun stapler validate "$target"
}

for target in "${apps[@]}"; do
  echo
  echo "== $target"
  case "$target" in
    *.dmg)
      # A disk image: its own signature (no hardened runtime: not code), notarized as it is.
      [ -f "$target" ] || die "no disk image $target"
      codesign --force --timestamp --sign "$hash" "$target"
      codesign --verify --strict "$target" || die "the signature does not verify"
      check_team "$target"
      if [ "$notarize" = 1 ]; then
        notarize_and_staple "$target" "$target"
        spctl --assess --type open --context context:primary-signature --verbose=2 "$target"
      fi
      ;;
    *)
      [ -d "$target/Contents/MacOS" ] || die "$target is not an app bundle or a .dmg"
      for lib in "$target"/Contents/Frameworks/*.dylib; do
        [ -e "$lib" ] && sign "$lib"
      done
      sign "$target"
      codesign --verify --strict --deep "$target" || die "the signature does not verify"
      check_team "$target"
      if [ "$notarize" = 1 ]; then
        work="$(mktemp -d)"
        ditto -c -k --keepParent "$target" "$work/upload.zip"
        notarize_and_staple "$target" "$work/upload.zip"
        rm -rf "$work"
        spctl --assess --type execute --verbose=2 "$target"  # "accepted, source=Notarized Developer ID"
      fi
      ;;
  esac
  [ "$notarize" = 1 ] || echo "Signed, not notarized (--no-notarize): other Macs still warn before opening it."
done
