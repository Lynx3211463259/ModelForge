param(
    # Empty = read the single source of truth from the VERSION file.
    [string]$Version = "",
    # Official llama.cpp release tag, e.g. b12532. Empty = newest release that
    # actually publishes a Windows asset for the selected backend.
    [string]$LlamaTag = "",
    # cpu | cuda | vulkan | rocm | sycl | openvino | opencl
    [string]$LlamaBackend = "cpu",
    [string]$LlamaArch = "x64",
    # Ship the official tag source archive as well (convert_hf_to_gguf.py, gguf-py).
    [switch]$SkipLlamaSource,
    [switch]$SkipLlama,
    [switch]$IncludeVenv
)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
if (-not $Version) { $Version = (Get-Content (Join-Path $Root "VERSION") -Raw).Trim() }
$ReleaseRoot = Join-Path $Root "release"
$Name = "ModelForge-$Version-win64"
$Stamp = Get-Date -Format "yyyyMMdd-HHmmss"
$Stage = Join-Path $ReleaseRoot "$Name-$Stamp"
$Zip = Join-Path $ReleaseRoot "$Name.zip"
if (Test-Path $Zip) { $Zip = Join-Path $ReleaseRoot "$Name-$Stamp.zip" }

$Repo = "ggml-org/llama.cpp"
$ApiBase = "https://api.github.com/repos/$Repo"
$Headers = @{ "User-Agent" = "ModelForge-Packager"; "Accept" = "application/vnd.github+json" }

New-Item -ItemType Directory -Path $Stage -Force | Out-Null
foreach ($dir in @("bin", "web", "docs", "scripts", "data\logs", "data\reports", "logs", "models", "output", "temp")) {
    New-Item -ItemType Directory -Path (Join-Path $Stage $dir) -Force | Out-Null
}

$exe = Join-Path $Root "build-win\bin\modelforge.exe"
if (-not (Test-Path $exe)) {
    throw "Build the backend first: build-win\bin\modelforge.exe"
}
Copy-Item $exe (Join-Path $Stage "modelforge.exe")
Copy-Item (Join-Path $Root "frontend\dist\*") (Join-Path $Stage "web") -Recurse
Copy-Item (Join-Path $Root "README.md") $Stage
Copy-Item (Join-Path $Root "LICENSE") $Stage -ErrorAction SilentlyContinue
Copy-Item (Join-Path $Root "docs\*") (Join-Path $Stage "docs") -Recurse
Copy-Item (Join-Path $Root "scripts\setup-python.ps1") (Join-Path $Stage "scripts")

