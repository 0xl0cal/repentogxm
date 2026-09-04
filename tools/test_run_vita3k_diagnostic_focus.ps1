<# Host behavior oracle for PID/HWND-bound foreground acquisition. #>
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
        'AttachThreadInput',
        'BringWindowToTop',
        'Exact Vita3K window did not become foreground.',
        'Exact Vita3K GUI thread did not acquire keyboard focus.',
        'SetActiveWindow',
        'SetFocus',
        'HasExactThreadFocus',
        'GetClientRect',
        'ClientToScreen',
        'GetAncestor(hit, 2u) == h',
        'currentOwner == expectedPid',
        'if (!promoted) return false;',
        'if (sent == 1)',
        'input_client_primes=')) {
    if (-not $runnerText.Contains($fragment)) {
        throw "Runner is missing focus fragment: $fragment"
    }
}

# Compile the runner's real native helper under a probe-only type name.  The
# behavioral oracle below uses a fake helper and therefore cannot otherwise
# catch a broken P/Invoke declaration or pointer-sized INPUT layout.
$nativeBlock = [regex]::Match(
    $runnerText, '(?s)Add-Type @"\r?\n(?<code>.*?)\r?\n"@')
if (-not $nativeBlock.Success) {
    throw 'Runner native C# block was not found.'
}
$probeSource = $nativeBlock.Groups['code'].Value.Replace(
    'RepentogxmVita3KWin32', 'RepentogxmVita3KWin32ContractProbe')
Add-Type -TypeDefinition $probeSource
$probeType = [RepentogxmVita3KWin32ContractProbe]
$sizeOfType = [Runtime.InteropServices.Marshal].GetMethods() |
    Where-Object {
        $_.Name -eq 'SizeOf' -and -not $_.IsGenericMethod -and
        $_.GetParameters().Count -eq 1 -and
        $_.GetParameters()[0].ParameterType -eq [Type]
    }
if (@($sizeOfType).Count -ne 1) {
    throw 'Could not select Marshal.SizeOf(Type) for the native-layout probe.'
}
$expectedInputSize = if ([IntPtr]::Size -eq 8) { 40 } else { 28 }
$actualInputSize = [int]$sizeOfType.Invoke(
    $null, @($probeType.GetNestedType('INPUT')))
if ($actualInputSize -ne $expectedInputSize) {
    throw "Native INPUT layout mismatch: expected $expectedInputSize, got $actualInputSize."
}

function Assert-NativeSignature {
    param(
        [string]$Name,
        [string]$ReturnType,
        [string[]]$ParameterTypes
    )
    $method = @($probeType.GetMethods(
        [Reflection.BindingFlags]'Public,NonPublic,Static') |
        Where-Object { $_.Name -eq $Name })
    if ($method.Count -ne 1) {
        throw "Expected one native helper named $Name, got $($method.Count)."
    }
    $actualParameters = @(foreach ($parameter in $method[0].GetParameters()) {
        $parameter.ParameterType.FullName
    })
    if ($method[0].ReturnType.FullName -ne $ReturnType -or
            ($actualParameters -join ',') -ne ($ParameterTypes -join ',')) {
        throw "Native helper signature mismatch: $Name."
    }
}

Assert-NativeSignature -Name 'ClickWindowForForeground' `
    -ReturnType 'System.Boolean' `
    -ParameterTypes @('System.IntPtr', 'System.UInt32')
Assert-NativeSignature -Name 'SendKeyToExactForeground' `
    -ReturnType 'System.Boolean' `
    -ParameterTypes @(
        'System.IntPtr', 'System.UInt32', 'System.Byte', 'System.Byte',
        'System.Boolean')
Assert-NativeSignature -Name 'ReleaseGlobalKey' `
    -ReturnType 'System.Void' `
    -ParameterTypes @('System.Byte', 'System.Byte')
Assert-NativeSignature -Name 'HasExactThreadFocus' `
    -ReturnType 'System.Boolean' `
    -ParameterTypes @('System.IntPtr', 'System.UInt32')

$beforeIndex = $runnerText.IndexOf('Capture-ExactWindow $beforeImage')
$primeIndex = $runnerText.IndexOf(
    'if ($AllowFocusClickFallback -and -not $NoMenuInput)', $beforeIndex)
$clientClickIndex = $runnerText.IndexOf(
    '[RepentogxmVita3KWin32]::ClickWindowForForeground(', $primeIndex)
