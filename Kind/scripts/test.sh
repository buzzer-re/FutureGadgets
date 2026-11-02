#!/usr/bin/env bash
# Unit tests only: they need no hardware. UI tests need a logged-in GUI session.
set -euo pipefail
cd "$(dirname "$0")/.."
xcodebuild -project Kind.xcodeproj -scheme Kind -destination 'platform=macOS' \
  -only-testing:KindTests test -quiet
