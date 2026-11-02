#!/bin/sh
# Installs the FutureGadgets terminal tools into $PREFIX/bin (default: ~/.local/bin).
#
# From a local clone, builds them from source:
#   ./install.sh                # all tools
#   ./install.sh mhtop          # only some
#
# Anywhere else, downloads the universal binaries from the latest GitHub Release:
#   curl -fsSL https://raw.githubusercontent.com/buzzer-re/FutureGadgets/main/install.sh | sh
#   curl -fsSL .../install.sh | sh -s -- mhtop
#
# Environment:
#   PREFIX   install under $PREFIX/bin   (default: ~/.local)
#   VERSION  release to download, e.g. 1.2.0   (default: latest; ignored for local builds)
set -eu

REPO="buzzer-re/FutureGadgets"
PREFIX="${PREFIX:-$HOME/.local}"
BIN="$PREFIX/bin"

die() { echo "error: $*" >&2; exit 1; }

[ "$(uname -s)" = Darwin ] || die "these tools are macOS only"

# Local clone: this file exists on disk next to install-gui.sh. When piped to `sh`, $0 is just "sh".
ROOT=""
if [ -f "$0" ]; then
  dir="$(cd "$(dirname "$0")" && pwd)"
  [ -f "$dir/install-gui.sh" ] && ROOT="$dir"
fi

mkdir -p "$BIN"

if [ -n "$ROOT" ]; then
  command -v make >/dev/null && command -v cc >/dev/null || die "make/cc not found (run: xcode-select --install)"
  if [ $# -gt 0 ]; then
    TOOLS="$*"
  else
    TOOLS=""
    for makefile in "$ROOT"/*/Makefile; do
      [ -e "$makefile" ] && TOOLS="$TOOLS $(basename "$(dirname "$makefile")")"
    done
  fi
  [ -n "$TOOLS" ] || die "no terminal tools found"
  for tool in $TOOLS; do
    [ -f "$ROOT/$tool/Makefile" ] || die "unknown tool: $tool"
    echo "==> $tool (building)"
    make -s -C "$ROOT/$tool"
    install -m 755 "$ROOT/$tool/$tool" "$BIN/$tool"
    make -s -C "$ROOT/$tool" clean
    echo "    installed $BIN/$tool"
  done
else
  if [ -n "${VERSION:-}" ]; then
    BASE="https://github.com/$REPO/releases/download/v${VERSION#v}"
  else
    BASE="https://github.com/$REPO/releases/latest/download"
  fi

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

  for tool in $TOOLS; do
    echo "==> $tool"
    fetch "$tool-macos.tar.gz"
    fetch "$tool-macos.tar.gz.sha256"
    (cd "$TMP" && shasum -a 256 -c "$tool-macos.tar.gz.sha256" >/dev/null) || die "checksum mismatch for $tool"
    tar -xzf "$TMP/$tool-macos.tar.gz" -C "$TMP" "$tool"
    install -m 755 "$TMP/$tool" "$BIN/$tool"
    echo "    installed $BIN/$tool"
  done
fi

case ":$PATH:" in
  *":$BIN:"*) ;;
  *) echo "note: $BIN is not on your PATH. Add it:  export PATH=\"$BIN:\$PATH\"" ;;
esac
