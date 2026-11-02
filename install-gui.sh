#!/bin/sh
# Installs the FutureGadgets GUI apps (macOS .app bundles) from the latest GitHub Release.
#
#   curl -fsSL https://raw.githubusercontent.com/buzzer-re/FutureGadgets/main/install-gui.sh | sh
#   curl -fsSL .../install-gui.sh | sh -s -- Kind       # only some apps
#
# Environment:
#   APP_DIR  where to put the apps   (default: /Applications, or ~/Applications if not writable)
#   VERSION  release to install, e.g. 1.2.0   (default: latest)
#
# Builds are ad-hoc signed, not notarized, so the quarantine flag is cleared after install.
set -eu

REPO="buzzer-re/FutureGadgets"

if [ -n "${VERSION:-}" ]; then
  BASE="https://github.com/$REPO/releases/download/v${VERSION#v}"
else
  BASE="https://github.com/$REPO/releases/latest/download"
fi

die() { echo "error: $*" >&2; exit 1; }

[ "$(uname -s)" = Darwin ] || die "these apps are macOS only"

if [ -z "${APP_DIR:-}" ]; then
  if [ -w /Applications ]; then APP_DIR=/Applications; else APP_DIR="$HOME/Applications"; fi
fi
mkdir -p "$APP_DIR"

TMP="$(mktemp -d)"
MNT="$TMP/mnt"
cleanup() {
  hdiutil detach "$MNT" -quiet 2>/dev/null || true
  rm -rf "$TMP"
}
trap cleanup EXIT

fetch() { curl -fsSL "$BASE/$1" -o "$TMP/$1" || die "cannot download $BASE/$1"; }

# gui-apps.txt: one "<App> <dmg file>" per line.
fetch gui-apps.txt
while read -r app dmg; do
  [ -n "$app" ] || continue
  if [ $# -gt 0 ]; then
    case " $* " in *" $app "*) ;; *) continue ;; esac
  fi
  echo "==> $app"
  fetch "$dmg"
  fetch "$dmg.sha256"
  (cd "$TMP" && shasum -a 256 -c "$dmg.sha256" >/dev/null) || die "checksum mismatch for $dmg"
  mkdir -p "$MNT"
  hdiutil attach "$TMP/$dmg" -nobrowse -readonly -quiet -mountpoint "$MNT" </dev/null || die "cannot mount $dmg"
  [ -d "$MNT/$app.app" ] || die "$app.app not found in $dmg"
  rm -rf "$APP_DIR/$app.app"
  ditto "$MNT/$app.app" "$APP_DIR/$app.app"
  hdiutil detach "$MNT" -quiet
  xattr -dr com.apple.quarantine "$APP_DIR/$app.app" 2>/dev/null || true
  echo "    installed $APP_DIR/$app.app"
done < "$TMP/gui-apps.txt"
