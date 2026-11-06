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
& python (Join-Path $PSScriptRoot 'test_vita_attrib_boundaries.py') --output $work
if ($LASTEXITCODE -ne 0) { throw 'Fresh attribute owner extraction failed' }

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
    'glPushAttrib', 'glPopAttrib',
    # ISAAC_VITA_SHADER_ATTRIB_LOCATION_MEMO and the shim location cache
    # rely on every attribute-location mutator bumping the generation;
    # glBindAttribLocation would move locations without one.
    'glBindAttribLocation')) {
    if ($surface.Contains('"' + $name + '"')) {
        throw "GL backend surface gained an unmodelled mutator: $name"
    }
}
# ISAAC_VITA_DISPLAY_CENTER_Y moves only the default framebuffer's viewport
# origin.  That is exact while the guest never names window rows any other
# way (scissor box, blit, pixel copy); glCopyTex* is refused above.  The
# guest does read pixels, so its name must stay visible to this check.
if (-not $surface.Contains('"glReadPixels"')) {
    throw "display-centre surface control disappeared: glReadPixels"
}
foreach ($name in @(
    'glScissor', 'glScissorArrayv', 'glScissorIndexed', 'glScissorIndexedv',
    'glBlitFramebuffer', 'glBlitNamedFramebuffer', 'glCopyPixels')) {
    if ($surface.Contains('"' + $name + '"')) {
        throw "GL backend surface gained a window-row API the display centre does not shift: $name"
    }
}
$outputs = @()
$modes = @(
    'off', 'on', 'typed-no-profile', 'typed', 'typed720',
    'redundancy', 'typed-redundancy', 'typed-bundle', 'coloroffset',
    'canonical', 'fxray', 'texture', 'texture-fxray', 'fbo', 'fbo720',
    'prod-elision', 'prod-elision-drop', 'prod-loc', 'prod-loc-verify',
    'prod-elision-loc-verify', 'prod-fill', 'prod-elision-drop-fill',
    'coalesce', 'coalesce-profile', 'coalesce-canonical',
    'coalesce-wrapper-time', 'coalesce-memo',
    'direct-plain', 'typed-direct', 'coalesce-direct', 'coalesce-direct-loc',
    'coalesce-direct-fill',
    'readback', 'prod-elision-drop-readback',
    'depth-scene', 'depth-carry',
    'prod-elision-drop-depth-scene', 'prod-elision-drop-depth-carry',
    'center-y', 'typed-center-y', 'prod-elision-drop-readback-center-y',
    'prod-elision-drop-depth-carry-center-y')
