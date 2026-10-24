param(
    [Parameter(Mandatory = $true)]
    [string]$Executable
)

$ErrorActionPreference = "Stop"
$root = Join-Path $env:TEMP "dogsc-project-history-$PID"

function Invoke-Project {
    param(
        [string]$Name,
        [string[]]$Arguments,
        [switch]$AllowFailure
    )
    $stdout = Join-Path $root "$Name.stdout.json"
    $stderr = Join-Path $root "$Name.stderr.txt"
    $process = Start-Process -FilePath $Executable -ArgumentList $Arguments -WorkingDirectory $env:WINDIR `
        -NoNewWindow -RedirectStandardOutput $stdout -RedirectStandardError $stderr -Wait -PassThru
    if ($process.ExitCode -ne 0) {
        if ($AllowFailure) { return [pscustomobject]@{ ExitCode = $process.ExitCode } }
        throw "$Name failed with exit $($process.ExitCode): $(Get-Content $stderr -Raw)"
    }
    return (Get-Content $stdout -Raw | ConvertFrom-Json)
}

try {
    if (Test-Path $root) { Remove-Item $root -Recurse -Force }
    New-Item -ItemType Directory -Path $root | Out-Null
    $library = Join-Path $root "library"
    $first = Join-Path $library "First"
    $second = Join-Path $library "Second"
    New-Item -ItemType Directory -Path $first, $second | Out-Null
    $project = @{ version = 27; title = "History smoke"; media = @{ screen = @{ relativePath = "screen.mp4"; sourceEndTime = 10.0 } } }
    $json = $project | ConvertTo-Json -Depth 8
    Set-Content (Join-Path $first "project.json") $json -Encoding utf8
    Set-Content (Join-Path $second "project.json") $json -Encoding utf8

    $listed = Invoke-Project "list" @("project", "list", $library)
    if (@($listed.projects).Count -ne 2) { throw "project list did not return two projects" }

    $projectPath = $first
    $null = Invoke-Project "inspect" @("project", "inspect", $projectPath)
    $begin = Invoke-Project "begin" @("project", "begin", $projectPath)
    if ($begin.session.dirty) { throw "fresh begin should not be dirty" }
    $draft = Invoke-Project "edit-draft" @("project", "edit-draft", $projectPath, "split", "5")
    if (-not $draft.session.dirty -or -not (Test-Path (Join-Path $projectPath ".project-recovery.json"))) {
        throw "draft recovery manifest was not created"
    }
    $restart = Invoke-Project "begin-recovery" @("project", "begin", $projectPath)
    if (-not $restart.session.hasRecovery) { throw "begin did not report recovery" }
    $discard = Invoke-Project "discard" @("project", "discard", $projectPath)
    if ((Test-Path (Join-Path $projectPath ".project-draft.json")) -or
        (Test-Path (Join-Path $projectPath ".project-recovery.json"))) { throw "discard left recovery files" }
    $null = Invoke-Project "split" @("project", "edit", $projectPath, "split", "5")
    $current = Get-Content (Join-Path $projectPath "project.json") -Raw | ConvertFrom-Json
    $segmentId = @($current.timeline.sourceSequence.segments)[1].id
    $null = Invoke-Project "rate" @("project", "edit", $projectPath, "rate", $segmentId, "2")
    $null = Invoke-Project "trim" @("project", "edit", $projectPath, "trim", $segmentId, "right", "6.5")

    $manifestPath = Join-Path $projectPath ".project-history\manifest.json"
    $manifest = Get-Content $manifestPath -Raw | ConvertFrom-Json
    if ($manifest.current -ne 3 -or @($manifest.states).Count -ne 4) { throw "history manifest after edits is invalid" }

    $undoOne = Invoke-Project "undo-one" @("project", "undo", $projectPath)
    $undoTwo = Invoke-Project "undo-two" @("project", "undo", $projectPath)
    if ([math]::Abs($undoTwo.outputDuration - 10) -gt 0.000001) { throw "two-step undo did not restore the split state" }
    $redo = Invoke-Project "redo" @("project", "redo", $projectPath)
    if ([math]::Abs($redo.outputDuration - 7.5) -gt 0.000001) { throw "redo did not restore the rate state" }
    $null = Invoke-Project "branch" @("project", "edit", $projectPath, "trim", $segmentId, "right", "6")
    $redoAfterBranch = Invoke-Project "redo-after-branch" @("project", "redo", $projectPath) -AllowFailure
    if ($redoAfterBranch.ExitCode -eq 0) { throw "redo survived a new branch" }

    $renamedPath = Join-Path $library "Renamed"
    $renamed = Invoke-Project "rename" @("project", "rename", $projectPath, "Renamed")
    if ($renamed.path -ne $renamedPath -or -not (Test-Path (Join-Path $renamedPath "project.json"))) {
        throw "rename result is invalid"
    }
    "project_history_smoke: list=2, edits=3, undo=2, redo=1, branch-redo=failed, rename=passed"
}
finally {
    if (Test-Path $root) { Remove-Item $root -Recurse -Force }
}
