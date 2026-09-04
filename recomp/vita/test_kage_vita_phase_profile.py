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
    # macros defined under ISAAC_VITA_GL_TIME_PROFILE, and every wrapper that
    # opens a bracket closes it.
    timer_block = re.search(
        r"#if defined\(ISAAC_VITA_GL_TIME_PROFILE\)\n"
        r"(?:(?!#endif).)*?static inline uint64_t gl_vita_time_now\(void\)"
        r"(?:(?!#endif).)*?# define GL_VITA_TIME_BEGIN\(\)"
        r"(?:(?!#endif).)*?# define GL_VITA_TIME_END\(bucket\)"
        r"(?:(?!#endif).)*?#else\n"
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


def main() -> int:
    vita = Path(__file__).resolve().parent
    runtime = vita.parent / "runtime"
    verify_hot_path_instrumentation(runtime)
    cc = compiler()
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
        # ISAAC_VITA_PHASE_PROFILE_OTHER: oth=p50/p95/max between lim and all;
        # worst case 357 -> 394 bytes (<= the 512-byte allowance ph120.s uses).
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
        completed_all_on = compile_and_run(
            "all-on",
            (
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
            ),
        )
        for suffix, extra_definitions in (
                ("base", ()),
                ("typed", ("-DISAAC_VITA_GL_TYPED_STATE_CACHE=1",)),
                ("canonical", ("-DISAAC_VITA_CANONICAL_QUAD_ZERO_COPY=1",)),
                ("texture", ("-DISAAC_VITA_TEXTURE_CHURN_PROFILE=1",)),
                ("gl-time", ("-DISAAC_VITA_GL_TIME_PROFILE=1",)),
                ("gl-time-canonical", (
                    "-DISAAC_VITA_GL_TIME_PROFILE=1",
                    "-DISAAC_VITA_CANONICAL_QUAD_ZERO_COPY=1",
                    "-DISAAC_VITA_GL_TYPED_STATE_CACHE=1"))):
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
        "357/375/161/-/-/-/-/-/-/-/-/-/-/-/-/-/-/-; "
        "limiter skips; log cost excluded)"
    )
    expected_on = (
        "Vita phase profile host oracle: PASS "
        "(records=4; max t/c/a/g/o/y/v/q/f/k/b/h/p/l/m/n/s/r="
        "357/375/161/119/-/-/-/-/-/-/-/-/-/-/-/-/-/-; "
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
        "394/375/161/-/-/-/-/-/-/-/-/-/-/-/-/-/-/-; "
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
            r"t/c/a/g/o/y/v/q/f/k/b/h/p/l/m/n/s/r=394/375/161(?:/-){13}"
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
        "357/375/161/119/-/-/-/-/-/-/-/-/-/-/-/-/-/-; "
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
        r"t/c/a/g/o/y/v/q/f/k/b/h/p/l/m/n/s/r=357/375/161(?:/-){15}; "
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
        "357/375/161/119/-/-/-/-/-/-/-/-/-/-/-/-/-/-; "
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
        "357/375/161/-/-/-/-/-/-/-/133/-/-/-/-/-/-/-; "
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
        "357/375/161/-/-/-/-/-/-/-/-/180/-/-/-/-/-/-; "
        "limiter skips; log cost excluded)"
    )
    if expected_bloom_half not in completed_bloom_half.stdout:
        raise AssertionError(
            "phase profile half-resolution Bloom oracle returned "
            f"unexpected output: {completed_bloom_half.stdout!r}"
        )
    poop_fx_match = re.search(
        r"\(records=4; max t/c/a/g/o/y/v/q/f/k/b/h/p/l/m/n/s/r="
        r"357/375/161/-/-/-/-/-/-/-/-/-/(\d+)/-/-/-/-/-; ",
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
        "357/375/161/-/-/-/-/-/-/-/-/-/-/293/293/269/-/-; "
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
        "357/375/161/-/-/-/334/348/279/-/-/-/-/293/293/269/-/-; "
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
        "357/375/161/-/-/235/-/-/-/-/-/-/-/-/-/-/-/-; "
        "limiter skips; log cost excluded)"
    )
    if expected_typed not in completed_typed.stdout:
        raise AssertionError(
            "phase profile typed-state oracle returned unexpected output: "
            f"{completed_typed.stdout!r}"
        )
    expected_texture = (
        "Vita phase profile host oracle: PASS "
        "(records=5; max t/c/a/g/o/y/v/q/f/k/b/h/p/l/m/n/s/r="
        "357/375/161/-/-/-/-/-/-/-/-/-/-/-/-/-/-/-; x=375/xd=208; "
        "limiter skips; log cost excluded)"
    )
    # gt is the fourth record: t, c, a, gt, x, xd, g.
    if ("Vita phase profile host oracle: PASS (records=7;"
            not in completed_gl_time.stdout):
        raise AssertionError(
            "phase profile GL-time oracle returned unexpected output: "
            f"{completed_gl_time.stdout!r}"
        )
    if expected_texture not in completed_texture.stdout:
        raise AssertionError(
            "phase profile texture-churn oracle returned unexpected output: "
            f"{completed_texture.stdout!r}"
        )
    expected_bundle = (
        "Vita phase profile host oracle: PASS "
        "(records=10; max t/c/a/g/o/y/v/q/f/k/b/h/p/l/m/n/s/r="
        "357/375/161/119/123/235/334/348/279/383/-/-/-/-/-/-/-/-; "
        "limiter skips; log cost excluded)"
    )
    if expected_bundle not in completed_bundle.stdout:
        raise AssertionError(
            "phase profile bundle oracle returned unexpected output: "
            f"{completed_bundle.stdout!r}"
        )
    expected_all_on = (
        "Vita phase profile host oracle: PASS "
        "(records=19; max t/c/a/g/o/y/v/q/f/k/b/h/p/l/m/n/s/r="
        "357/375/161/119/123/235/334/348/279/383/-/-/281/293/293/269/"
    )
    if expected_all_on not in completed_all_on.stdout or \
            "; x=375/xd=208; limiter skips; log cost excluded)" not in completed_all_on.stdout:
        raise AssertionError(
            "phase profile all-ON oracle returned unexpected output: "
            f"{completed_all_on.stdout!r}"
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
    print(completed_bundle.stdout.strip())
    print(completed_all_on.stdout.strip())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
