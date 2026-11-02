#!/bin/sh
# Installs the FutureGadgets terminal tools (universal macOS binaries) from the latest GitHub Release.
#
#   curl -fsSL https://raw.githubusercontent.com/buzzer-re/FutureGadgets/main/install.sh | sh
#   curl -fsSL .../install.sh | sh -s -- mhtop          # only some tools
#
# Environment:
#   PREFIX   install under $PREFIX/bin   (default: ~/.local)
#   VERSION  release to install, e.g. 1.2.0   (default: latest)
set -eu

REPO="buzzer-re/FutureGadgets"
PREFIX="${PREFIX:-$HOME/.local}"
BIN="$PREFIX/bin"

if [ -n "${VERSION:-}" ]; then
  BASE="https://github.com/$REPO/releases/download/v${VERSION#v}"
else
  BASE="https://github.com/$REPO/releases/latest/download"
fi

die() { echo "error: $*" >&2; exit 1; }

[ "$(uname -s)" = Darwin ] || die "these tools are macOS only"

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

fetch() { curl -fsSL "$BASE/$1" -o "$TMP/$1" || die "cannot download $BASE/$1"; }

if [ $# -gt 0 ]; then
  TOOLS="$*"
else
  fetch tui-tools.txt
  TOOLS="$(cat "$TMP/tui-tools.txt")"
fi
[ -n "$TOOLS" ] || die "the release has no terminal tools"

mkdir -p "$BIN"
for tool in $TOOLS; do
  echo "==> $tool"
  fetch "$tool-macos.tar.gz"
  fetch "$tool-macos.tar.gz.sha256"
  (cd "$TMP" && shasum -a 256 -c "$tool-macos.tar.gz.sha256" >/dev/null) || die "checksum mismatch for $tool"
  tar -xzf "$TMP/$tool-macos.tar.gz" -C "$TMP" "$tool"
  install -m 755 "$TMP/$tool" "$BIN/$tool"
  echo "    installed $BIN/$tool"
done

case ":$PATH:" in
  *":$BIN:"*) ;;
  *) echo "note: $BIN is not on your PATH. Add it:  export PATH=\"$BIN:\$PATH\"" ;;
esac
