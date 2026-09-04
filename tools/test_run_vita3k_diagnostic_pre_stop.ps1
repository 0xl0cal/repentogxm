<# Host behavior oracle for pre-stop versus emulator-shutdown attribution. #>
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
foreach ($fragment in @(
        '$script:preStopOutput = Read-Stdout',
        'shutdown_host_access_violation=',
        'shutdown_invalid_arm_read=',
        '$preStopOutput -match ''guest stop|KAGE BOUNDARY FAIL''')) {
    if (-not $runnerText.Contains($fragment)) {
        throw "Runner is missing pre-stop attribution fragment: $fragment"
    }
}
$captureIndex = $runnerText.IndexOf('$script:preStopOutput = Read-Stdout')
$closeIndex = $runnerText.IndexOf('$script:vita3kProcess.CloseMainWindow()')
if ($captureIndex -lt 0 -or $closeIndex -lt 0 -or $captureIndex -ge $closeIndex) {
    throw 'Runner does not freeze stdout before asking Vita3K to close.'
}

$readStdoutAst = $ast.Find({
    param($node)
    $node -is [System.Management.Automation.Language.FunctionDefinitionAst] -and
        $node.Name -eq 'Read-Stdout'
}, $true)
if (-not $readStdoutAst) {
    throw 'Read-Stdout AST was not found.'
}
Invoke-Expression $readStdoutAst.Extent.Text

# Model the multi-object pipeline result observed while Start-Process was
# appending redirected stdout.  Read-Stdout must preserve all chunks while
# returning one System.String, or the runner's typed soak classifier aborts.
$stdoutPath = Join-Path $env:TEMP (
    'isaac-read-stdout-' + [guid]::NewGuid().ToString('N') + '.log')
[System.IO.File]::WriteAllText($stdoutPath, 'fixture')
function Get-Content {
    [CmdletBinding()]
    param(
        [string]$LiteralPath,
        [switch]$Raw
    )
    'first-'
    'second'
}
try {
    $joinedOutput = Read-Stdout
} finally {
    Remove-Item Function:\Get-Content
    Remove-Item -LiteralPath $stdoutPath -Force
}
if ($joinedOutput -isnot [string] -or
        @($joinedOutput).Count -ne 1 -or
        $joinedOutput -ne 'first-second') {
    throw "Read-Stdout did not normalize concurrent chunks: $($joinedOutput | Out-String)"
}

$stdoutPath = Join-Path $env:TEMP (
    'isaac-read-stdout-growing-' + [guid]::NewGuid().ToString('N') + '.log')
[System.IO.File]::WriteAllText($stdoutPath, 'seed')
$writerJob = Start-Job -ScriptBlock {
    param([string]$Path)
    $bytes = [System.Text.Encoding]::UTF8.GetBytes(('x' * 4096) + "`n")
    $stream = [System.IO.File]::Open(
        $Path,
        [System.IO.FileMode]::Append,
        [System.IO.FileAccess]::Write,
        [System.IO.FileShare]::ReadWrite)
    try {
        for ($index = 0; $index -lt 80; ++$index) {
            $stream.Write($bytes, 0, $bytes.Length)
            $stream.Flush()
            Start-Sleep -Milliseconds 2
        }
    } finally {
        $stream.Dispose()
    }
} -ArgumentList $stdoutPath
try {
    do {
        $snapshot = Read-Stdout
        if ($snapshot -isnot [string] -or @($snapshot).Count -ne 1) {
            throw 'Read-Stdout returned a non-string during concurrent append.'
        }
        Start-Sleep -Milliseconds 1
    } while ((Get-Job -Id $writerJob.Id).State -eq 'Running')
    Wait-Job -Id $writerJob.Id -Timeout 10 | Out-Null
    Receive-Job -Id $writerJob.Id -ErrorAction Stop | Out-Null
    $snapshot = Read-Stdout
    if ($snapshot -isnot [string] -or @($snapshot).Count -ne 1) {
        throw 'Read-Stdout returned a non-string after concurrent append.'
    }
} finally {
    Stop-Job -Id $writerJob.Id -ErrorAction SilentlyContinue
    Remove-Job -Id $writerJob.Id -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $stdoutPath -Force -ErrorAction SilentlyContinue
}

$functionAst = $ast.Find({
    param($node)
    $node -is [System.Management.Automation.Language.FunctionDefinitionAst] -and
        $node.Name -eq 'Get-DiagnosticAccessCounts'
}, $true)
if (-not $functionAst) {
    throw 'Get-DiagnosticAccessCounts AST was not found.'
}
Invoke-Expression $functionAst.Extent.Text

$pre = @'
game output
EXCEPTION_ACCESS_VIOLATION pre
Invalid read of uint32_t at 0x0
'@
$final = $pre + @'
shutdown output
EXCEPTION_ACCESS_VIOLATION shutdown
EXCEPTION_ACCESS_VIOLATION shutdown2
Invalid read of uint16_t at 0x0
'@
$counts = Get-DiagnosticAccessCounts -PreStopOutput $pre -FinalOutput $final
if ($counts.PreStopHost -ne 1 -or $counts.ShutdownHost -ne 2 -or
        $counts.PreStopArm -ne 1 -or $counts.ShutdownArm -ne 1) {
    throw "Unexpected access attribution: $($counts | Out-String)"
}

$truncatedFinal = Get-DiagnosticAccessCounts `
    -PreStopOutput 'EXCEPTION_ACCESS_VIOLATION' -FinalOutput ''
if ($truncatedFinal.PreStopHost -ne 1 -or $truncatedFinal.ShutdownHost -ne 0) {
    throw 'A truncated final log produced a negative shutdown count.'
}

Write-Host 'Vita3K diagnostic phase attribution: PASS (pre-stop fatal; shutdown separate)'
