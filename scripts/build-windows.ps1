param(
    [string]$BuildDir = "build-win",
    [string]$ToolchainRoot = "",
    [switch]$SkipFrontend
)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
Push-Location $Root
try {
    if (-not $SkipFrontend) {
        Push-Location (Join-Path $Root "frontend")
        if (-not (Test-Path "node_modules")) {
            npm install --no-audit --no-fund
            if ($LASTEXITCODE -ne 0) { throw "npm install failed" }
        }
        # Native tools write progress/warnings to stderr. With
        # ErrorActionPreference=Stop that would abort the script, so judge
        # success by the exit code instead.
        $nativeErrorAction = $ErrorActionPreference
        $ErrorActionPreference = "Continue"
        try {
            npm run build
            if ($LASTEXITCODE -ne 0) { throw "frontend build failed ($LASTEXITCODE)" }
        }
        finally {
            $ErrorActionPreference = $nativeErrorAction
        }
        Pop-Location
    }

    $configure = @("-S", ".", "-B", $BuildDir, "-DCMAKE_BUILD_TYPE=Release")
    if ($ToolchainRoot) {
        $bin = Join-Path $ToolchainRoot "bin"
        $env:PATH = "$bin;$env:PATH"
        $configure += @(
            "-G", "MinGW Makefiles",
            "-DCMAKE_C_COMPILER=$(Join-Path $bin 'gcc.exe')",
            "-DCMAKE_CXX_COMPILER=$(Join-Path $bin 'g++.exe')",
            "-DCMAKE_MAKE_PROGRAM=$(Join-Path $bin 'mingw32-make.exe')"
        )
    }

    # Compiler warnings arrive on stderr; treat the exit code as the source of truth.
    $nativeErrorAction = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try {
        & cmake @configure
        if ($LASTEXITCODE -ne 0) { throw "cmake configure failed ($LASTEXITCODE)" }
        & cmake --build $BuildDir -j 4
        if ($LASTEXITCODE -ne 0) { throw "cmake build failed ($LASTEXITCODE)" }
    }
    finally {
        $ErrorActionPreference = $nativeErrorAction
    }

    $exe = Join-Path $Root (Join-Path $BuildDir "bin/modelforge.exe")
    if (-not (Test-Path $exe)) { throw "build finished but $exe was not found" }
    Write-Host ""
    Write-Host "ModelForge built: $exe"
    Write-Host "Start: $exe serve --root `"$Root`""
}
finally {
    Pop-Location
}
