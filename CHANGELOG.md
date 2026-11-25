# Changelog

All notable changes to ModelForge are documented here.
The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and
this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [1.1.0-beta.2] - 2026-11-25

### Added

- The quantization picker now estimates the output size of every supported quant
  type from the selected model and shows its share of the unquantized baseline
  (`不量化（仅输出 F16/F32） · 约 X（基准 100%）`, `Q4_K_M · 约 X（占不量化 F16 的 Y%）`).
  Options update on model / intermediate-precision / quant changes; missing
  parameter counts or unknown bit widths produce an explicit notice instead of a
  number, and descriptions no longer hardcode percentages. The quantization lab
  shows the same ratios and excludes non-estimable types from its charts.

### Fixed

- The directory/file picker now opens above the dialog that launched it. Every
  `AppModal` shares one stack (`components/modalStack.ts`), so the newest dialog
  gets the highest z-index, Escape closes only the topmost one, body scroll stays
  locked until the last dialog closes, and focus returns to the previous control.


## [1.1.0-beta.1] - 2026-11-24

### Added

- **Startup update check** for both ModelForge and llama.cpp, controlled by the
  new `autoCheckUpdates` setting (default on) with a switch next to the update
  channel in Settings. `GET /api/v1/updates/startup` checks both components and
  returns only actionable updates; the UI shows a top banner with component,
  current → latest version, channel/badge, asset size and an **Update** button
  that runs the matching flow (app download → apply, or official llama.cpp
  asset install with progress). Offline, rate-limited, failed or up-to-date
  checks never block startup and show no banner. Results are cached for 10
  minutes per browser session; dismissing the banner lasts for that session.

- Hugging Face **dataset downloads**: `/api/v1/huggingface/resolve` and
  `/api/v1/downloads` accept `repoType=dataset` (auto-detected from
  `huggingface.co/datasets/...` URLs). Dataset files are classified as
  parquet/arrow/csv/jsonl/text/image/audio/video/archive/tensor, land in the
  new `datasetsDir`, and are registered in the offline library instead of the
  model table. The download page has a model/dataset switch.
- **Multiple co-installed llama.cpp backends**: each official release extracts
  into its own `releases/<asset>` folder and is tracked in `backends.json`.
  `GET /api/v1/llama/backends`, `POST /api/v1/llama/backends/{id}/activate`,
  `DELETE /api/v1/llama/backends/{id}` and
  `POST /api/v1/llama/backends/refresh`. Switching only rewrites tool paths and
  `modelforge-runtime.json`; the active backend cannot be deleted and the last
  backend is always kept.
- **Offline resource library** (`offline_library`): one registry for local
  models, datasets and llama.cpp backends with on-disk presence/size checks.
  `GET /api/v1/library`, `POST /api/v1/library/rescan`,
  `POST /api/v1/library/export`, `POST /api/v1/library/import`,
  `POST /api/v1/library/verify`, `GET /api/v1/library/bundles` and
  `DELETE /api/v1/library/{id}` (unregisters only, never deletes files).
  Bundles contain `modelforge-offline-bundle.json` plus `SHA256SUMS.txt` and can
  optionally copy every resource file for air-gapped machines.
- **Dependency health self-check** (`health_check`): database integrity,
  directory existence/writability, free disk space, Python and conversion
  dependencies, convert script, every llama.cpp tool, backend inventory,
  offline-library integrity, listening/web-root settings, and an opt-in network
  probe for the Hugging Face token. Exposed as `GET|POST
  /api/v1/health/check`, `GET /api/v1/health/summary` and
  `POST /api/v1/health/fix` (safe fixes: create dirs, re-detect tool paths,
  rescan backends, rescan library). New **离线资源库 / Library** and
  **健康自检 / Health check** pages.
- `datasetsDir` setting (Settings → 目录与运行) with a graphical picker.

### Changed

- `HfClient::classifyFile()` takes the repository type; dataset repositories use
  their own category set. `apiModelUrl`/`apiTreeUrl`/`resolveUrl` gained an
  optional `repoType` argument (default `model`, existing callers unchanged).
- `download_tasks` gained a `repo_type` column (additive migration).
- Backend detection now recognises `.so`/`.dylib` names and falls back to the
  official asset name, so Linux/macOS releases and static builds are classified
  correctly.

## [1.0.1] - 2026-11-23

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

- File mode for the picker: settings fields for executables and scripts now
  have a Browse button, with a read-only `/api/v1/fs/file` validation endpoint
  and file entries in `/api/v1/fs/list?files=true`.
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



- Switching the UI language no longer remounts the Settings page with stale

  server data (the router view is no longer keyed by locale). The selected

  language is now written explicitly to the form and persisted immediately;

  reload restores it from the server in both directions.

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
