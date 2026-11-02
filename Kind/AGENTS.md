# AGENTS.md

Guidance for coding agents working on **Kind**, a native macOS (SwiftUI) Kindle manager.
Read the README first for the product overview and developer commands. This file covers
the conventions and the non-obvious facts learned from running against real hardware.

## Ground rules

- **No third-party code.** The MTP driver, book parsers and sidecar decoder are written from
  scratch on purpose. Don't add Swift packages, vendored libraries, or code ported from
  libmtp or Calibre. Apple frameworks are fine.
- **Never commit real device or user data.** That covers Kindle serial numbers, book titles and
  file names from a real device, ASINs, reading timestamps, sidecar dumps, local paths,
  usernames and hostnames. If a real file is needed as a test fixture, anonymize it first:
  swap titles and ASINs for invented ones, overwrite timestamps with synthetic values, and keep
  the byte structure (see `KindTests/RealSidecarTests.swift`). Before committing, grep the
  staged diff for `/Users/`, serial-like strings and personal names.
- Keep debugging artifacts (dumps, traces, screenshots) out of the repo, and delete them when
  you're done.

## Commands

```sh
xcodebuild -project Kind.xcodeproj -scheme Kind -destination 'platform=macOS' build
xcodebuild -project Kind.xcodeproj -scheme Kind -destination 'platform=macOS' -only-testing:KindTests test
xcodebuild -project Kind.xcodeproj -scheme Kind -destination 'platform=macOS' test   # + UI tests
```

Unit tests must pass with no Kindle attached. Run them after any change under `Core/` or
`Device/`.

## Project mechanics

- **Folder-synchronized groups:** any file created under `Kind/`, `KindTests/` or `KindUITests/`
  joins its target automatically. Don't hand-edit `project.pbxproj` to add files. Change build
  settings through Xcode (or the Xcode tooling), not by editing the pbxproj text.
- **Swift 5 mode, `SWIFT_DEFAULT_ACTOR_ISOLATION = MainActor`, approachable concurrency.**
  Unannotated types in the app target are main-actor isolated. So:
  - Mark every non-UI value type, enum and helper `nonisolated` (parsers, models, protocol
    code). Existing files show the pattern.
  - UI state is `@Observable` and lives on the main actor (`AppModel`, `DeviceManager`,
    `KindleDevice`, `TransferCenter`).
  - The test targets do **not** default to MainActor.
- **Don't edit sources with `sed -i`** (or any tool that writes a temp file beside the target).
  The synchronized folder picks up the temp file as a resource, and the next build fails with
  "couldn't be opened because there is no such file". Rewrite files in place instead.
- **A test target that fails to compile can still "run".** Xcode may execute the previous test
  binary, and new tests show up as "No result" instead of failing. When a new test reports no
  result, check the build log for compile errors in the test target first.
- **Upcoming feature `MemberImportVisibility` is on:** import every module whose members a
  file uses (`import os`, `import SwiftUI`, `import UniformTypeIdentifiers`, …), even if another
  file already imports it. This applies to test files too.
- Extensions of `nonisolated` types that live in SwiftUI files still default to MainActor.
  Mark them `nonisolated extension` if non-UI code or tests use them.

## Architecture

```
UI (SwiftUI)  →  AppModel / KindleDevice / TransferCenter  (@Observable, MainActor)
              →  LibraryScanner, LibraryActions           (parsing, edits, uploads)
              →  KindleFileSystem protocol
                   ├─ MassStorageFileSystem  (mounted FAT volume, older Kindles)
                   └─ MTPFileSystem (actor)  →  MTPSession  →  MTPTransport
                                                                 ├─ USBMTPTransport (IOUSBHost)
                                                                 └─ FakeMTPDevice   (tests)
```

- Everything above `KindleFileSystem` is transport-agnostic, so new features should go
  through that protocol.
- `MTPFileSystem` is an actor whose executor is a private `DispatchSerialQueue`. `MTPSession`
  does blocking USB I/O and must only run on that queue. It is `@unchecked Sendable` because of
  that confinement, so keep it confined.
- Parsers read byte ranges through `RandomAccessSource`, so scanning over USB fetches only the
  headers it needs. Keep new parsers range-based. Don't download whole books to read metadata.
- All device-mutating work (upload, delete, edit, export) goes through `TransferCenter.enqueue`,
  which serializes operations and publishes progress.
- Metadata edits are made on a downloaded copy and then uploaded back. Never write in place on
  the device.

## USB / MTP facts learned on real hardware

These all caused real failures. Don't regress them.

1. **Interface matching:** an IOKit matching dictionary of `IOUSBHostInterface` + `idVendor`
   alone never matches, because interface matching follows USB class-spec key combinations.
   `USBDeviceMonitor` matches every `IOUSBHostInterface` and filters by vendor in code.
2. **Kindle interface shape (a current Kindle Paperwhite):** vendor `0x1949`, interface class `0xFF`,
   subclass `0xFF`, `kUSBString = "MTP"`. It is not the PTP class `6`. Bulk IN `0x81`, bulk
   OUT `0x01`, interrupt `0x82`, 512-byte packets.
