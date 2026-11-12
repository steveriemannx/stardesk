#!/usr/bin/env bash
# One-time: create a self-signed code-signing identity ("StarDesk Local Dev")
# and trust it, so the installed app keeps the SAME signature (and therefore
# its macOS Screen Recording / Accessibility grants) across rebuilds.
#
#   ./scripts/setup-signing.sh
#
# macOS may show a trust/authorization dialog - approve it once.
set -euo pipefail

IDENTITY="StarDesk Local Dev"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

if security find-identity -v -p codesigning 2>/dev/null | grep -q "$IDENTITY"; then
    echo "identity already available: $IDENTITY"
    exit 0
fi

cat > "$WORK/cs.cnf" <<CNF
[req]
distinguished_name = dn
prompt = no
x509_extensions = ext
[dn]
CN = $IDENTITY
[ext]
basicConstraints = critical,CA:false
keyUsage = critical,digitalSignature
extendedKeyUsage = critical,codeSigning
CNF

openssl req -x509 -newkey rsa:2048 -nodes -keyout "$WORK/key.pem" \
    -out "$WORK/cert.pem" -days 3650 -config "$WORK/cs.cnf" 2>/dev/null
KEYCHAIN="$HOME/Library/Keychains/login.keychain-db"
security add-trusted-cert -r trustRoot -p codeSign -k "$KEYCHAIN" "$WORK/cert.pem"
security import "$WORK/key.pem" -k "$KEYCHAIN" -T /usr/bin/codesign -T /usr/bin/security
security import "$WORK/cert.pem" -k "$KEYCHAIN" -T /usr/bin/codesign -T /usr/bin/security 2>/dev/null || true

echo "created identity: $IDENTITY"
security find-identity -v -p codesigning "$KEYCHAIN" | grep -i "$IDENTITY" | head -2
