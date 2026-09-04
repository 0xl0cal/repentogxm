[CmdletBinding()]
param(
    [string]$Runner = ''
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

if (-not $Runner) {
    $Runner = Join-Path $PSScriptRoot 'run_vita3k_diagnostic.ps1'
}

$runnerPath = (Resolve-Path -LiteralPath $Runner).Path
$tokens = $null
$parseErrors = $null
$runnerAst = [System.Management.Automation.Language.Parser]::ParseFile(
    $runnerPath, [ref]$tokens, [ref]$parseErrors)
if ($parseErrors.Count -ne 0) {
    throw "Runner parse failed: $($parseErrors[0].Message)"
}

$functionAsts = @($runnerAst.FindAll({
    param($node)
    $node -is [System.Management.Automation.Language.FunctionDefinitionAst]
}, $true))
foreach ($functionName in @(
        'Confirm-InstalledOpenALConfig',
        'Get-OpenALAudioContractFailure')) {
    $matches = @($functionAsts | Where-Object Name -eq $functionName)
    if ($matches.Count -ne 1) {
        throw "Expected one $functionName definition, found $($matches.Count)."
    }
    . ([scriptblock]::Create($matches[0].Extent.Text))
}

function Assert-ThrowsLike {
    param(
        [Parameter(Mandatory = $true)]
        [scriptblock]$Action,
        [Parameter(Mandatory = $true)]
        [string]$Pattern
    )

    try {
        & $Action
    } catch {
        if ($_.Exception.Message -notmatch $Pattern) {
            throw "Unexpected failure: $($_.Exception.Message)"
        }
        return
    }
    throw "Expected failure matching: $Pattern"
}

$expectedHash = 'B82EA13EDBC3A88C83F360FCDA2C5E470A6411517352C8B43842C9AB716CF9BE'
$sourceConfigPath = Join-Path $PSScriptRoot '..\recomp\vita\alsoft.conf'
$sourceConfig = (Resolve-Path -LiteralPath $sourceConfigPath).Path
$sourceHash = (Get-FileHash -LiteralPath $sourceConfig -Algorithm SHA256).Hash
if ($sourceHash -ne $expectedHash) {
    throw "Source alsoft.conf hash changed: $sourceHash"
}

$testRoot = Join-Path ([IO.Path]::GetTempPath()) ("repentogxm-openal-runner-{0}" -f [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $testRoot | Out-Null
try {
    $missingConfig = Join-Path $testRoot 'missing\alsoft.conf'
    Assert-ThrowsLike {
        Confirm-InstalledOpenALConfig -Path $missingConfig `
            -ExpectedSha256 $expectedHash
    } 'Installed OpenAL config not found'

    $wrongConfig = Join-Path $testRoot 'old-starving-alsoft.conf'
    [IO.File]::WriteAllText($wrongConfig, "[general]`nsources = 64`n")
    Assert-ThrowsLike {
        Confirm-InstalledOpenALConfig -Path $wrongConfig `
            -ExpectedSha256 $expectedHash
    } 'Installed OpenAL config hash mismatch'

    Assert-ThrowsLike {
        Confirm-InstalledOpenALConfig -Path $wrongConfig `
            -ExpectedSha256 'not-a-sha'
    } 'exactly 64 hex digits'

    $exactConfig = Join-Path $testRoot 'alsoft.conf'
    Copy-Item -LiteralPath $sourceConfig -Destination $exactConfig
    $actualHash = Confirm-InstalledOpenALConfig -Path $exactConfig `
        -ExpectedSha256 $expectedHash.ToLowerInvariant()
    if ($actualHash -ne $expectedHash) {
        throw "Exact config returned the wrong hash: $actualHash"
    }

    $readyLog = 'KAGE VITA AUDIO INIT: status=ready stage=complete ' +
        'manager_sources=64 device_sources=80 stream_headroom=16'
    if (Get-OpenALAudioContractFailure -Output $readyLog -Required $true) {
        throw 'Audio-ready fixture failed the runtime contract.'
    }

    $mismatchLog =
        'KAGE VITA AUDIO INIT: status=source-capacity-mismatch stage=openal'
    $failure = Get-OpenALAudioContractFailure -Output $mismatchLog `
        -Required $true
    if ($failure -ne 'OpenAL source-capacity mismatch was logged') {
        throw "Mismatch fixture returned: $failure"
    }

    $missingLog = 'Missing file at C:\app\alsoft.conf (target path: app0:/alsoft.conf)'
    $failure = Get-OpenALAudioContractFailure -Output $missingLog -Required $true
    if ($failure -ne 'installed OpenAL config was missing at runtime') {
        throw "Missing-file fixture returned: $failure"
    }

    $failure = Get-OpenALAudioContractFailure -Output 'unrelated output' `
        -Required $true
    if ($failure -ne 'OpenAL ready marker was not logged') {
        throw "No-ready fixture returned: $failure"
    }

    $hostileAudioOffLog = "$missingLog`n$mismatchLog"
    if (Get-OpenALAudioContractFailure -Output $hostileAudioOffLog `
            -Required $false) {
        throw 'Disabled Audio-ON contract changed Audio-OFF behavior.'
    }

    $runnerText = [IO.File]::ReadAllText($runnerPath)
    foreach ($requiredText in @(
            'openal_config_sha256=',
            'audio_ready=',
            'audio_source_capacity_mismatch=',
            'audio_config_missing=')) {
        if (-not $runnerText.Contains($requiredText)) {
            throw "Runner summary field is missing: $requiredText"
        }
    }
} finally {
    $resolvedTestRoot = [IO.Path]::GetFullPath($testRoot)
    $resolvedTempRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
    if ($resolvedTestRoot -eq $resolvedTempRoot -or
            -not $resolvedTestRoot.StartsWith(
                $resolvedTempRoot, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing unsafe test cleanup: $resolvedTestRoot"
    }
    Remove-Item -LiteralPath $resolvedTestRoot -Recurse -Force
}

Write-Host 'Vita3K OpenAL config runner contract: PASS (no emulator launched)'
