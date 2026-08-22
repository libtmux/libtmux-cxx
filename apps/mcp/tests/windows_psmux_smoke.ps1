[CmdletBinding(DefaultParameterSetName = "Smoke")]
param(
    [Parameter(Mandatory = $true, ParameterSetName = "Smoke")]
    [string] $Server,

    [Parameter(Mandatory = $true, ParameterSetName = "RegistryHelperSelfTest")]
    [switch] $RegistryHelperSelfTest
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Assert-True {
    param([bool] $Condition, [string] $Message)
    if (-not $Condition) {
        throw $Message
    }
}

function Get-PsmuxRegistryFiles {
    param([string] $Root, [string] $Filter)

    try {
        $rootItem = Get-Item -LiteralPath $Root -Force -ErrorAction Stop
    }
    catch [System.Management.Automation.ItemNotFoundException] {
        return
    }
    if (-not $rootItem.PSIsContainer) {
        throw "psmux registry root is not a directory: $Root"
    }
    return @(
        Get-ChildItem -LiteralPath $Root `
            -Filter $Filter `
            -Force -ErrorAction Stop
    )
}

function Resolve-PsmuxProfileRoot {
    param(
        [AllowEmptyString()]
        [string] $UserProfile = [Environment]::GetEnvironmentVariable(
            "USERPROFILE",
            "Process"
        ),
        [AllowEmptyString()]
        [string] $FolderProfile = [Environment]::GetFolderPath("UserProfile"),
        [AllowEmptyString()]
        [string] $HomeDrive = [Environment]::GetEnvironmentVariable(
            "HOMEDRIVE",
            "Process"
        ),
        [AllowEmptyString()]
        [string] $HomePath = [Environment]::GetEnvironmentVariable(
            "HOMEPATH",
            "Process"
        ),
        [AllowEmptyString()]
        [string] $HomeValue = [Environment]::GetEnvironmentVariable(
            "HOME",
            "Process"
        )
    )

    if (-not [string]::IsNullOrWhiteSpace($UserProfile)) {
        return $UserProfile
    }
    if (-not [string]::IsNullOrWhiteSpace($FolderProfile)) {
        return $FolderProfile
    }
    if (-not [string]::IsNullOrWhiteSpace($HomeDrive) -and
        -not [string]::IsNullOrWhiteSpace($HomePath)) {
        $combinedHome = "$HomeDrive$HomePath"
        if ([IO.Directory]::Exists($combinedHome)) {
            return $combinedHome
        }
    }
    if (-not [string]::IsNullOrWhiteSpace($HomeValue)) {
        return $HomeValue
    }
    throw "could not resolve the Windows profile used by psmux"
}

function Test-FullCleanupProven {
    param([bool] $ProcessCleanupProven, [bool] $FixtureCleanupProven)

    return $ProcessCleanupProven -and $FixtureCleanupProven
}

function Invoke-RegistryHelperSelfTest {
    $testRoot = Join-Path (
        [IO.Path]::GetTempPath()
    ) "libtmux-cxx-mcp-registry-$([Guid]::NewGuid().ToString('N'))"
    try {
        $null = [IO.Directory]::CreateDirectory($testRoot)
        $matchingPath = Join-Path $testRoot "owned.sid"
        $matchingDirectoryPath = Join-Path $testRoot "owned.directory"
        [IO.File]::WriteAllText($matchingPath, "")
        $null = [IO.Directory]::CreateDirectory($matchingDirectoryPath)
        [IO.File]::WriteAllText((Join-Path $testRoot "other.sid"), "")

        $missing = @(
            Get-PsmuxRegistryFiles `
                -Root (Join-Path $testRoot "missing") `
                -Filter "owned.*"
        )
        Assert-True ($missing.Count -eq 0) "missing registry root was not empty"

        $matching = @(
            Get-PsmuxRegistryFiles -Root $testRoot -Filter "owned.*"
        )
        Assert-True ($matching.Count -eq 2) "registry filter was not exact"
        Assert-True (
            @(
                $matching | Where-Object {
                    -not $_.PSIsContainer -and $_.FullName -eq $matchingPath
                }
            ).Count -eq 1
        ) "registry helper did not return the exact file"
        Assert-True (
            @(
                $matching | Where-Object {
                    $_.PSIsContainer -and
                    $_.FullName -eq $matchingDirectoryPath
                }
            ).Count -eq 1
        ) "registry helper hid the exact matching directory"

        $fileRootFailed = $false
        try {
            $null = @(
                Get-PsmuxRegistryFiles -Root $matchingPath -Filter "owned.*"
            )
        }
        catch {
            $fileRootFailed = $true
        }
        Assert-True $fileRootFailed "non-directory registry root passed as empty"

        $queryErrorFailed = $false
        try {
            $null = @(
                Get-PsmuxRegistryFiles `
                    -Root "$testRoot$([char]0)invalid" `
                    -Filter "owned.*"
            )
        }
        catch {
            $queryErrorFailed = $true
        }
        Assert-True $queryErrorFailed "invalid registry query passed as empty"

        $separator = [IO.Path]::DirectorySeparatorChar
        $combinedHome = Join-Path $testRoot "combined-home"
        $null = [IO.Directory]::CreateDirectory($combinedHome)
        $resolved = Resolve-PsmuxProfileRoot `
            -UserProfile "user-profile" `
            -FolderProfile "folder-profile" `
            -HomeDrive $testRoot `
            -HomePath "${separator}combined-home" `
            -HomeValue "home-profile"
        Assert-True ($resolved -eq "user-profile") "USERPROFILE did not win"
        $resolved = Resolve-PsmuxProfileRoot `
            -UserProfile "" `
            -FolderProfile "folder-profile" `
            -HomeDrive $testRoot `
            -HomePath "${separator}combined-home" `
            -HomeValue "home-profile"
        Assert-True (
            $resolved -eq "folder-profile"
        ) "GetFolderPath profile did not win"
        $resolved = Resolve-PsmuxProfileRoot `
            -UserProfile "" `
            -FolderProfile "" `
            -HomeDrive $testRoot `
            -HomePath "${separator}combined-home" `
            -HomeValue "home-profile"
        Assert-True (
            $resolved -eq $combinedHome
        ) "existing HOMEDRIVE and HOMEPATH profile did not win"
        $resolved = Resolve-PsmuxProfileRoot `
            -UserProfile "" `
            -FolderProfile "" `
            -HomeDrive $testRoot `
            -HomePath "${separator}missing-home" `
            -HomeValue "home-profile"
        Assert-True ($resolved -eq "home-profile") "HOME fallback did not win"

        Assert-True (
            -not (Test-FullCleanupProven -ProcessCleanupProven $false `
                -FixtureCleanupProven $true)
        ) "fixture proof hid an unproved MCP process"
        Assert-True (
            -not (Test-FullCleanupProven -ProcessCleanupProven $true `
                -FixtureCleanupProven $false)
        ) "process proof hid an unproved psmux fixture"
        Assert-True (
            Test-FullCleanupProven -ProcessCleanupProven $true `
                -FixtureCleanupProven $true
        ) "complete cleanup proof was rejected"

        Write-Output "PASS: fail-closed psmux cleanup helpers"
    }
    finally {
        if (Test-Path -LiteralPath $testRoot) {
            Remove-Item `
                -LiteralPath $testRoot `
                -Recurse -Force -ErrorAction Stop
        }
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

if ($RegistryHelperSelfTest) {
    Invoke-RegistryHelperSelfTest
    exit 0
}

$suffix = [Guid]::NewGuid().ToString("N")
$socketName = "libtmux-cxx-mcp-$suffix"
$sessionName = "mcp-$suffix"
$profileRoot = Resolve-PsmuxProfileRoot
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
$mcpProcessId = $null
$processCleanupProven = $true
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
        Get-PsmuxRegistryFiles `
            -Root $registryRoot `
            -Filter "$socketName`__$sessionName.*"
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
    $processCleanupProven = $false
    $processStarted = $script:mcpProcess.Start()
    if (-not $processStarted) {
        $processCleanupProven = $true
    }
    Assert-True $processStarted "failed to start MCP server"
    $mcpProcessId = [string]$script:mcpProcess.Id

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
    $processExited = $script:mcpProcess.WaitForExit(5000)
    if ($processExited) {
        $processCleanupProven = $true
    }
    Assert-True $processExited "MCP server did not stop after stdin closed"
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
    $processCleanupDetail = $null
    $configPresenceUnproved = $false
    $killSucceeded = -not $fixtureAttempted
    $fixtureCleanupProven = -not $fixtureAttempted
    try {
        if ($null -ne $script:mcpProcess) {
            if (-not $processCleanupProven) {
                if ($null -eq $mcpProcessId) {
                    try {
                        $mcpProcessId = [string]$script:mcpProcess.Id
                    }
                    catch {
                    }
                }
                $processCleanupDetail = $null
                try {
                    $script:mcpProcess.StandardInput.Close()
                }
                catch {
                    $processCleanupDetail = $_.Exception.Message
                }

                $processExited = $false
                try {
                    $processExited = $script:mcpProcess.WaitForExit(3000)
                }
                catch {
                    if ($null -eq $processCleanupDetail) {
                        $processCleanupDetail = $_.Exception.Message
                    }
                }
                if (-not $processExited) {
                    try {
                        $script:mcpProcess.Kill()
                    }
                    catch {
                        if ($null -eq $processCleanupDetail) {
                            $processCleanupDetail = $_.Exception.Message
                        }
                    }
                    try {
                        $processExited = $script:mcpProcess.WaitForExit(3000)
                    }
                    catch {
                        if ($null -eq $processCleanupDetail) {
                            $processCleanupDetail = $_.Exception.Message
                        }
                    }
                }
                if ($processExited) {
                    $processCleanupProven = $true
                }
            }

            if ($processCleanupProven) {
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
        if (-not $processCleanupProven -and $null -eq $cleanupFailure) {
            $cleanupFailure = "MCP process exit could not be proven"
            if (-not [string]::IsNullOrWhiteSpace($processCleanupDetail)) {
                $cleanupFailure += ": $processCleanupDetail"
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
                        Get-PsmuxRegistryFiles `
                            -Root $registryRoot `
                            -Filter "$socketName`__$sessionName.*"
                    )
                    $liveResidue = @(
                        $residue | Where-Object {
                            -not $_.PSIsContainer -and
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
                        $_.PSIsContainer -or
                        $_.Extension -notin ".sid", ".port", ".key", ".pid"
                    }
                )
                if ($unknownResidue.Count -ne 0 -and $null -eq $cleanupFailure) {
                    $cleanupFailure =
                        "psmux left $($unknownResidue.Count) unknown exact registry item(s)"
                }

                if ($killSucceeded -and $liveResidue.Count -eq 0 -and
                    $unknownResidue.Count -eq 0) {
                    foreach ($file in @(
                        $residue | Where-Object {
                            -not $_.PSIsContainer -and $_.Extension -eq ".sid"
                        }
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
                    Get-PsmuxRegistryFiles `
                        -Root $registryRoot `
                        -Filter "$socketName`__$sessionName.sid" |
                        Where-Object { -not $_.PSIsContainer }
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
            $fullCleanupProven = Test-FullCleanupProven `
                -ProcessCleanupProven $processCleanupProven `
                -FixtureCleanupProven $fixtureCleanupProven
            if ($fullCleanupProven) {
                try {
                    Remove-Item `
                        -LiteralPath $configPath `
                        -Force -ErrorAction Stop
                }
                catch [System.Management.Automation.ItemNotFoundException] {
                }
                catch {
                    if ($null -eq $cleanupFailure) {
                        $cleanupFailure =
                            "failed to remove task config: $($_.Exception.Message)"
                    }
                }

                try {
                    $null = Get-Item `
                        -LiteralPath $configPath `
                        -Force -ErrorAction Stop
                    $configPresenceUnproved = $true
                    if ($null -eq $cleanupFailure) {
                        $cleanupFailure = "task config still exists after cleanup"
                    }
                }
                catch [System.Management.Automation.ItemNotFoundException] {
                }
                catch {
                    $configPresenceUnproved = $true
                    if ($null -eq $cleanupFailure) {
                        $cleanupFailure =
                            "failed to verify task config cleanup: " +
                            "$($_.Exception.Message)"
                    }
                }
            }
        }
    }
    if ($null -ne $cleanupFailure) {
        $fullCleanupProven = Test-FullCleanupProven `
            -ProcessCleanupProven $processCleanupProven `
            -FixtureCleanupProven $fixtureCleanupProven
        if (-not $fullCleanupProven -or $configPresenceUnproved) {
            $recovery = "$cleanupFailure; recovery socket=$socketName " +
                "session=$sessionName config=$configPath"
            if (-not $processCleanupProven) {
                $recoveryProcessId = $mcpProcessId
                if ([string]::IsNullOrWhiteSpace($recoveryProcessId)) {
                    $recoveryProcessId = "unknown"
                }
                $recovery += " mcp-pid=$recoveryProcessId"
            }
            throw $recovery
        }
        throw $cleanupFailure
    }
}
