# FutureGadgets

A collection of tools I built to solve random problems and improve quality of life. Most of them are built with AI agents. The repo keeps their source only: binaries come from [Releases](../../releases).

| Tool | Kind | Description |
| --- | --- | --- |
| [macdirstat](macdirstat) | TUI | A terminal implementation of the Windows tool [WinDirStat](https://windirstat.net), written in plain C with no dependencies. |
| [mhtop](mhtop) | TUI | A better `htop` for macOS, written in plain C with no dependencies. |
| [Kind](Kind) | GUI | A Kindle manager for macOS, written in Swift. |

## Install

Terminal tools (all of them, into `~/.local/bin`):

```sh
curl -fsSL https://raw.githubusercontent.com/buzzer-re/FutureGadgets/main/install.sh | sh
```

GUI apps (into `/Applications`):

```sh
curl -fsSL https://raw.githubusercontent.com/buzzer-re/FutureGadgets/main/install-gui.sh | sh
```

Both accept tool names to install only some (`... | sh -s -- mhtop`), and honor `VERSION` to pin a release, e.g. `VERSION=1.0.0`. `install.sh` also honors `PREFIX`; `install-gui.sh` honors `APP_DIR`.

## Build from source

Each folder is self-contained. See its own README or Makefile.

```sh
make -C mhtop          # TUI tools: produces ./<tool>/<tool>
make -C macdirstat
Kind/scripts/build-dmg.sh   # GUI apps: produces Kind/build/Kind-<version>.dmg
```

## Releases

Pushing a tag like `v1.0.0` runs [`release.yml`](.github/workflows/release.yml), which discovers tools by convention and publishes everything in one release:

- `<dir>/Makefile` makes it a TUI tool. CI runs `make` and publishes a universal (arm64 + x86_64) `<dir>-macos.tar.gz` with a `.sha256`.
- `<dir>/scripts/build-dmg.sh` makes it a GUI app. CI runs `scripts/test.sh` if present, then the script, and publishes the DMG with a `.sha256`.

The workflow also publishes `tui-tools.txt` and `gui-apps.txt`, which the installers read.
