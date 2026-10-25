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
    $cleanBegin = Invoke-Project "clean-begin" @("project", "begin", $second)
    if ($cleanBegin.session.dirty -or $cleanBegin.session.hasRecovery) { throw "fresh begin reported a dirty/recovery state" }
    if (Test-Path (Join-Path $second ".project-recovery.json")) { throw "fresh begin wrote a recovery manifest" }
    $cleanRestart = Invoke-Project "clean-restart" @("project", "begin", $second)
    if ($cleanRestart.session.dirty -or $cleanRestart.session.hasRecovery) { throw "clean reopen reported false recovery" }
    $secondDraft = Invoke-Project "second-draft" @("project", "edit-draft", $second, "split", "5")
    if (-not $secondDraft.session.dirty -or -not $secondDraft.session.hasRecovery) { throw "edited draft was not marked dirty" }
    $secondSave = Invoke-Project "second-save" @("project", "save", $second)
    if ($secondSave.session.dirty -or $secondSave.session.hasRecovery) { throw "saved draft remained dirty" }
    $secondInspect = Invoke-Project "second-inspect" @("project", "inspect", $second)
    if ($secondInspect.session.dirty -or $secondInspect.session.hasRecovery) { throw "inspect reported a stale recovery after save" }
    $noOpDraft = Invoke-Project "no-op-draft" @("project", "edit-draft", $second, "style", "background", "黑曜石网格")
    if ($noOpDraft.session.dirty -or $noOpDraft.session.hasRecovery) { throw "no-op edit marked a clean draft dirty" }
    if (Test-Path (Join-Path $second ".project-recovery.json")) { throw "no-op edit left a recovery manifest" }
    $nextDraft = Invoke-Project "edit-after-save" @("project", "edit-draft", $second, "style", "background", "工程白网格")
    if (-not $nextDraft.session.dirty -or -not $nextDraft.session.hasRecovery) { throw "editing after save did not create a new dirty draft" }
    $null = Invoke-Project "discard-after-save" @("project", "discard", $second)
    $afterDiscardDraft = Invoke-Project "edit-after-discard" @("project", "edit-draft", $second, "style", "background", "暗夜星阵")
    if (-not $afterDiscardDraft.session.dirty) { throw "editing after discard did not create a fresh draft" }

    $projectPath = $first
    $null = Invoke-Project "inspect" @("project", "inspect", $projectPath)
    $begin = Invoke-Project "begin" @("project", "begin", $projectPath)
    if ($begin.session.dirty -or $begin.session.hasRecovery) { throw "fresh begin should have clean state" }
    if (Test-Path (Join-Path $projectPath ".project-recovery.json")) { throw "fresh begin wrote a recovery manifest" }
    $cleanReopen = Invoke-Project "clean-reopen" @("project", "begin", $projectPath)
    if ($cleanReopen.session.dirty -or $cleanReopen.session.hasRecovery) { throw "reopening unchanged project reported false recovery" }
    $draft = Invoke-Project "edit-draft" @("project", "edit-draft", $projectPath, "split", "5")
    if (-not $draft.session.dirty -or -not (Test-Path (Join-Path $projectPath ".project-recovery.json"))) {
        throw "draft recovery manifest was not created"
    }
    $restart = Invoke-Project "begin-recovery" @("project", "begin", $projectPath)
    if (-not $restart.session.dirty -or -not $restart.session.hasRecovery) { throw "begin did not report dirty recovery" }
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
