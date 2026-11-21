param(
    [string]$Python = "python",
    [string]$Requirements = ""
)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
Push-Location $Root
try {
    if (-not $Requirements) {
        $portableRequirements = Join-Path $Root "tools\llama.cpp\requirements\requirements-convert_hf_to_gguf.txt"
        $sourceRequirements = Join-Path (Split-Path -Parent $Root) "llama.cpp\requirements\requirements-convert_hf_to_gguf.txt"
        if (Test-Path $portableRequirements) {
            $Requirements = $portableRequirements
        } else {
            $Requirements = $sourceRequirements
        }
    }
    if (-not (Test-Path $Requirements)) {
        throw "Conversion requirements not found: $Requirements. Pass -Requirements <path>."
    }

    if (-not (Test-Path ".venv")) {
        & $Python -m venv .venv
        if ($LASTEXITCODE -ne 0) { throw "Unable to create .venv" }
    }

    $venvPython = Join-Path $Root ".venv\Scripts\python.exe"
    & $venvPython -m pip install --upgrade pip
    & $venvPython -m pip install -r $Requirements
    if ($LASTEXITCODE -ne 0) { throw "pip install failed" }

    Write-Host "Python environment ready: $venvPython"
    Write-Host "ModelForge will auto-detect this virtual environment."
}
finally {
    Pop-Location
}
