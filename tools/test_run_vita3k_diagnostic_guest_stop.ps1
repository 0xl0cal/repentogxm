<#
Focused fixture test for Vita3K soak failure precedence and run boundaries.
It parses the runner and invokes only its pure failure classifier; Vita3K is
never started.
#>
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
        $node.Name -eq 'Get-SoakTerminalFailure'
}, $true)
if (-not $functionAst) {
    throw 'Get-SoakTerminalFailure AST was not found.'
}
Invoke-Expression $functionAst.Extent.Text

$stale = @'
[59.944 thr 0x9] guest stop: run=7 addr=0x11111111 fault=stale.dll!old return=0x22222222
[59.945 thr 0x9] KAGE BOUNDARY FAIL: stale pre-run evidence
'@
$runStart = $stale.Length

$guestOutput = $stale + @'
[1.000 thr 0x9] [kage-vita] present heartbeat count=41
[1.250 thr 0x9] guest stop: run=1 addr=0x98606480 fault=VCRUNTIME140.dll!_CxxThrowException return=0x005ebdb5
'@
$guestBeforeStall = Get-SoakTerminalFailure `
    -Output $guestOutput `
    -CurrentRunOutputStart $runStart `
    -PresentStalled $true `
    -PresentStallSeconds 30 `
    -LastPresentCount 41
if ($guestBeforeStall.Kind -ne 'guest-stop') {
    throw "Guest stop lost precedence to: $($guestBeforeStall.Kind)"
}
$expectedGuestFailure = 'Primary guest fault: guest stop: run=1 addr=0x98606480 fault=VCRUNTIME140.dll!_CxxThrowException return=0x005ebdb5'
if ($guestBeforeStall.Message -ne $expectedGuestFailure) {
    throw "Unexpected primary guest failure: $($guestBeforeStall.Message)"
}

$noProgressOutput = $stale + @'
[1.000 thr 0x9] [kage-vita] present heartbeat count=41
[31.000 thr 0x9] unrelated diagnostic output
'@
$realStall = Get-SoakTerminalFailure `
    -Output $noProgressOutput `
    -CurrentRunOutputStart $runStart `
    -PresentStalled $true `
    -PresentStallSeconds 30 `
    -LastPresentCount 41
if ($realStall.Kind -ne 'present-stall' -or
        $realStall.Message -ne
        'No present heartbeat advanced for 30 seconds (last count 41).') {
    throw "Real no-progress was not classified as a stall: $($realStall | Out-String)"
}

$benignOutput = $stale + @'
[1.000 thr 0x9] [kage-vita] present heartbeat count=42
'@
$staleHit = Get-SoakTerminalFailure `
    -Output $benignOutput `
    -CurrentRunOutputStart $runStart `
    -PresentStalled $false `
    -PresentStallSeconds 30 `
    -LastPresentCount 42
if ($null -ne $staleHit) {
    throw "Stale/pre-run guest text produced a false hit: $($staleHit.Kind)"
}

$boundaryOutput = $stale + @'
[2.000 thr 0x9] KAGE BOUNDARY FAIL: actual fault=malloc addr=0x98606400
'@
$boundaryBeforeStall = Get-SoakTerminalFailure `
    -Output $boundaryOutput `
    -CurrentRunOutputStart $runStart `
    -PresentStalled $true `
    -PresentStallSeconds 30 `
    -LastPresentCount 42
if ($boundaryBeforeStall.Kind -ne 'boundary-fail') {
    throw "Boundary failure lost precedence to: $($boundaryBeforeStall.Kind)"
}

Write-Host 'Vita3K guest-stop fail-fast runner: PASS (fixtures only)'
