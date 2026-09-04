$ErrorActionPreference = 'Stop'

$root = if ($env:ISAAC_FIRST_FRAME_TEST_ROOT) {
    (Resolve-Path $env:ISAAC_FIRST_FRAME_TEST_ROOT).Path
} else {
    (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
}
$work = Join-Path $env:TEMP 'isaac-first-frame-oracle-x86'
New-Item -ItemType Directory -Path $work -Force | Out-Null

$vswhere = Join-Path ${env:ProgramFiles(x86)} `
    'Microsoft Visual Studio\Installer\vswhere.exe'
$installation = & $vswhere -products * -latest `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
    -property installationPath
if (-not $installation) { throw 'Visual C++ x86 tools were not found' }
$developerShell = Join-Path $installation 'Common7\Tools\VsDevCmd.bat'
$runtime = Join-Path $root 'runtime'
$vita = Join-Path $root 'vita'
$output = Join-Path $work 'gl-vita-first-frame-oracle.exe'
$directOutput = Join-Path $work 'gl-vita-direct-default-oracle.exe'
$config = Join-Path $vita 'gl_vita_first_frame_oracle_config.h'
$command = '"' + $developerShell + '" -arch=x86 -host_arch=x64 >nul && ' +
    'cl /nologo /std:c11 /O2 /W4 /WX /wd4310 ' +
    '/D_CRT_SECURE_NO_WARNINGS /DISAAC_GL_VITA_BACKEND_ORACLE=1 ' +
    '/DISAAC_GL_VITA_FIRST_FRAME_ORACLE=1 /FI"' + $config + '" ' +
    '/I"' + $runtime + '" /I"' + $vita + '" ' +
    '"' + (Join-Path $runtime 'gl_bridge.c') + '" ' +
    '"' + (Join-Path $runtime 'gl_vita_backend.c') + '" ' +
    '"' + (Join-Path $runtime 'guest_stack_legacy_oracle_stub.c') + '" ' +
    '"' + (Join-Path $vita 'gl_vita_first_frame_oracle.c') + '" ' +
    '/Fe:"' + $output + '" /Fo:"' + $work + '\\"'

& cmd.exe /d /s /c $command
if ($LASTEXITCODE -ne 0) { throw "x86 oracle compile failed: $LASTEXITCODE" }
& $output
if ($LASTEXITCODE -ne 0) { throw "x86 oracle failed: $LASTEXITCODE" }

$directCommand = '"' + $developerShell + '" -arch=x86 -host_arch=x64 >nul && ' +
    'cl /nologo /std:c11 /O2 /W4 /WX /wd4310 ' +
    '/D_CRT_SECURE_NO_WARNINGS /DISAAC_GL_VITA_BACKEND_ORACLE=1 ' +
    '/DISAAC_GL_VITA_FIRST_FRAME_ORACLE=1 ' +
    '/DISAAC_VITA_DIRECT_DEFAULT=1 /FI"' + $config + '" ' +
    '/I"' + $runtime + '" /I"' + $vita + '" ' +
    '"' + (Join-Path $runtime 'gl_bridge.c') + '" ' +
    '"' + (Join-Path $runtime 'gl_vita_backend.c') + '" ' +
    '"' + (Join-Path $runtime 'guest_stack_legacy_oracle_stub.c') + '" ' +
    '"' + (Join-Path $vita 'gl_vita_first_frame_oracle.c') + '" ' +
    '/Fe:"' + $directOutput + '" /Fo:"' + $work + '\\"'

& cmd.exe /d /s /c $directCommand
if ($LASTEXITCODE -ne 0) {
    throw "x86 direct-default oracle compile failed: $LASTEXITCODE"
}
& $directOutput
if ($LASTEXITCODE -ne 0) {
    throw "x86 direct-default oracle failed: $LASTEXITCODE"
}

Get-FileHash -Algorithm SHA256 `
    (Join-Path $runtime 'gl_vita_backend.c'), `
    (Join-Path $runtime 'gl_vita_backend.h'), `
    (Join-Path $runtime 'kage_vita_backend.c'), `
    (Join-Path $runtime 'kage_vita_generated_hooks.c'), `
    (Join-Path $runtime 'manual_kage_vita.c'), `
    (Join-Path $vita 'CMakeLists.txt'), `
    (Join-Path $vita 'gl_vita_first_frame_oracle.c'), `
    (Join-Path $vita 'gl_vita_first_frame_oracle_config.h'), `
    (Join-Path $vita 'gl_vita_first_frame_oracle_vitagl.h'), `
    (Join-Path $vita 'gl_vita_first_frame_oracle_vitagl_undef.h'), `
    (Join-Path $vita 'test_gl_vita_first_frame.sh'), `
    $output, `
    $directOutput
