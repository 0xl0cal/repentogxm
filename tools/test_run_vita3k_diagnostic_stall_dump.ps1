<#[
Static and optional live-cdb regression test for the Vita3K stall capture.

The default run has no external debugger dependency.  Pass -CdbPath to attach
to a disposable PowerShell process, write a full-memory dump through a path
containing spaces, detach, and prove that the target remains alive.
]#>
[CmdletBinding()]
param(
    [string]$CdbPath = ''
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$runnerPath = Join-Path $PSScriptRoot 'run_vita3k_diagnostic.ps1'
$tokens = $null
$parseErrors = $null
$ast = [System.Management.Automation.Language.Parser]::ParseFile(
    $runnerPath, [ref]$tokens, [ref]$parseErrors)
if ($parseErrors.Count -ne 0) {
    throw "Runner parse failed: $($parseErrors[0].Message)"
}
$runnerText = Get-Content -LiteralPath $runnerPath -Raw

$requiredFragments = @(
    '[string]$StallDebuggerPath',
    "'.dump /ma /o `"' + `$stallDumpPath + '`"'",
    "'qd'",
    'Invoke-StallDumpCapture',
    'Capture-ExactWindow $stallImage',
    'stall_dump_status=',
    'stall_dump_sha256='
)
foreach ($fragment in $requiredFragments) {
    if (-not $runnerText.Contains($fragment)) {
        throw "Runner is missing stall-capture fragment: $fragment"
    }
}
if ($runnerText -notmatch
        '(?s)Invoke-StallDumpCapture\s*\}\s*catch.*?Capture-ExactWindow \$stallImage.*?throw "No present heartbeat advanced') {
    throw 'Stall capture is not ordered before the watchdog failure/cleanup.'
}

$functionAst = $ast.Find({
    param($node)
    $node -is [System.Management.Automation.Language.FunctionDefinitionAst] -and
        $node.Name -eq 'Invoke-StallDumpCapture'
}, $true)
if (-not $functionAst) { throw 'Invoke-StallDumpCapture AST was not found.' }
Invoke-Expression $functionAst.Extent.Text

if (-not $CdbPath) {
    Write-Host 'Vita3K stall dump runner: PASS (static; live cdb skipped)'
    exit 0
}

$resolvedStallDebugger = (Resolve-Path -LiteralPath $CdbPath).Path
$testRoot = Join-Path $env:TEMP (
    'isaac stall dump test ' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $testRoot | Out-Null
$stallDumpPath = Join-Path $testRoot 'stall full.dmp'
$stallCdbLog = Join-Path $testRoot 'stall cdb.log'
$stallCdbStdout = Join-Path $testRoot 'stall cdb stdout.log'
$stallCdbCommands = Join-Path $testRoot 'stall cdb commands.txt'
$script:stallDumpStatus = 'armed'
$script:vita3kProcess = $null
$script:vita3kPid = 0

try {
    $script:vita3kProcess = Start-Process -FilePath powershell.exe -ArgumentList @(
        '-NoProfile', '-Command', 'Start-Sleep -Seconds 30') -PassThru
    $script:vita3kPid = $script:vita3kProcess.Id
    Invoke-StallDumpCapture

    $script:vita3kProcess.Refresh()
    if ($script:vita3kProcess.HasExited) {
        throw 'Disposable target exited after cdb detached.'
    }
    if ($script:stallDumpStatus -ne 'captured') {
        throw "Unexpected capture state: $($script:stallDumpStatus)"
    }
    if ((Get-Item -LiteralPath $stallDumpPath).Length -lt 1MB) {
        throw 'Live cdb dump is unexpectedly small.'
    }
    $commandText = Get-Content -LiteralPath $stallCdbCommands -Raw
    if (-not $commandText.Contains('.dump /ma /o "' + $stallDumpPath + '"')) {
        throw 'Command file did not preserve the quoted dump path.'
    }
    if ((Get-Content -LiteralPath $stallCdbLog -Raw) -notmatch
            'REPENTOGXM_STALL_CAPTURE_(BEGIN|END)') {
        throw 'Live cdb log does not contain the capture marker.'
    }
    Write-Host 'Vita3K stall dump runner: PASS (static + live cdb detach)'
} finally {
    if ($script:vita3kProcess) {
        $script:vita3kProcess.Refresh()
        if (-not $script:vita3kProcess.HasExited) {
            Stop-Process -Id $script:vita3kProcess.Id -Force
            $script:vita3kProcess.WaitForExit()
        }
    }
    if (Test-Path -LiteralPath $testRoot) {
        Remove-Item -LiteralPath $testRoot -Recurse -Force
    }
}
