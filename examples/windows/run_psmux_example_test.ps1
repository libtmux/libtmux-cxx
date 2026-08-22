[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$Runner,
    [Parameter(Mandatory = $true)]
    [string]$PowerShellExecutable
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Assert-True {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) {
        throw $Message
    }
}

function Invoke-Runner {
    param([string[]]$Arguments)

    $savedPreference = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $output = @(
            & $PowerShellExecutable `
                -NoLogo `
                -NoProfile `
                -NonInteractive `
                -ExecutionPolicy Bypass `
                -File $Runner `
                @Arguments 2>&1
        )
        return @{
            ExitCode = $LASTEXITCODE
            Output = @($output | ForEach-Object { $_.ToString() })
        }
    }
    finally {
        $ErrorActionPreference = $savedPreference
    }
}

$testRoot = Join-Path (
    [IO.Path]::GetTempPath()
) "libtmux-cxx-psmux-runner-$([Guid]::NewGuid().ToString('N'))"
$fakeTmux = Join-Path $testRoot 'fake-tmux.cmd'
$fakeExample = Join-Path $testRoot 'fake-example.cmd'
$invocationLog = Join-Path $testRoot 'tmux-invocations.log'
$collisionConfig = Join-Path $testRoot 'existing.conf'
$recoveryConfig = Join-Path $testRoot 'recovery.conf'
$oldLog = [Environment]::GetEnvironmentVariable(
    'LIBTMUX_FAKE_TMUX_LOG',
    'Process'
)
$oldKillExit = [Environment]::GetEnvironmentVariable(
    'LIBTMUX_FAKE_TMUX_KILL_EXIT',
    'Process'
)
$oldUserProfile = [Environment]::GetEnvironmentVariable(
    'USERPROFILE',
    'Process'
)

try {
    [IO.Directory]::CreateDirectory($testRoot) | Out-Null
    [IO.File]::WriteAllText(
        $fakeTmux,
        "@echo off`r`n" +
        ">>`"%LIBTMUX_FAKE_TMUX_LOG%`" echo %*`r`n" +
        "if `"%~4`"==`"new-session`" exit /b 0`r`n" +
        "if `"%~4`"==`"kill-session`" " +
        "exit /b %LIBTMUX_FAKE_TMUX_KILL_EXIT%`r`n" +
        "exit /b 91`r`n"
    )
    [IO.File]::WriteAllText($fakeExample, "@exit /b 0`r`n")
    [Environment]::SetEnvironmentVariable(
        'LIBTMUX_FAKE_TMUX_LOG',
        $invocationLog,
        'Process'
    )
    [Environment]::SetEnvironmentVariable(
        'LIBTMUX_FAKE_TMUX_KILL_EXIT',
        '47',
        'Process'
    )
    [Environment]::SetEnvironmentVariable(
        'USERPROFILE',
        $testRoot,
        'Process'
    )

    [IO.File]::WriteAllText($collisionConfig, 'do not overwrite')
    $collision = Invoke-Runner @(
        '-Example',
        $fakeExample,
        '-TmuxExecutable',
        $fakeTmux,
        '-ConfigPath',
        $collisionConfig
    )
    Assert-True (
        $collision.ExitCode -ne 0
    ) 'prelaunch config collision unexpectedly succeeded'
    Assert-True (
        -not [IO.File]::Exists($invocationLog)
    ) 'prelaunch config failure invoked tmux'
    Assert-True (
        [IO.File]::ReadAllText($collisionConfig) -eq 'do not overwrite'
    ) 'prelaunch config collision overwrote its existing file'

    $cleanup = Invoke-Runner @(
        '-Example',
        $fakeExample,
        '-TmuxExecutable',
        $fakeTmux,
        '-ConfigPath',
        $recoveryConfig
    )
    Assert-True (
        $cleanup.ExitCode -ne 0
    ) 'fixture cleanup failure unexpectedly succeeded'
    Assert-True (
        [IO.File]::Exists($recoveryConfig)
    ) 'fixture cleanup failure removed its recovery config'
    Assert-True (
        [IO.File]::ReadAllText($recoveryConfig).Length -eq 0
    ) 'fixture cleanup failure changed its recovery config'

    $invocations = @([IO.File]::ReadAllLines($invocationLog))
    Assert-True (
        $invocations.Count -eq 2
    ) "expected two exact tmux invocations, got $($invocations.Count)"
    $created = [regex]::Match(
        $invocations[0],
        '^-u -L ([^ ]+) new-session -d -s ([^ ]+)$'
    )
    Assert-True $created.Success 'fixture creation arguments were not exact'
    $socketName = $created.Groups[1].Value
    $sessionName = $created.Groups[2].Value
    Assert-True (
        $sessionName -eq 'example'
    ) 'fixture creation changed the stable session name'
    Assert-True (
        $invocations[1] -eq
            "-u -L $socketName kill-session -t $sessionName"
    ) 'fixture cleanup did not use the exact creation identifiers'

    $combinedOutput = $cleanup.Output -join "`n"
    Assert-True (
        $combinedOutput.Contains('cleanup failed with exit 47')
    ) 'cleanup failure omitted the command exit code'
    Assert-True (
        $combinedOutput.Contains("recovery socket=$socketName")
    ) 'cleanup failure omitted the recovery socket'
    Assert-True (
        $combinedOutput.Contains("session=$sessionName")
    ) 'cleanup failure omitted the recovery session'
    Assert-True (
        $combinedOutput.Contains("config=$recoveryConfig")
    ) 'cleanup failure omitted the recovery config path'

    Write-Output 'PASS: Windows psmux runner cleanup safety'
}
finally {
    [Environment]::SetEnvironmentVariable(
        'LIBTMUX_FAKE_TMUX_LOG',
        $oldLog,
        'Process'
    )
    [Environment]::SetEnvironmentVariable(
        'LIBTMUX_FAKE_TMUX_KILL_EXIT',
        $oldKillExit,
        'Process'
    )
    [Environment]::SetEnvironmentVariable(
        'USERPROFILE',
        $oldUserProfile,
        'Process'
    )
    if ([IO.Directory]::Exists($testRoot)) {
        Remove-Item -LiteralPath $testRoot -Recurse -Force
    }
}
