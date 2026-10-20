#!/bin/bash
# Stores what Apple's notary service needs in your login keychain, once per Mac, as a profile that
# scripts/macos/sign.sh uses (xcrun notarytool store-credentials). The values come from scripts/macos/signing.json
# (not committed; start from scripts/macos/signing.example.json):
#
#   "teamId": "ABCDE12345"            your Team ID (developer.apple.com > Account > Membership details)
#   "appleId": "you@example.com"      the Apple ID of your developer account
#   "notaryProfile": "sshtermaq-notary"   the profile's name in the keychain
#
# notarytool then asks for an app-specific password for that Apple ID (account.apple.com > Sign-In and Security >
# App-Specific Passwords), checks it with Apple and keeps it in the keychain; it never goes into a file.
# Instead of an Apple ID, an App Store Connect API key works too (no password asked; for build machines):
#   "apiKey": "/path/AuthKey_XXXXXXXXXX.p8", "apiKeyId": "XXXXXXXXXX", "apiIssuer": "<issuer UUID>"
#
# Usage: scripts/macos/notary-setup.sh        (SSHTERMAQ_SIGN_CONFIG: another config file)
set -euo pipefail

root="$(cd "$(dirname "$0")/../.." && pwd)"
config="${SSHTERMAQ_SIGN_CONFIG:-$root/scripts/macos/signing.json}"

die() { echo "error: $*" >&2; exit 1; }
value() { plutil -extract "$1" raw -o - "$config" 2>/dev/null || true; }

[ "${1:-}" = "-h" ] || [ "${1:-}" = "--help" ] && { sed -n '2,18p' "$0"; exit 0; }
[ -f "$config" ] || die "no $config: copy scripts/macos/signing.example.json there and fill in teamId and appleId"
plutil -convert xml1 -o /dev/null "$config" || die "$config is not valid JSON"

team_id="$(value teamId)"
apple_id="$(value appleId)"
profile="$(value notaryProfile)"
profile="${SSHTERMAQ_NOTARY_PROFILE:-${profile:-sshtermaq-notary}}"
api_key="$(value apiKey)"
api_key_id="$(value apiKeyId)"
api_issuer="$(value apiIssuer)"

if [ -n "$api_key" ]; then
  [ -f "$api_key" ] || die "apiKey: no file $api_key"
  [ -n "$api_key_id" ] && [ -n "$api_issuer" ] || die "apiKey needs apiKeyId and apiIssuer in $config"
  echo "Storing notary profile \"$profile\" with the API key $api_key_id"
  xcrun notarytool store-credentials "$profile" --key "$api_key" --key-id "$api_key_id" --issuer "$api_issuer"
else
  [ -n "$team_id" ] || die "teamId is empty in $config (developer.apple.com > Account > Membership details)"
  [ -n "$apple_id" ] || die "appleId is empty in $config"
  echo "Storing notary profile \"$profile\" for $apple_id (team $team_id)."
  echo "notarytool asks for an app-specific password: make one at account.apple.com >"
  echo "Sign-In and Security > App-Specific Passwords."
  xcrun notarytool store-credentials "$profile" --apple-id "$apple_id" --team-id "$team_id"
fi
echo
echo "Done. Check signing and notarization with: scripts/macos/sign.sh --check"
