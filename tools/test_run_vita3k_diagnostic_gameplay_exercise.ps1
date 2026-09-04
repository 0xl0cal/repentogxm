<# Host behavior oracle for the opt-in Vita3K gameplay exercise. #>
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
        '[switch]$ExerciseGameplay',
        'gameplay_exercise=',
        'gameplay_exercise_pulses=',
        '[int]$MenuInputDelaySeconds = 5',
        '[switch]$RequireInputTransitions',
        'input_delivery=exact-foreground-keybd-event',
        'SendKeyToExactForeground',
        'ReleaseGlobalKey',
        'Release-ExerciseKeys')) {
    if (-not $runnerText.Contains($fragment)) {
        throw "Runner is missing gameplay-exercise fragment: $fragment"
    }
}
if ($runnerText.Contains('PostKeyMessage') -or
        $runnerText.Contains('[RepentogxmVita3KWin32]::keybd_event')) {
    throw 'Runner bypasses its exact-foreground native input helper.'
}
if ($runnerText.Contains('$RequireInputTransitions -and $inputRequested')) {
    throw 'Explicit transition validation is incorrectly disabled for external input.'
}

Add-Type @"
using System;
using System.Collections.Generic;
public static class RepentogxmVita3KWin32 {
    public static readonly List<string> Events = new List<string>();
    public static int SendCount;
    public static int FailOnSend = -1;
    public static IntPtr Foreground = new IntPtr(1234);
    public static bool SendKeyToExactForeground(IntPtr h, uint pid,
            byte vk, byte scan, bool down) {
        Events.Add(h.ToString() + ":" + pid.ToString() + ":" + vk.ToString() + ":" +
            scan.ToString() + ":" + down.ToString());
        return SendCount++ != FailOnSend;
    }
    public static void ReleaseGlobalKey(byte vk, byte scan) {
        Events.Add("global:" + vk.ToString() + ":" + scan.ToString());
    }
    public static IntPtr GetForegroundWindow() { return Foreground; }
    public static bool HasExactThreadFocus(IntPtr h, uint expectedPid) {
        return h == new IntPtr(1234) && expectedPid == 77u;
    }
}
"@

function Get-ExactWindow { return [IntPtr]1234 }
function Focus-ExactWindow { return [IntPtr]1234 }
foreach ($name in @('Release-ExerciseKeys', 'Press-ExercisePair')) {
    $functionAst = $ast.Find({
        param($node)
        $node -is [System.Management.Automation.Language.FunctionDefinitionAst] -and
            $node.Name -eq $name
    }, $true)
    if (-not $functionAst) { throw "$name AST was not found." }
    Invoke-Expression $functionAst.Extent.Text
}

$script:exerciseHeldKeys = @()
$script:exercisePulseCount = 0
$script:vita3kPid = 77
for ($index = 0; $index -lt 4; ++$index) {
    Press-ExercisePair $index
    if ($script:exerciseHeldKeys.Count -ne 2) {
        throw "Pair $index did not hold exactly two keys."
    }
    Release-ExerciseKeys
    if ($script:exerciseHeldKeys.Count -ne 0) {
        throw "Pair $index left a held key after release."
    }
}

$expected = @(
    '1234:77:87:17:True', '1234:77:73:23:True',
    'global:87:17', 'global:73:23',
    '1234:77:68:32:True', '1234:77:76:38:True',
    'global:68:32', 'global:76:38',
    '1234:77:83:31:True', '1234:77:75:37:True',
    'global:83:31', 'global:75:37',
    '1234:77:65:30:True', '1234:77:74:36:True',
    'global:65:30', 'global:74:36'
)
$actual = @([RepentogxmVita3KWin32]::Events)
if ($actual.Count -ne $expected.Count) {
    throw "Expected $($expected.Count) key events, got $($actual.Count)."
}
for ($index = 0; $index -lt $expected.Count; ++$index) {
    if ($actual[$index] -ne $expected[$index]) {
        throw "Key event ${index}: expected $($expected[$index]), got $($actual[$index])."
    }
}
if ($script:exercisePulseCount -ne 4) {
    throw "Expected four exercise pulses, got $($script:exercisePulseCount)."
}

[RepentogxmVita3KWin32]::Events.Clear()
$script:exerciseHeldKeys = @(
    [pscustomobject]@{ VirtualKey = [byte]0x57; ScanCode = [byte]0x11 },
    [pscustomobject]@{ VirtualKey = [byte]0x49; ScanCode = [byte]0x17 }
)
Release-ExerciseKeys -BestEffort
if ((@([RepentogxmVita3KWin32]::Events) -join ',') -ne
        'global:87:17,global:73:23' -or
        $script:exerciseHeldKeys.Count -ne 0) {
    throw 'Best-effort cleanup did not issue unconditional global key-up events.'
}

[RepentogxmVita3KWin32]::Events.Clear()
[RepentogxmVita3KWin32]::SendCount = 0
[RepentogxmVita3KWin32]::FailOnSend = 1
$partialFailed = $false
try { Press-ExercisePair 0 } catch { $partialFailed = $true }
if (-not $partialFailed -or
        (@([RepentogxmVita3KWin32]::Events) -join ',') -ne
        '1234:77:87:17:True,1234:77:73:23:True,global:87:17') {
    throw 'Partial pair failure did not release the already-pressed global key.'
}

Write-Host 'Vita3K gameplay exercise: PASS (PID-bound input; unconditional cleanup)'
