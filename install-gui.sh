#!/bin/sh
# Builds and installs the FutureGadgets GUI apps from source, on this Mac.
#
#   curl -fsSL https://raw.githubusercontent.com/buzzer-re/FutureGadgets/main/install-gui.sh | sh
#   curl -fsSL .../install-gui.sh | sh -s -- Kind       # only some apps
#
# Needs git and Xcode (xcodebuild). Clones the repo into a temp folder, runs each app's
# scripts/build-app.sh, copies the .app into place and deletes the clone.
#
# Environment:
#   APP_DIR  where to put the apps   (default: /Applications, or ~/Applications if not writable)
#   REF      branch or tag to build  (default: main)
set -eu

REPO_URL="https://github.com/buzzer-re/FutureGadgets.git"
REF="${REF:-main}"

die() { echo "error: $*" >&2; exit 1; }

[ "$(uname -s)" = Darwin ] || die "these apps are macOS only"
command -v git >/dev/null || die "git not found (run: xcode-select --install)"
xcodebuild -version >/dev/null 2>&1 || die "Xcode not found. Install it from the App Store, then run: sudo xcode-select -s /Applications/Xcode.app"

if [ -z "${APP_DIR:-}" ]; then
  if [ -w /Applications ]; then APP_DIR=/Applications; else APP_DIR="$HOME/Applications"; fi
fi
mkdir -p "$APP_DIR"

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

echo "==> Cloning $REPO_URL ($REF)"
git clone --quiet --depth 1 --branch "$REF" "$REPO_URL" "$TMP/src" </dev/null

# A GUI app is any top-level folder with scripts/build-app.sh; the folder name is the app name.
APPS="$*"
if [ -z "$APPS" ]; then
  for script in "$TMP"/src/*/scripts/build-app.sh; do
    [ -e "$script" ] || continue
    APPS="$APPS $(basename "$(dirname "$(dirname "$script")")")"
  done
fi
[ -n "$APPS" ] || die "no GUI apps found"

for app in $APPS; do
  script="$TMP/src/$app/scripts/build-app.sh"
  [ -x "$script" ] || die "unknown app: $app"
  echo "==> Building $app"
  "$script" "$TMP/out/$app" </dev/null
  rm -rf "$APP_DIR/$app.app"
  ditto "$TMP/out/$app/$app.app" "$APP_DIR/$app.app"
  echo "    installed $APP_DIR/$app.app"
done
