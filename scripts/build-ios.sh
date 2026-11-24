#!/usr/bin/env bash
# iOS build — BLOCKED on this host.
#
# iOS does not allow arbitrary background HTTP servers: a console daemon has no
# deployment path, so ModelForge on iOS would have to become a normal app that hosts
# its own frontend inside a WKWebView and keeps the server in-process (which iOS
# suspends when backgrounded). That requires Apple's SDK (Xcode), a paid developer
# certificate, and App Store / ad-hoc signing — none of which exist on Windows and
# none of which can be reproduced with a cross-compiler.
#
# What this script does: attempts a syntax/type-only compile against the darwin
# stubs zig ships. Zig has no iOS libc headers without the Apple SDK, so on Windows
# the check stops at <sys/param.h>. It does NOT produce a runnable iOS artifact;
# a real check requires Xcode on macOS.
#
# Reproducible unblocking steps on a Mac:
#   1. Install Xcode + Command Line Tools.
#   2. xcodebuild with the iOS SDK and a SwiftUI/WKWebView shell that embeds this
#      binary and opens http://127.0.0.1:PORT/.
#   3. Sign with a valid Development/App Store certificate; submit for review.
#   4. Real device verification via TestFlight/DeviceSupport.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
source scripts/cross-common.sh

log "iOS：仅做语法/类型静态检查（zig darwin stub，无 Apple SDK，不产出可运行产物）。"
ZIG="${ZIG:-zig}"
"$ZIG" c++ -std=c++17 -fsyntax-only $CPPFLAGS \
    -I "$ROOT/backend/include" -I "$ROOT/backend/third_party" \
    -target aarch64-ios $SRC_CPP 2>&1 | grep -vE "warning: argument unused" || true
die "iOS 无法在 Windows 上构建/签名/分发：需要 Xcode、Apple SDK 与开发者证书（见 scripts/build-ios.sh 顶部说明）。"
