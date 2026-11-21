# Contributing to ModelForge

Thanks for your interest in improving ModelForge. This guide covers local setup,
build commands, tests and review expectations.

## Requirements

| Tool | Version | Notes |
| --- | --- | --- |
| CMake | 3.16+ | Build system for the C++ backend |
| C++ compiler | GCC 9+ / MSVC 2019+ | Release builds use MinGW-w64 GCC 16.2.0 |
| Node.js | 20+ | Frontend build |
| Python | 3.10+ | Required for real conversion through llama.cpp scripts |
| zig | 0.13 / 0.14 | Optional, for POSIX cross-builds |

## Build

Windows (PowerShell):

```powershell
scripts\build-windows.ps1 -ToolchainRoot C:\path\to\mingw64
```

The script builds the Web UI and the backend, then writes
`build-win\bin\modelforge.exe`.

Backend only:

```powershell
cmake -S . -B build-win -G "MinGW Makefiles" `
  -DCMAKE_C_COMPILER=C:/path/mingw64/bin/gcc.exe `
  -DCMAKE_CXX_COMPILER=C:/path/mingw64/bin/g++.exe
cmake --build build-win -j 4
```

Frontend only:

```bash
cd frontend
npm install
npm run build
```

POSIX cross-builds:

```bash
bash scripts/build-linux.sh
bash scripts/build-macos.sh
bash scripts/build-android.sh
bash scripts/build-openeuler.sh
```

Set `ZIG=/path/to/zig` when zig is not on `PATH`.

## Tests

```bash
ctest --test-dir build-win --output-on-failure
build-win/bin/modelforge_tests
```

Integration smoke tests live under `tests/integration` and require a running
server (`modelforge serve`).

## Code Style

- C++17, four-space indentation, no exceptions across module boundaries.
- Keep platform-specific code inside `#ifdef` blocks, and mirror the change on
  every supported platform instead of leaving a Windows-only path.
- Vue 3 `<script setup>` with TypeScript. Use the existing semantic CSS variables
  rather than hard-coded colors.
- User-facing strings go through the i18n dictionary; the Chinese source string
  is the key and an English translation is provided alongside it.

## Pull Requests

- Keep one logical change per pull request.
- Describe what changed, why, how it was tested, and any compatibility impact.
- Update `docs/` and `CHANGELOG.md` when behavior changes.
- Do not commit build outputs, models, databases, tokens or `release/` contents.

## Reporting Bugs

Open an issue with the bug report template. Include the version, operating
system, reproduction steps, expected and actual behavior, and logs with secrets
removed.
