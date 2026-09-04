$ErrorActionPreference = 'Stop'

$root = if ($env:ISAAC_RBO_TEST_ROOT) {
    (Resolve-Path $env:ISAAC_RBO_TEST_ROOT).Path
} else {
    (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
}
$work = if ($env:ISAAC_RBO_HOST_OUT) {
    $env:ISAAC_RBO_HOST_OUT
} else {
    Join-Path $env:TEMP 'isaac-rbo-oracle-x86'
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
    throw 'Visual C++ x86 tools were not found'
}
$developerShell = Join-Path $installation 'Common7\Tools\VsDevCmd.bat'
$runtime = Join-Path $root 'runtime'
$surfacePath = Join-Path $runtime 'gl_surface_generated.inc'
$surface = Get-Content -LiteralPath $surfacePath -Raw
foreach ($name in @(
    'glActiveTexture', 'glBindTexture', 'glBlendFuncSeparate', 'glDepthFunc',
    'glEnableVertexAttribArray', 'glDisableVertexAttribArray',
    'glVertexAttribPointer', 'glViewport', 'glDeleteTextures',
    'glBindFramebuffer', 'glDeleteFramebuffers')) {
    if (-not $surface.Contains('"' + $name + '"')) {
        throw "typed-state surface owner disappeared: $name"
    }
}
# These APIs can mutate a cached tuple or compact ray texture without entering
# its exact wrapper.  Fail closed if the generated guest surface gains one.
foreach ($name in @(
    'glBindTextureUnit', 'glBindTextures',
    'glCopyTexImage2D', 'glCopyTexSubImage2D',
    'glCompressedTexImage2D', 'glCompressedTexSubImage2D',
    'glGenerateMipmap', 'glPixelStorei', 'glTexStorage2D',
    'glTextureStorage2D', 'glTextureSubImage2D', 'glTextureView',
    'glFramebufferTextureLayer', 'glGetTexLevelParameteriv',
    'glBlendFunc', 'glBlendFunci', 'glBlendFuncSeparatei',
    'glBindBuffer', 'glBindVertexArray',
    'glViewportArrayv', 'glViewportIndexedf', 'glViewportIndexedfv',
    'glPushAttrib', 'glPopAttrib')) {
    if ($surface.Contains('"' + $name + '"')) {
        throw "GL backend surface gained an unmodelled mutator: $name"
    }
}
$outputs = @()
foreach ($mode in @(
    'off', 'on', 'typed-no-profile', 'typed', 'typed720',
    'redundancy', 'typed-redundancy', 'typed-bundle', 'coloroffset',
    'canonical', 'fxray', 'texture', 'texture-fxray', 'fbo', 'fbo720',
    'prod-elision', 'prod-loc', 'prod-loc-verify',
    'prod-elision-loc-verify')) {
    $modeWork = Join-Path $work $mode
    New-Item -ItemType Directory -Path $modeWork -Force | Out-Null
    $output = Join-Path $modeWork 'gl-vita-rbo-oracle.exe'
    $objectDirectory = $modeWork + '\\'
    $displayDefine = if (
            $mode -eq 'on' -or $mode -eq 'typed720' -or
            $mode -eq 'typed-bundle' -or $mode -eq 'fbo720') {
        '/DISAAC_VITA_DISPLAY_RASTER_720=1 '
    } else {
        ''
    }
    $redundancyDefine = if (
            $mode -eq 'redundancy' -or $mode -eq 'typed-redundancy' -or
            $mode -eq 'typed-bundle') {
        '/DISAAC_VITA_GL_REDUNDANCY_CACHE=1 /DISAAC_VITA_PHASE_PROFILE=1 '
    } else {
        ''
    }
    $typedDefine = if ($mode -eq 'typed-no-profile') {
        '/DISAAC_VITA_GL_TYPED_STATE_CACHE=1 '
    } elseif ($mode -eq 'typed' -or $mode -eq 'typed720') {
        '/DISAAC_VITA_GL_TYPED_STATE_CACHE=1 /DISAAC_VITA_PHASE_PROFILE=1 '
    } elseif (
            $mode -eq 'typed-redundancy' -or $mode -eq 'typed-bundle') {
        '/DISAAC_VITA_GL_TYPED_STATE_CACHE=1 '
    } else {
        ''
    }
    $coloroffsetDefine = if ($mode -eq 'coloroffset') {
        '/DISAAC_VITA_COLOROFFSET_GPU_OPTIMIZATIONS=1 ' +
        '/DISAAC_VITA_VITAGL_GPU_DRAW_OPTIMIZATIONS=1 '
    } else {
        ''
    }
    $coloroffsetSource = if ($mode -eq 'coloroffset') {
        '"' + (Join-Path $runtime 'gl_vita_coloroffset_source.c') + '" '
    } else {
        ''
    }
    $canonicalDefine = if ($mode -eq 'canonical') {
        '/DISAAC_VITA_CANONICAL_QUAD_ZERO_COPY=1 ' +
        '/DISAAC_VITA_VITAGL_GPU_DRAW_OPTIMIZATIONS=1 ' +
        '/DISAAC_VITA_PHASE_PROFILE=1 '
    } else {
        ''
    }
    $fxrayDefine = if (
            $mode -eq 'fxray' -or $mode -eq 'texture-fxray') {
        '/DISAAC_VITA_FXRAY_ALPHA_MASK=1 '
    } else {
        ''
    }
    $textureDefine = if (
            $mode -eq 'texture' -or $mode -eq 'texture-fxray') {
        '/DISAAC_VITA_TEXTURE_CHURN_PROFILE=1 ' +
        '/DISAAC_VITA_PHASE_PROFILE=1 '
    } else {
        ''
    }
    $fboDefine = if ($mode -eq 'fbo' -or $mode -eq 'fbo720') {
        '/DISAAC_VITA_FBO_CLEAR_ELISION=1 ' +
        '/DISAAC_VITA_FBO_RASTER_SCALE=1 ' +
        '/DISAAC_VITA_FBO_RASTER_SCALE_NUM=1 ' +
        '/DISAAC_VITA_FBO_RASTER_SCALE_DEN=2 ' +
        '/DISAAC_VITA_PHASE_PROFILE=1 '
    } else {
        ''
    }
    # prod-*: the production option set that reaches gl_vita_backend.c (see
    # the CMakeLists.txt GL_REDUNDANCY_CACHE / GL_TYPED_STATE_CACHE /
    # GL_SHIM_FASTDISPATCH / CANONICAL_QUAD_ZERO_COPY scopes plus
    # PHASE_PROFILE) with the device-gated options on top: clear elision
    # alone, the location cache with and without its VERIFY shadow, both.
    $prodDefine = if ($mode -like 'prod-*') {
        '/DISAAC_VITA_GL_REDUNDANCY_CACHE=1 ' +
        '/DISAAC_VITA_GL_TYPED_STATE_CACHE=1 ' +
        '/DISAAC_VITA_GL_SHIM_FASTDISPATCH=1 ' +
        '/DISAAC_VITA_CANONICAL_QUAD_ZERO_COPY=1 ' +
        '/DISAAC_VITA_PHASE_PROFILE=1 '
    } else {
        ''
    }
    $elisionDefine = if (
            $mode -eq 'prod-elision' -or $mode -eq 'prod-elision-loc-verify') {
        '/DISAAC_VITA_FBO_CLEAR_ELISION=1 '
    } else {
        ''
    }
    $locationDefine = if (
            $mode -eq 'prod-loc' -or $mode -eq 'prod-loc-verify' -or
            $mode -eq 'prod-elision-loc-verify') {
        '/DISAAC_VITA_GL_LOCATION_CACHE=1 '
    } else {
        ''
    }
    $verifyDefine = if (
            $mode -eq 'prod-loc-verify' -or $mode -eq 'prod-elision-loc-verify') {
        '/DISAAC_VITA_GL_LOCATION_CACHE_VERIFY=1 '
    } else {
        ''
    }
    $command = '"' + $developerShell + '" -arch=x86 -host_arch=x64 >nul && ' +
        'cl /nologo /std:c11 /O2 /W4 /WX /wd4310 ' +
        '/D_CRT_SECURE_NO_WARNINGS /DISAAC_GL_VITA_BACKEND_ORACLE=1 ' +
        '/DISAAC_VITA_IO_PROFILE=1 ' + $displayDefine + $redundancyDefine +
        $typedDefine +
        $coloroffsetDefine + $canonicalDefine + $fxrayDefine +
        $textureDefine + $fboDefine +
        $prodDefine + $elisionDefine + $locationDefine + $verifyDefine +
        '/DGUEST_IMAGE_BASE=0x98000000u /I"' + $runtime + '" ' +
        '"' + (Join-Path $runtime 'gl_bridge.c') + '" ' +
        '"' + (Join-Path $runtime 'gl_vita_backend.c') + '" ' +
        $coloroffsetSource +
        '"' + (Join-Path $runtime 'guest_stack_legacy_oracle_stub.c') + '" ' +
        '"' + (Join-Path $runtime 'gl_vita_backend_oracle.c') + '" ' +
        '/Fe:"' + $output + '" /Fo:"' + $objectDirectory + '"'

    & cmd.exe /d /s /c $command
    if ($LASTEXITCODE -ne 0) {
        throw "x86 RBO/display-$mode oracle compilation failed: $LASTEXITCODE"
    }
    & $output
    if ($LASTEXITCODE -ne 0) {
        throw "x86 RBO/display-$mode lifecycle oracle failed: $LASTEXITCODE"
    }
    $outputs += $output
}

$hashInputs = @(
    (Join-Path $runtime 'gl_bridge.c')
    (Join-Path $runtime 'gl_vita_backend.c')
    (Join-Path $runtime 'manual_kage_vita.c')
    (Join-Path $runtime 'gl_vita_backend_oracle.c')
    (Join-Path $runtime 'gl_vita_backend_test_vitagl.h')
    $surfacePath
) + $outputs
Get-FileHash -Algorithm SHA256 -Path $hashInputs
