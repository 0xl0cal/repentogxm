param(
    [string]$OwnedSourceAsset = '',
    [string]$CapturedVertexGxp = '',
    [string]$CapturedFragmentGxp = ''
)

$ErrorActionPreference = 'Stop'

$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$runtime = Join-Path $root 'runtime'
$stock = Join-Path $PSScriptRoot 'vitagl-stock-reference'
$work = if ($env:ISAAC_COLOROFFSET_HOST_OUT) {
    $env:ISAAC_COLOROFFSET_HOST_OUT
} else {
    Join-Path $env:TEMP 'isaac-coloroffset-gpu-oracle'
}
New-Item -ItemType Directory -Path $work -Force | Out-Null

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere)) {
    throw 'vswhere.exe was not found'
}
$installation = & $vswhere -products * -latest `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
    -property installationPath
if (-not $installation) {
    throw 'Visual C++ tools were not found'
}
$developerShell = Join-Path $installation 'Common7\Tools\VsDevCmd.bat'

$sourceExe = Join-Path $work 'coloroffset-source-oracle.exe'
$policyExe = Join-Path $work 'coloroffset-policy-oracle.exe'
$objectDirectory = $work + '\\'
$sourceCommand = '"' + $developerShell + '" -arch=x64 -host_arch=x64 >nul && ' +
    'cl /nologo /std:c11 /O2 /W4 /WX /I"' + $runtime + '" ' +
    '"' + (Join-Path $runtime 'gl_vita_coloroffset_source.c') + '" ' +
    '"' + (Join-Path $runtime 'gl_vita_coloroffset_source_oracle.c') + '" ' +
    '/Fe:"' + $sourceExe + '" /Fo:"' + $objectDirectory + '"'
$policyCommand = '"' + $developerShell + '" -arch=x64 -host_arch=x64 >nul && ' +
    'cl /nologo /std:c11 /O2 /W4 /WX /I"' + $stock + '" ' +
    '"' + (Join-Path $stock 'coloroffset_gpu_policy_oracle.c') + '" ' +
    '/Fe:"' + $policyExe + '" /Fo:"' + $objectDirectory + '"'

& cmd.exe /d /s /c $sourceCommand
if ($LASTEXITCODE -ne 0) { throw "source oracle compilation failed: $LASTEXITCODE" }
if (([bool]$OwnedSourceAsset -ne [bool]$CapturedVertexGxp) -or
    ([bool]$OwnedSourceAsset -ne [bool]$CapturedFragmentGxp)) {
    throw 'OwnedSourceAsset and both captured GXP paths must be supplied together'
}
if ($OwnedSourceAsset) {
    $ownedSource = (Resolve-Path -LiteralPath $OwnedSourceAsset).Path
    $vertexGxp = (Resolve-Path -LiteralPath $CapturedVertexGxp).Path
    $fragmentGxp = (Resolve-Path -LiteralPath $CapturedFragmentGxp).Path
    & $sourceExe $ownedSource
} else {
    & $sourceExe
}
if ($LASTEXITCODE -ne 0) { throw "source oracle failed: $LASTEXITCODE" }

& cmd.exe /d /s /c $policyCommand
if ($LASTEXITCODE -ne 0) { throw "policy oracle compilation failed: $LASTEXITCODE" }
if ($OwnedSourceAsset) {
    & $policyExe $vertexGxp $fragmentGxp
} else {
    & $policyExe
}
if ($LASTEXITCODE -ne 0) { throw "policy oracle failed: $LASTEXITCODE" }

Get-FileHash -Algorithm SHA256 -Path @(
    (Join-Path $runtime 'gl_vita_coloroffset_source.c')
    (Join-Path $runtime 'gl_vita_coloroffset_source_oracle.c')
    (Join-Path $stock 'isaac_coloroffset_gpu_policy.h')
    (Join-Path $stock 'coloroffset_gpu_policy_oracle.c')
    $sourceExe
    $policyExe
)
