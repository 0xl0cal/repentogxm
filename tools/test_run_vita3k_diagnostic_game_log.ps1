<# Host behavior oracle for stale-tail-safe guest and native log rotation. #>
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
$expectedNativePath = "'ux0\data\isaacr001\first-arm-fault.log'"
$staleNativePath = "'ux0\data\isaac-first-arm\first-arm-fault.log'"
if ([regex]::Matches($runnerText, [regex]::Escape($expectedNativePath)).Count -ne 1 -or
        $runnerText.Contains($staleNativePath)) {
    throw 'Runner production native-log path is not exact.'
}
foreach ($name in @(
        'Rotate-GameLogForRun', 'Finalize-GameLogEvidence',
        'Rotate-NativeLogForRun', 'Finalize-NativeLogEvidence')) {
    $functionAst = $ast.Find({
        param($node)
        $node -is [System.Management.Automation.Language.FunctionDefinitionAst] -and
            $node.Name -eq $name
    }, $true)
    if (-not $functionAst) { throw "$name AST was not found." }
    Invoke-Expression $functionAst.Extent.Text
}

$root = Join-Path $env:TEMP (
    'isaac-game-log-test-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $root | Out-Null
try {
    $gameLogPath = Join-Path $root 'save\log.txt'
    $gameLogPreRun = Join-Path $root 'evidence\game-log.pre-run.txt'
    $gameLogCurrent = Join-Path $root 'evidence\game-log.current.txt'
    $nativeLogPath = Join-Path $root 'native\first-arm-fault.log'
    $nativeLogPreRun = Join-Path $root 'evidence\first-arm-fault.pre-run.log'
    $nativeLogCurrent = Join-Path $root 'evidence\first-arm-fault.current.log'
    New-Item -ItemType Directory -Path (Split-Path -Parent $gameLogPath) | Out-Null
    New-Item -ItemType Directory -Path (Split-Path -Parent $gameLogPreRun) | Out-Null
    New-Item -ItemType Directory -Path (Split-Path -Parent $nativeLogPath) | Out-Null
    $script:vita3kProcess = [pscustomobject]@{ Id = 1 }

    Set-Content -LiteralPath $gameLogPath -Value 'old-log' -NoNewline
    $script:gameLogRotation = 'source-missing'
    Rotate-GameLogForRun
    if ((Test-Path -LiteralPath $gameLogPath) -or
            (Get-Content -LiteralPath $gameLogPreRun -Raw) -ne 'old-log') {
        throw 'Existing log was not moved intact before launch.'
    }
    Set-Content -LiteralPath $gameLogPath -Value 'new-log' -NoNewline
    Finalize-GameLogEvidence
    if ((Get-Content -LiteralPath $gameLogCurrent -Raw) -ne 'new-log' -or
            (Get-Content -LiteralPath $gameLogPreRun -Raw) -ne 'old-log' -or
            (Get-Content -LiteralPath $gameLogPath -Raw) -ne 'new-log' -or
            $script:gameLogRotation -ne 'captured-current') {
        throw 'Current and pre-run logs were not preserved separately.'
    }

    Set-Content -LiteralPath $nativeLogPath -Value 'old-native' -NoNewline
    $script:nativeLogRotation = 'source-missing'
    Rotate-NativeLogForRun
    if ((Test-Path -LiteralPath $nativeLogPath) -or
            (Get-Content -LiteralPath $nativeLogPreRun -Raw) -ne 'old-native') {
        throw 'Existing native log was not moved intact before launch.'
    }
    Set-Content -LiteralPath $nativeLogPath -Value 'new-native' -NoNewline
    Finalize-NativeLogEvidence
    if ((Get-Content -LiteralPath $nativeLogCurrent -Raw) -ne 'new-native' -or
            (Get-Content -LiteralPath $nativeLogPreRun -Raw) -ne 'old-native' -or
            (Get-Content -LiteralPath $nativeLogPath -Raw) -ne 'new-native' -or
            $script:nativeLogRotation -ne 'captured-current') {
        throw 'Current and pre-run native logs were not preserved separately.'
    }

    Remove-Item -LiteralPath $gameLogCurrent, $gameLogPreRun, $gameLogPath -Force
    Set-Content -LiteralPath $gameLogPath -Value 'restore-me' -NoNewline
    $script:gameLogRotation = 'source-missing'
    Rotate-GameLogForRun
    Finalize-GameLogEvidence
    if ((Get-Content -LiteralPath $gameLogPath -Raw) -ne 'restore-me' -or
            (Test-Path -LiteralPath $gameLogPreRun) -or
            $script:gameLogRotation -ne 'restored-no-current') {
        throw 'Pre-run log was not restored when the game created no new log.'
    }

    Remove-Item -LiteralPath $nativeLogCurrent, $nativeLogPreRun, `
        $nativeLogPath -Force
    Set-Content -LiteralPath $nativeLogPath -Value 'restore-native' -NoNewline
    $script:nativeLogRotation = 'source-missing'
    Rotate-NativeLogForRun
    Finalize-NativeLogEvidence
    if ((Get-Content -LiteralPath $nativeLogPath -Raw) -ne 'restore-native' -or
            (Test-Path -LiteralPath $nativeLogPreRun) -or
            $script:nativeLogRotation -ne 'restored-no-current') {
        throw 'Pre-run native log was not restored when no new log was created.'
    }

    Write-Host 'Vita3K guest/native log rotation: PASS (fresh current + no-current restore)'
} finally {
    if (Test-Path -LiteralPath $root) {
        Remove-Item -LiteralPath $root -Recurse -Force
    }
}
