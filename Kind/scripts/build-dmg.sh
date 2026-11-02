#!/usr/bin/env bash
# Builds a universal (Apple silicon + Intel), ad-hoc signed Kind.app and packages it as a DMG.
#
#   scripts/build-dmg.sh [version]
#
# version       Marketing version to stamp into the app (e.g. 1.2.0). Defaults to the
#               project's MARKETING_VERSION.
# BUILD_NUMBER  Build number (CFBundleVersion). Defaults to 1.
# OUT_DIR       Output folder. Defaults to ./build.
#
# Ad-hoc signing needs no Apple Developer account, but Gatekeeper blocks the app on first
# launch because it isn't notarized. The release notes tell users how to open it anyway.
set -euo pipefail

cd "$(dirname "$0")/.."

VERSION="${1:-}"
BUILD_NUMBER="${BUILD_NUMBER:-1}"
OUT_DIR="${OUT_DIR:-build}"
DERIVED="$OUT_DIR/DerivedData"
STAGE="$OUT_DIR/dmg"

rm -rf "$OUT_DIR"
mkdir -p "$OUT_DIR"

build_args=(
  -project Kind.xcodeproj
  -scheme Kind
  -configuration Release
  -destination 'generic/platform=macOS'
  -derivedDataPath "$DERIVED"
  ARCHS="arm64 x86_64"
  ONLY_ACTIVE_ARCH=NO
  CODE_SIGN_STYLE=Manual
  CODE_SIGN_IDENTITY=-
  DEVELOPMENT_TEAM=
  CURRENT_PROJECT_VERSION="$BUILD_NUMBER"
)
if [[ -n "$VERSION" ]]; then
  build_args+=(MARKETING_VERSION="$VERSION")
fi

echo "==> Building Kind (Release, universal)"
xcodebuild "${build_args[@]}" build -quiet

APP="$DERIVED/Build/Products/Release/Kind.app"
[[ -d "$APP" ]] || { echo "error: $APP was not produced" >&2; exit 1; }

echo "==> Signing ad hoc"
codesign --force --deep --sign - --timestamp=none "$APP"
codesign --verify --deep --strict --verbose=2 "$APP"
lipo -info "$APP/Contents/MacOS/Kind"

if [[ -z "$VERSION" ]]; then
  VERSION="$(/usr/libexec/PlistBuddy -c 'Print CFBundleShortVersionString' "$APP/Contents/Info.plist")"
fi

echo "==> Packaging DMG"
mkdir -p "$STAGE"
ditto "$APP" "$STAGE/Kind.app"
ln -s /Applications "$STAGE/Applications"

DMG_NAME="Kind-$VERSION.dmg"
hdiutil create -volname "Kind $VERSION" -srcfolder "$STAGE" -fs HFS+ -format UDZO -ov "$OUT_DIR/$DMG_NAME" -quiet
hdiutil verify "$OUT_DIR/$DMG_NAME" -quiet
(cd "$OUT_DIR" && shasum -a 256 "$DMG_NAME" > "$DMG_NAME.sha256")

echo "==> Done"
echo "$OUT_DIR/$DMG_NAME"
cat "$OUT_DIR/$DMG_NAME.sha256"
