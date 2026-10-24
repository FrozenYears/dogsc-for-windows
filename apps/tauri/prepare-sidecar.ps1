param(
    [string]$BuildRoot = "../../out/vcpkg-native"
)

$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$source = Join-Path $here "$BuildRoot/tools/capture_cli/Release"
$destination = Join-Path $here "src-tauri/binaries"
$target = "x86_64-pc-windows-msvc"

if (-not (Test-Path (Join-Path $source "dogsc_capture.exe"))) {
    throw "dogsc_capture.exe not found in $source. Build the C++ Release target first."
}

New-Item -ItemType Directory -Force -Path $destination | Out-Null
Get-ChildItem -Path $destination -File | Remove-Item -Force
Copy-Item (Join-Path $source "dogsc_capture.exe") (Join-Path $destination "dogsc_capture-$target.exe")
Get-ChildItem -Path $source -Filter "*.dll" | Copy-Item -Destination $destination -Force
Write-Host "Prepared sidecar: $destination"
