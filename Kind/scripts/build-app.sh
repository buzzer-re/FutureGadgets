#!/usr/bin/env bash
# Builds an ad-hoc signed Kind.app for this Mac's architecture.
#
#   scripts/build-app.sh [out_dir]
#
# out_dir  Where Kind.app is written. Defaults to ./build.
#
# Ad-hoc signing needs no Apple Developer account. A locally built app carries no quarantine
# flag, so Gatekeeper lets it open.
set -euo pipefail

cd "$(dirname "$0")/.."

OUT_DIR="${1:-build}"
DERIVED="$OUT_DIR/DerivedData"

mkdir -p "$OUT_DIR"

xcodebuild \
  -project Kind.xcodeproj \
  -scheme Kind \
  -configuration Release \
  -destination 'generic/platform=macOS' \
  -derivedDataPath "$DERIVED" \
  ONLY_ACTIVE_ARCH=YES \
  CODE_SIGN_STYLE=Manual \
  CODE_SIGN_IDENTITY=- \
  DEVELOPMENT_TEAM= \
  build -quiet

APP="$DERIVED/Build/Products/Release/Kind.app"
[[ -d "$APP" ]] || { echo "error: $APP was not produced" >&2; exit 1; }

rm -rf "$OUT_DIR/Kind.app"
ditto "$APP" "$OUT_DIR/Kind.app"
rm -rf "$DERIVED"
codesign --force --deep --sign - --timestamp=none "$OUT_DIR/Kind.app"
codesign --verify --deep --strict "$OUT_DIR/Kind.app"
echo "$OUT_DIR/Kind.app"
