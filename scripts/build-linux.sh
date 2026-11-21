#!/usr/bin/env bash
# Linux (x86_64 + aarch64) build script. Produces statically linked musl binaries so
# they run on any modern Linux distribution regardless of the installed glibc.
# Usage:  bash scripts/build-linux.sh
# Requires: zig (any 0.13/0.14), bash, tar.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
source scripts/cross-common.sh

build_with dist/linux-x64    modelforge x86_64-linux-musl    -pthread -static
build_with dist/linux-arm64  modelforge aarch64-linux-musl   -pthread -static

# systemd unit
mkdir -p dist/linux-x64 dist/linux-arm64
cat > dist/linux-x64/modelforge.service <<EOF
[Unit]
Description=ModelForge local model conversion service
After=network.target

[Service]
Type=simple
ExecStart=/opt/modelforge/modelforge serve --no-browser
Restart=on-failure
RestartSec=3
WorkingDirectory=/opt/modelforge
User=modelforge

[Install]
WantedBy=multi-user.target
EOF
cp dist/linux-x64/modelforge.service dist/linux-arm64/modelforge.service

stage dist/linux-x64   linux  x64   tar.gz
stage dist/linux-arm64 linux  arm64 tar.gz
echo "Linux 构建完成。"
