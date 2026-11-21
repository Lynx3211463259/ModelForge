param(
    [string]$LlamaCpp = "",
    [string]$ToolchainRoot = "",
    [string]$BuildDir = "build-modelforge"
)

# DEVELOPMENT ONLY. ModelForge ships the official llama.cpp release artifacts
# (see scripts/package-windows.ps1) and never distributes self-built binaries.
# Use this script only to debug a local llama.cpp checkout; its output must not
# be copied into a release package.

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
if (-not $LlamaCpp) {
    $LlamaCpp = Join-Path (Split-Path -Parent $Root) "llama.cpp"
}
if (-not (Test-Path (Join-Path $LlamaCpp "CMakeLists.txt"))) {
    throw "llama.cpp source not found: $LlamaCpp"
}
if (-not $ToolchainRoot) {
    throw "Pass -ToolchainRoot with a MinGW-w64 installation (bin/gcc.exe)."
}

$bin = Join-Path $ToolchainRoot "bin"
$env:PATH = "$bin;$env:PATH"
Push-Location $LlamaCpp
try {
    & cmake -S . -B $BuildDir -G "MinGW Makefiles" `
        -DCMAKE_BUILD_TYPE=Release `
        "-DCMAKE_C_COMPILER=$(Join-Path $bin 'gcc.exe')" `
        "-DCMAKE_CXX_COMPILER=$(Join-Path $bin 'g++.exe')" `
        "-DCMAKE_MAKE_PROGRAM=$(Join-Path $bin 'mingw32-make.exe')" `
        -DLLAMA_BUILD_EXAMPLES=ON `
        -DLLAMA_BUILD_TOOLS=ON `
        -DLLAMA_BUILD_SERVER=ON `
        -DLLAMA_BUILD_TESTS=OFF `
        -DLLAMA_CURL=OFF `
        -DGGML_NATIVE=ON `
        "-DCMAKE_CXX_FLAGS=-D_WIN32_WINNT=0x0A00" `
        "-DCMAKE_EXE_LINKER_FLAGS=-static -static-libgcc -static-libstdc++"
    if ($LASTEXITCODE -ne 0) { throw "llama.cpp configure failed" }

    & cmake --build $BuildDir --target llama-quantize llama-cli llama-bench llama-server -j 6
    if ($LASTEXITCODE -ne 0) { throw "llama.cpp build failed" }

    Write-Host "llama.cpp tools built in $LlamaCpp\$BuildDir\bin"
}
finally {
    Pop-Location
}
