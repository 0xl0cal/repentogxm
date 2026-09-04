<# Host behavior oracle for fail-closed diagnostic evidence directories. #>
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
$functionAst = $ast.Find({
    param($node)
    $node -is [System.Management.Automation.Language.FunctionDefinitionAst] -and
        $node.Name -eq 'Initialize-EvidenceDirectory'
}, $true)
if (-not $functionAst) {
    throw 'Initialize-EvidenceDirectory AST was not found.'
}
Invoke-Expression $functionAst.Extent.Text

$root = Join-Path $env:TEMP (
    'isaac-evidence-dir-test-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $root | Out-Null
try {
    $fresh = Join-Path $root 'fresh'
    $resolvedFresh = Initialize-EvidenceDirectory -Path $fresh
    if (-not (Test-Path -LiteralPath $resolvedFresh -PathType Container)) {
        throw 'A missing evidence directory was not created.'
    }

    $empty = Join-Path $root 'empty'
    New-Item -ItemType Directory -Path $empty | Out-Null
    $resolvedEmpty = Initialize-EvidenceDirectory -Path $empty
    if ($resolvedEmpty -ne (Resolve-Path -LiteralPath $empty).Path) {
        throw 'An empty evidence directory did not resolve exactly.'
    }

    Set-Content -LiteralPath (Join-Path $empty 'old-evidence.txt') `
        -Value 'must-survive' -NoNewline
    try {
        [void](Initialize-EvidenceDirectory -Path $empty)
        throw 'A non-empty evidence directory was accepted.'
    } catch {
        if ($_.Exception.Message -notlike
                'Refusing to overwrite non-empty evidence directory:*') {
            throw
        }
    }
    if ((Get-Content -LiteralPath (Join-Path $empty 'old-evidence.txt') -Raw) `
            -ne 'must-survive') {
        throw 'Rejected evidence was modified.'
    }

    $file = Join-Path $root 'not-a-directory'
    Set-Content -LiteralPath $file -Value 'file' -NoNewline
    try {
        [void](Initialize-EvidenceDirectory -Path $file)
        throw 'A file was accepted as an evidence directory.'
    } catch {
        if ($_.Exception.Message -notlike
                'Evidence output is not a directory:*') {
            throw
        }
    }

    Write-Host 'Vita3K evidence directory: PASS (new/empty only; existing evidence preserved)'
} finally {
    if (Test-Path -LiteralPath $root) {
        Remove-Item -LiteralPath $root -Recurse -Force
    }
}
