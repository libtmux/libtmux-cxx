[CmdletBinding(DefaultParameterSetName = 'Example')]
param(
    [Parameter(Mandatory = $true, ParameterSetName = 'Example')]
    [string]$Example,
    [Parameter(Mandatory = $true, ParameterSetName = 'Mcp')]
    [string]$McpServer
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$namespace = "libtmux-cxx-example-$PID-$([Guid]::NewGuid().ToString('N'))"
$session = 'example'
$base = "${namespace}__${session}"
$config = Join-Path ([IO.Path]::GetTempPath()) "$namespace.conf"
$registry = Join-Path ([Environment]::GetFolderPath('UserProfile')) '.psmux'
$exitCode = 1

try {
    [IO.File]::WriteAllText($config, '')
    $env:PATHEXT = '.COM;.EXE;.BAT;.CMD'
    $env:PSMUX_NO_WARM = '1'
    $env:PSMUX_CONFIG_FILE = $config
    foreach ($name in @(
        'TMUX', 'TMUX_PANE', 'PSMUX_ACTIVE', 'PSMUX_SESSION',
        'PSMUX_SESSION_NAME', 'PSMUX_TARGET_FULL', 'PSMUX_TARGET_SESSION',
        'PSMUX_REMOTE_ATTACH'
    )) {
        [Environment]::SetEnvironmentVariable($name, $null, 'Process')
    }

    $fixtureOutput = @(
        & tmux.exe -u -L $namespace new-session -d -s $session 2>&1
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
    try {
        $cleanupOutput = @(
            & tmux.exe -u -L $namespace kill-session -t $session 2>&1
        )
        if ($LASTEXITCODE -ne 0) {
            [Console]::Error.WriteLine(
                "psmux session cleanup failed: $($cleanupOutput -join '; ')"
            )
            $exitCode = 1
        }
    }
    catch {
        [Console]::Error.WriteLine("psmux session cleanup failed: $_")
        $exitCode = 1
    }
    $deadline = [DateTime]::UtcNow.AddSeconds(3)
    do {
        $residue = @(
            Get-ChildItem -LiteralPath $registry -Filter "$base.*" -File `
                -ErrorAction SilentlyContinue
        )
        $live = @($residue | Where-Object { $_.Extension -ne '.sid' })
        if ($live.Count -eq 0) {
            $residue | Where-Object Extension -eq '.sid' |
                Remove-Item -Force -ErrorAction SilentlyContinue
            break
        }
        Start-Sleep -Milliseconds 20
    } while ([DateTime]::UtcNow -lt $deadline)
    $remaining = @(
        Get-ChildItem -LiteralPath $registry -Filter "$base.*" -File `
            -ErrorAction SilentlyContinue
    )
    if ($remaining.Count -ne 0) {
        $remaining | ForEach-Object {
            [Console]::Error.WriteLine("psmux residue remained: $($_.FullName)")
        }
        $exitCode = 1
    }
    try {
        if (Test-Path -LiteralPath $config) {
            Remove-Item -LiteralPath $config -Force -ErrorAction Stop
        }
        if (Test-Path -LiteralPath $config) {
            throw "the isolated psmux config still exists"
        }
    }
    catch {
        [Console]::Error.WriteLine("psmux config cleanup failed: $_")
        $exitCode = 1
    }
}

exit $exitCode
