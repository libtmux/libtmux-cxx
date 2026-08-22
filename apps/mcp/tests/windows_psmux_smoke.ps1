param(
    [Parameter(Mandatory = $true)]
    [string] $Server
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Assert-True {
    param([bool] $Condition, [string] $Message)
    if (-not $Condition) {
        throw $Message
    }
}

function Send-Message {
    param([hashtable] $Message)
    $line = $Message | ConvertTo-Json -Compress -Depth 12
    $script:mcpProcess.StandardInput.WriteLine($line)
    $script:mcpProcess.StandardInput.Flush()
}

function Read-Response {
    param($ExpectedId)
    $deadline = [DateTime]::UtcNow.AddSeconds(15)
    while ([DateTime]::UtcNow -lt $deadline) {
        $remaining = [int][Math]::Max(
            1,
            ($deadline - [DateTime]::UtcNow).TotalMilliseconds
        )
        $read = $script:mcpProcess.StandardOutput.ReadLineAsync()
        if (-not $read.Wait($remaining)) {
            throw "timed out waiting for MCP response $ExpectedId"
        }
        $line = $read.Result
        if ($null -eq $line) {
            throw "MCP server closed stdout before response $ExpectedId"
        }
        $message = $line | ConvertFrom-Json
        if ($message.PSObject.Properties.Name -contains "id" -and
            [string]$message.id -eq [string]$ExpectedId) {
            return $message
        }
    }
    throw "timed out waiting for MCP response $ExpectedId"
}

function Invoke-Request {
    param([hashtable] $Request)
    Send-Message $Request
    $response = Read-Response $Request.id
    if ($response.PSObject.Properties.Name -contains "error") {
        throw ($response.error | ConvertTo-Json -Compress -Depth 8)
    }
    return $response
}

function Invoke-Tool {
    param([int] $Id, [string] $Name, [hashtable] $Arguments)
    $response = Invoke-Request @{
        jsonrpc = "2.0"
        id = $Id
        method = "tools/call"
        params = @{ name = $Name; arguments = $Arguments }
    }
    if ($response.result.isError) {
        throw ($response.result.content | ConvertTo-Json -Compress -Depth 8)
    }
    return $response.result.structuredContent
}

$suffix = [Guid]::NewGuid().ToString("N")
$socketName = "libtmux-cxx-mcp-$suffix"
$sessionName = "mcp-$suffix"
$profileRoot = [Environment]::GetFolderPath("UserProfile")
if ([string]::IsNullOrWhiteSpace($profileRoot)) {
    $homeDrive = [Environment]::GetEnvironmentVariable("HOMEDRIVE", "Process")
    $homePath = [Environment]::GetEnvironmentVariable("HOMEPATH", "Process")
    if (-not [string]::IsNullOrWhiteSpace($homeDrive) -and
        -not [string]::IsNullOrWhiteSpace($homePath)) {
        $profileRoot = "$homeDrive$homePath"
    }
}
if ([string]::IsNullOrWhiteSpace($profileRoot)) {
    $profileRoot = [Environment]::GetEnvironmentVariable("HOME", "Process")
}
Assert-True (
    -not [string]::IsNullOrWhiteSpace($profileRoot)
) "could not resolve the Windows profile used by psmux"
$registryRoot = Join-Path $profileRoot ".psmux"
$configPath = Join-Path ([IO.Path]::GetTempPath()) "libtmux-cxx-mcp-$suffix.conf"
$environmentNames = @(
    "PATHEXT",
    "PSMUX_ACTIVE",
    "PSMUX_CONFIG_FILE",
    "PSMUX_DATA_DIR",
    "PSMUX_NO_WARM",
    "PSMUX_REMOTE_ATTACH",
    "PSMUX_SESSION",
    "PSMUX_SESSION_NAME",
    "PSMUX_TARGET",
    "PSMUX_TARGET_FULL",
    "PSMUX_TARGET_SESSION",
    "TMUX",
    "TMUX_PANE"
)
$originalEnvironment = @{}
$script:mcpProcess = $null
$fixtureAttempted = $false
$sessionId = $null

foreach ($name in $environmentNames) {
    $originalEnvironment[$name] =
        [Environment]::GetEnvironmentVariable($name, "Process")
}

try {
    foreach ($name in $environmentNames) {
        [Environment]::SetEnvironmentVariable($name, $null, "Process")
    }
    [Environment]::SetEnvironmentVariable(
        "PATHEXT",
        ".COM;.EXE;.BAT;.CMD",
        "Process"
    )
    [IO.File]::WriteAllText($configPath, "")
    [Environment]::SetEnvironmentVariable(
        "PSMUX_CONFIG_FILE",
        $configPath,
        "Process"
    )
    [Environment]::SetEnvironmentVariable("PSMUX_NO_WARM", "1", "Process")
    $preexisting = @(
        Get-ChildItem -LiteralPath $registryRoot `
            -Filter "$socketName`__$sessionName.*" `
            -File -ErrorAction SilentlyContinue
    )
    Assert-True ($preexisting.Count -eq 0) "psmux namespace already has registry state"

    $fixtureAttempted = $true
    $fixtureOutput = @(
        & tmux.exe -u -L $socketName new-session -d -s $sessionName 2>&1
    )
    Assert-True (
        $LASTEXITCODE -eq 0
    ) "failed to create exact psmux fixture: $($fixtureOutput -join '; ')"

    $start = New-Object System.Diagnostics.ProcessStartInfo
    $start.FileName = $Server
    $start.Arguments = "--socket-name $socketName"
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.RedirectStandardInput = $true
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    $script:mcpProcess = New-Object System.Diagnostics.Process
    $script:mcpProcess.StartInfo = $start
    Assert-True ($script:mcpProcess.Start()) "failed to start MCP server"

    $initialized = Invoke-Request @{
        jsonrpc = "2.0"
        id = "initialize"
        method = "initialize"
        params = @{
            protocolVersion = "2025-06-18"
            capabilities = @{}
            clientInfo = @{ name = "windows-psmux-smoke"; version = "1" }
        }
    }
    Assert-True (
        $initialized.result.protocolVersion -eq "2025-06-18"
    ) "unexpected MCP protocol version"
    Send-Message @{
        jsonrpc = "2.0"
        method = "notifications/initialized"
        params = @{}
    }

    $listed = Invoke-Request @{
        jsonrpc = "2.0"
        id = 1
        method = "tools/list"
        params = @{}
    }
    $actualTools = @($listed.result.tools | ForEach-Object { $_.name } | Sort-Object)
    $expectedTools = @(
        "inspect_tmux",
        "list_session_panes",
        "list_sessions",
        "list_windows"
    )
    Assert-True (
        ($actualTools -join ",") -eq ($expectedTools -join ",")
    ) "Windows MCP tool catalog is not the four-tool read-only subset"

    $sessions = Invoke-Tool 2 "list_sessions" @{}
    $ownedSessions = @(
        $sessions.sessions | Where-Object {
            $_.name -eq $sessionName
        }
    )
    Assert-True ($ownedSessions.Count -eq 1) "fixture session was not listed exactly"
    $sessionId = [string]$ownedSessions[0].id
    Assert-True ($sessionId -match '^\$[0-9]+$') "invalid session ID"

    $windows = Invoke-Tool 3 "list_windows" @{ session = $sessionId }
    $sessionWindows = @($windows.windows)
    Assert-True ($sessionWindows.Count -ge 1) "created session has no window"
    foreach ($window in $sessionWindows) {
        Assert-True (
            $window.session_id -eq $sessionId
        ) "window escaped its exact session"
    }

    $panes = Invoke-Tool 4 "list_session_panes" @{ session = $sessionId }
    $sessionPanes = @($panes.panes)
    Assert-True ($sessionPanes.Count -ge 1) "created session has no pane"
    foreach ($pane in $sessionPanes) {
        Assert-True (
            $pane.session_id -eq $sessionId
        ) "pane escaped its exact session"
    }

    $inspected = Invoke-Tool 5 "inspect_tmux" @{}
    $inspectedSessions = @(
        $inspected.sessions | Where-Object { $_.id -eq $sessionId }
    )
    Assert-True ($inspectedSessions.Count -eq 1) "inspect_tmux lost the session"
    Assert-True (@($inspected.windows).Count -ge 1) "inspect_tmux lost the window"
    Assert-True (@($inspected.panes).Count -ge 1) "inspect_tmux lost the pane"

    $script:mcpProcess.StandardInput.Close()
    Assert-True (
        $script:mcpProcess.WaitForExit(5000)
    ) "MCP server did not stop after stdin closed"
    $stderr = $script:mcpProcess.StandardError.ReadToEnd()
    Assert-True (
        $script:mcpProcess.ExitCode -eq 0
    ) "MCP server exited $($script:mcpProcess.ExitCode): $stderr"
    $script:mcpProcess.Dispose()
    $script:mcpProcess = $null

    Write-Output "PASS: native Windows MCP psmux smoke"
}
finally {
    $cleanupFailure = $null
    $killSucceeded = -not $fixtureAttempted
    $fixtureCleanupProven = -not $fixtureAttempted
    try {
        if ($null -ne $script:mcpProcess) {
            try {
                $script:mcpProcess.StandardInput.Close()
                if (-not $script:mcpProcess.WaitForExit(3000)) {
                    $script:mcpProcess.Kill()
                    $script:mcpProcess.WaitForExit()
                }
            }
            catch {
                if ($null -eq $cleanupFailure) {
                    $cleanupFailure =
                        "MCP process cleanup failed: $($_.Exception.Message)"
                }
            }
            finally {
                try {
                    $script:mcpProcess.Dispose()
                }
                catch {
                    if ($null -eq $cleanupFailure) {
                        $cleanupFailure =
                            "MCP process disposal failed: $($_.Exception.Message)"
                    }
                }
                $script:mcpProcess = $null
            }
        }

        if ($fixtureAttempted) {
            try {
                & tmux.exe -u -L $socketName kill-session -t $sessionName 2>$null
                if ($LASTEXITCODE -eq 0) {
                    $killSucceeded = $true
                }
                elseif ($null -eq $cleanupFailure) {
                    $cleanupFailure = "exact psmux session cleanup failed with exit " +
                        "$LASTEXITCODE"
                }
            }
            catch {
                if ($null -eq $cleanupFailure) {
                    $cleanupFailure =
                        "exact psmux session cleanup failed: $($_.Exception.Message)"
                }
            }
        }

        if ($fixtureAttempted) {
            try {
                $residueDeadline = [DateTime]::UtcNow.AddSeconds(3)
                do {
                    $residue = @(
                        Get-ChildItem -LiteralPath $registryRoot `
                            -Filter "$socketName`__$sessionName.*" `
                            -File -ErrorAction SilentlyContinue
                    )
                    $liveResidue = @(
                        $residue | Where-Object {
                            $_.Extension -in ".port", ".key", ".pid"
                        }
                    )
                    if ($liveResidue.Count -eq 0) {
                        break
                    }
                    Start-Sleep -Milliseconds 100
                } while ([DateTime]::UtcNow -lt $residueDeadline)
                if ($liveResidue.Count -ne 0 -and $null -eq $cleanupFailure) {
                    $cleanupFailure =
                        "psmux left $($liveResidue.Count) exact live registry file(s)"
                }

                $unknownResidue = @(
                    $residue | Where-Object {
                        $_.Extension -notin ".sid", ".port", ".key", ".pid"
                    }
                )
                if ($unknownResidue.Count -ne 0 -and $null -eq $cleanupFailure) {
                    $cleanupFailure =
                        "psmux left $($unknownResidue.Count) unknown exact registry file(s)"
                }

                if ($killSucceeded -and $liveResidue.Count -eq 0 -and
                    $unknownResidue.Count -eq 0) {
                    foreach ($file in @(
                        $residue | Where-Object { $_.Extension -eq ".sid" }
                    )) {
                        try {
                            Remove-Item `
                                -LiteralPath $file.FullName `
                                -Force `
                                -ErrorAction Stop
                        }
                        catch {
                            if ($null -eq $cleanupFailure) {
                                $cleanupFailure =
                                    "failed to remove exact SID file: " +
                                    "$($_.Exception.Message)"
                            }
                        }
                    }
                }
                $sidResidue = @(
                    Get-ChildItem -LiteralPath $registryRoot `
                        -Filter "$socketName`__$sessionName.sid" `
                        -File -ErrorAction SilentlyContinue
                )
                if ($killSucceeded -and $liveResidue.Count -eq 0 -and
                    $unknownResidue.Count -eq 0 -and
                    $sidResidue.Count -ne 0 -and $null -eq $cleanupFailure) {
                    $cleanupFailure =
                        "psmux left $($sidResidue.Count) exact SID registry file(s)"
                }
                $fixtureCleanupProven =
                    $killSucceeded -and $liveResidue.Count -eq 0 -and
                    $unknownResidue.Count -eq 0 -and $sidResidue.Count -eq 0
            }
            catch {
                if ($null -eq $cleanupFailure) {
                    $cleanupFailure =
                        "psmux registry cleanup failed: $($_.Exception.Message)"
                }
            }
        }
    }
    finally {
        try {
            foreach ($name in $environmentNames) {
                try {
                    [Environment]::SetEnvironmentVariable(
                        $name,
                        $originalEnvironment[$name],
                        "Process"
                    )
                }
                catch {
                    if ($null -eq $cleanupFailure) {
                        $cleanupFailure =
                            "failed to restore ${name}: $($_.Exception.Message)"
                    }
                }
            }
        }
        finally {
            try {
                if ($fixtureCleanupProven -and
                    (Test-Path -LiteralPath $configPath)) {
                    Remove-Item -LiteralPath $configPath -Force -ErrorAction Stop
                }
                if ($fixtureCleanupProven -and
                    (Test-Path -LiteralPath $configPath)) {
                    if ($null -eq $cleanupFailure) {
                        $cleanupFailure = "task config still exists after cleanup"
                    }
                }
            }
            catch {
                if ($null -eq $cleanupFailure) {
                    $cleanupFailure =
                        "failed to remove task config: $($_.Exception.Message)"
                }
            }
        }
    }
    if ($null -ne $cleanupFailure) {
        if (-not $fixtureCleanupProven -or [IO.File]::Exists($configPath)) {
            throw "$cleanupFailure; recovery socket=$socketName " +
                "session=$sessionName config=$configPath"
        }
        throw $cleanupFailure
    }
}
