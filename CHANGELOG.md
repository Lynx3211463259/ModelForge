# Changelog

All notable changes to ModelForge are documented here.
The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and
this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

- Deleting a model can now remove its files from disk. `DELETE /models/{id}`
  accepts `deleteFiles=true`; the Web UI always uses it after an explicit
  confirmation and reports removed count, freed space and warnings.
- Deletion refuses to run while the model is referenced by an active
  conversion task or an enabled workflow (`409 MODEL_IN_USE`), keeps files
  shared with other model records or located outside the model directory, and
  keeps the record when a file cannot be removed so the user can retry.

- Update channels for the in-app updater: `stable` reads `/releases/latest`,
  `nightly` prefers prereleases (falling back to stable), and `auto` picks the
  highest version of any kind. Selectable in Settings, persisted as
  `updateChannel`, and reported back as `channel` in `/api/v1/updates/check`.

### Added

- A graphical directory picker for settings and model import: lazy-loaded folder
  tree with expand/collapse, location shortcuts, up navigation, direct path
  entry, selected-path display and confirm/cancel. Backed by read-only
  `/api/v1/fs/roots` and `/api/v1/fs/list` endpoints that report not-found,
  not-a-directory, permission and truncation states.

### Changed

- `--port` and `--no-browser` are now runtime-only flags and no longer overwrite
  saved settings. Port fallback never rewrites the configured port; `/health`
  reports the actual port, the configured port and the fallback reason, and the
  UI shows a prominent banner for the duration of the fallback.

### Fixed

- Invalid port values are rejected with `400 SETTINGS_INVALID` instead of being
  silently reset to the default port.
- Settings page shows the running executable's version from `/api/v1/health`
  before any update check, and refreshes it after a self-update restarts the
  process. A single root `VERSION` file now feeds CMake, POSIX cross-builds,
  Windows packaging and release scripts so filenames and reported versions
  cannot drift apart.

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
