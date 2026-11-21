#!/usr/bin/env bash
# macOS (arm64 + x86_64) build script. Produces a console daemon linked against
# macOS libSystem; distribution requires `codesign`/`notarization` on a Mac (see
# docs/cross-platform.md for the signing steps and why it cannot run on Windows).
# Usage:  bash scripts/build-macos.sh
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
source scripts/cross-common.sh

build_with dist/macos-arm64 modelforge aarch64-macos-none -pthread
build_with dist/macos-x64   modelforge x86_64-macos-none  -pthread

# launchd plist (installed under ~/Library/LaunchAgents on a real Mac)
cat > dist/macos-arm64/com.modelforge.plist <<'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>Label</key><string>com.modelforge</string>
  <key>ProgramArguments</key>
  <array>
    <string>/usr/local/opt/modelforge/modelforge</string>
    <string>serve</string>
    <string>--no-browser</string>
  </array>
  <key>RunAtLoad</key><true/>
  <key>KeepAlive</key><true/>
</dict>
</plist>
EOF
cp dist/macos-arm64/com.modelforge.plist dist/macos-x64/com.modelforge.plist

stage dist/macos-arm64 macos arm64 tar.gz
stage dist/macos-x64   macos x64   tar.gz
echo "macOS 构建完成（未签名；分发需在 macOS 上 codesign + notarize）。"
