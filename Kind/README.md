# Kind — a Kindle manager for macOS

Plug in a Kindle and browse, add, edit and export its books. No Android File Transfer,
Calibre or other third-party code: the MTP driver, the book parsers and the Kindle sidecar
decoder are all written from scratch in this project.

## How it talks to the Kindle

| Kindle | Transport | Code |
| --- | --- | --- |
| ~2021 and newer (Paperwhite 5, Scribe, Kindle 11…) | **MTP** over raw USB bulk pipes (IOUSBHost, user space) | `Device/MTP`, `Device/USB` |
| Older models | USB mass storage (mounted volume) | `Device/MassStorage` |

Both transports implement `KindleFileSystem`, so everything above that layer is shared.
The MTP interface is opened with `.deviceSeize`, which makes macOS' Image Capture daemon
let go of the device.

## Development

### Requirements

- macOS 26 or later
- Xcode 26 or later (the project uses folder-synchronized groups, `objectVersion = 110`)
- A Kindle and a USB data cable, for anything that touches hardware

No package dependencies. Nothing to install.

### Build and run

```sh
open Kind.xcodeproj          # then ⌘R with the "Kind" scheme and "My Mac"
```

or from the command line:

```sh
xcodebuild -project Kind.xcodeproj -scheme Kind -destination 'platform=macOS' build
```

Signing is set to Automatic with no team, which is enough to run locally. To distribute a
build, pick your team under *Signing & Capabilities*. The App Sandbox stays off (see below).

### Tests

```sh
# Unit tests only: parsers, progress, and the MTP driver against an in-memory fake device.
xcodebuild -project Kind.xcodeproj -scheme Kind -destination 'platform=macOS' \
  -only-testing:KindTests test

# Everything, including UI tests
xcodebuild -project Kind.xcodeproj -scheme Kind -destination 'platform=macOS' test
```

Unit tests need no hardware. UI tests launch the app with `-KindDisableDevices`, so they never
touch a connected Kindle. Pass the same argument (*Scheme → Run → Arguments*) to work on the UI
without a device grabbing the session.

SwiftUI previews are the fastest way to iterate on UI: `Kind/UI/PreviewSupport.swift` builds a
populated fake library. Previews also never connect to hardware.

### Install

Kind is not published as a binary. The repo-root installer clones this repo, builds Kind with
Xcode for this Mac's architecture and copies it to `/Applications`:

```sh
curl -fsSL https://raw.githubusercontent.com/buzzer-re/FutureGadgets/main/install-gui.sh | sh
```

To build without installing, run `scripts/build-app.sh` (writes `build/Kind.app`). The build is
ad-hoc signed, which needs no Apple Developer account. A locally built app has no quarantine
flag, so Gatekeeper lets it open.

### Update checks

Kind checks `api.github.com/repos/buzzer-re/FutureGadgets/releases/latest` a few seconds after launch,
then every 12 hours (and from *Kind → Check for Updates…*). When the latest release tag is newer
than the running version, a card appears at the bottom of the sidebar. From there the user can
read the release notes, skip that version, or download the update. Releases here carry no DMG, so
*download* opens the release page; update by running the installer again. (When a release does
include a `.dmg`, Kind saves it to Downloads, checks it against the release's `.sha256` file and
opens it.)

- The repository and its releases must be **public**. The check runs without a token, and private
  repositories answer 404, which Kind reports as "no releases".
- Tags must be plain versions (`v1.2.3`). Dev builds report `MARKETING_VERSION` from the
  project, so they only see releases newer than that.
- Previews, tests and the `-KindDisableUpdateCheck` launch argument turn automatic checks off.

### Debugging with a Kindle

- **Logs:** the app logs under the `app.kind` subsystem.
  ```sh
  log stream --predicate 'subsystem == "app.kind"' --level info
  ```
  Look for `Kindle MTP interface found`, `MTP session open` and `Library ready`.
- **Is the Kindle visible to macOS?**
  ```sh
  ioreg -p IOService -l -w0 -r -c IOUSBHostInterface | grep -E 'MTP@|idVendor|UsbExclusiveOwner'
  ```
  Kindles use vendor ID `6473` (0x1949). `UsbExclusiveOwner` shows which process holds it.
- **"The Kindle is in use by another app":** only one process can own the MTP interface. Quit
  Android File Transfer, OpenMTP, Calibre, or a leftover copy of Kind:
  ```sh
  pkill -9 -f Kind.app/Contents/MacOS/Kind
  ```
  If it's still stuck, unplug and replug the Kindle.
- **Reset cached data:** parsed metadata and covers are cached per device. Delete these to
  force a full rescan:
  ```
  ~/Library/Application Support/app.kind.Kind/Libraries/
  ~/Library/Caches/app.kind.Kind/Covers/
  ```

## Layout

```
Kind/
  Core/Binary     byte reader/writer, random-access sources
  Core/Formats    MOBI/AZW/AZW3 read+rewrite, PDF (PDFKit), KFX metadata, KRDS sidecars → progress
  Device          filesystem protocol, MTP protocol/session/actor, IOKit hot-plug, device model
  Library         Book model, scanner, caches, transfer queue, actions (upload/delete/export/edit)
  UI              SwiftUI: sidebar, cover grid/table, inspector, metadata editor, transfer tray
KindTests         parser tests + a fake MTP device that drives the real session code
KindUITests       launch/smoke tests (run with devices disabled)
```

See [AGENTS.md](AGENTS.md) for conventions and the hard-won details of working with the
USB/MTP stack.

## Notes and limits

- **Reading progress** comes from the `.sdr` sidecar files the Kindle writes
  (`percent.of.book`, last-read position and time, highlights). Some KFX books only store an
  opaque position, so the app shows "Reading" and the last-read date without a percentage.
- **Metadata editing** covers MOBI/AZW3 (EXTH records, embedded cover, and the Kindle's
  `system/thumbnails` image) and PDF (info dictionary; a new cover is optionally inserted
  as page one, since that is what the Kindle shows). DRM-protected books are read-only.
  Edits are made on a downloaded copy and then sent back, so a failed write can't damage the
  book on the device.
- The App Sandbox is off because the app needs raw USB access and access to mounted volumes.
  That's normal for device utilities, but it rules out the Mac App Store as-is.
