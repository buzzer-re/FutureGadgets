# FutureGadgets

A collection of tools I built to solve random problems and improve quality of life. Most of them are built with AI agents. The repo keeps their source only. The terminal tools' binaries come from [Releases](../../releases); GUI apps are built on your machine.

| Tool | Kind | Description |
| --- | --- | --- |
| [macdirstat](macdirstat) | TUI | A terminal implementation of the Windows tool [WinDirStat](https://windirstat.net), written in plain C with no dependencies. |
| [mhtop](mhtop) | TUI | A better `htop` for macOS, written in plain C with no dependencies. |
| [Kind](Kind) | GUI | A Kindle manager for macOS, written in Swift. |

## Install

Terminal tools: downloads the release binaries (all of them, into `~/.local/bin`):

```sh
curl -fsSL https://raw.githubusercontent.com/buzzer-re/FutureGadgets/main/install.sh | sh
```

GUI apps: clones this repo, builds with Xcode and installs into `/Applications` (needs `git` and Xcode):

```sh
curl -fsSL https://raw.githubusercontent.com/buzzer-re/FutureGadgets/main/install-gui.sh | sh
```

Both accept tool names to install only some (`... | sh -s -- mhtop`). `install.sh` honors `PREFIX` and `VERSION` (pin a release, e.g. `VERSION=1.0.0`). `install-gui.sh` honors `APP_DIR` and `REF` (branch or tag to build).

## Build from source

Each folder is self-contained. See its own README or Makefile.

```sh
make -C mhtop          # TUI tools: produces ./<tool>/<tool>
make -C macdirstat
Kind/scripts/build-app.sh   # GUI apps: produces Kind/build/Kind.app
```

## Releases

Pushing a tag like `v1.0.0` runs [`release.yml`](.github/workflows/release.yml). Every folder with a `Makefile` is built as a universal (arm64 + x86_64) binary and published as `<dir>-macos.tar.gz` with a `.sha256`. The workflow also publishes `tui-tools.txt`, which `install.sh` reads.

GUI apps are not released. Any folder with `scripts/build-app.sh` is picked up by `install-gui.sh`.
