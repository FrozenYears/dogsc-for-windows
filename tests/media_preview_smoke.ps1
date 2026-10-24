param(
    [Parameter(Mandatory = $true)]
    [string]$Executable
)

$ErrorActionPreference = "Stop"
$root = Join-Path $env:TEMP "dogsc-media-preview-$PID"

function Invoke-Capture {
    param([string]$Codec, [string]$Name)
    $dir = Join-Path $root $Name
    $out = Join-Path $root "$Name.record.out"
    $err = Join-Path $root "$Name.record.err"
    $process = Start-Process -FilePath $Executable -ArgumentList @(
        "record", $dir, "--monitor", "0", "--fps", "30", "--codec", $Codec, "--duration", "0.2"
    ) -WorkingDirectory $env:WINDIR -NoNewWindow -RedirectStandardOutput $out -RedirectStandardError $err -Wait -PassThru
    if ($process.ExitCode -ne 0) { throw "$Codec recording failed: $(Get-Content $err -Raw)" }
    return $dir
}

function Invoke-JsonCommand {
    param([string]$Name, [string[]]$Arguments, [switch]$AllowFailure)
    $out = Join-Path $root "$Name.out.json"
    $err = Join-Path $root "$Name.err.txt"
    $process = Start-Process -FilePath $Executable -ArgumentList $Arguments -WorkingDirectory $env:WINDIR `
        -NoNewWindow -RedirectStandardOutput $out -RedirectStandardError $err -Wait -PassThru
    if ($process.ExitCode -ne 0) {
        if ($AllowFailure) { return [pscustomobject]@{ ExitCode = $process.ExitCode; Error = Get-Content $err -Raw } }
        throw "$Name failed with exit $($process.ExitCode): $(Get-Content $err -Raw)"
    }
    $json = Get-Content $out -Raw | ConvertFrom-Json
    if ([string]::IsNullOrWhiteSpace($json.data)) { throw "$Name returned no image data" }
    return $json
}

function Invoke-Stream {
    param([string]$Name, [string]$Project)
    $inputPath = Join-Path $root "$Name.in.txt"
    $outputPath = Join-Path $root "$Name.out.jsonl"
    $errorPath = Join-Path $root "$Name.err.txt"
    Set-Content $inputPath "F 0`nF 0.1`nQ`n" -Encoding ascii
    $process = Start-Process -FilePath $Executable -ArgumentList @("project", "stream", $Project, "320") `
        -WorkingDirectory $env:WINDIR -NoNewWindow -RedirectStandardInput $inputPath `
        -RedirectStandardOutput $outputPath -RedirectStandardError $errorPath -Wait -PassThru
    if ($process.ExitCode -ne 0) { throw "$Name failed with exit $($process.ExitCode): $(Get-Content $errorPath -Raw)" }
    $lines = @(Get-Content $outputPath | Where-Object { $_ })
    if ($lines.Count -ne 2) { throw "$Name returned $($lines.Count) frames" }
    $first = $lines[0] | ConvertFrom-Json
    if ($first.width -ne 320) { throw "$Name returned invalid dimensions" }
}

try {
    if (Test-Path $root) { Remove-Item $root -Recurse -Force }
    New-Item -ItemType Directory -Path $root | Out-Null
    foreach ($codec in @("h264", "hevc")) {
        $project = Invoke-Capture $codec $codec
        $zero = Invoke-JsonCommand "$codec-zero" @("project", "frame", $project, "0", "320")
        $seek = Invoke-JsonCommand "$codec-seek" @("project", "frame", $project, "0.1", "320")
        if ($zero.width -ne 320 -or $seek.height -le 0) { throw "$codec frame dimensions invalid" }
        Invoke-Stream "$codec-stream" $project
        $bad = Invoke-JsonCommand "$codec-eof" @("project", "frame", $project, "999", "320") -AllowFailure
        if ($bad.ExitCode -eq 0) { throw "$codec EOF unexpectedly succeeded" }
        $composite = Invoke-JsonCommand "$codec-composite" @("project", "composite-frame", $project, "0", "320")
        if ($composite.width -ne 320) { throw "$codec composite dimensions invalid" }
    }
    "media_preview_smoke: H264/HEVC frame, seek, stream, EOF and D2D composite passed"
}
finally {
    if (Test-Path $root) { Remove-Item $root -Recurse -Force }
}
