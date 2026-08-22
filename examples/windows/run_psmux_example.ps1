[CmdletBinding(DefaultParameterSetName = 'Example')]
param(
    [Parameter(Mandatory = $true, ParameterSetName = 'Example')]
    [string]$Example,
    [Parameter(Mandatory = $true, ParameterSetName = 'Mcp')]
    [string]$McpServer,
    [ValidateNotNullOrEmpty()]
    [string]$TmuxExecutable = 'tmux.exe',
    [string]$ConfigPath
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Get-ExactRegistryEntries {
    param([string]$RegistryRoot, [string]$Pattern)

    try {
        $attributes = [IO.File]::GetAttributes($RegistryRoot)
    }
    catch [IO.FileNotFoundException] {
        return
    }
    catch [IO.DirectoryNotFoundException] {
        return
    }
    if (($attributes -band [IO.FileAttributes]::Directory) -eq 0) {
        throw "psmux registry root is not a directory: $RegistryRoot"
    }
    Get-ChildItem -LiteralPath $RegistryRoot -Filter $Pattern -Force `
        -ErrorAction Stop
}

$namespace = "libtmux-cxx-example-$PID-$([Guid]::NewGuid().ToString('N'))"
$session = 'example'
$base = "${namespace}__${session}"
if ($PSBoundParameters.ContainsKey('ConfigPath')) {
    if ([string]::IsNullOrWhiteSpace($ConfigPath) -or
        -not [IO.Path]::IsPathRooted($ConfigPath)) {
        throw 'ConfigPath must be a non-empty absolute path'
    }
    $config = [IO.Path]::GetFullPath($ConfigPath)
}
else {
    $config = Join-Path ([IO.Path]::GetTempPath()) "$namespace.conf"
}
$profileRoot = [Environment]::GetEnvironmentVariable('USERPROFILE', 'Process')
if ([string]::IsNullOrWhiteSpace($profileRoot)) {
    $profileRoot = [Environment]::GetFolderPath('UserProfile')
}
if ([string]::IsNullOrWhiteSpace($profileRoot)) {
    $homeDrive = [Environment]::GetEnvironmentVariable('HOMEDRIVE', 'Process')
    $homePath = [Environment]::GetEnvironmentVariable('HOMEPATH', 'Process')
    if (-not [string]::IsNullOrWhiteSpace($homeDrive) -and
        -not [string]::IsNullOrWhiteSpace($homePath)) {
        $combinedHome = "$homeDrive$homePath"
        if ([IO.Directory]::Exists($combinedHome)) {
            $profileRoot = $combinedHome
        }
    }
}
if ([string]::IsNullOrWhiteSpace($profileRoot)) {
    $profileRoot = [Environment]::GetEnvironmentVariable('HOME', 'Process')
}
if ([string]::IsNullOrWhiteSpace($profileRoot)) {
    throw 'could not resolve the Windows profile used by psmux'
}
$registry = Join-Path $profileRoot '.psmux'
$environmentNames = @(
    'PATHEXT',
    'PSMUX_ACTIVE',
    'PSMUX_CONFIG_FILE',
    'PSMUX_DATA_DIR',
    'PSMUX_NO_WARM',
    'PSMUX_REMOTE_ATTACH',
    'PSMUX_SESSION',
    'PSMUX_SESSION_NAME',
    'PSMUX_TARGET',
    'PSMUX_TARGET_FULL',
    'PSMUX_TARGET_SESSION',
    'TMUX',
    'TMUX_PANE'
)
$configCreated = $false
$fixtureAttempted = $false
$exitCode = 1

try {
    $configStream = [IO.File]::Open(
        $config,
        [IO.FileMode]::CreateNew,
        [IO.FileAccess]::Write,
        [IO.FileShare]::None
    )
    $configCreated = $true
    $configStream.Dispose()

    foreach ($name in $environmentNames) {
        [Environment]::SetEnvironmentVariable($name, $null, 'Process')
    }
    $env:PATHEXT = '.COM;.EXE;.BAT;.CMD'
    $env:PSMUX_NO_WARM = '1'
    $env:PSMUX_CONFIG_FILE = $config

    $preexisting = @(
        Get-ExactRegistryEntries $registry "$base.*"
    )
    if ($preexisting.Count -ne 0) {
        throw 'psmux namespace already has registry state'
    }

    $fixtureAttempted = $true
    $fixtureOutput = @(
        & $TmuxExecutable -u -L $namespace new-session -d -s $session 2>&1
    )
    if ($LASTEXITCODE -ne 0) {
        throw "psmux fixture creation failed: $($fixtureOutput -join '; ')"
    }
    if ($PSCmdlet.ParameterSetName -eq 'Mcp') {
        & $McpServer --socket-name $namespace
    }
    else {
        & $Example $namespace $session
    }
    $exitCode = $LASTEXITCODE
}
finally {
    $cleanupFailure = $null
    $killSucceeded = -not $fixtureAttempted
    $fixtureCleanupProven = -not $fixtureAttempted

    if ($fixtureAttempted) {
        try {
            $cleanupOutput = @(
                & $TmuxExecutable -u -L $namespace kill-session -t $session 2>&1
            )
            if ($LASTEXITCODE -eq 0) {
                $killSucceeded = $true
            }
            else {
                $cleanupFailure =
                    "exact psmux session cleanup failed with exit $LASTEXITCODE"
                if ($cleanupOutput.Count -ne 0) {
                    $cleanupFailure += ": $($cleanupOutput -join '; ')"
                }
            }
        }
        catch {
            $cleanupFailure =
                "exact psmux session cleanup failed: $($_.Exception.Message)"
        }

        try {
            $deadline = [DateTime]::UtcNow.AddSeconds(3)
            do {
                $residue = @(
                    Get-ExactRegistryEntries $registry "$base.*"
                )
                $live = @(
                    $residue | Where-Object {
                        -not $_.PSIsContainer -and
                        $_.Extension -in '.port', '.key', '.pid'
                    }
                )
                if ($live.Count -eq 0) {
                    break
                }
                Start-Sleep -Milliseconds 100
            } while ([DateTime]::UtcNow -lt $deadline)

            $unknown = @(
                $residue | Where-Object {
                    $_.PSIsContainer -or
                    $_.Extension -notin '.sid', '.port', '.key', '.pid'
                }
            )
            if ($live.Count -ne 0 -and $null -eq $cleanupFailure) {
                $cleanupFailure =
                    "psmux left $($live.Count) exact live registry file(s)"
            }
            if ($unknown.Count -ne 0 -and $null -eq $cleanupFailure) {
                $cleanupFailure =
                    "psmux left $($unknown.Count) unknown exact registry file(s)"
            }

            if ($killSucceeded -and $live.Count -eq 0 -and
                $unknown.Count -eq 0) {
                $residue | Where-Object {
                    -not $_.PSIsContainer -and $_.Extension -eq '.sid'
                } |
                    Remove-Item -Force -ErrorAction Stop
            }

            $remaining = @(
                Get-ExactRegistryEntries $registry "$base.*"
            )
            if ($remaining.Count -ne 0 -and $null -eq $cleanupFailure) {
                $cleanupFailure =
                    "psmux left $($remaining.Count) exact registry file(s)"
            }
            $fixtureCleanupProven =
                $killSucceeded -and $remaining.Count -eq 0
        }
        catch {
            if ($null -eq $cleanupFailure) {
                $cleanupFailure =
                    "psmux registry cleanup failed: $($_.Exception.Message)"
            }
        }
    }

    try {
        if ($fixtureCleanupProven -and $configCreated) {
            Remove-Item -LiteralPath $config -Force -ErrorAction Stop
            $configStillExists = $true
            try {
                [void][IO.File]::GetAttributes($config)
            }
            catch [IO.FileNotFoundException] {
                $configStillExists = $false
            }
            catch [IO.DirectoryNotFoundException] {
                $configStillExists = $false
            }
            if ($configStillExists) {
                throw 'the isolated psmux config still exists'
            }
        }
    }
    catch {
        if ($null -eq $cleanupFailure) {
            $cleanupFailure =
                "psmux config cleanup failed: $($_.Exception.Message)"
        }
    }

    if ($null -ne $cleanupFailure) {
        $exitCode = 1
        [Console]::Error.WriteLine(
            "$cleanupFailure; recovery socket=$namespace " +
            "session=$session config=$config"
        )
    }
}

exit $exitCode
