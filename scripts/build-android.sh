#!/usr/bin/env bash
# Android (Termux) build script. ModelForge on Android is a console daemon that runs
# inside Termux (or adb shell). Statically linked musl binaries need no bionic and
# only the Linux syscall ABI, so they run unmodified in Termux.
# Usage:  bash scripts/build-android.sh
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
source scripts/cross-common.sh

build_with dist/android-arm64 modelforge aarch64-linux-musl -pthread -static
build_with dist/android-x64   modelforge x86_64-linux-musl  -pthread -static

# Termux service script (run inside Termux: bash run.sh)
cat > dist/android-arm64/run.sh <<'EOF'
#!/data/data/com.termux/files/usr/bin/bash
# Run ModelForge inside Termux. Port 8080 maps to the local device only.
cd "$(dirname "$0")"
pkg install -y curl python 2>/dev/null || true
./modelforge serve --no-browser
EOF
cp dist/android-arm64/run.sh dist/android-x64/run.sh

stage dist/android-arm64 android arm64 tar.gz
stage dist/android-x64   android x64   tar.gz
echo "Android 构建完成（Termux 控制台形态）。"