$menuIndex = $runnerText.IndexOf('if (-not $NoMenuInput) {', $primeIndex)
if ($beforeIndex -lt 0 -or $primeIndex -le $beforeIndex -or
        $clientClickIndex -le $primeIndex -or
        $menuIndex -le $clientClickIndex) {
    throw 'Explicit client-focus prime is not between the before image and first menu input.'
}

Add-Type @"
using System;
using System.Collections.Generic;
public static class RepentogxmVita3KWin32 {
    public static IntPtr Foreground = new IntPtr(9000);
    public static bool BlockForeground;
    public static readonly List<string> Events = new List<string>();
    public static IntPtr GetForegroundWindow() { return Foreground; }
    public static uint GetWindowThreadProcessId(IntPtr h, out uint pid) {
        pid = h == new IntPtr(1234) ? 77u : 88u;
        return h == new IntPtr(1234) ? 30u : 10u;
    }
    public static uint GetCurrentThreadId() { return 20u; }
    public static bool AttachThreadInput(uint from, uint to, bool attach) {
        Events.Add("attach:" + from + ":" + to + ":" + attach);
        return true;
    }
    public static bool ShowWindow(IntPtr h, int n) {
        Events.Add("show:" + h + ":" + n); return true;
    }
    public static bool BringWindowToTop(IntPtr h) {
        Events.Add("top:" + h); return true;
    }
    public static bool SetForegroundWindow(IntPtr h) {
        Events.Add("foreground:" + h);
        if (!BlockForeground) Foreground = h;
        return !BlockForeground;
    }
    public static IntPtr SetActiveWindow(IntPtr h) {
        Events.Add("active:" + h); return IntPtr.Zero;
    }
    public static IntPtr SetFocus(IntPtr h) {
        Events.Add("focus:" + h); return IntPtr.Zero;
    }
    public static bool HasExactThreadFocus(IntPtr h, uint expectedPid) {
        Events.Add("has-focus:" + h + ":" + expectedPid);
        return !BlockForeground && h == new IntPtr(1234) && expectedPid == 77u;
    }
    public static bool ClickWindowForForeground(IntPtr h, uint expectedPid) {
        Events.Add("click:" + h + ":" + expectedPid);
        return false;
    }
}
"@

function Get-ExactWindow { return [IntPtr]1234 }
function Start-Sleep { param([int]$Milliseconds) }
$AllowFocusClickFallback = $false
$script:focusClickFallbackCount = 0
$script:vita3kPid = 77
$functionAst = $ast.Find({
    param($node)
    $node -is [System.Management.Automation.Language.FunctionDefinitionAst] -and
        $node.Name -eq 'Focus-ExactWindow'
}, $true)
if (-not $functionAst) { throw 'Focus-ExactWindow AST was not found.' }
Invoke-Expression $functionAst.Extent.Text

$actualHandle = Focus-ExactWindow
if ($actualHandle -ne [IntPtr]1234) {
    throw "Focus returned the wrong handle: $actualHandle"
}
$expected = @(
    'attach:20:10:True',
    'attach:20:30:True',
    'show:1234:9',
    'top:1234',
    'foreground:1234',
    'active:1234',
    'focus:1234',
    'attach:20:30:False',
    'attach:20:10:False',
    'has-focus:1234:77'
)
$actual = @([RepentogxmVita3KWin32]::Events)
if ($actual.Count -ne $expected.Count) {
    throw "Expected $($expected.Count) focus events, got $($actual.Count)."
}
for ($index = 0; $index -lt $expected.Count; ++$index) {
    if ($actual[$index] -ne $expected[$index]) {
        throw "Focus event ${index}: expected $($expected[$index]), got $($actual[$index])."
    }
}

[RepentogxmVita3KWin32]::Events.Clear()
[RepentogxmVita3KWin32]::Foreground = [IntPtr]9000
[RepentogxmVita3KWin32]::BlockForeground = $true
$AllowFocusClickFallback = $true
$focusFailure = $null
try {
    [void](Focus-ExactWindow)
} catch {
    $focusFailure = $_
}
if (-not $focusFailure -or
        $focusFailure.Exception.Message -ne
            'Exact Vita3K window did not become foreground.') {
    throw 'Focus failure was incorrectly rescued by the client-prime option.'
}
if (@([RepentogxmVita3KWin32]::Events | Where-Object {
            $_ -like 'click:*'
        }).Count -ne 0 -or $script:focusClickFallbackCount -ne 0) {
    throw 'Focus-ExactWindow still uses global click input to acquire foreground.'
}

Write-Host 'Vita3K exact-window focus: PASS (native contract; no click-to-focus fallback)'
