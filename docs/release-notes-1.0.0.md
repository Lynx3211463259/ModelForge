# ModelForge 1.0.0

## Downloads

<!-- DOWNLOADS:START -->
Choose the archive for your operating system and architecture. All builds are ZIP files.

### Windows

- [Windows x64 — portable](https://github.com/Lynx3211463259/ModelForge/releases/download/v1.0.0/ModelForge-1.0.0-win64.zip)

### macOS

- [macOS Apple Silicon (arm64)](https://github.com/Lynx3211463259/ModelForge/releases/download/v1.0.0/ModelForge-1.0.0-macos-arm64.zip)
- [macOS Intel (x64)](https://github.com/Lynx3211463259/ModelForge/releases/download/v1.0.0/ModelForge-1.0.0-macos-x64.zip)

### Linux

- [Linux x64 (static musl)](https://github.com/Lynx3211463259/ModelForge/releases/download/v1.0.0/ModelForge-1.0.0-linux-x64.zip)
- [Linux arm64 (static musl)](https://github.com/Lynx3211463259/ModelForge/releases/download/v1.0.0/ModelForge-1.0.0-linux-arm64.zip)

### Android

- [Android arm64 (Termux)](https://github.com/Lynx3211463259/ModelForge/releases/download/v1.0.0/ModelForge-1.0.0-android-arm64.zip)
- [Android x64 (Termux)](https://github.com/Lynx3211463259/ModelForge/releases/download/v1.0.0/ModelForge-1.0.0-android-x64.zip)

### openEuler

- [openEuler x64 (static musl)](https://github.com/Lynx3211463259/ModelForge/releases/download/v1.0.0/ModelForge-1.0.0-openeuler-x64.zip)
- [openEuler arm64 (static musl)](https://github.com/Lynx3211463259/ModelForge/releases/download/v1.0.0/ModelForge-1.0.0-openeuler-arm64.zip)

iOS: no distributable build (signing required; no long-lived background HTTP service). Source stays syntax-checked only.

Verify your download with `SHA256SUMS.txt` in the Assets section below.
<!-- DOWNLOADS:END -->

First public release of ModelForge — a local AI model management, conversion and
quantization workstation with a C++17 backend and a Vue 3 Web UI.

## Overview

ModelForge turns the usual ad-hoc conversion routine into a tracked pipeline:
models are scanned and analyzed, conversions run as real child processes through
llama.cpp, every artifact is validated and checksummed, and each output stays
linked to the task and log that produced it.

## Features

- Local model library: config, Safetensors headers, shard indexes and tokenizer parsing.
- Integrity checks across config, weights, shards and tokenizer files.
- Architecture analysis: layers, hidden size, attention/KV heads, vocabulary,
  context length and parameter scale.
- Real GGUF conversion via `convert_hf_to_gguf.py`.
- Real quantization via `llama-quantize`, quant types parsed from `--help`.
- Two-stage pipeline with an optional retained intermediate GGUF.
- Task queue with priority, pause, cancel, retry and a concurrency limit.
- Live events over WebSocket, with SSE and HTTP polling fallback.
- Structured logs from real stdout/stderr, with filtering, search and export.
- Honest progress states: `measured`, `estimated`, `indeterminate`.
- GGUF validation and streamed SHA256.
- Hugging Face download center: filtering, concurrent downloads, Range resume,
  pause/resume/cancel/retry, checksum verification and restart recovery.
- llama.cpp runtime manager: official release discovery, update checks,
  platform asset matching and SHA256 verification.
- Quantization presets and the quantization lab.
- Model version tree, conversion history and workflow management.
- PPL evaluation through `llama-perplexity`.
- Task-level CPU, memory and IO sampling.
- Folder watching with automatic import.
- Dark / light / follow-system themes and Simplified Chinese / English UI.
- OpenAPI 3.0.3 generated from the registered routes.

## Improvements

- Portable Windows package now embeds the application icon in `modelforge.exe`
  and ships favicon assets with the Web UI.
- Build scripts tolerate compiler warnings without aborting the build.

## Known Issues

- POSIX targets (Linux, macOS, Android, openEuler) are cross-compiled and
  statically checked but have not been run on their target devices yet.
- macOS binaries are unsigned; Gatekeeper will block them until signed and notarized.
- iOS has no distributable artifact: the platform cannot host a long-lived
  background HTTP service and requires signing.
- A single-node deployment only; remote conversion nodes are planned.

## Installation

Windows:

1. Download `ModelForge-1.0.0-win64.zip`.
2. Extract it anywhere.
3. Run `start.bat`, or `modelforge.exe serve --no-browser` from a terminal.
4. The server selects a free port and opens the browser.

Linux / macOS / Android (Termux) / openEuler:

```bash
unzip ModelForge-1.0.0-linux-x64.zip
cd linux-x64
./modelforge serve --no-browser
```

Android runs inside Termux: `bash run.sh`.

## Requirements

- Windows 10/11 x64 for the validated build.
- Python 3.10+ with the llama.cpp conversion dependencies for real conversions.
- Free disk space for the source model, the intermediate GGUF and the output.
- llama.cpp assets come from the bundled official release or the runtime page.

## Verification

- Windows backend and Web UI built with MinGW-w64 GCC 16.2.0; unit tests pass.
- Every archive's SHA256 is listed in `SHA256SUMS.txt`.
- Byte-identical `modelforge.exe` is embedded in the Windows package.

Verify a download with:

```bash
sha256sum -c SHA256SUMS.txt
```