3. **IOUSBHost DMA buffers can't be resized.** Setting `.length` on an
   `ioData(withCapacity:)` buffer crashes the process. A transfer moves exactly `length` bytes,
   so allocate at the exact size (see the size-keyed cache in `USBMTPTransport`).
4. **Refined Swift names:** several IOUSBHost methods are `NS_REFINED_FOR_SWIFT`. Use
   `__sendIORequest(with:bytesTransferred:completionTimeout:)`, `__abort(with:)` and
   `IOUSBHostInterface(__ioService:options:queue:interestHandler:)`. When unsure, check the
   SDK headers instead of guessing.
5. **Only one process may own the interface** ("Exclusive open of usb object failed").
   `.deviceSeize` does not evict another user-space client. Other MTP apps, and stale copies of
   Kind, block us. `DeviceManager` retries for 30 s and shows a "Kindle is in use" message
   with Retry.
6. **Stale sessions:** a Kind process killed mid-session leaves the Kindle with an open
   session and queued packets. `MTPSession.open()` drains the IN pipe first, and
   transactions skip containers whose transaction ID doesn't match. The app closes the
   session on quit (`closeImmediately`), cancelling in-flight I/O first so quitting can't hang.
7. **Anything that launches the app grabs the Kindle.** That includes Xcode previews, code
   snippets and UI tests. `DeviceManager.start()` bails out when `XCODE_RUNNING_FOR_PREVIEWS=1`
   or the `-KindDisableDevices` argument is present. Keep that guard. When probing hardware
   from a snippet, first stop any running copy of the app.
8. A data phase ending exactly on a packet boundary is followed by a zero-length packet. The
   session writes one after such sends and tolerates empty reads before responses.

## Updates

- `UpdateChecker` asks GitHub's `releases/latest` API, compares the tag with
  `CFBundleShortVersionString` using `AppVersion`, and downloads the release's `.dmg` with its
  `.sha256` checksum when a release has one. Releases of this repo ship no DMG, so the checker
  falls back to opening the release page.
- Automatic checks are off in previews, under XCTest and with `-KindDisableUpdateCheck` (the UI
  tests pass it). Keep test and preview runs off the network.

## Kindle on-disk facts

- Books live under `documents/` (store purchases and samples go under
  `documents/Downloads/Items01/`). `documents/dictionaries` is ignored.
- Each book has a `<name>.sdr/` sidecar folder with **several generations** of KRDS files:
  `.azw3f/.azw3r/.mbs/.mbp1` (older), and, for PDFs the Kindle has converted,
  `<uuid>/book.sdr/*.yjf|.yjr` (current). Pick the **newest `lpr` by timestamp**, not the
  first file found.
- KRDS: signature `00 00 00 00 00 1A B1 26`, typed big-endian values, fields bracketed by
  tags `0xFE`/`0xFF`. UTF strings are `u8 isNull, u16 length, bytes`. `lpr` is
  `[byte version, string position, long epochMillis]`.
- Position formats:
  - MOBI byte offset (`"12345"`).
  - Plain page number.
  - KFX-style `BASE64EID:N`. For converted PDFs, the page index is `N >> 16` (matched against
    real devices).
  - Store KFX books: no page count is available, so fall back to `book.info.store` percent or
    last-read date only.
- `book.info.store` → `[long num.words, double percent.of.book]`. Stale files often hold
  misleadingly tiny values, so only trust the percentage from the newest record.
- Amazon writes `:` as `_` in file names (`It_ A Subtitle_B0XXXXXXXX_sample.kfx`).
  `Book.cleanTitle` handles that and `Title - Author` names.
- Store-book covers come from `system/thumbnails/thumbnail_<ASIN>_<cdeType>_portrait.jpg`.
- Folders scanned from a real device include large `book.kdf` files (tens of MB). The scanner
  skips sidecars over 4 MB, so keep that limit.

## Code style

- Match the surrounding code: small focused types, `nonisolated` value types, doc comments
  that explain *why* (format quirks, hardware behavior) rather than restating the code.
- Errors shown to users are plain sentences (`LocalizedError`) that say what to do next.
- UI: SwiftUI only, animations via `withAnimation(.smooth / .snappy / .spring)`, serif design
  for titles, `Theme` for shared constants. Provide `#Preview`s using `PreviewSupport` for new
  views, and keep previews hardware-free.
- Log with `Logger(subsystem: "app.kind", category: …)`. Use `.notice` or higher for lifecycle
  events (device found, session open, library ready, failures) so they appear without extra
  log-level flags. Keep `.info`/`.debug` for detail.

## Verifying changes

1. Build, then run the unit tests.
2. For UI changes, render the relevant `#Preview` and look at it.
3. For device changes, run the app with a Kindle attached and watch the `app.kind` log for
   `MTP session open` and `Library ready: N books …`. Check `ioreg` for `UsbExclusiveOwner` if
   it doesn't connect.
4. Before committing, review the staged diff for personal data (see Ground rules).
