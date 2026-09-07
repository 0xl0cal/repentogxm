#!/usr/bin/env python3
"""Compile and run the aggregate Vita phase-profile oracle on the host."""

from __future__ import annotations

import os
import re
import shutil
import subprocess
import tempfile
from pathlib import Path


def compiler() -> str:
    for name in ("cc", "gcc", "clang"):
        found = shutil.which(name)
        if found:
            return found
    candidates = (
        Path(os.environ.get("ProgramFiles", r"C:\Program Files")) /
        "LLVM" / "bin" / "clang.exe",
        Path(os.environ.get("ProgramFiles", r"C:\Program Files")) /
        "Microsoft Visual Studio" / "2022" / "Community" / "VC" /
        "Tools" / "Llvm" / "x64" / "bin" / "clang.exe",
    )
    for candidate in candidates:
        if candidate.is_file():
            return str(candidate)
    raise RuntimeError("no host C compiler found")


def function_body(source: str, signature: str) -> str:
    start = source.index(signature)
    opening = source.index("{", start)
    return brace_body(source, opening)


def brace_body(source: str, opening: int) -> str:
    depth = 0
    for index in range(opening, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[opening:index + 1]
    raise AssertionError(f"unterminated function: {signature}")


def verify_hot_path_instrumentation(runtime: Path) -> None:
    guest = (runtime / "guest.c").read_text(encoding="utf-8")
    lookup = function_body(guest, "guest_fn guest_lookup(")
    call = function_body(guest, "void guest_call(")
    if lookup.count("GUEST_PHASE_PROFILE_NOTE_LOOKUP();") != 1:
        raise AssertionError("guest_lookup counter is absent or duplicated")
    if lookup.count("++profile_iterations;") != 1 or lookup.count(
            "GUEST_PHASE_PROFILE_ADD_LOOKUP_ITERATIONS(") != 2:
        raise AssertionError("guest_lookup iteration census is incomplete")
    if call.count("GUEST_PHASE_PROFILE_NOTE_CALL();") != 1:
        raise AssertionError("guest_call counter is absent or duplicated")
    # ISAAC_VITA_GUEST_DISPATCH_TABLE: the historical body above is selected
    # as guest_call (option OFF, byte-identical object) or as the out-of-line
    # miss path guest_call_slow (option ON), which pays exactly one
    # dispatch_slow increment in place of the guest_calls one.  The inline
    # probe is a second guest_call definition that pays exactly one
    # dispatch_calls increment, so hits are derived as calls - slow.
    if not re.search(
            r"#if defined\(ISAAC_VITA_GUEST_DISPATCH_TABLE\)\s*"
            r"static GUEST_DISPATCH_NOINLINE void guest_call_slow\("
            r"CPU \*__restrict c,\s*uint32_t addr\)\s*#else\s*"
            r"void guest_call\(CPU \*__restrict c, uint32_t addr\)\s*"
            r"#endif\s*\{", guest):
        raise AssertionError(
            "the complete classification is not the option-OFF guest_call")
    if call.count("GUEST_PHASE_PROFILE_NOTE_DISPATCH_SLOW();") != 1:
        raise AssertionError(
            "guest_call_slow counter is absent or duplicated")
    fast = function_body(
        guest[guest.index("void guest_call(") + 1:], "void guest_call(")
    if fast.count("GUEST_PHASE_PROFILE_NOTE_DISPATCH_CALL();") != 1:
        raise AssertionError(
            "guest_call dispatch-table counter is absent or duplicated")
    if fast.count("guest_dispatch_probe(addr)") != 1 or \
            fast.count("guest_call_slow(c, addr)") != 1:
        raise AssertionError("guest_call fast path shape changed")
    if "GUEST_PHASE_PROFILE_NOTE_DISPATCH_SLOW();" in fast or \
            "GUEST_PHASE_PROFILE_NOTE_CALL();" in fast or \
            "GUEST_PHASE_PROFILE_NOTE_DISPATCH_CALL();" in call or \
            "guest_dispatch_probe(" in call:
        raise AssertionError("dispatch counters crossed paths")
    if "sceKernelGetProcessTimeWide" in lookup or \
            "sceKernelGetProcessTimeWide" in call or \
            "sceKernelGetProcessTimeWide" in fast:
        raise AssertionError("dispatch census acquired a per-call clock")

    gl_source = (runtime / "gl_vita_backend.c").read_text(encoding="utf-8")
    wrappers = {
        "vita_glDrawElements(": "draw_elements",
        "vita_glClear(": "clear",
        "vita_glBindTexture(": "bind_texture",
        "vita_glUseProgram(": "use_program",
        "vita_glTexImage2D(": "tex_image",
        "vita_glTexSubImage2D(": "tex_sub_image",
        "vita_glVertexAttribPointer(": "attrib_pointer",
        "vita_glBindFramebuffer(": "bind_framebuffer",
    }
    uniform_wrappers = (
        "vita_glUniform1fv(", "vita_glUniform1i(",
        "vita_glUniform1iv(", "vita_glUniform2fv(",
        "vita_glUniform2iv(", "vita_glUniform3fv(",
        "vita_glUniform3iv(", "vita_glUniform4fv(",
        "vita_glUniform4iv(", "vita_glUniformMatrix2fv(",
        "vita_glUniformMatrix3fv(", "vita_glUniformMatrix4fv(",
    )
    for signature, field in wrappers.items():
        body = function_body(gl_source, f"static void {signature}")
        marker = f"GL_VITA_PHASE_COUNT({field});"
        if body.count(marker) != 1:
            raise AssertionError(
                f"{signature[:-1]} counter is absent or duplicated"
            )
        if "sceKernelGetProcessTimeWide" in body:
            if (signature != "vita_glTexImage2D(" or
                    body.count("sceKernelGetProcessTimeWide") != 2 or
                    body.count("ISAAC_VITA_TEXTURE_CHURN_PROFILE") != 2):
                raise AssertionError(
                    f"{signature[:-1]} acquired an unscoped per-call clock"
                )
        if "sceClibPrintf" in body or "printf(" in body:
            raise AssertionError(f"{signature[:-1]} acquired a per-call log")
    # The fill census helpers the wrappers call (ISAAC_VITA_GL_FILL_CENSUS):
    # projection arithmetic and pass bookkeeping only; the dump lines leave
    # through take-window (the reporter), never from a per-draw/clear path.
    for signature in (
        "static void gl_vita_fill_census_draw(",
        "static void gl_vita_fill_census_clear(",
        "static void gl_vita_fill_pass_touch(",
        "static void gl_vita_fbo_owed_materialize(",
    ):
        body = function_body(gl_source, signature)
        for needle in ("sceClibPrintf", "printf(", "isaac_vita_log(",
                       "sceKernelGetProcessTime", "glGet", "glFinish"):
            if needle in body:
                raise AssertionError(
                    f"{signature[:-1]} acquired {needle} on the draw/clear path"
                )
    gen_texture = function_body(gl_source, "static void vita_glGenTextures(")
    if (gen_texture.count("sceKernelGetProcessTimeWide") != 2 or
            gen_texture.count("ISAAC_VITA_TEXTURE_CHURN_PROFILE") != 2):
        raise AssertionError("glGenTextures timer escaped its diagnostic guard")
    if "sceClibPrintf" in gen_texture or "printf(" in gen_texture:
        raise AssertionError("glGenTextures acquired a per-call log")
    delete_texture = function_body(
        gl_source, "static void vita_glDeleteTextures("
    )
    if (delete_texture.count("sceKernelGetProcessTimeWide") != 4 or
            delete_texture.count("ISAAC_VITA_TEXTURE_CHURN_PROFILE") != 3):
        raise AssertionError(
            "glDeleteTextures split timer escaped its diagnostic guards"
        )
    if "sceClibPrintf" in delete_texture or "printf(" in delete_texture:
        raise AssertionError("glDeleteTextures acquired a per-call log")
    for signature in uniform_wrappers:
        body = function_body(gl_source, f"static void {signature}")
        if body.count("GL_VITA_PHASE_COUNT(uniform);") != 1:
            raise AssertionError(
                f"{signature[:-1]} uniform counter is absent or duplicated"
            )
    for signature in (
            "vita_glEnableVertexAttribArray(",
            "vita_glDisableVertexAttribArray("):
        body = function_body(gl_source, f"static void {signature}")
        if body.count("GL_VITA_PHASE_COUNT(attrib_toggle);") != 1:
            raise AssertionError(
                f"{signature[:-1]} attrib-toggle counter is absent or duplicated"
            )
    for signature in (
            "vita_glBlendFuncSeparate(", "vita_glCullFace(",
            "vita_glDepthFunc(", "vita_glViewport("):
        body = function_body(gl_source, f"static void {signature}")
        if body.count("GL_VITA_PHASE_COUNT(state);") != 1:
            raise AssertionError(
                f"{signature[:-1]} state counter is absent or duplicated"
            )
    # GL time profile: the only clock the wrappers may reach is the pair of
    # macros defined under ISAAC_VITA_GL_TIME_PROFILE and not SDK_SPARSE.
    # Both OFF and sparse expand to no clocks; FULL retains balanced brackets.
    timer_block = re.search(
        r"#if defined\(ISAAC_VITA_GL_TIME_PROFILE\)\n"
        r"(?:(?!#endif).)*?# if !defined\(ISAAC_VITA_GL_TIME_SDK_SPARSE\)"
        r" \|\| !ISAAC_VITA_GL_TIME_SDK_SPARSE\n"
        r"static inline uint64_t gl_vita_time_now\(void\)"
        r"(?:(?!#endif).)*?# define GL_VITA_TIME_BEGIN\(\)"
        r"(?:(?!#endif).)*?# define GL_VITA_TIME_END\(bucket\)"
        r"(?:(?!#endif).)*?# else\n"
        r"/\* Sparse mode observes native SDK sites, not these high-frequency wrappers\. \*/\n"
        r"# define GL_VITA_TIME_BEGIN\(\) \(\(void\)0\)\n"
        r"# define GL_VITA_TIME_END\(bucket\) \(\(void\)0\)\n# endif\n#else\n"
        r"# define GL_VITA_TIME_BEGIN\(\) \(\(void\)0\)\n"
        r"# define GL_VITA_TIME_END\(bucket\) \(\(void\)0\)\n#endif",
        gl_source, re.DOTALL)
    if timer_block is None:
        raise AssertionError("GL time-profile macros escaped their option guard")
    if gl_source.count("gl_vita_time_now(void)") != 1:
        raise AssertionError("GL time-profile clock helper is duplicated")
    for signature in list(wrappers) + list(uniform_wrappers) + [
            "vita_glEnableVertexAttribArray(",
            "vita_glDisableVertexAttribArray(", "vita_glBlendFuncSeparate(",
            "vita_glCullFace(", "vita_glDepthFunc(", "vita_glViewport(",
            "vita_glActiveTexture(", "vita_glEnable(",
            "vita_glTexParameteri(", "vita_glClearColor(",
            "vita_glClearDepth("]:
        body = function_body(gl_source, f"static void {signature}")
        begins = body.count("GL_VITA_TIME_BEGIN();")
        ends = body.count("GL_VITA_TIME_END(")
        if begins == 0 or begins != ends:
            raise AssertionError(
                f"{signature[:-1]} GL time bracket is missing or unbalanced "
                f"({begins} begin / {ends} end)"
            )

    cmake = (runtime.parent / "vita" / "CMakeLists.txt").read_text(
        encoding="utf-8"
    )
    profile_sources = re.compile(
        r'if\(ISAAC_VITA_PHASE_PROFILE\).*?set_property\(SOURCE\s+'
        r'"\$\{ISAAC_RUNTIME\}/guest\.c"\s+'
        r'"\$\{ISAAC_RUNTIME\}/gl_vita_backend\.c"',
        re.DOTALL,
    )
    if profile_sources.search(cmake) is None:
        raise AssertionError("profile macros are not source-scoped to guest/GL")
    if re.search(
            r'option\(ISAAC_VITA_GL_TIME_SDK_SPARSE\s+"[^"]+"\s+OFF\)',
            cmake) is None:
        raise AssertionError("SDK sparse GL timer is not default OFF")
    sparse_sources = re.compile(
        r'if\(ISAAC_VITA_GL_TIME_SDK_SPARSE\)\s+set_property\(SOURCE\s+'
        r'"\$\{ISAAC_RUNTIME\}/gl_vita_backend\.c"\s+'
        r'"\$\{ISAAC_RUNTIME\}/kage_vita_backend\.c"\s+'
        r'"\$\{ISAAC_RUNTIME\}/kage_vita_phase_profile\.c"\s+'
        r'APPEND PROPERTY COMPILE_DEFINITIONS ISAAC_VITA_GL_TIME_SDK_SPARSE=1\)',
    )
    if sparse_sources.search(cmake) is None or cmake.count(
            "COMPILE_DEFINITIONS ISAAC_VITA_GL_TIME_SDK_SPARSE=1") != 1:
        raise AssertionError("SDK sparse macro escaped its three native owners")
    # scope=attrib replay timer: nested in the SDK_SPARSE block, gated on the
    # attrib fast path, owned by the replay TU and the phase profiler only.
    sparse_attrib_sources = re.compile(
        r'if\(ISAAC_VITA_GL_TIME_SDK_SPARSE\)\s+set_property\(SOURCE\s+'
        r'"\$\{ISAAC_RUNTIME\}/gl_vita_backend\.c"\s+'
        r'"\$\{ISAAC_RUNTIME\}/kage_vita_backend\.c"\s+'
        r'"\$\{ISAAC_RUNTIME\}/kage_vita_phase_profile\.c"\s+'
        r'APPEND PROPERTY COMPILE_DEFINITIONS ISAAC_VITA_GL_TIME_SDK_SPARSE=1\)\s+'
        r'message\(STATUS "[^"]+"\)\s+'
        r'if\(ISAAC_VITA_SHADER_ATTRIB_FASTPATH\)\s+(?:#[^\n]*\n\s*)*'
        r'set_property\(SOURCE\s+'
        r'"\$\{ISAAC_RUNTIME\}/host_vita_shader_attrib_fastpath\.c"\s+'
        r'"\$\{ISAAC_RUNTIME\}/kage_vita_phase_profile\.c"\s+'
        r'APPEND PROPERTY COMPILE_DEFINITIONS ISAAC_VITA_GL_TIME_SDK_SPARSE_ATTRIB=1\)',
    )
    if sparse_attrib_sources.search(cmake) is None or cmake.count(
            "ISAAC_VITA_GL_TIME_SDK_SPARSE_ATTRIB=1") != 1:
        raise AssertionError("SDK sparse attrib macro escaped its two owners")
    if re.search(
            r'option\(ISAAC_VITA_GL_TYPED_STATE_CACHE\s+'
            r'"[^"]+"\s+OFF\)', cmake) is None:
        raise AssertionError("typed GL state cache is not default OFF")
    typed_sources = re.compile(
        r'if\(ISAAC_VITA_GL_TYPED_STATE_CACHE\).*?'
        r'set_property\(SOURCE\s+'
        r'"\$\{ISAAC_RUNTIME\}/gl_vita_backend\.c"\s+'
        r'"\$\{ISAAC_RUNTIME\}/kage_vita_phase_profile\.c".*?'
        r'ISAAC_VITA_GL_TYPED_STATE_CACHE=1',
        re.DOTALL,
    )
    if typed_sources.search(cmake) is None:
        raise AssertionError("typed GL state cache escaped its two source owners")
    if re.search(
            r'option\(ISAAC_VITA_TEXTURE_CHURN_PROFILE\s+'
            r'"[^"]+"\s+OFF\)', cmake) is None:
        raise AssertionError("texture-churn profile is not default OFF")
    texture_sources = re.compile(
        r'if\(ISAAC_VITA_TEXTURE_CHURN_PROFILE\).*?'
        r'set_property\(SOURCE\s+'
        r'"\$\{ISAAC_RUNTIME\}/gl_vita_backend\.c"\s+'
        r'"\$\{ISAAC_RUNTIME\}/kage_vita_phase_profile\.c".*?'
        r'ISAAC_VITA_TEXTURE_CHURN_PROFILE=1',
        re.DOTALL,
    )
    if texture_sources.search(cmake) is None:
        raise AssertionError(
            "texture-churn macro escaped its exact two native owners"
        )
    # GL fill census: both knobs default OFF, gated on ISAAC_VITA_PHASE_PROFILE
    # (and the dump on the census), and the macro reaches exactly the typed
    # GL edge, the present owner and the reporter.
    for knob in ("ISAAC_VITA_GL_FILL_CENSUS", "ISAAC_VITA_GL_FILL_CENSUS_DUMP"):
        if re.search(
                r'option\(' + knob + r'\s+"[^"]+"\s+OFF\)', cmake) is None:
            raise AssertionError(f"{knob} is not default OFF")
    if re.search(
            r'if\(ISAAC_VITA_GL_FILL_CENSUS AND NOT ISAAC_VITA_PHASE_PROFILE\)\s*'
            r'message\(FATAL_ERROR', cmake) is None:
        raise AssertionError("fill census is not gated on the phase profiler")
    if re.search(
            r'if\(ISAAC_VITA_GL_FILL_CENSUS_DUMP AND NOT '
            r'ISAAC_VITA_GL_FILL_CENSUS\)\s*message\(FATAL_ERROR',
            cmake) is None:
        raise AssertionError("fill census dump is not gated on the census")
    if re.search(
            r'if\(ISAAC_VITA_GL_FILL_CENSUS AND '
            r'ISAAC_VITA_DISPLAY_RASTER_SCALED\)\s*message\(FATAL_ERROR',
            cmake) is None:
        raise AssertionError(
            "fill census does not fail closed on a scaled display raster"
        )
    if ('# if defined(ISAAC_VITA_DISPLAY_RASTER_720)\n'
            '#  error "ISAAC_VITA_GL_FILL_CENSUS measures the native 960x544 '
            'display') not in gl_source:
        raise AssertionError(
            "gl_vita_backend.c fill census lacks the raster fail-closed error"
        )
    fill_sources = re.compile(
        r'if\(ISAAC_VITA_GL_FILL_CENSUS\).*?'
        r'set_property\(SOURCE\s+'
        r'"\$\{ISAAC_RUNTIME\}/gl_vita_backend\.c"\s+'
        r'"\$\{ISAAC_RUNTIME\}/kage_vita_backend\.c"\s+'
        r'"\$\{ISAAC_RUNTIME\}/kage_vita_phase_profile\.c".*?'
        r'ISAAC_VITA_GL_FILL_CENSUS=1',
        re.DOTALL,
    )
    if fill_sources.search(cmake) is None:
        raise AssertionError(
            "fill census macro escaped its exact three native owners"
        )
    # GL wrapper time (ISAAC_VITA_GL_WRAPPER_TIME): default OFF, gated on the
    # phase profiler, the GL time profile (its bodies are the subtrahend),
    # the fast dispatch (entry-indexed buckets) and against SDK_SPARSE; the
    # macro reaches exactly the bridge, the typed GL edge (ph120.gd draw
    # split), the present owner, the reporter and (conditionally) the attrib
    # replay TU; the bridge and the replay TU also receive the GL time
    # profile define their shared bucket type needs.  Two clock reads per
    # wrapper when ON and none in the adapter run when OFF.
    if re.search(
            r'option\(ISAAC_VITA_GL_WRAPPER_TIME\s+"[^"]+"\s+OFF\)',
            cmake) is None:
        raise AssertionError("ISAAC_VITA_GL_WRAPPER_TIME is not default OFF")
    for requirement in ("ISAAC_VITA_PHASE_PROFILE", "ISAAC_VITA_GL_TIME_PROFILE",
                        "ISAAC_VITA_GL_SHIM_FASTDISPATCH"):
        if re.search(
                r'if\(ISAAC_VITA_GL_WRAPPER_TIME AND NOT ' + requirement +
                r'\)\s*message\(FATAL_ERROR', cmake) is None:
            raise AssertionError(
                f"GL wrapper time is not gated on {requirement}")
    if re.search(
            r'if\(ISAAC_VITA_GL_WRAPPER_TIME AND ISAAC_VITA_GL_TIME_SDK_SPARSE\)'
            r'\s*message\(FATAL_ERROR', cmake) is None:
        raise AssertionError("GL wrapper time does not reject SDK_SPARSE")
    wrapper_sources = re.compile(
        r'if\(ISAAC_VITA_GL_WRAPPER_TIME\).*?'
        r'set_property\(SOURCE\s+'
        r'"\$\{ISAAC_RUNTIME\}/gl_bridge\.c"\s+'
        r'APPEND PROPERTY COMPILE_DEFINITIONS\s+'
        r'ISAAC_VITA_GL_TIME_PROFILE=1\)\s*'
        r'set_property\(SOURCE\s+'
        r'"\$\{ISAAC_RUNTIME\}/gl_bridge\.c"\s+'
        r'"\$\{ISAAC_RUNTIME\}/gl_vita_backend\.c"\s+'
        r'"\$\{ISAAC_RUNTIME\}/kage_vita_backend\.c"\s+'
        r'"\$\{ISAAC_RUNTIME\}/kage_vita_phase_profile\.c"\s+'
        r'APPEND PROPERTY COMPILE_DEFINITIONS\s+'
        r'ISAAC_VITA_GL_WRAPPER_TIME=1\)\s*'
        r'if\(ISAAC_VITA_SHADER_ATTRIB_FASTPATH\)\s*set_property\(SOURCE\s+'
        r'"\$\{ISAAC_RUNTIME\}/host_vita_shader_attrib_fastpath\.c"\s+'
        r'APPEND PROPERTY COMPILE_DEFINITIONS\s+'
        r'ISAAC_VITA_GL_TIME_PROFILE=1\s+'
        r'ISAAC_VITA_GL_WRAPPER_TIME=1\)',
        re.DOTALL,
    )
    if cmake.count("ISAAC_VITA_GL_WRAPPER_TIME=1") != 2:
        raise AssertionError("GL wrapper time macro has more than its two owner lists")
    if wrapper_sources.search(cmake) is None:
        raise AssertionError(
            "GL wrapper time macro escaped its exact native owners"
        )
    bridge = (runtime / "gl_bridge.c").read_text(encoding="utf-8")
    run_owned = function_body(
        bridge, "static inline int guest_gl_run_owned(")
    if run_owned.count("guest_gl_wrapper_time_now()") != 2 or \
            run_owned.count("#if defined(ISAAC_VITA_GL_WRAPPER_TIME)") != 1 or \
            run_owned.count("entry->adapter(c);") != 2:
        raise AssertionError(
            "guest_gl_run_owned wrapper-time bracket is not one entry read "
            "plus one exit read under the define with the bare adapter run "
            "otherwise"
        )
    # ph120.gd draw split: the wrapper body carries one ENTER (dispatch from
    # the bridge entry read), three fresh-read MARKs (sync, classify,
    # census), and per gt bracket one AT on the BEGIN local (fbo) and one
    # BODY_DONE on the END read (gl + tail publish), so the split adds four
    # reads per draw and shares the rest with gt; every macro expands to
    # nothing without the define, and the END-reuse variant of
    # GL_VITA_TIME_END exists only under it.
    draw_body = function_body(gl_source, "static void vita_glDrawElements(")
    if (draw_body.count("GL_VITA_DRAW_SPLIT_ENTER();") != 1 or
            draw_body.count("GL_VITA_DRAW_SPLIT_MARK(") != 3 or
            draw_body.count("GL_VITA_DRAW_SPLIT_MARK(SYNC);") != 1 or
            draw_body.count("GL_VITA_DRAW_SPLIT_MARK(CLASSIFY);") != 1 or
            draw_body.count("GL_VITA_DRAW_SPLIT_MARK(CENSUS);") != 1 or
            draw_body.count("GL_VITA_DRAW_SPLIT_AT(FBO, gl_vita_time_started_at);")
            != draw_body.count("GL_VITA_TIME_BEGIN();") or
            draw_body.count("GL_VITA_DRAW_SPLIT_BODY_DONE();")
            != draw_body.count("GL_VITA_TIME_END(") or
            draw_body.count("GL_VITA_DRAW_SPLIT_CANONICAL();") != 1 or
            "gl_vita_time_now(" in draw_body):
        raise AssertionError(
            "vita_glDrawElements draw split is not ENTER + 3 MARK + one "
            "AT(FBO)/BODY_DONE per gt bracket + one CANONICAL"
        )
    for macro in ("GL_VITA_DRAW_SPLIT_ENTER()", "GL_VITA_DRAW_SPLIT_AT(part, at)",
                  "GL_VITA_DRAW_SPLIT_MARK(part)",
                  "GL_VITA_DRAW_SPLIT_BODY_DONE()",
                  "GL_VITA_DRAW_SPLIT_CANONICAL()"):
        if gl_source.count(f"# define {macro} ((void)0)") != 1:
            raise AssertionError(f"{macro} has no empty expansion without the knob")
    if gl_source.count("#  if defined(ISAAC_VITA_GL_WRAPPER_TIME)") != 1 or \
            gl_source.count("gl_vita_time_started_at = gl_vita_time_ended_at;") != 1:
        raise AssertionError(
            "GL_VITA_TIME_END end-read reuse is not scoped to the wrapper-time knob"
        )
    for signature in ("vita_glDrawElements(", "vita_glUniformMatrix4fv("):
        body = function_body(gl_source, f"static void {signature}")
        if "sceKernelGetProcessTimeWide" in body or "g_isaac_vita_gl_wrapper_entry_at" in body:
            raise AssertionError(f"{signature[:-1]} reads the clock outside the macros")


def verify_sdk_sparse_explicit_zero(runtime: Path, cc: str) -> None:
    """An explicit zero must preserve absent-option FULL/OFF behavior."""
    vita = runtime.parent / "vita"
    modes = (
        ("off", ()),
        ("off-zero", ("-DISAAC_VITA_GL_TIME_SDK_SPARSE=0",)),
        ("full", ("-DISAAC_VITA_GL_TIME_PROFILE=1",)),
        ("full-zero", (
            "-DISAAC_VITA_GL_TIME_PROFILE=1",
            "-DISAAC_VITA_GL_TIME_SDK_SPARSE=0",
        )),
    )
    with tempfile.TemporaryDirectory(prefix="isaac-phase-sparse-zero-") as temporary:
        root = Path(temporary)
        outputs, objects = {}, {}
        for name, definitions in modes:
            executable = root / (name + (".exe" if os.name == "nt" else ""))
            command = [
                cc, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                "-DISAAC_KAGE_VITA_PHASE_PROFILE_ORACLE=1",
                "-DISAAC_VITA_PHASE_PROFILE=1",
                "-DISAAC_VITA_PHASE_PROFILE_BUILD_ID=\"phase:oracle\"",
                *definitions, f"-I{runtime}", f"-I{vita}",
                str(runtime / "kage_vita_phase_profile.c"),
                str(runtime / "kage_vita_phase_profile_oracle.c"),
                "-o", str(executable),
            ]
            subprocess.run(command, check=True)
            completed = subprocess.run(
                [str(executable)], check=True, text=True, capture_output=True)
            outputs[name] = (completed.stdout, completed.stderr)
            expected_records = 5 if name.startswith("full") else 3
            if f"PASS (records={expected_records};" not in completed.stdout or \
                    "sdk32=" in completed.stdout:
                raise AssertionError(f"{name} selected wrong profile: {completed.stdout!r}")
            gl_object = root / (name + ".o")
            command = [
                cc, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                "-Wno-int-to-pointer-cast", "-Wno-pointer-to-int-cast",
                "-DISAAC_GL_VITA_BACKEND_ORACLE=1",
                "-DISAAC_VITA_PHASE_PROFILE=1", *definitions,
                f"-I{runtime}", "-c", str(runtime / "gl_vita_backend.c"),
                "-o", str(gl_object),
            ]
            if "clang" in Path(cc).name.lower():
                command.insert(6, "-Wno-deprecated-non-prototype")
            subprocess.run(command, check=True)
            objects[name] = bytearray(gl_object.read_bytes())
            if os.name == "nt":
                # Same narrow normalization as test_vita_png_texel_init.py:
                # COFF TimeDateStamp differs between sequential compilations.
                # No section, relocation, code or symbol bytes are excluded.
                objects[name][4:8] = bytes(4)
        for name in ("off", "full"):
            if outputs[name] != outputs[name + "-zero"]:
                raise AssertionError(f"SDK_SPARSE=0 changed the {name} phase output")
            if objects[name] != objects[name + "-zero"]:
                raise AssertionError(f"SDK_SPARSE=0 changed the {name} GL object")
    stamp_note = " (COFF timestamp excluded)" if os.name == "nt" else ""
    print("SDK sparse explicit-zero: PASS FULL/OFF phase output and GL object identity" +
          stamp_note)


def main() -> int:
    vita = Path(__file__).resolve().parent
    runtime = vita.parent / "runtime"
    verify_hot_path_instrumentation(runtime)
    cc = compiler()
    verify_sdk_sparse_explicit_zero(runtime, cc)
    with tempfile.TemporaryDirectory(prefix="isaac-phase-profile-") as value:
        def compile_and_run(
            suffix: str, definitions: tuple[str, ...]
        ) -> subprocess.CompletedProcess:
            executable = Path(value) / (
                f"phase-profile-{suffix}.exe" if os.name == "nt" else
                f"phase-profile-{suffix}"
            )
            command = [
                cc, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                "-DISAAC_KAGE_VITA_PHASE_PROFILE_ORACLE=1",
                "-DISAAC_VITA_PHASE_PROFILE=1",
                "-DISAAC_VITA_PHASE_PROFILE_BUILD_ID=\"phase:oracle\"",
                f"-I{runtime}", f"-I{vita}",
                str(runtime / "kage_vita_phase_profile.c"),
                str(runtime / "kage_vita_phase_profile_oracle.c"),
                "-o", str(executable),
            ]
            for definition in reversed(definitions):
                command.insert(7, definition)
            if "-DISAAC_VITA_FULLSPEED_SCHEDULER=1" in definitions:
                command.insert(
                    -2, str(runtime / "kage_vita_fullspeed_scheduler.c")
                )
            subprocess.run(command, check=True)
            completed = subprocess.run(
                [str(executable)], check=False, text=True,
                capture_output=True,
            )
            if completed.returncode != 0:
                raise AssertionError(
                    f"phase profile {suffix} oracle failed: "
                    f"stdout={completed.stdout!r} stderr={completed.stderr!r}"
                )
            return completed

        completed_off = compile_and_run("off", ())
        completed_halo_zero = compile_and_run("halo-zero", (
            "-DISAAC_VITA_LASER_HALO_PROFILE=0",))
        completed_halo = compile_and_run("halo-counter-only", (
            "-DISAAC_VITA_LASER_HALO_PROFILE=1",))
        if completed_halo_zero.stdout != completed_off.stdout:
            raise AssertionError("explicit-zero halo observer changed the baseline")
        if "PASS (records=4;" not in completed_halo.stdout:
            raise AssertionError(completed_halo.stdout)
        print("[halo-counter-only] " + completed_halo.stdout.strip())
        # ISAAC_VITA_PHASE_PROFILE_OTHER: oth=p50/p95/max between lim and all;
        # worst case 433 -> 470 bytes (<= the 512-byte allowance ph120.s uses).
        completed_other = compile_and_run(
            "other", ("-DISAAC_VITA_PHASE_PROFILE_OTHER=1",)
        )
        completed_other_scheduler = compile_and_run(
            "other-scheduler", (
                "-DISAAC_VITA_PHASE_PROFILE_OTHER=1",
                "-DISAAC_VITA_FULLSPEED_SCHEDULER=1",
            )
        )
        completed_on = compile_and_run(
            "on", ("-DISAAC_VITA_GUEST_LOOKUP_CACHE=1",)
        )
        completed_dispatch = compile_and_run(
            "dispatch", (
                "-DISAAC_VITA_GUEST_LOOKUP_CACHE=1",
                "-DISAAC_VITA_GUEST_DISPATCH_TABLE=1",
            )
        )
        completed_import_kinds = compile_and_run(
            "import-kinds", ("-DISAAC_VITA_PROFILE_IMPORT_KINDS=1",)
        )
        completed_dispatch_import_kinds = compile_and_run(
            "dispatch-import-kinds", (
                "-DISAAC_VITA_GUEST_LOOKUP_CACHE=1",
                "-DISAAC_VITA_GUEST_DISPATCH_TABLE=1",
                "-DISAAC_VITA_PROFILE_IMPORT_KINDS=1",
            )
        )
        # wf/opt-gltok: typed-GL tokens as dispatch-table keys.  ph120.d
        # becomes d(c,s,sf,g) (worst case 145 B); ph120.c, ph120.i and the
        # ph120.ik identity keep their values with the GL hits in dyn.
        completed_dispatch_gltok = compile_and_run(
            "dispatch-gltok", (
                "-DISAAC_VITA_GUEST_LOOKUP_CACHE=1",
                "-DISAAC_VITA_GUEST_DISPATCH_TABLE=1",
                "-DISAAC_VITA_GL_SHIM_TABLE_TOKENS=1",
            )
        )
        completed_dispatch_gltok_import_kinds = compile_and_run(
            "dispatch-gltok-import-kinds", (
                "-DISAAC_VITA_GUEST_LOOKUP_CACHE=1",
                "-DISAAC_VITA_GUEST_DISPATCH_TABLE=1",
                "-DISAAC_VITA_GL_SHIM_TABLE_TOKENS=1",
                "-DISAAC_VITA_PROFILE_IMPORT_KINDS=1",
            )
        )
        completed_pill = compile_and_run(
            "pill", ("-DISAAC_VITA_PILL_BLOOM_BYPASS=1",)
        )
        completed_bloom_half = compile_and_run(
            "bloom-half",
            ("-DISAAC_VITA_TRANSIENT_BLOOM_HALF_RES=1",),
        )
        completed_poop_fx = compile_and_run(
            "poop-fx", ("-DISAAC_VITA_POOP_FX_PROFILE=1",)
        )
        completed_poop_fx_cap = compile_and_run(
            "poop-fx-cap",
            (
                "-DISAAC_VITA_POOP_FX_PROFILE=1",
                "-DISAAC_VITA_POOP_FX_SINGLE_CLOUD=1",
            ),
        )
        completed_laser = compile_and_run(
            "laser", ("-DISAAC_VITA_LASER_PROFILE=1",)
        )
        completed_laser_gpu = compile_and_run(
            "laser-gpu",
            (
                "-DISAAC_VITA_LASER_PROFILE=1",
                "-DISAAC_VITA_VITAGL_GPU_DRAW_OPTIMIZATIONS=1",
            ),
        )
        completed_typed = compile_and_run(
            "typed", ("-DISAAC_VITA_GL_TYPED_STATE_CACHE=1",)
        )
        completed_attrib_coalesce = compile_and_run(
            "attrib-coalesce", (
                "-DISAAC_VITA_GL_TYPED_STATE_CACHE=1",
                "-DISAAC_VITA_GL_ATTRIB_ENABLE_COALESCE=1",
            )
        )
        completed_texture = compile_and_run(
            "texture", ("-DISAAC_VITA_TEXTURE_CHURN_PROFILE=1",)
        )
        completed_gl_time = compile_and_run(
            "gl-time",
            (
                "-DISAAC_VITA_GL_TIME_PROFILE=1",
                "-DISAAC_VITA_GUEST_LOOKUP_CACHE=1",
                "-DISAAC_VITA_TEXTURE_CHURN_PROFILE=1",
            ),
        )
        # ISAAC_VITA_GL_WRAPPER_TIME: ph120.gw, gd, gu between gt and gx
        # (records 5 -> 8 on the bare GL-time leg); the oracle pins both
        # windows' exact text, the first-loop-head discard and the
        # take-and-zero, and the all-ones lengths 379/370/232 (< 384) appear
        # as "; gw=379/gd=370/gu=232".
        completed_gl_wrapper_time = compile_and_run(
            "gl-wrapper-time",
            (
                "-DISAAC_VITA_GL_TIME_PROFILE=1",
                "-DISAAC_VITA_GL_WRAPPER_TIME=1",
            ),
        )
        sdk_sparse_definitions = (
            "-DISAAC_VITA_GL_TIME_PROFILE=1",
            "-DISAAC_VITA_GL_TIME_SDK_SPARSE=1",
            "-DISAAC_VITA_CANONICAL_QUAD_ZERO_COPY=1",
        )
        completed_sdk_sparse = compile_and_run("sdk-sparse", sdk_sparse_definitions)
        # ISAAC_VITA_GL_TIME_SDK_SPARSE_ATTRIB (CMake: SDK_SPARSE and the
        # attrib fast path both ON) adds the scope=attrib row after
        # scope=queue: records 8 -> 9, exact window texts and the 273-byte
        # all-ones length are pinned in the oracle.
        completed_sdk_sparse_attrib = compile_and_run(
            "sdk-sparse-attrib", sdk_sparse_definitions + (
                "-DISAAC_VITA_GL_TIME_SDK_SPARSE_ATTRIB=1",))
        completed_sdk_sparse_valid_region = {
            value: compile_and_run(
                f"sdk-sparse-valid-region-{value}", sdk_sparse_definitions + (
                    f"-DISAAC_VITA_STOCK_FBO_VALID_REGION={value}",
                ),
            )
            for value in ("1", "2")
        }
        # ISAAC_VITA_STOCK_FBO_VALID_REGION (1 = observe, 2 = on) appends
        # vr(s,p,u,a,e,v,c) to ph120.gx (worst case 253 -> 348 B) and adds
        # the ph120.vz size census right after it (326 B): records 8 -> 9.
        # The profiler only takes the counters, so both values share the
        # oracle's pinned lines; the #error leg (valid region without the GL
        # time profile) is a compile-time rejection, not a runtime mode.
        completed_gl_time_valid_region = {
            value: compile_and_run(
                f"gl-time-valid-region-{value}",
                (
                    "-DISAAC_VITA_GL_TIME_PROFILE=1",
                    "-DISAAC_VITA_GUEST_LOOKUP_CACHE=1",
                    "-DISAAC_VITA_TEXTURE_CHURN_PROFILE=1",
                    f"-DISAAC_VITA_STOCK_FBO_VALID_REGION={value}",
                ),
            )
            for value in ("1", "2")
        }
        # ISAAC_VITA_FBO_CLEAR_ELISION adds ph120.e after the vitaGL records;
        # the depth-drop sub-mode appends a,d to its clear() field (the
        # counters themselves are pinned by gl_vita_backend_oracle.c).
        completed_gl_time_elision = compile_and_run(
            "gl-time-elision",
            (
                "-DISAAC_VITA_GL_TIME_PROFILE=1",
                "-DISAAC_VITA_FBO_CLEAR_ELISION=1",
            ),
        )
        completed_gl_time_elision_drop = compile_and_run(
            "gl-time-elision-drop",
            (
                "-DISAAC_VITA_GL_TIME_PROFILE=1",
                "-DISAAC_VITA_FBO_CLEAR_ELISION=1",
                "-DISAAC_VITA_FBO_CLEAR_ELISION_DEPTH_DROP=1",
            ),
        )
        # ISAAC_VITA_GL_FILL_CENSUS adds ph120.fa/ph120.fp after ph120.f (or
        # in its place without the vitaGL draw policy); the oracle pins both
        # records' exact text for two windows and their all-ones lengths
        # (381/365 < 384).  The second leg is the A/B eboot's record set:
        # GPU draw policy + GL time profile + clear elision/depth drop.
        completed_fill = compile_and_run(
            "fill", ("-DISAAC_VITA_GL_FILL_CENSUS=1",)
        )
        completed_fill_gl_time = compile_and_run(
            "fill-gl-time",
            (
                "-DISAAC_VITA_GL_FILL_CENSUS=1",
                "-DISAAC_VITA_GL_TIME_PROFILE=1",
                "-DISAAC_VITA_VITAGL_GPU_DRAW_OPTIMIZATIONS=1",
                "-DISAAC_VITA_FBO_CLEAR_ELISION=1",
                "-DISAAC_VITA_FBO_CLEAR_ELISION_DEPTH_DROP=1",
            ),
        )
        completed_bundle = compile_and_run(
            "bundle",
            (
                "-DISAAC_VITA_GUEST_LOOKUP_CACHE=1",
                "-DISAAC_VITA_GL_REDUNDANCY_CACHE=1",
                "-DISAAC_VITA_GL_TYPED_STATE_CACHE=1",
                "-DISAAC_VITA_VITAGL_GPU_DRAW_OPTIMIZATIONS=1",
                "-DISAAC_VITA_COLOROFFSET_GPU_OPTIMIZATIONS=1",
            ),
        )
        # ISAAC_VITA_COLOROFFSET_FS_PROBE (diagnostic 0008 probe): ph120.kp
        # directly after ph120.k on the ColorOffset bundle (records 7 -> 8);
        # the PASS string gains "; kp=<N>" only under the define (424 = the
        # 32-character build id plus 22 all-ones counters, two as %08x, below
        # the 512-byte line allowance).  Both modes print the same shape; the
        # oracle pins mode= from the define.
        fs_probe_definitions = (
            "-DISAAC_VITA_VITAGL_GPU_DRAW_OPTIMIZATIONS=1",
            "-DISAAC_VITA_COLOROFFSET_GPU_OPTIMIZATIONS=1",
        )
        completed_fs_probe = compile_and_run(
            "fs-probe", fs_probe_definitions + (
                "-DISAAC_VITA_COLOROFFSET_FS_PROBE=1",
            )
        )
        completed_fs_probe_nodiscard = compile_and_run(
            "fs-probe-nodiscard", fs_probe_definitions + (
                "-DISAAC_VITA_COLOROFFSET_FS_PROBE=2",
            )
        )
        completed_fs_probe_pair = compile_and_run(
            "fs-probe-pair", fs_probe_definitions + (
                "-DISAAC_VITA_COLOROFFSET_FS_PROBE=3",
                "-DISAAC_VITA_COLOROFFSET_PLAIN_VERTEX_PAIR_ORACLE=1",
            )
        )
        # Optional weak FP16 endpoint: startup discard, two take-and-zero
        # windows and absent-endpoint omission are checked by the same oracle.
        completed_fs_probe_half = compile_and_run(
            "fs-probe-half", fs_probe_definitions + (
                "-DISAAC_VITA_COLOROFFSET_FS_PROBE=3",
                "-DISAAC_VITA_COLOROFFSET_PLAIN_FP16_ORACLE=1",
            )
        )
        # Production forbids this combination; it still bounds the formatter
        # with both optional suffixes and every uint32_t at its maximum.
        completed_fs_probe_pair_half = compile_and_run(
            "fs-probe-pair-half", fs_probe_definitions + (
                "-DISAAC_VITA_COLOROFFSET_FS_PROBE=3",
                "-DISAAC_VITA_COLOROFFSET_PLAIN_VERTEX_PAIR_ORACLE=1",
                "-DISAAC_VITA_COLOROFFSET_PLAIN_FP16_ORACLE=1",
            )
        )
        all_on_definitions = (
            "-DISAAC_VITA_GUEST_LOOKUP_CACHE=1",
            "-DISAAC_VITA_GL_REDUNDANCY_CACHE=1",
            "-DISAAC_VITA_GL_TYPED_STATE_CACHE=1",
            "-DISAAC_VITA_VITAGL_GPU_DRAW_OPTIMIZATIONS=1",
            "-DISAAC_VITA_COLOROFFSET_GPU_OPTIMIZATIONS=1",
            "-DISAAC_VITA_POOP_FX_PROFILE=1",
            "-DISAAC_VITA_POOP_FX_SINGLE_CLOUD=1",
            "-DISAAC_VITA_LASER_PROFILE=1",
            "-DISAAC_VITA_FULLSPEED_SCHEDULER=1",
            "-DISAAC_VITA_TEXTURE_CHURN_PROFILE=1",
        )
        completed_all_on = compile_and_run("all-on", all_on_definitions)
        # ISAAC_VITA_HEAP_CENSUS: ph120.mem appended last (records 3 -> 4,
        # all-on 19 -> 20); the PASS string gains "; mem=<N>" only under the
        # define, so every other leg's exact string below stays frozen.  The
        # _LUA sub-define adds the arena fields without changing the length.
        completed_heap_census = compile_and_run(
            "heap-census", ("-DISAAC_VITA_HEAP_CENSUS=1",)
        )
        completed_heap_census_lua = compile_and_run(
            "heap-census-lua", (
                "-DISAAC_VITA_HEAP_CENSUS=1",
                "-DISAAC_VITA_HEAP_CENSUS_LUA=1",
            )
        )
        completed_all_on_census = compile_and_run(
            "all-on-census", all_on_definitions + (
                "-DISAAC_VITA_HEAP_CENSUS=1",
                "-DISAAC_VITA_HEAP_CENSUS_LUA=1",
            )
        )
        # ISAAC_VITA_KAGE_QUAD_FASTPATH: ph120.kq appended after mem (records
        # 3 -> 4); the PASS string gains "; kq=<N>" only under the define.
        completed_kage_quad = compile_and_run(
            "kage-quad", ("-DISAAC_VITA_KAGE_QUAD_FASTPATH=1",)
        )
        for suffix, extra_definitions in (
                ("base", ()),
                ("typed", ("-DISAAC_VITA_GL_TYPED_STATE_CACHE=1",)),
                ("canonical", ("-DISAAC_VITA_CANONICAL_QUAD_ZERO_COPY=1",)),
                ("texture", ("-DISAAC_VITA_TEXTURE_CHURN_PROFILE=1",)),
                ("gl-time", ("-DISAAC_VITA_GL_TIME_PROFILE=1",)),
                ("sdk-sparse", (
                    "-DISAAC_VITA_GL_TIME_PROFILE=1",
                    "-DISAAC_VITA_GL_TIME_SDK_SPARSE=1")),
                ("sdk-sparse-canonical", sdk_sparse_definitions + (
                    "-DISAAC_VITA_GL_TYPED_STATE_CACHE=1",)),
                ("gl-time-canonical", (
                    "-DISAAC_VITA_GL_TIME_PROFILE=1",
                    "-DISAAC_VITA_CANONICAL_QUAD_ZERO_COPY=1",
                    "-DISAAC_VITA_GL_TYPED_STATE_CACHE=1")),
                # Fill census alone, and with the draw dump on top of the
                # production shim options the A/B eboot carries.
                ("fill", ("-DISAAC_VITA_GL_FILL_CENSUS=1",)),
                ("fill-dump-prod", (
                    "-DISAAC_VITA_GL_FILL_CENSUS=1",
                    "-DISAAC_VITA_GL_FILL_CENSUS_DUMP=1",
                    "-DISAAC_VITA_GL_TIME_PROFILE=1",
                    "-DISAAC_VITA_CANONICAL_QUAD_ZERO_COPY=1",
                    "-DISAAC_VITA_GL_TYPED_STATE_CACHE=1",
                    "-DISAAC_VITA_GL_REDUNDANCY_CACHE=1",
                    "-DISAAC_VITA_FBO_CLEAR_ELISION=1",
                    "-DISAAC_VITA_FBO_CLEAR_ELISION_DEPTH_DROP=1"))):
            gl_object = Path(value) / f"gl-vita-phase-profile-{suffix}.o"
            gl_command = [
                cc, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                "-Wno-int-to-pointer-cast", "-Wno-pointer-to-int-cast",
                "-DISAAC_GL_VITA_BACKEND_ORACLE=1",
                "-DISAAC_VITA_PHASE_PROFILE=1",
                *extra_definitions,
                f"-I{runtime}", "-c", str(runtime / "gl_vita_backend.c"),
                "-o", str(gl_object),
            ]
            if "clang" in Path(cc).name.lower():
                gl_command.insert(6, "-Wno-deprecated-non-prototype")
            subprocess.run(gl_command, check=True)
        if os.name == "nt":
            guest_object = Path(value) / "guest-phase-profile.o"
            subprocess.run([
                cc, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                "-Wno-unused-function", "-Wno-deprecated-declarations",
                "-DISAAC_VITA_PHASE_PROFILE=1", f"-I{runtime}",
                "-c", str(runtime / "guest.c"), "-o", str(guest_object),
            ], check=True)
    expected_off = (
        "Vita phase profile host oracle: PASS "
        "(records=3; max t/c/a/g/o/y/v/q/f/k/b/h/p/l/m/n/s/r="
        "433/375/161/-/-/-/-/-/-/-/-/-/-/-/-/-/-/-; "
        "limiter skips; log cost excluded)"
    )
    expected_on = (
        "Vita phase profile host oracle: PASS "
        "(records=4; max t/c/a/g/o/y/v/q/f/k/b/h/p/l/m/n/s/r="
        "433/375/161/119/-/-/-/-/-/-/-/-/-/-/-/-/-/-; "
        "limiter skips; log cost excluded)"
    )
    if expected_off not in completed_off.stdout:
        raise AssertionError(
            f"phase profile OFF oracle returned unexpected output: "
            f"{completed_off.stdout!r}"
        )
    expected_other = (
        "Vita phase profile host oracle: PASS "
        "(records=3; max t/c/a/g/o/y/v/q/f/k/b/h/p/l/m/n/s/r="
        "470/375/161/-/-/-/-/-/-/-/-/-/-/-/-/-/-/-; "
        "limiter skips; log cost excluded)"
    )
    if expected_other not in completed_other.stdout:
        raise AssertionError(
            "phase profile oth oracle returned unexpected output: "
            f"{completed_other.stdout!r}"
        )
    # oth next to the 503-byte ph120.s record: both inside the 512 allowance.
    if re.search(
            r"Vita phase profile host oracle: PASS \(records=6; max "
            r"t/c/a/g/o/y/v/q/f/k/b/h/p/l/m/n/s/r=470/375/161(?:/-){13}"
            r"/503/276; limiter skips; log cost excluded\)",
            completed_other_scheduler.stdout) is None:
        raise AssertionError(
            "phase profile oth+scheduler oracle returned unexpected output: "
            f"{completed_other_scheduler.stdout!r}"
        )
    if expected_on not in completed_on.stdout:
        raise AssertionError(
            f"phase profile ON oracle returned unexpected output: "
            f"{completed_on.stdout!r}"
        )
    expected_dispatch = (
        "Vita phase profile host oracle: PASS "
        "(records=6; max t/c/a/g/o/y/v/q/f/k/b/h/p/l/m/n/s/r="
        "433/375/161/119/-/-/-/-/-/-/-/-/-/-/-/-/-/-; "
        "d=132/i=269; "
        "limiter skips; log cost excluded)"
    )
    if expected_dispatch not in completed_dispatch.stdout:
        raise AssertionError(
            "phase profile dispatch-table oracle returned unexpected output: "
            f"{completed_dispatch.stdout!r}"
        )
    # ISAAC_VITA_PROFILE_IMPORT_KINDS: ph120.ik/ph120.ih appended last, both
    # identity outcomes exercised inside the oracle, both records < 384 B.
    import_kinds = re.search(
        r"Vita phase profile host oracle: PASS \(records=5; max "
        r"t/c/a/g/o/y/v/q/f/k/b/h/p/l/m/n/s/r=433/375/161(?:/-){15}; "
        r"ik/ih=(\d+)/(\d+); limiter skips; log cost excluded\)",
        completed_import_kinds.stdout,
    )
    if import_kinds is None:
        raise AssertionError(
            f"phase profile import-kinds oracle returned unexpected output: "
            f"{completed_import_kinds.stdout!r}"
        )
    if (int(import_kinds[1]), int(import_kinds[2])) != (332, 305):
        raise AssertionError(
            "ph120.ik/ph120.ih worst-case record lengths drifted: "
            f"{import_kinds[1]}/{import_kinds[2]} (documented 332/305)"
        )
    # Both census readers on the one array (guest.h
    # g_guest_phase_profile_import_calls): d and i after g, ik and ih last,
    # and the ph120.ik identity computed on the derived g(c,l).
    expected_dispatch_import_kinds = (
        "Vita phase profile host oracle: PASS "
        "(records=8; max t/c/a/g/o/y/v/q/f/k/b/h/p/l/m/n/s/r="
        "433/375/161/119/-/-/-/-/-/-/-/-/-/-/-/-/-/-; "
        "d=132/i=269; ik/ih=332/305; "
        "limiter skips; log cost excluded)"
    )
    if expected_dispatch_import_kinds not in \
            completed_dispatch_import_kinds.stdout:
        raise AssertionError(
            "phase profile dispatch-table + import-kinds oracle returned "
            f"unexpected output: {completed_dispatch_import_kinds.stdout!r}"
        )
    expected_dispatch_gltok = expected_dispatch.replace(
        "d=132/i=269", "d=145/i=269")
    if expected_dispatch_gltok not in completed_dispatch_gltok.stdout:
        raise AssertionError(
            "phase profile dispatch-table + GL tokens oracle returned "
            f"unexpected output: {completed_dispatch_gltok.stdout!r}"
        )
    expected_dispatch_gltok_import_kinds = \
        expected_dispatch_import_kinds.replace("d=132/i=269", "d=145/i=269")
    if expected_dispatch_gltok_import_kinds not in \
            completed_dispatch_gltok_import_kinds.stdout:
        raise AssertionError(
            "phase profile dispatch-table + GL tokens + import-kinds oracle "
            "returned unexpected output: "
            f"{completed_dispatch_gltok_import_kinds.stdout!r}"
        )
    expected_pill = (
        "Vita phase profile host oracle: PASS "
        "(records=4; max t/c/a/g/o/y/v/q/f/k/b/h/p/l/m/n/s/r="
        "433/375/161/-/-/-/-/-/-/-/133/-/-/-/-/-/-/-; "
        "limiter skips; log cost excluded)"
    )
    if expected_pill not in completed_pill.stdout:
        raise AssertionError(
            "phase profile pill-Bloom oracle returned unexpected output: "
            f"{completed_pill.stdout!r}"
        )
    expected_bloom_half = (
        "Vita phase profile host oracle: PASS "
        "(records=4; max t/c/a/g/o/y/v/q/f/k/b/h/p/l/m/n/s/r="
        "433/375/161/-/-/-/-/-/-/-/-/180/-/-/-/-/-/-; "
        "limiter skips; log cost excluded)"
    )
    if expected_bloom_half not in completed_bloom_half.stdout:
        raise AssertionError(
            "phase profile half-resolution Bloom oracle returned "
            f"unexpected output: {completed_bloom_half.stdout!r}"
        )
    poop_fx_match = re.search(
        r"\(records=4; max t/c/a/g/o/y/v/q/f/k/b/h/p/l/m/n/s/r="
        r"433/375/161/-/-/-/-/-/-/-/-/-/(\d+)/-/-/-/-/-; ",
        completed_poop_fx.stdout,
    )
    if poop_fx_match is None or int(poop_fx_match.group(1)) != 281:
        raise AssertionError(
            "phase profile PoopFx oracle returned unexpected output: "
            f"{completed_poop_fx.stdout!r}"
        )
    if completed_poop_fx_cap.stdout != completed_poop_fx.stdout:
        raise AssertionError(
            "PoopFx single-cloud accounting changed the bounded record shape"
        )
    expected_laser = (
        "Vita phase profile host oracle: PASS "
        "(records=6; max t/c/a/g/o/y/v/q/f/k/b/h/p/l/m/n/s/r="
        "433/375/161/-/-/-/-/-/-/-/-/-/-/293/293/269/-/-; "
        "limiter skips; log cost excluded)"
    )
    if expected_laser not in completed_laser.stdout:
        raise AssertionError(
            "phase profile laser oracle returned unexpected output: "
            f"{completed_laser.stdout!r}"
        )
    expected_laser_gpu = (
        "Vita phase profile host oracle: PASS "
        "(records=9; max t/c/a/g/o/y/v/q/f/k/b/h/p/l/m/n/s/r="
        "433/375/161/-/-/-/334/348/279/-/-/-/-/293/293/269/-/-; "
        "limiter skips; log cost excluded)"
    )
    if expected_laser_gpu not in completed_laser_gpu.stdout:
        raise AssertionError(
            "phase profile laser+GPU oracle returned unexpected output: "
            f"{completed_laser_gpu.stdout!r}"
        )
    expected_typed = (
        "Vita phase profile host oracle: PASS "
        "(records=4; max t/c/a/g/o/y/v/q/f/k/b/h/p/l/m/n/s/r="
        "433/375/161/-/-/235/-/-/-/-/-/-/-/-/-/-/-/-; "
        "limiter skips; log cost excluded)"
    )
    if expected_typed not in completed_typed.stdout:
        raise AssertionError(
            "phase profile typed-state oracle returned unexpected output: "
            f"{completed_typed.stdout!r}"
        )
    coalesce_max = re.search(
        r"PASS \(records=4; max t/c/a/g/o/y/v/q/f/k/b/h/p/l/m/n/s/r="
        r"433/375/161/-/-/(\d+)/", completed_attrib_coalesce.stdout)
    if coalesce_max is None or not 235 < int(coalesce_max[1]) < 384:
        raise AssertionError(
            "attribute coalescing changed record count or exceeded line bound: "
            f"{completed_attrib_coalesce.stdout!r}"
        )
    print("[attrib-coalesce]", completed_attrib_coalesce.stdout.strip())
    expected_texture = (
        "Vita phase profile host oracle: PASS "
        "(records=5; max t/c/a/g/o/y/v/q/f/k/b/h/p/l/m/n/s/r="
        "433/375/161/-/-/-/-/-/-/-/-/-/-/-/-/-/-/-; x=375/xd=208; "
        "limiter skips; log cost excluded)"
    )
    # gt is the fourth record and gx (the 0006 EndScene split) follows it:
    # t, c, a, gt, gx, x, xd, g.
    if ("Vita phase profile host oracle: PASS (records=8;"
            not in completed_gl_time.stdout):
        raise AssertionError(
            "phase profile GL-time oracle returned unexpected output: "
            f"{completed_gl_time.stdout!r}"
        )
    # t, c, a, gt, gw, gd, gu, gx: the wrapper-time records sit between gt
    # and gx.
    expected_gl_wrapper_time = (
        "Vita phase profile host oracle: PASS "
        "(records=8; max t/c/a/g/o/y/v/q/f/k/b/h/p/l/m/n/s/r="
        "433/375/161/-/-/-/-/-/-/-/-/-/-/-/-/-/-/-; gw=379/gd=370/gu=232; "
        "limiter skips; log cost excluded)"
    )
    if expected_gl_wrapper_time not in completed_gl_wrapper_time.stdout:
        raise AssertionError(
            "phase profile GL wrapper-time oracle returned unexpected "
            f"output: {completed_gl_wrapper_time.stdout!r}"
        )
    for label, expected_records, completed in (
            ("sdk-sparse", 8, completed_sdk_sparse),
            *((f"sdk-sparse-valid-region-{value}", 9, completed)
              for value, completed in completed_sdk_sparse_valid_region.items())):
        if (f"Vita phase profile host oracle: PASS (records={expected_records};"
                not in completed.stdout):
            raise AssertionError(f"{label} unexpected output: {completed.stdout!r}")
        lengths = re.search(r"; sdk32=(\d+)/(\d+)/(\d+)/(\d+);", completed.stdout)
        if lengths is None or any(not 0 < int(size) <= 512 for size in lengths.groups()):
            raise AssertionError(f"{label} missing/unbounded scope rows: {completed.stdout!r}")
        if "; attrib=" in completed.stdout:
            raise AssertionError(f"{label} emitted the scope=attrib row without its define")
    # scope=attrib row: t, c, a, gt(scene, canonical, clear, queue, attrib), gx.
    if ("Vita phase profile host oracle: PASS (records=9;"
            not in completed_sdk_sparse_attrib.stdout):
        raise AssertionError(
            f"sdk-sparse-attrib unexpected output: {completed_sdk_sparse_attrib.stdout!r}")
    attrib_length = re.search(
        r"; sdk32=\d+/\d+/\d+/\d+; attrib=(\d+);", completed_sdk_sparse_attrib.stdout)
    if attrib_length is None or not 0 < int(attrib_length.group(1)) < 384:
        raise AssertionError(
            f"sdk-sparse-attrib scope row missing/unbounded: {completed_sdk_sparse_attrib.stdout!r}")
    # t, c, a, gt, gx(+vr), vz, x, xd, g: the 0009 census record follows gx.
    for value, completed in completed_gl_time_valid_region.items():
        if ("Vita phase profile host oracle: PASS (records=9;"
                not in completed.stdout):
            raise AssertionError(
                "phase profile GL-time + FBO valid-region oracle "
                f"(mode {value}) returned unexpected output: "
                f"{completed.stdout!r}"
            )
    # t, c, a, gt, gx, e: the clear-elision record (ph120.e) follows the GL
    # records in both the plain and the depth-drop sub-mode (records=6); the
    # oracle pins clear(x,r,p) vs clear(x,r,p,a,d) and the window deltas.
    for suffix, completed in (
            ("", completed_gl_time_elision),
            ("-drop", completed_gl_time_elision_drop)):
        if ("Vita phase profile host oracle: PASS (records=6;"
                not in completed.stdout):
            raise AssertionError(
                f"phase profile GL-time + clear-elision{suffix} oracle "
                f"returned unexpected output: {completed.stdout!r}"
            )
    if expected_texture not in completed_texture.stdout:
        raise AssertionError(
            "phase profile texture-churn oracle returned unexpected output: "
            f"{completed_texture.stdout!r}"
        )
    # t, c, a, fa, fp: the fill census records follow the (absent) ph120.f;
    # all-ones fa=381 / fp=381 bytes, both under the 384-byte log body.
    expected_fill = (
        "Vita phase profile host oracle: PASS "
        "(records=5; max t/c/a/g/o/y/v/q/f/k/b/h/p/l/m/n/s/r="
        "433/375/161/-/-/-/-/-/-/-/-/-/-/-/-/-/-/-; fa=381/fp=381; "
        "limiter skips; log cost excluded)"
    )
    if expected_fill not in completed_fill.stdout:
        raise AssertionError(
            "phase profile fill-census oracle returned unexpected output: "
            f"{completed_fill.stdout!r}"
        )
    # t, c, a, gt, gx, v, q, f, fa, fp, e: the A/B eboot's record set.
    expected_fill_gl_time = (
        "Vita phase profile host oracle: PASS "
        "(records=11; max t/c/a/g/o/y/v/q/f/k/b/h/p/l/m/n/s/r="
        "433/375/161/-/-/-/334/348/279/-/-/-/-/-/-/-/-/-; fa=381/fp=381; "
        "limiter skips; log cost excluded)"
    )
    if expected_fill_gl_time not in completed_fill_gl_time.stdout:
        raise AssertionError(
            "phase profile fill-census + GL-time oracle returned unexpected "
            f"output: {completed_fill_gl_time.stdout!r}"
        )
    expected_bundle = (
        "Vita phase profile host oracle: PASS "
        "(records=10; max t/c/a/g/o/y/v/q/f/k/b/h/p/l/m/n/s/r="
        "433/375/161/119/123/235/334/348/279/383/-/-/-/-/-/-/-/-; "
        "limiter skips; log cost excluded)"
    )
    if expected_bundle not in completed_bundle.stdout:
        raise AssertionError(
            "phase profile bundle oracle returned unexpected output: "
            f"{completed_bundle.stdout!r}"
        )
    expected_fs_probe = (
        "Vita phase profile host oracle: PASS "
        "(records=8; max t/c/a/g/o/y/v/q/f/k/b/h/p/l/m/n/s/r="
        "433/375/161/-/-/-/334/348/279/383/-/-/-/-/-/-/-/-; kp=424; "
        "limiter skips; log cost excluded)"
    )
    for label, completed in (
            ("fs-probe", completed_fs_probe),
            ("fs-probe-nodiscard", completed_fs_probe_nodiscard)):
        if expected_fs_probe not in completed.stdout:
            raise AssertionError(
                f"phase profile {label} oracle returned unexpected output: "
                f"{completed.stdout!r}"
            )
    expected_fs_probe_pair = expected_fs_probe.replace("kp=424", "kp=469")
    if expected_fs_probe_pair not in completed_fs_probe_pair.stdout:
        raise AssertionError(
            "phase profile private pair counters/record bound changed: "
            f"{completed_fs_probe_pair.stdout!r}"
        )
    for label, completed, length in (
            ("fs-probe-half", completed_fs_probe_half, 456),
            ("fs-probe-pair-half", completed_fs_probe_pair_half, 501)):
        expected = expected_fs_probe.replace("kp=424", f"kp={length}")
        if expected not in completed.stdout:
            raise AssertionError(
                f"phase profile {label} counters/record bound changed: "
                f"{completed.stdout!r}"
            )
    expected_all_on = (
        "Vita phase profile host oracle: PASS "
        "(records=19; max t/c/a/g/o/y/v/q/f/k/b/h/p/l/m/n/s/r="
        "433/375/161/119/123/235/334/348/279/383/-/-/281/293/293/269/"
    )
    if expected_all_on not in completed_all_on.stdout or \
            "; x=375/xd=208; limiter skips; log cost excluded)" not in completed_all_on.stdout:
        raise AssertionError(
            "phase profile all-ON oracle returned unexpected output: "
            f"{completed_all_on.stdout!r}"
        )
    # ISAAC_VITA_HEAP_CENSUS: one ph120.mem per window (worst case 459 B with
    # every field all-ones and why=badalloc, inside the 512-byte allowance
    # ph120.s already uses), appended after ik/ih; the entry owner's
    # out-of-cadence records and the terminal/not-running guards are proven
    # inside the oracle.
    expected_heap_census = (
        "Vita phase profile host oracle: PASS "
        "(records=4; max t/c/a/g/o/y/v/q/f/k/b/h/p/l/m/n/s/r="
        "433/375/161/-/-/-/-/-/-/-/-/-/-/-/-/-/-/-; mem=459; "
        "limiter skips; log cost excluded)"
    )
    if expected_heap_census not in completed_heap_census.stdout:
        raise AssertionError(
            "phase profile heap-census oracle returned unexpected output: "
            f"{completed_heap_census.stdout!r}"
        )
    if expected_heap_census not in completed_heap_census_lua.stdout:
        raise AssertionError(
            "phase profile heap-census+Lua oracle returned unexpected output: "
            f"{completed_heap_census_lua.stdout!r}"
        )
    # ISAAC_VITA_KAGE_QUAD_FASTPATH: one ph120.kq per window (378 B with
    # every field all-ones: 23 ten-digit numbers behind the build id),
    # discarded once at the first loop head and taken at every window flush;
    # the stub take and the exact window texts are pinned inside the oracle.
    expected_kage_quad = (
        "Vita phase profile host oracle: PASS "
        "(records=4; max t/c/a/g/o/y/v/q/f/k/b/h/p/l/m/n/s/r="
        "433/375/161/-/-/-/-/-/-/-/-/-/-/-/-/-/-/-; kq=378; "
        "limiter skips; log cost excluded)"
    )
    if expected_kage_quad not in completed_kage_quad.stdout:
        raise AssertionError(
            "phase profile kage-quad oracle returned unexpected output: "
            f"{completed_kage_quad.stdout!r}"
        )
    expected_all_on_census = expected_all_on.replace(
        "records=19", "records=20")
    if expected_all_on_census not in completed_all_on_census.stdout or \
            "; x=375/xd=208; mem=459; limiter skips; log cost excluded)" \
            not in completed_all_on_census.stdout:
        raise AssertionError(
            "phase profile all-ON + heap-census oracle returned unexpected "
            f"output: {completed_all_on_census.stdout!r}"
        )
    print(expected_off)
    print(expected_other)
    print(completed_other_scheduler.stdout.strip())
    print(expected_on)
    print(expected_dispatch)
    print(completed_import_kinds.stdout.strip())
    print(expected_dispatch_import_kinds)
    print("[dispatch-gltok] " + expected_dispatch_gltok)
    print("[dispatch-gltok-import-kinds] " +
          expected_dispatch_gltok_import_kinds)
    print(expected_pill)
    print(expected_bloom_half)
    print(completed_poop_fx.stdout.strip())
    print(completed_poop_fx_cap.stdout.strip())
    print(expected_laser)
    print(expected_laser_gpu)
    print(expected_typed)
    print(expected_texture)
    print("[sdk-sparse] " + completed_sdk_sparse.stdout.strip())
    print("[sdk-sparse-attrib] " + completed_sdk_sparse_attrib.stdout.strip())
    for value, completed in completed_sdk_sparse_valid_region.items():
        print(f"[sdk-sparse-valid-region-{value}] " + completed.stdout.strip())
    print("[fill] " + expected_fill)
    print("[fill-gl-time] " + expected_fill_gl_time)
    print("[gl-wrapper-time] " + expected_gl_wrapper_time)
    print(completed_bundle.stdout.strip())
    print("[fs-probe] " + expected_fs_probe)
    print("[fs-probe-nodiscard] " + completed_fs_probe_nodiscard.stdout.strip())
    print("[fs-probe-pair] " + completed_fs_probe_pair.stdout.strip())
    print("[fs-probe-half] " + completed_fs_probe_half.stdout.strip())
    print("[fs-probe-pair-half] " + completed_fs_probe_pair_half.stdout.strip())
    print(completed_all_on.stdout.strip())
    print("[heap-census] " + expected_heap_census)
    print("[all-on-census] " + completed_all_on_census.stdout.strip())
    print("[kage-quad] " + expected_kage_quad)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