# ---------------------------------------------------------------------------
# llama.cpp: distributed exactly as published by upstream.
#
# No self-built binaries, no trimming, no renaming, no repacking:
#   tools\llama.cpp\downloads\<official asset>.zip   original archive bytes
#   tools\llama.cpp\releases\<asset stem>\           the archive extracted as-is
#   tools\llama.cpp\source\llama.cpp-<tag>\          official tag source archive as-is
#   tools\llama.cpp\modelforge-runtime.json          ModelForge sidecar (tag + sha256)
# ---------------------------------------------------------------------------
if (-not $SkipLlama) {
    $llamaRoot = Join-Path $Stage "tools\llama.cpp"
    $downloads = Join-Path $llamaRoot "downloads"
    $releases = Join-Path $llamaRoot "releases"
    $sources = Join-Path $llamaRoot "source"
    # Persistent cache so repeated packaging does not re-download ~80 MB.
    $cache = Join-Path $ReleaseRoot ".cache\llama"
    foreach ($dir in @($downloads, $releases, $sources, $cache)) {
        New-Item -ItemType Directory -Path $dir -Force | Out-Null
    }

    $assetPattern = "^llama-(?<tag>[A-Za-z0-9.]+)-bin-win-(?<flavor>.+)-(?<arch>x64|arm64)\.zip$"
    $release = $null
    $asset = $null

    # 100 releases keeps pinned older tags (e.g. b12532) resolvable after new builds land.
    $releasesJson = Invoke-RestMethod -Uri "$ApiBase/releases?per_page=100" -Headers $Headers
    foreach ($item in $releasesJson) {
        if ($item.draft) { continue }
        if ($LlamaTag -and $item.tag_name -ne $LlamaTag) { continue }
        $candidates = @()
        foreach ($candidate in $item.assets) {
            $match = [regex]::Match($candidate.name, $assetPattern)
            if (-not $match.Success) { continue }
            if ($match.Groups["arch"].Value -ne $LlamaArch) { continue }
            if (-not $match.Groups["flavor"].Value.StartsWith($LlamaBackend, [System.StringComparison]::OrdinalIgnoreCase)) { continue }
            $candidates += @{ Asset = $candidate; Flavor = $match.Groups["flavor"].Value }
        }
        if ($candidates.Count -gt 0) {
            $picked = $candidates | Sort-Object { $_.Flavor } -Descending | Select-Object -First 1
            $release = $item
            $asset = $picked.Asset
            break
        }
    }

    if (-not $asset) {
        $requested = if ($LlamaTag) { "tag $LlamaTag" } else { "any release" }
        throw "No official llama.cpp $LlamaBackend/$LlamaArch Windows asset found for $requested. Check -LlamaBackend/-LlamaTag."
    }

    $tag = $release.tag_name
    Write-Host "llama.cpp official release: $tag asset: $($asset.name)"

    $archivePath = Join-Path $cache $asset.name
    $expected = $asset.digest
    if ($expected -and $expected.StartsWith("sha256:")) {
        $expected = $expected.Substring(7)
    }
    $reuse = $false
    if (Test-Path $archivePath) {
        $existingHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $archivePath).Hash.ToLowerInvariant()
        if ((-not $expected) -or $existingHash -eq $expected.ToLowerInvariant()) {
            $reuse = $true
            Write-Host "Reusing verified official archive: $archivePath"
        }
    }
    if (-not $reuse) {
        Invoke-WebRequest -Uri $asset.browser_download_url -OutFile $archivePath -Headers @{ "User-Agent" = "ModelForge-Packager" }
    }
    $actualSize = (Get-Item $archivePath).Length
    if ($asset.size -and $actualSize -ne $asset.size) {
        throw "Asset size mismatch: expected $($asset.size) bytes, got $actualSize"
    }
    $hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $archivePath).Hash.ToLowerInvariant()
    if ($expected -and $hash -ne $expected.ToLowerInvariant()) {
        throw "sha256 mismatch for $($asset.name): expected $expected, got $hash"
    }

    # Ship the untouched official archive next to its extraction.
    Copy-Item $archivePath (Join-Path $downloads $asset.name) -Force

    $stem = [System.IO.Path]::GetFileNameWithoutExtension($asset.name)
    $target = Join-Path $releases $stem
    Expand-Archive -LiteralPath $archivePath -DestinationPath $target -Force
    if (-not (Test-Path (Join-Path $target "llama-quantize.exe"))) {
        throw "Extracted official asset has no llama-quantize.exe: $target"
    }

    $sourceName = $null
    $sourceHash = $null
    if (-not $SkipLlamaSource) {
        $sourceName = "llama.cpp-$tag.zip"
        $sourceArchive = Join-Path $cache $sourceName
        if (-not (Test-Path $sourceArchive)) {
            Invoke-WebRequest -Uri "https://github.com/$Repo/archive/refs/tags/$tag.zip" -OutFile $sourceArchive -Headers @{ "User-Agent" = "ModelForge-Packager" }
        } else {
            Write-Host "Reusing cached official source archive: $sourceArchive"
        }
        $sourceHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $sourceArchive).Hash.ToLowerInvariant()
        # Only the extracted tree is shipped; the 41 MB source zip stays in the cache
        # so the package is not carrying the same content twice.
        Expand-Archive -LiteralPath $sourceArchive -DestinationPath $sources -Force
        $converter = Join-Path (Join-Path $sources "llama.cpp-$tag") "convert_hf_to_gguf.py"
        if (-not (Test-Path $converter)) {
            throw "Official source archive has no convert_hf_to_gguf.py: $converter"
        }
    }

    $manifest = [ordered]@{
        tag          = $tag
        asset        = $asset.name
        backend      = $LlamaBackend
        arch         = $LlamaArch
        sha256       = $hash
        size         = $actualSize
        verified     = [bool]$expected
        releaseUrl   = $release.html_url
        repo         = $Repo
        source       = "github-release"
        sourceArchive = if ($sourceName) { [ordered]@{ name = $sourceName; sha256 = $sourceHash } } else { $null }
        packagedAt   = (Get-Date).ToUniversalTime().ToString("yyyy-MM-ddTHH:mm:ssZ")
    }
    $manifestJson = $manifest | ConvertTo-Json -Depth 5
    [System.IO.File]::WriteAllText((Join-Path $llamaRoot "modelforge-runtime.json"), $manifestJson, (New-Object System.Text.UTF8Encoding($false)))
}

if ($IncludeVenv -and (Test-Path (Join-Path $Root ".venv"))) {
    Copy-Item (Join-Path $Root ".venv") (Join-Path $Stage ".venv") -Recurse
}

$launcher = @'
@echo off
setlocal
set "ROOT=%~dp0"
"%ROOT%modelforge.exe" serve --root "%ROOT%" %*
endlocal
'@
Set-Content -Path (Join-Path $Stage "start.bat") -Value $launcher -Encoding ASCII

Compress-Archive -Path (Join-Path $Stage "*") -DestinationPath $Zip -CompressionLevel Optimal
Write-Host "Package created: $Zip"
Write-Host "llama.cpp is bundled as the untouched official release archive + extraction; no self-built binaries are included."
Write-Host "Note: the package does not include models, the SQLite database, logs, or a Python virtual environment unless -IncludeVenv is used."
