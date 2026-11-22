# Changelog

All notable changes to ModelForge are documented here.
The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and
this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [1.0.0] - 2026-11-21

First public release. Consolidates the functionality developed through the
0.1.0 – 1.2.0 internal iterations into a single documented 1.0.0 baseline.
The public version number starts at 1.0.0; it supersedes those internal builds.

### Added

- In-app software update: GitHub Release check with semantic version
  comparison, release notes preview, confirmed download with SHA256SUMS
  verification, and a platform-specific apply script that replaces files only
  after the running process exits (`/api/v1/updates/*`, Settings page entry).
- Local model library: scan Safetensors directories, parse `config.json`, shard
  indexes, tokenizer files and Safetensors headers.
- Model analysis: layer count, hidden size, attention/KV heads, vocabulary size,
  context length and parameter scale.
- Real GGUF conversion through llama.cpp `convert_hf_to_gguf.py`.
- Real quantization through `llama-quantize`, with quant types parsed from
  `--help` at runtime.
- Two-stage pipeline that keeps the intermediate GGUF and the quantized output
  separate.
- Task queue with priority, pause, cancel, retry and a concurrency limit.
- Realtime events over WebSocket, with SSE and HTTP polling fallback.
- Structured logs sourced from real child processes, with level filtering,
  search, pause-follow and export.
- Progress semantics that distinguish `measured`, `estimated` and
  `indeterminate` instead of inventing percentages.
- Hugging Face download center with resume, cancel, retry and token storage
  (DPAPI on Windows, 0600 file on POSIX).
- llama.cpp runtime manager: official release discovery, platform asset
  matching, SHA256 verification, update checks and portable runtime install.
- Quantization presets and the quantization lab.
- Model version tree, conversion history and workflow management.
- Perplexity (PPL) quality evaluation through `llama-perplexity`.
- Task-level resource sampling (CPU, memory, IO).
- Folder watching with automatic import.
- Dark / light / system themes and a Simplified Chinese / English UI.
- OpenAPI 3.0 document generated from the registered routes.
- Windows portable package plus cross-built Linux, macOS, Android (Termux) and
  openEuler packages.

### Notes

- Windows is built and validated on real hardware.
- POSIX targets are cross-compiled, linked and statically checked, but have not
  been executed on their target devices yet.
- iOS has no distributable artifact: the platform cannot host a long-lived
  background HTTP service and requires signing.
