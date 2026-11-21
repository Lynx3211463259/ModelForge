#!/usr/bin/env bash
# openEuler (x86_64 + aarch64) build script. openEuler is a Linux distribution;
# statically linked musl binaries avoid glibc-version coupling with the host. The
# systemd unit below matches the standard /usr/lib/systemd layout of openEuler.
# Usage:  bash scripts/build-openeuler.sh
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
source scripts/cross-common.sh

build_with dist/openeuler-x64   modelforge x86_64-linux-musl  -pthread -static
build_with dist/openeuler-arm64 modelforge aarch64-linux-musl -pthread -static

cat > dist/openeuler-x64/modelforge.service <<'EOF'
[Unit]
Description=ModelForge local model conversion service
After=network.target

[Service]
Type=simple
ExecStart=/usr/local/bin/modelforge serve --no-browser
Restart=on-failure
RestartSec=3

[Install]
WantedBy=multi-user.target
EOF
cp dist/openeuler-x64/modelforge.service dist/openeuler-arm64/modelforge.service

stage dist/openeuler-x64   openeuler x64   tar.gz
stage dist/openeuler-arm64 openeuler arm64 tar.gz
echo "openEuler 构建完成。"
