param(
    [string]$SidecarDirectory = (Join-Path $PSScriptRoot "../../apps/tauri/src-tauri/binaries"),
    [string]$OutputPath = (Join-Path $PSScriptRoot "../../out/release/ffmpeg-dll-manifest.json"),
    [string]$SourceArchive = "",
    [string]$InstallerPath = ""
)

$ErrorActionPreference = "Stop"
$sidecar = (Resolve-Path $SidecarDirectory).Path
$dlls = @(Get-ChildItem -LiteralPath $sidecar -Filter "*.dll" -File | Where-Object { $_.Name -match "^(avcodec|avformat|avutil|swresample|swscale)" })
if ($dlls.Count -eq 0) { throw "未找到 FFmpeg DLL：$sidecar" }

$entries = @($dlls | Sort-Object Name | ForEach-Object {
    $hash = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
    [ordered]@{ name = $_.Name; bytes = $_.Length; sha256 = $hash }
})
$source = $null
if ($SourceArchive) {
    $archive = Get-Item -LiteralPath $SourceArchive
    $source = [ordered]@{
        path = $archive.FullName
        bytes = $archive.Length
        sha256 = (Get-FileHash -LiteralPath $archive.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
    }
}
$installer = $null
if ($InstallerPath) {
    $package = Get-Item -LiteralPath $InstallerPath
    $installer = [ordered]@{
        path = $package.FullName
        bytes = $package.Length
        sha256 = (Get-FileHash -LiteralPath $package.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
    }
}

$manifest = [ordered]@{
    schema = 1
    generatedAtUtc = [DateTime]::UtcNow.ToString("o")
    dynamicLinkingRequired = $true
    gplOrNonfreeBuildsAllowed = $false
    sourceArchive = $source
    sourceArchiveRequiredBeforeRelease = $true
    installer = $installer
    dlls = $entries
}

$destination = Split-Path -Parent $OutputPath
New-Item -ItemType Directory -Force -Path $destination | Out-Null
$manifest | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $OutputPath -Encoding utf8
Write-Output "FFmpeg manifest written: $OutputPath"