if ($env:ISAAC_RBO_HOST_MODES) {
    foreach ($requested in $env:ISAAC_RBO_HOST_MODES.Split(',')) {
        if ($requested -notin $modes) { throw "Unknown host mode: $requested" }
    }
}
foreach ($mode in $modes) {
    if ($env:ISAAC_RBO_HOST_MODES -and
        $mode -notin $env:ISAAC_RBO_HOST_MODES.Split(',')) { continue }
    if ($env:ISAAC_GL_RECORD -eq '1' -and $mode -like '*direct*') {
        # Compile-time incompatible (#error/FATAL): DIRECT_STATE bypasses the ring.
        Write-Output "SKIP record mode $mode (SHADER_ATTRIB_DIRECT_STATE excluded)"
        continue
    }
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
    # *center-y: ISAAC_VITA_DISPLAY_CENTER_Y (issue #5, 5.8b), alone, with the
    # typed-state cache and phase profile, and on the recipe-B-shaped
    # production set (elision + depth drop with the readback fix, and with
    # the depth carry; the two log sinks do not combine in one oracle).
    $centerDefine = if ($mode -like '*center-y') {
        '/DISAAC_VITA_DISPLAY_CENTER_Y=1 ' +
        $(if ($mode -eq 'typed-center-y') { '/DISAAC_VITA_GL_TYPED_STATE_CACHE=1 /DISAAC_VITA_PHASE_PROFILE=1 ' } else { '' })
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
    # prod-fill / prod-elision-drop-fill: the GL fill census with its
    # one-frame draw dump (ph120.fa/fp shadows, pass model, kpx arithmetic)
    # alone and on top of the depth-drop elision, whose absorbed clears the
    # census must not count.
    $fillDefine = if (
            $mode -eq 'prod-fill' -or $mode -eq 'prod-elision-drop-fill') {
        '/DISAAC_VITA_GL_FILL_CENSUS=1 ' +
        '/DISAAC_VITA_GL_FILL_CENSUS_DUMP=1 '
    } else {
        ''
    }
    # readback / prod-elision-drop-readback: the offscreen readback sync
    # (GXM drain before a glReadPixels of a colour texture written since
    # the last drain, issue #10) alone and on top of the shipped
    # depth-drop elision.
    $readbackDefine = if (
            $mode -eq 'readback' -or $mode -eq 'prod-elision-drop-readback' -or
            $mode -eq 'prod-elision-drop-readback-center-y') {
        '/DISAAC_VITA_FBO_READBACK_FIX=1 '
    } else {
        ''
    }
    $elisionDefine = if (
            $mode -eq 'prod-elision' -or $mode -eq 'prod-elision-loc-verify') {
        '/DISAAC_VITA_FBO_CLEAR_ELISION=1 '
    } elseif ($mode -eq 'prod-elision-drop' -or
            $mode -eq 'prod-elision-drop-fill' -or
            $mode -eq 'prod-elision-drop-readback' -or
            $mode -eq 'prod-elision-drop-readback-center-y' -or
            $mode -like 'prod-elision-drop-depth-*') {
        # Depth-drop sub-mode: colour clears native, owed depth clear dropped
        # at the owing FBO's colour re-attach (ph120.e a/d counters).
        '/DISAAC_VITA_FBO_CLEAR_ELISION=1 ' +
        '/DISAAC_VITA_FBO_CLEAR_ELISION_DEPTH_DROP=1 '
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
    $coalesceDefine = if ($mode -like 'coalesce*') {
        '/DISAAC_VITA_GL_ATTRIB_ENABLE_COALESCE=1 /DISAAC_VITA_GL_TYPED_STATE_CACHE=1 ' +
        '/DISAAC_VITA_GL_SHIM_FASTDISPATCH=1 /DISAAC_VITA_SHADER_ATTRIB_FASTPATH=1 ' +
        $(if ($mode -ne 'coalesce') { '/DISAAC_VITA_PHASE_PROFILE=1 ' } else { '' }) +
        $(if ($mode -eq 'coalesce-canonical') { '/DISAAC_VITA_CANONICAL_QUAD_ZERO_COPY=1 ' } else { '' }) +
        # coalesce-wrapper-time: ISAAC_VITA_GL_WRAPPER_TIME on top of the GL
        # time profile (gl_bridge.c owns the profile; gl_vita_backend.c runs
        # the ph120.gd draw split whose gl part must equal the gt draw bracket).
        $(if ($mode -eq 'coalesce-wrapper-time') { '/DISAAC_VITA_GL_TIME_PROFILE=1 /DISAAC_VITA_GL_WRAPPER_TIME=1 ' } else { '' }) +
        # coalesce-memo: ISAAC_VITA_SHADER_ATTRIB_LOCATION_MEMO (+VERIFY) on
        # the backend side only - the generation word the replay memo is
        # keyed on and its bump sites (the replay TU itself is proven by
        # recomp/test_vita_shader_attrib_fastpath.py).
        $(if ($mode -eq 'coalesce-memo') { '/DISAAC_VITA_SHADER_ATTRIB_LOCATION_MEMO=1 /DISAAC_VITA_SHADER_ATTRIB_LOCATION_MEMO_VERIFY=1 ' } else { '' })
    } else { '' }
    # *direct*: ISAAC_VITA_SHADER_ATTRIB_DIRECT_STATE, the one-call batch the
    # attrib replays hand their attribute set to.  The oracle drives the same
    # scripted replays through the per-call wrappers and the batch and
    # compares the fake-vitaGL call trace, the typed-state shadow, the
    # coalescer's pending count and every phase counter: without the typed
    # state cache (direct-plain), with it (typed-direct), with the coalescer
    # (coalesce-direct), plus the shim location cache answering the batch's
    # lookups (-loc) and the fill census shadows (-fill).
    $directDefine = if ($mode -like '*direct*') {
        '/DISAAC_VITA_SHADER_ATTRIB_DIRECT_STATE=1 ' +
        '/DISAAC_VITA_GL_SHIM_FASTDISPATCH=1 /DISAAC_VITA_SHADER_ATTRIB_FASTPATH=1 ' +
        $(if ($mode -eq 'typed-direct') { '/DISAAC_VITA_GL_TYPED_STATE_CACHE=1 /DISAAC_VITA_PHASE_PROFILE=1 ' } else { '' }) +
        $(if ($mode -eq 'coalesce-direct-loc') { '/DISAAC_VITA_GL_LOCATION_CACHE=1 ' } else { '' }) +
        $(if ($mode -eq 'coalesce-direct-fill') { '/DISAAC_VITA_GL_FILL_CENSUS=1 ' } else { '' })
    } else { '' }
    # depth-*: issue #15.  The facade models vitaGL's per-scene depth (new
    # scene = background 1.0) and replays Isaac's nested Dizzy/Mirror pass
    # sequences; *-depth-scene is the stock negative control (composites
    # rejected), *-depth-carry adds ISAAC_VITA_FBO_DEPTH_CARRY (they pass).
    $depthDefine = if ($mode -like '*depth-scene' -or $mode -like '*depth-carry' -or
            $mode -eq 'prod-elision-drop-depth-carry-center-y') {
        '/DORACLE_DEPTH_SCENE_MODEL=1 ' +
        $(if ($mode -like '*depth-carry*') { '/DISAAC_VITA_FBO_DEPTH_CARRY=1 ' } else { '' })
    } else { '' }
    # ISAAC_GL_RECORD=1: the same mode and checks with the stage-1 inline
    # recorder installed in front of the shim (ISAAC_VITA_GL_THREAD_RECORD).
    $recordDefine = ''
    $recordSources = ''
    # ISAAC_GL_RECORD_ATTRIB_BATCH=1 (with ISAAC_GL_RECORD=1): RENDER60 S4,
    # ISAAC_VITA_GL_RECORD_ATTRIB_BATCH on every unit of both layers.  Layer 1
    # (EAGER lifecycle) only compiles the batch; the layer-2 scenario's S4
    # section hands each attribute set to the recorder in one call in the
    # record/worker/proof/census variants and issues the per-call sequence in
    # the direct one, so the SHA-equal traces compare batch against per-call.
    $batchDefine = if ($env:ISAAC_GL_RECORD_ATTRIB_BATCH -eq '1') {
        '/DISAAC_VITA_GL_RECORD_ATTRIB_BATCH=1 '
    } else { '' }
    if ($env:ISAAC_GL_RECORD -eq '1') {
        $recordDefine = '/DISAAC_VITA_GL_THREAD_RECORD=1 /DISAAC_GL_RECORD_ORACLE_EAGER=1 ' + $batchDefine
        foreach ($unit in @('gl_record_backend.c', 'gl_record_ring.c', 'gl_record_locations.c')) {
            $recordSources += '"' + (Join-Path $runtime $unit) + '" '
        }
    }
    $command = '"' + $developerShell + '" -arch=x86 -host_arch=x64 >nul && ' +
        'cl /nologo /std:c11 /O2 /W4 /WX /wd4310 ' +
        '/D_CRT_SECURE_NO_WARNINGS /DISAAC_GL_VITA_BACKEND_ORACLE=1 ' +
        '/DISAAC_VITA_IO_PROFILE=1 ' + $displayDefine + $redundancyDefine +
        $typedDefine +
        $coloroffsetDefine + $canonicalDefine + $fxrayDefine +
        $textureDefine + $fboDefine +
        $prodDefine + $elisionDefine + $locationDefine + $verifyDefine + $coalesceDefine +
        $fillDefine + $directDefine + $readbackDefine + $depthDefine + $recordDefine +
        $centerDefine +
        '/I"' + $work + '" ' +
        '/DGUEST_IMAGE_BASE=0x98000000u /I"' + $runtime + '" ' +
        '"' + (Join-Path $runtime 'gl_bridge.c') + '" ' +
        '"' + (Join-Path $runtime 'gl_vita_backend.c') + '" ' +
        $coloroffsetSource + $recordSources +
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

    if ($env:ISAAC_GL_RECORD -eq '1') {
        # Layer 2: the same mode flags, deferred recorder (no EAGER) vs direct,
        # each with and without poisoning client buffers after every call.
        $modeFlags = '/DISAAC_VITA_IO_PROFILE=1 ' + $displayDefine + $redundancyDefine +
            $typedDefine + $coloroffsetDefine + $canonicalDefine + $fxrayDefine +
            $textureDefine + $fboDefine + $prodDefine + $elisionDefine +
            $locationDefine + $verifyDefine + $coalesceDefine + $fillDefine +
            $directDefine + $readbackDefine + $batchDefine + $centerDefine
        $traces = @()
        # worker: stage 2, a real consumer thread with random delays.
        # proof: the worker in ownership-proof mode (waits after every record).
        # census: the worker with ISAAC_VITA_GL_RECORD_OPCODE_CENSUS (same trace;
        # the scenario checks the census against the window and prints coverage).
        foreach ($variant in @('direct', 'record', 'worker', 'proof', 'census')) {
            # ISAAC_GL_RECORD_MUTATION: oracle self-test define; the run MUST fail.
            $variantDefine = if ($variant -ne 'direct') {
                '/DISAAC_VITA_GL_THREAD_RECORD=1 ' +
                $(if ($variant -eq 'worker' -or $variant -eq 'proof' -or $variant -eq 'census') { '/DISAAC_VITA_GL_THREAD_WORKER=1 /DISAAC_GL_RECORD_WORKER_JITTER=1 ' } else { '' }) +
                $(if ($variant -eq 'census') { '/DISAAC_GL_RECORD_OPCODE_CENSUS=1 ' } else { '' }) +
                $(if ($variant -eq 'proof') { '/DISAAC_VITA_GL_THREAD_WORKER_PROOF=1 ' } else { '' }) +
                $(if ($env:ISAAC_GL_RECORD_MUTATION) { '/D' + $env:ISAAC_GL_RECORD_MUTATION + '=1 ' } else { '' })
            } else { '' }
            $variantSources = if ($variant -ne 'direct') { $recordSources } else { '' }
            $scenarioDir = Join-Path $modeWork ('scenario-' + $variant)
            New-Item -ItemType Directory -Path $scenarioDir -Force | Out-Null
            $scenarioExe = Join-Path $scenarioDir 'scenario.exe'
            $scenarioCommand = '"' + $developerShell + '" -arch=x86 -host_arch=x64 >nul && ' +
                'cl /nologo /std:c11 /O2 /W4 /WX /wd4310 ' +
                '/D_CRT_SECURE_NO_WARNINGS /DISAAC_GL_VITA_BACKEND_ORACLE=1 ' +
                '/DISAAC_GL_RECORD_SCENARIO=1 ' + $modeFlags + $variantDefine +
                '/I"' + $work + '" /DGUEST_IMAGE_BASE=0x98000000u /I"' + $runtime + '" ' +
                '"' + (Join-Path $runtime 'gl_bridge.c') + '" ' +
                '"' + (Join-Path $runtime 'gl_vita_backend.c') + '" ' +
                $coloroffsetSource + $variantSources +
                '"' + (Join-Path $runtime 'guest_stack_legacy_oracle_stub.c') + '" ' +
                '"' + (Join-Path $runtime 'gl_vita_backend_oracle.c') + '" ' +
                '"' + (Join-Path $PSScriptRoot 'host_tests\gl_record_scenario.c') + '" ' +
                '/Fe:"' + $scenarioExe + '" /Fo:"' + $scenarioDir + '\\"'
            & cmd.exe /d /s /c $scenarioCommand
            if ($LASTEXITCODE -ne 0) { throw "record scenario $variant-$mode compilation failed" }
            foreach ($poison in @('0', '1')) {
                $trace = Join-Path $scenarioDir ('trace-' + $poison + '.txt')
                $scenarioOut = & $scenarioExe $trace $poison
                $scenarioOut | ForEach-Object { if ($_ -like 'census coverage*' -or $_ -like 'census window*') { "$variant-$mode poison=$poison $_" } else { $_ } }
                if ($LASTEXITCODE -ne 0) { throw "record scenario $variant-$mode poison=$poison failed" }
                $traces += $trace
            }
        }
        $reference = Get-FileHash -Algorithm SHA256 -Path $traces[0]
        foreach ($trace in $traces) {
            if ((Get-FileHash -Algorithm SHA256 -Path $trace).Hash -ne $reference.Hash) {
                throw "record scenario trace differs in $mode : $trace vs $($traces[0])"
            }
        }
        $lines = (Get-Content -LiteralPath $traces[0]).Count
        Write-Output "PASS record scenario $mode : direct/record x poison 0/1 identical ($lines lines, $($reference.Hash.Substring(0, 16)))"
    }
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
