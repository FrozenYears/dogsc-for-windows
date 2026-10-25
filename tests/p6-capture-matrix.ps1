param(
    [Parameter(Mandatory = $true)]
    [string]$Executable,
    [Parameter(Mandatory = $true)]
    [string]$OutputDirectory,
    [int]$Monitor = 0,
    [ValidateSet(30, 60, 90, 120)]
    [int]$FrameRate = 60,
    [ValidateRange(1, 86400)]
    [int]$DurationSeconds = 60,
    [ValidateSet("auto", "hevc", "h264", "prores")]
    [string]$Codec = "hevc",
    [switch]$SystemAudio,
    [int]$MaxRepeatedFrames = 1000,
    [int]$MaxAudioDiscontinuities = 0
)

$ErrorActionPreference = "Stop"
$exe = (Resolve-Path $Executable).Path
$output = [System.IO.Path]::GetFullPath($OutputDirectory)
if (Test-Path $output) { throw "输出目录已存在，拒绝覆盖：$output" }
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $output) | Out-Null

$encoderText = (& $exe "encoders" | Out-String)
if ($LASTEXITCODE -ne 0) { throw "编码器枚举失败：$encoderText" }
$encoderReport = $encoderText | ConvertFrom-Json

$recordArguments = @("record", $output, "--monitor", $Monitor, "--fps", $FrameRate,
    "--codec", $Codec, "--duration", $DurationSeconds)
if ($SystemAudio) { $recordArguments += "--system-audio" }
& $exe @recordArguments
if ($LASTEXITCODE -ne 0) { throw "录制失败，exit=$LASTEXITCODE" }

$capturePath = Join-Path $output "capture.json"
$mediaPath = Join-Path $output "screen.mp4"
$capture = Get-Content $capturePath -Raw | ConvertFrom-Json
if ($capture.status -ne "finalized") { throw "录制没有 finalized：$($capture.status)" }
if ([int]$capture.videoFrames -ne $FrameRate * $DurationSeconds) {
    throw "输出帧数不符：$($capture.videoFrames) / $($FrameRate * $DurationSeconds)"
}
if ([int]$capture.discardedNonMonotonicFrames -ne 0) {
    throw "存在非单调丢帧：$($capture.discardedNonMonotonicFrames)"
}
if ([int]$capture.repeatedFrames -gt $MaxRepeatedFrames) {
    throw "重复帧超出阈值：$($capture.repeatedFrames) > $MaxRepeatedFrames"
}
if ($SystemAudio -and [int]$capture.systemAudioDiscontinuities -gt $MaxAudioDiscontinuities) {
    throw "系统音频 discontinuity 超出阈值：$($capture.systemAudioDiscontinuities) > $MaxAudioDiscontinuities"
}

$probeText = (& ffprobe -v error -show_streams -show_format -of json $mediaPath | Out-String)
if ($LASTEXITCODE -ne 0) { throw "ffprobe 失败：$probeText" }
$probe = $probeText | ConvertFrom-Json
$video = @($probe.streams | Where-Object codec_type -eq "video")[0]
if (-not $video -or [int]$video.nb_frames -ne $FrameRate * $DurationSeconds) {
    throw "媒体视频帧数不符：$($video.nb_frames)"
}
$audio = @($probe.streams | Where-Object codec_type -eq "audio")[0]
if ($SystemAudio -and (-not $audio -or [int]$audio.sample_rate -ne 48000 -or [int]$audio.channels -ne 2)) {
    throw "系统音频不是 48kHz/2ch"
}

$report = [ordered]@{
    schema = 1
    executable = $exe
    outputDirectory = $output
    requested = [ordered]@{
        monitor = $Monitor
        frameRate = $FrameRate
        durationSeconds = $DurationSeconds
        codec = $Codec
        systemAudio = [bool]$SystemAudio
        maxRepeatedFrames = $MaxRepeatedFrames
        maxAudioDiscontinuities = $MaxAudioDiscontinuities
    }
    encoderEnumeration = $encoderReport
    capture = $capture
    probe = $probe
}
$reportPath = Join-Path $output "p6-matrix-report.json"
$report | ConvertTo-Json -Depth 20 | Set-Content -LiteralPath $reportPath -Encoding utf8
Write-Output "P6 capture matrix passed: $reportPath"
