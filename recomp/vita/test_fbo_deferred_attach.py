#!/usr/bin/env python3
"""Host proofs for ISAAC_VITA_FBO_DEFERRED_ATTACH.

Usage: python -B recomp/vita/test_fbo_deferred_attach.py [--base REV] [--keep DIR]

1. sources   every edit is a whole `#if defined(ISAAC_VITA_FBO_DEFERRED_ATTACH)`
             block: with those blocks removed no knob identifier remains, and
             each touched runtime file is byte-identical to --base (default
             22327fa, the T2 base this lane is ported onto; the check is not
             optional).  CMake: option OFF, four FATAL checks, one define on
             six sources.  Every vglSwapBuffers owner but the trophy setup
             pump calls the present hook, and manual_kage_vita.c flushes
             before its native framebuffer delete.
             Lane TXR's ISAAC_VITA_FBO_READBACK_CENSUS_SKIP nests its
             directives inside the readback fix's blocks, where a whole-block
             strip cannot reach them: every touched file is first read with
             that knob undefined (census_off_reading: each condition naming
             it evaluated from a fixed shape table, unknown shapes refused,
             directive count pinned per file, planted guards self-tested).
2. oracle    host_tests/fbo_deferred_attach_oracle.c (MSVC x86) is built
             without and with the knob in three flavours (plain; elision =
             CLEAR_ELISION + READBACK_FIX + DEPTH_CARRY; release-fbo = that +
             CLEAR_ELISION_DEPTH_DROP) against the real gl_vita_backend.c and
             a mock vitaGL that prints every native call.  For each flavour
             the ON trace must equal the OFF trace minus exactly the attaches
             an independent rule (below, computed from the OFF trace) calls
             superseded, with identical model state at every native consumer
             and identical values returned to the guest.  Mutants must FAIL
             that comparison: the backend without its OWED flush or with it
             inside the owed quad's bind window, and the shutdown scenario
             without the external flush.
2b. record   host_tests/gl_record_scenario.c run_defer (scenario 14): the
             attach patterns behind the GL recorder, with one
             REC_KAGE_BIND_PACKET and one attribute batch record between an
             attach and what follows it.  Built per-call (direct), with the
             deferred recorder and with the render worker, knob OFF and ON,
             client memory poisoned or not: all traces of one knob state
             are byte-identical, and the ON trace equals the OFF trace with
             the rule applied (computed here from the OFF trace), counters
             included.  Besides the held attach superseded on its own
             framebuffer the script has one held across a bind of another
             framebuffer and superseded there (forwarded, class OTHER), one
             read by glGetIntegerv(GL_ALPHA_BITS) (forwarded, class OTHER,
             the value returned to the guest compared), and two Binds whose
             uniform is not in the location cache (the packet is published
             in two halves around a synchronous glGetUniformLocation).
             Mutants must FAIL: a call the bind packet replays forwarding
             the held attach, a draw not forwarding it, glGetIntegerv not
             forwarding it, the attach of another framebuffer dropped
             instead of forwarded, a packet published behind the records
             that followed it, a cold Bind that does not publish the staged
             half of its packet before the location query.
3. model     FABLE's render-target lifecycle model (host_tests/
             a01_rt_reuse_sim.py, copied) replays its S1 and S4 sequences
             with the knob's rule: render-target creates per window.
"""
from __future__ import annotations

import argparse
import importlib.util
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
RECOMP = HERE.parent
ROOT = RECOMP.parent
RUNTIME = RECOMP / "runtime"
KNOB = "ISAAC_VITA_FBO_DEFERRED_ATTACH"
GUARD = f"#if defined({KNOB})"
CENSUS_GUARD = "#if defined(ISAAC_VITA_GXM_RT_CREATE_CENSUS)"
TOUCHED = (
    "recomp/runtime/gl_vita_backend.c",
    "recomp/runtime/gl_vita_backend.h",
    "recomp/runtime/kage_vita_backend.c",
    "recomp/runtime/kage_vita_continue_profile.c",
    "recomp/runtime/kage_vita_loading.c",
    "recomp/runtime/kage_vita_phase_profile.c",
    "recomp/runtime/manual_kage_vita.c",
)
DEFAULT_BASE = "22327fa179e6c6d43ab3c30747674e13149a6931"
# The owner line (r60-owner-20261111) merges the lane into the released
# int-test head.  There the OFF surface cannot equal the lane base: it must
# equal the clean three-way merge of DEFAULT_MERGED and the lane base.
DEFAULT_MERGED = "80ca55eec2f910a4dd62c37004289c7fb375dbbb"
OWED_QUAD = ("    gl_vita_fbo_native_clear("
             "s_fbo_owed_mask, s_fbo_owed_depth_bits);\n")
PRESENT_HOOK = "gl_vita_backend_fbo_deferred_attach_present();"
EXTERNAL_HOOK = "gl_vita_backend_fbo_deferred_attach_external"
OWED_FLUSH = ("    gl_vita_defer_attach_flush("
              "ISAAC_VITA_FBO_DEFER_FLUSH_OWED);\n")
IDENTIFIERS = re.compile(
    r"FBO_DEFERRED_ATTACH|defer_attach|DeferredAttach|deferred_attach|"
    r"FBO_DEFER_FLUSH|GL_VITA_DEFER_|ph120\.da")
# Lane TXR (020-texel-20261112): the readback census knob, read OFF before
# the strips.  Its directives per touched file (all others: none).
CENSUS_SKIP = "ISAAC_VITA_FBO_READBACK_CENSUS_SKIP"
CENSUS_SKIP_DIRECTIVES = {"recomp/runtime/gl_vita_backend.c": 16}

sys.path.insert(0, str(RECOMP))


def require(condition, message):
    if not condition:
        raise AssertionError(message)


# ---- 1. sources -----------------------------------------------------------------

def strip_knob_blocks(text: str, guard: str = GUARD) -> tuple[str, int]:
    """Remove whole blocks opened by the knob's guard at column 0 (the same
    walk as recomp/test_theora_cadence_profile.py strip_profile_blocks)."""
    result = []
    depth = 0
    blocks = 0
    for line in text.splitlines(keepends=True):
        if not depth and line.startswith(guard):
            depth = 1
            blocks += 1
            continue
        if depth:
            if re.match(r"#\s*if", line):
                depth += 1
            elif re.match(r"#\s*endif", line):
                depth -= 1
            continue
        result.append(line)
    require(depth == 0, "unterminated knob block")
    return "".join(result), blocks


def census_off_shape(cond: str):
    """Value, with CENSUS_SKIP undefined, of one #if/#elif condition naming
    it.  None: a shape valid only where the text is already dropped (the
    1-or-2 check inside the knob's own block).  Any other spelling is
    refused, never guessed."""
    knob = CENSUS_SKIP
    shapes = {
        f"defined({knob})": False,
        f"!defined({knob}) || {knob} != 2": True,
        f"defined({knob}) && {knob} == 1": False,
        f"defined({knob}) && {knob} == 2": False,
        f"{knob} != 1 && {knob} != 2": None,
    }
    flat = " ".join(cond.replace("\\\n", " ").split())
    require(flat in shapes, f"{knob}: unrecognised condition {flat!r}")
    return shapes[flat]


def census_off_reading(text: str) -> tuple[str, int]:
    """The text as the preprocessor sees it with CENSUS_SKIP undefined, for
    that knob's directives only: each #if/#elif naming it is evaluated
    (census_off_shape), the directive lines of its groups and their dead
    arms dropped, #else arms kept; every other line and directive is left as
    written.  A #elif naming the knob on a group opened by another condition
    (the end of the readback fix's block, the knob's #error without the fix)
    is false OFF: that arm is dropped, the group's #endif kept, and it must
    be the group's last arm.  Returns (text, directives naming the knob)."""
    lines = text.splitlines(keepends=True)
    out = []
    # [kind, emitting, flag]: kind "m" = a group opened by the knob (flag:
    # an arm taken); "o" = any other group (flag: its knob #elif seen).
    stack = []
    count = 0
    i = 0
    while i < len(lines):
        full = lines[i]
        while full.rstrip("\n").endswith("\\") and i + 1 < len(lines):
            i += 1
            full += lines[i]
        i += 1
        live = all(entry[1] for entry in stack)
        found = re.match(r"\s*#\s*(if|ifdef|ifndef|elif|else|endif)\b(.*)",
                         full, re.S)
        if not found:
            if live:
                out.append(full)
            continue
        kw, cond = found.group(1), found.group(2).strip()
        names = CENSUS_SKIP in cond
        require(not (names and kw in ("ifdef", "ifndef", "else", "endif")),
                f"{CENSUS_SKIP}: #{kw} naming the knob is not a known shape")
        if kw == "if" and names:
            count += 1
            value = census_off_shape(cond)
            require(value is not None or not live,
                    f"{CENSUS_SKIP}: {cond!r} where the text is live")
            stack.append(["m", bool(value), bool(value)])
            continue
        if kw in ("if", "ifdef", "ifndef"):
            stack.append(["o", True, False])
            if live:
                out.append(full)
            continue
        require(stack, f"#{kw} without a group")
        top = stack[-1]
        parent = all(entry[1] for entry in stack[:-1])
        if top[0] == "m":
            if kw == "elif":
                require(names, f"{CENSUS_SKIP}: #elif {cond!r} on a knob group")
                count += 1
                value = census_off_shape(cond)
                require(value is not None or not parent,
                        f"{CENSUS_SKIP}: {cond!r} where the text is live")
                top[1] = not top[2] and bool(value)
                top[2] = top[2] or bool(value)
            elif kw == "else":
                top[1] = not top[2]
                top[2] = True
            else:
                stack.pop()
            continue
        if kw == "elif" and names:
            count += 1
            require(census_off_shape(cond) is False and not top[2],
                    f"{CENSUS_SKIP}: #elif {cond!r} on another group must be "
                    f"false OFF and its only knob arm")
            top[1] = False
            top[2] = True
            continue
        require(not (top[2] and kw != "endif"),
                f"#{kw} after the knob's #elif on another group")
        if kw == "endif":
            stack.pop()
            if parent:
                out.append(full)
            continue
        if live:
            out.append(full)
    require(not stack, f"{CENSUS_SKIP}: unbalanced groups")
    return "".join(out), count


def census_off_reading_selftest() -> None:
    """Planted guards: a line OFF compiles must survive census_off_reading,
    a line it does not compile must go, and unknown shapes are refused."""
    knob = CENSUS_SKIP
    sample = (
        "a\n"
        f"# if !defined({knob}) || \\\n    {knob} != 2\n"
        "KEEP_STOCK\n"
        f"# elif defined({knob})\n"
        "DROP_ON\n"
        "# endif\n"
        f"# if defined({knob}) && \\\n    {knob} == 2\n"
        "DROP_SKIP\n"
        f"#  if {knob} != 1 && \\\n    {knob} != 2\n"
        "DROP_NESTED\n"
        "#  endif\n"
        "# else\n"
        "KEEP_ELSE\n"
        f"#  if defined({knob})\n"
        "DROP_INNER\n"
        "#  endif\n"
        "# endif\n"
        f"# if defined({knob}) && {knob} == 1\n"
        "DROP_OBSERVE\n"
        f"# elif defined({knob})\n"
        "DROP_ON_TOO\n"
        "# endif\n"
        "#if defined(OTHER_FIX)\n"
        "# if X\n"
        "KEEP_FIX\n"
        "# endif\n"
        f"#elif defined({knob})\n"
        "DROP_ERROR\n"
        "#endif\n"
        "z\n")
    text, count = census_off_reading(sample)
    require(text == ("a\nKEEP_STOCK\nKEEP_ELSE\n#if defined(OTHER_FIX)\n"
                     "# if X\nKEEP_FIX\n# endif\n#endif\nz\n") and count == 8,
            f"census OFF reading self-test: {count} {text!r}")
    for bad in (f"#if {knob} >= 2\n#endif\n",
                f"#if {knob} != 1 && {knob} != 2\n#endif\n",
                f"#ifdef {knob}\n#endif\n",
                f"#if defined({knob})\n#elif defined(OTHER)\n#endif\n",
                f"#if defined(OTHER)\n#elif defined({knob})\nx\n#else\ny\n"
                "#endif\n",
                f"#if defined(OTHER)\n#elif !defined({knob}) || "
                f"{knob} != 2\n#endif\n"):
        try:
            census_off_reading(bad)
        except AssertionError:
            continue
        raise AssertionError(f"census OFF reading accepted {bad!r}")


ADMIT_BEHIND_GUARD = re.compile(
    r"\s*#\s*if\s+defined\(ISAAC_VITA_GL_THREAD_ADMIT_BEHIND\)")
# Outermost GL admit behind blocks per touched file (the counts
# verify_admit_behind_off_reading prints for these two files).
ADMIT_BEHIND_BLOCKS = {
    "recomp/runtime/kage_vita_backend.c": 7,
    "recomp/runtime/kage_vita_phase_profile.c": 4,
}


# ISAAC_VITA_UPDATE_HITCH_PROBE, merged at int-020 (58f8d1f, via the F-BA
# lane): whole blocks of its own, no #else arm; its own OFF proof is
# vita/test_kage_vita_update_hitch.py.  Outermost blocks per touched file.
HITCH_PROBE_GUARD = re.compile(
    r"\s*#\s*if\s+defined\(ISAAC_VITA_UPDATE_HITCH_PROBE\)\s*$")
HITCH_PROBE_BLOCKS = {
    "recomp/runtime/kage_vita_phase_profile.c": 1,
}


# ISAAC_VITA_GL_DRAW_PACKET (ROAD60-SYNTHESIS.md lane C, 020-drawpkt-
# 20261112 on int-020 d874a40): whole blocks of its own in the one touched
# unit here, no #else arm; its own OFF proofs are vita/test_gl_draw_packet.py
# and the box whole-tree OFF identity.  Outermost blocks per touched file.
DRAW_PACKET_GUARD = re.compile(
    r"\s*#\s*if\s+defined\(ISAAC_VITA_GL_DRAW_PACKET\)\s*$")
DRAW_PACKET_BLOCKS = {
    "recomp/runtime/kage_vita_phase_profile.c": 4,
}


def strip_admit_behind_blocks(text: str) -> tuple[str, int]:
    """Remove whole GL admit behind blocks, guard at any indentation; an
    #else/#elif arm of one is refused rather than guessed at."""
    result = []
    depth = 0
    blocks = 0
    for line in text.splitlines(keepends=True):
        if not depth:
            if ADMIT_BEHIND_GUARD.match(line):
                depth = 1
                blocks += 1
                continue
            result.append(line)
            continue
        if re.match(r"\s*#\s*if", line):
            depth += 1
        elif re.match(r"\s*#\s*endif", line):
            depth -= 1
        elif depth == 1 and re.match(r"\s*#\s*(else|elif)", line):
            raise AssertionError("#else on a GL admit behind block is not modelled")
    require(depth == 0, "unterminated GL admit behind block")
    return "".join(result), blocks



def strip_knob_blocks_re(text: str, guard, what: str,
                         own_separator: bool = False) -> tuple[str, int]:
    """Remove whole blocks whose opening line matches `guard`, at any
    indentation; an #else/#elif arm of one is refused rather than guessed.
    own_separator: a block inserted between two blank lines brought one of
    them, so the blank line after it goes too."""
    result = []
    depth = 0
    blocks = 0
    eat_blank = False
    for line in text.splitlines(keepends=True):
        if not depth:
            if eat_blank and not line.strip():
                eat_blank = False
                continue
            eat_blank = False
            if guard.match(line):
                depth = 1
                blocks += 1
                continue
            result.append(line)
            continue
        if re.match(r"\s*#\s*if", line):
            depth += 1
        elif re.match(r"\s*#\s*endif", line):
            depth -= 1
            if not depth:
                eat_blank = (own_separator and bool(result)
                             and not result[-1].strip())
        elif depth == 1 and re.match(r"\s*#\s*(else|elif)", line):
            raise AssertionError(f"#else on a {what} block is not modelled")
    require(depth == 0, f"unterminated {what} block")
    return "".join(result), blocks

def merged_surface(base: str, other: str, name: str):
    """base:name merged into other:name over their merge base, or None
    when other is unknown here or the merge is not clean."""
    def rev(*args):
        done = subprocess.run(["git", *args], cwd=ROOT,
                              stdout=subprocess.PIPE,
                              stderr=subprocess.DEVNULL)
        return done.stdout if done.returncode == 0 else None
    if not other or rev("rev-parse", "--verify", "--quiet",
                        f"{other}^{{commit}}") is None:
        return None
    ancestor = rev("merge-base", base, other)
    if ancestor is None:
        return None
    ancestor = ancestor.decode("ascii").strip()
    with tempfile.TemporaryDirectory(prefix="fbo-defer-merge-") as tmp:
        paths = []
        for tag, revision in (("ours", other), ("anc", ancestor),
                              ("theirs", base)):
            blob = rev("show", f"{revision}:{name}")
            if blob is None:
                return None
            path = Path(tmp) / tag
            path.write_bytes(blob)
            paths.append(str(path))
        done = subprocess.run(["git", "merge-file", "-p", *paths],
                              cwd=ROOT, stdout=subprocess.PIPE,
                              stderr=subprocess.DEVNULL)
    return done.stdout if done.returncode == 0 else None


def check_sources(base: str, merged: str = "") -> None:
    total = 0
    via_merge = 0
    require(base, "--base must name a revision")
    resolved = subprocess.run(
        ["git", "rev-parse", "--verify", "--quiet", f"{base}^{{commit}}"],
        cwd=ROOT, stdout=subprocess.PIPE)
    require(resolved.returncode == 0,
            f"--base {base} is not a commit of this repository")
    census_off_reading_selftest()
    for name in TOUCHED:
        current = (ROOT / name).read_bytes().decode("utf-8")
        require("\r" not in current, f"{name}: CR line ending")
        # Lane TXR's census knob nests inside the readback fix's blocks:
        # read OFF first, its directive count pinned per file.
        current, census = census_off_reading(current)
        require(census == CENSUS_SKIP_DIRECTIVES.get(name, 0),
                f"{name}: {census} directives name {CENSUS_SKIP}, expected "
                f"{CENSUS_SKIP_DIRECTIVES.get(name, 0)}")
        # The lane's second knob (measurement only; its own proofs are
        # vita/test_gxm_rt_create_census.py) lives in whole blocks of its
        # own in kage_vita_phase_profile.c: taken out first, uncounted.
        current, _ = strip_knob_blocks(current, CENSUS_GUARD)
        current, draw_packet = strip_knob_blocks_re(current, DRAW_PACKET_GUARD,
                                                    "GL draw packet")
        require(draw_packet == DRAW_PACKET_BLOCKS.get(name, 0),
                f"{name}: {draw_packet} GL draw packet blocks, not "
                f"{DRAW_PACKET_BLOCKS.get(name, 0)}")
        # So does ISAAC_VITA_GL_THREAD_ADMIT_BEHIND, merged onto this base
        # at 7d23e50 (its own OFF proof, against 66ce1c8, is
        # vita/test_kage_vita_fullspeed_scheduler.py
        # verify_admit_behind_off_reading); some of its guards are indented.
        current, admit = strip_admit_behind_blocks(current)
        current, hitch = strip_knob_blocks_re(current, HITCH_PROBE_GUARD,
                                              "update hitch probe",
                                              own_separator=True)
        require(hitch == HITCH_PROBE_BLOCKS.get(name, 0),
                f"{name}: {hitch} update hitch probe blocks, not "
                f"{HITCH_PROBE_BLOCKS.get(name, 0)}")
        require(admit == ADMIT_BEHIND_BLOCKS.get(name, 0),
                f"{name}: {admit} GL admit behind blocks, not "
                f"{ADMIT_BEHIND_BLOCKS.get(name, 0)}")
        stripped, blocks = strip_knob_blocks(current)
        require(blocks > 0, f"{name}: no knob block")
        leak = IDENTIFIERS.search(stripped)
        require(leak is None,
                f"{name}: knob identifier outside a knob block: "
                f"{leak.group(0) if leak else ''}")
        total += blocks
        old = subprocess.check_output(
            ["git", "show", f"{base}:{name}"], cwd=ROOT)
        if stripped.encode("utf-8") != old:
            expected = merged_surface(base, merged, name)
            require(expected is not None and
                    stripped.encode("utf-8") == expected,
                    f"{name}: OFF surface differs from {base}"
                    + (f" and from its clean merge into {merged}"
                       if merged else ""))
            via_merge += 1
    backend = (RUNTIME / "gl_vita_backend.c").read_text(encoding="utf-8")
    # Every consumer class is spelled at least once, and the native attach
    # the knob forwards is the only new glFramebufferTexture2D call.
    for cls in ("DRAW", "CLEAR", "OWED", "READ", "PRESENT", "TEXTURE", "OTHER"):
        require(f"gl_vita_defer_attach_flush(ISAAC_VITA_FBO_DEFER_FLUSH_{cls})"
                in backend, f"flush class {cls} unused")
    require(len(re.findall(r"(?<![A-Za-z_])glFramebufferTexture2D\(\n",
                           backend)) == 2, "native attach call sites")
    require(backend.count(OWED_FLUSH) == 1, "one OWED flush line")
    # The owed flush precedes the owed quad's own rebind computation.
    owed = re.search(r"^static void gl_vita_fbo_owed_materialize\(void\)\n"
                     r".*?^}\n", backend, re.S | re.M).group(0)
    require(0 < owed.find(OWED_FLUSH) <
            owed.find("rebind = s_fbo_owed_framebuffer"),
            "OWED flush must precede the owed quad's rebind")
    # Swap owners: the hook sits right before the swap (loading, continue
    # overlay) or earlier in the same present (kage_vita_backend.c; its
    # trophy setup pump is the one unhooked vglSwapBuffers, GL_TRUE).
    for unit, hooks, near in (("kage_vita_loading.c", 2, True),
                              ("kage_vita_continue_profile.c", 2, True),
                              ("kage_vita_backend.c", 2, False)):
        lines = (RUNTIME / unit).read_text(encoding="utf-8").splitlines()
        swaps = [i for i, l in enumerate(lines)
                 if l.strip() == "vglSwapBuffers(GL_FALSE);"]
        hook_lines = [i for i, l in enumerate(lines)
                      if l.strip() == PRESENT_HOOK]
        require(len(hook_lines) == hooks, f"{unit}: {hooks} present hooks")
        require(swaps, f"{unit}: no vglSwapBuffers(GL_FALSE)")
        for swap in swaps:
            before = [h for h in hook_lines if h < swap]
            require(before, f"{unit}:{swap + 1}: swap without a present hook")
            if near:
                require(swap - before[-1] <= 12 and not any(
                    before[-1] < other < swap for other in swaps),
                    f"{unit}:{swap + 1}: present hook not next to the swap")
    trophy = (RUNTIME / "kage_vita_backend.c").read_text(encoding="utf-8")
    require(trophy.count("vglSwapBuffers(GL_TRUE);") == 1 and
            "isaac_vita_trophies_setup(kage_vita_trophy_present);" in trophy,
            "the unhooked swap is the trophy setup pump only")
    # manual_kage_vita.c deletes the manager framebuffer in vitaGL directly.
    manual = (RUNTIME / "manual_kage_vita.c").read_text(encoding="utf-8")
    destroy = re.search(r"^static void destroy_render_targets\(.*?^}\n",
                        manual, re.S | re.M).group(0)
    first_native = min(p for p in (destroy.find("mk_gl(MK_BIND_FB"),
                                   destroy.find("glBindFramebuffer("))
                       if p >= 0)
    require(destroy.count(EXTERNAL_HOOK) == 2 and
            0 < destroy.rfind(EXTERNAL_HOOK) < first_native,
            "destroy_render_targets must flush before its native calls")
    require(manual.count("glDeleteFramebuffers(") == 2 and
            destroy.count("glDeleteFramebuffers(") == 1 and
            "case MK_DELETE_FB: glDeleteFramebuffers(1, &g->name); break;"
            in manual, "manual_kage_vita.c: native framebuffer deletes")
    require(len(re.findall(r"^static void vita_glBindFramebuffer\(.*?^}\n",
                           backend, re.S | re.M)) == 1, "bind wrapper")
    bind = re.search(r"^static void vita_glBindFramebuffer\(.*?^}\n", backend,
                     re.S | re.M).group(0)
    require("defer_attach" not in bind, "glBindFramebuffer must not flush")
    viewport = re.search(r"^static void vita_glViewport\(.*?^}\n", backend,
                         re.S | re.M).group(0)
    require("defer_attach" not in viewport, "glViewport must not flush")

    cmake = (HERE / "CMakeLists.txt").read_text(encoding="utf-8")
    require(re.search(rf'option\({KNOB}\s+"[^"]+" OFF\)', cmake), "option OFF")
    require(cmake.count(f"if({KNOB} AND") == 4, "four FATAL checks")
    for text in (
            f"{KNOB} requires the translated KAGE runtime",
            f"{KNOB} requires ISAAC_VITA_VITAGL_STOCK_REFERENCE=ON and ",
            f"{KNOB} requires FIRST_FRAME, KNOWN_COLOR, SCREENSHOT ",
            f"{KNOB} requires ISAAC_VITA_FBO_RASTER_SCALE=OFF and "):
        require(text in cmake, f"CMake message: {text}")
    wiring = cmake.split(f"    if({KNOB})\n", 1)[1].split("    endif()", 1)[0]
    require(wiring.count(f"{KNOB}=1") == 1, "one define")
    for unit in ("gl_vita_backend.c", "kage_vita_phase_profile.c",
                 "kage_vita_backend.c", "kage_vita_continue_profile.c",
                 "kage_vita_loading.c", "manual_kage_vita.c"):
        require(f'"${{ISAAC_RUNTIME}}/{unit}"' in wiring, f"define on {unit}")
    require(wiring.count('"${ISAAC_RUNTIME}/') == 6, "exactly six sources")
    cadence = (RECOMP / "test_theora_cadence_profile.py").read_text(
        encoding="utf-8")
    require(f'line.startswith("{GUARD}")' in cadence,
            "cadence OFF-surface strip list")
    print(f"fbo deferred attach sources: {CENSUS_SKIP} read OFF before the "
          f"strips ({sum(CENSUS_SKIP_DIRECTIVES.values())} directives; "
          f"planted guards and refused shapes self-tested)")
    print(f"fbo deferred attach sources: PASS ({total} knob blocks in "
          f"{len(TOUCHED)} files, no identifier outside them, OFF surface "
          f"byte-identical to {base[:7]}"
          + (f" ({via_merge} files as its clean three-way merge into "
             f"{merged[:7]})" if via_merge else "")
          + f" once the {sum(ADMIT_BEHIND_BLOCKS.values())} GL admit behind, "
          f"{sum(HITCH_PROBE_BLOCKS.values())} update hitch probe and "
          f"{sum(DRAW_PACKET_BLOCKS.values())} GL draw packet "
          "blocks are out; CMake option/checks/define scope; "
          f"swap owners and native destroy hooked)")


# ---- 2. oracle ------------------------------------------------------------------

FLAVOURS = {
    "plain": [],
    "elision": [
        "/DISAAC_VITA_FBO_CLEAR_ELISION=1",
        "/DISAAC_VITA_FBO_READBACK_FIX=1",
        "/DISAAC_VITA_FBO_DEPTH_CARRY=1",
    ],
    "release-fbo": [
        "/DISAAC_VITA_FBO_CLEAR_ELISION=1",
        "/DISAAC_VITA_FBO_CLEAR_ELISION_DEPTH_DROP=1",
        "/DISAAC_VITA_FBO_READBACK_FIX=1",
        "/DISAAC_VITA_FBO_DEPTH_CARRY=1",
    ],
    # The released owner recipe: release-fbo plus the logical viewport
    # (DISPLAY_CENTER_Y replays a native glViewport when the draw
    # framebuffer changes, a call a deferred bind does not flush).
    "owner-center-y": [
        "/DISAAC_VITA_FBO_CLEAR_ELISION=1",
        "/DISAAC_VITA_FBO_CLEAR_ELISION_DEPTH_DROP=1",
        "/DISAAC_VITA_FBO_READBACK_FIX=1",
        "/DISAAC_VITA_FBO_DEPTH_CARRY=1",
        "/DISAAC_VITA_DISPLAY_CENTER_Y=1",
    ],
}
# Native calls that neither read nor change what an attach sets in vitaGL.
NEUTRAL = ("bindfb", "viewport", "bindtex", "gxmfinish", "enable",
           "cleardepth")
FRAMEBUFFER_TARGETS = ("8d40", "8ca9")  # GL_FRAMEBUFFER, GL_DRAW_FRAMEBUFFER


def build_and_run(env, compiler, work: Path, flavour: str, on: bool,
                  mutant: str = "") -> list[str]:
    """mutant: "" | "no-owed-flush" (the backend without its OWED flush
    line) | "late-owed-flush" (that line moved inside the owed quad's bind
    window) | "no-external-flush" (scenario 13 without the destroy flush)."""
    tag = f"{flavour}-{'on' if on else 'off'}" + (f"-{mutant}" if mutant else "")
    backend = RUNTIME / "gl_vita_backend.c"
    extra = []
    if mutant in ("no-owed-flush", "late-owed-flush"):
        text = backend.read_bytes().decode("utf-8")
        require(text.count(OWED_FLUSH) == 1 and text.count(OWED_QUAD) == 1,
                "mutant anchors")
        text = text.replace(OWED_FLUSH, "")
        if mutant == "late-owed-flush":
            text = text.replace(OWED_QUAD, OWED_FLUSH + OWED_QUAD)
        backend = work / f"mutant-{tag}" / "gl_vita_backend.c"
        backend.parent.mkdir()
        backend.write_bytes(text.encode("utf-8"))
    elif mutant == "no-external-flush":
        extra = ["/DORACLE_SKIP_EXTERNAL_FLUSH=1"]
    else:
        require(not mutant, f"unknown mutant {mutant}")
    exe = work / f"fbo-defer-{tag}.exe"
    objdir = work / f"obj-{tag}"
    objdir.mkdir()
    command = [
        compiler, "/nologo", "/std:c11", "/O2", "/W4", "/WX", "/wd4310",
        "/D_CRT_SECURE_NO_WARNINGS", "/DISAAC_GL_VITA_BACKEND_ORACLE=1",
        "/DGUEST_IMAGE_BASE=0x98000000u",
    ] + FLAVOURS[flavour] + ([f"/D{KNOB}=1"] if on else []) + extra + [
        "/I", str(RUNTIME),
        str(RUNTIME / "gl_bridge.c"),
        str(backend),
        str(RUNTIME / "guest_stack_legacy_oracle_stub.c"),
        str(HERE / "host_tests" / "fbo_deferred_attach_oracle.c"),
        "/Fe:" + str(exe), "/Fo:" + str(objdir) + os.sep,
    ]
    built = subprocess.run(command, env=env, cwd=work, stdout=subprocess.PIPE,
                           stderr=subprocess.STDOUT)
    log = built.stdout.decode("cp866", errors="replace")
    (work / f"build-{tag}.log").write_text(log, encoding="utf-8")
    if built.returncode != 0 or not exe.is_file():
        print(log[-6000:])
        raise AssertionError(f"oracle build failed: {tag}")
    ran = subprocess.run([str(exe)], cwd=work, stdout=subprocess.PIPE,
                         stderr=subprocess.STDOUT)
    out = ran.stdout.decode("ascii", errors="replace").replace("\r\n", "\n")
    (work / f"trace-{tag}.txt").write_text(out, encoding="utf-8", newline="\n")
    if ran.returncode != 0:
        print(out[-4000:])
        raise AssertionError(f"oracle run failed: {tag} rc={ran.returncode}")
    lines = out.splitlines()
    require(lines and lines[-1] == "E done", f"{tag}: truncated trace")
    return lines


def tracked_attach(fields: list[str]) -> bool:
    # N attach <target> <attachment> <textarget> <texture> <level> fb=<n>
    return (fields[2] in FRAMEBUFFER_TARGETS and fields[3] == "8ce0" and
            fields[4] == "de1" and fields[5] != "0" and fields[6] == "0" and
            fields[7] != "fb=0")


def superseded_in(off: list[str]) -> set[int]:
    """Independent statement of the rule, on the OFF trace: a tracked attach
    is superseded iff the next native call that is not neutral is a tracked
    attach of the same framebuffer.  A status query is neutral for the
    draw-side targets (vitaGL: COMPLETE iff a texture is attached, and the
    pending texture is non-zero) and a consumer for READ_FRAMEBUFFER."""
    gone = set()
    pending = None  # (line index, fb)
    status_target = None
    for index, line in enumerate(off):
        fields = line.split()
        if fields[0] == "T" and fields[1] == "status":
            status_target = fields[2]
            continue
        if fields[0] != "N":
            continue
        call = fields[1]
        if call in NEUTRAL or (call == "getint" and fields[2] == "texbinding"):
            continue
        if call == "status":
            require(status_target is not None, "status without its target")
            neutral = status_target in FRAMEBUFFER_TARGETS
            status_target = None
            if neutral:
                continue
            pending = None
            continue
        if call == "attach" and tracked_attach(fields):
            if pending is not None and pending[1] == fields[7]:
                gone.add(pending[0])
            pending = (index, fields[7])
            continue
        pending = None
    return gone


def compare(flavour: str, off: list[str], on: list[str]) -> dict:
    def pick(lines, predicate):
        return [line for line in lines if predicate(line)]

    # (a) every boundary, every guest-visible value, the guest call trace.
    for kind in ("S ", "G ", "T ", "E "):
        require(pick(off, lambda l: l.startswith(kind)) ==
                pick(on, lambda l: l.startswith(kind)),
                f"{flavour}: '{kind.strip()}' lines differ")
    # (b) the model state at every native consumer except a status query the
    # knob answers itself (its value is in the G lines).
    consumer = lambda l: l.startswith("C ") and not l.startswith("C status ")
    off_c, on_c = pick(off, consumer), pick(on, consumer)
    if off_c != on_c:
        for index, (a, b) in enumerate(zip(off_c, on_c)):
            if a != b:
                raise AssertionError(
                    f"{flavour}: consumer {index} state differs\n  OFF {a}\n  ON  {b}")
        raise AssertionError(f"{flavour}: consumer count {len(off_c)} != {len(on_c)}")
    # (c) every other native call, in order.
    other = lambda l: (l.startswith("N ") and
                       l.split()[1] not in ("attach", "bindfb", "status"))
    require(pick(off, other) == pick(on, other),
            f"{flavour}: native calls other than attach/bind/status differ")
    # (d) attaches: ON == OFF minus exactly the superseded ones.
    gone = superseded_in(off)
    expected = [line for index, line in enumerate(off)
                if line.startswith("N attach ") and index not in gone]
    actual = pick(on, lambda l: l.startswith("N attach "))
    require(expected == actual,
            f"{flavour}: ON attaches are not OFF minus the superseded ones "
            f"({len(actual)} vs {len(expected)} expected)")
    # (e) binds: OFF's are a subsequence of ON's; the extra ones are the
    # rebind pairs around a forwarded attach.
    off_b = pick(off, lambda l: l.startswith("N bindfb "))
    on_b = iter(pick(on, lambda l: l.startswith("N bindfb ")))
    require(all(any(b == candidate for candidate in on_b) for b in off_b),
            f"{flavour}: OFF binds are not a subsequence of ON binds")
    # (f) status: ON's native queries are OFF's minus the locally answered.
    counters = [dict(zip("dxs", map(int, re.findall(r"[dxs]=(\d+)", l)[:3])),
                     f=[int(v) for v in l.split("f=")[1].split(",")])
                for l in on if l.startswith("D ")]
    require(len(counters) == len(pick(on, lambda l: l.startswith("S "))),
            f"{flavour}: one counter line per scenario")
    total = {key: sum(c[key] for c in counters) for key in "dxs"}
    flushed = [sum(c["f"][i] for c in counters) for i in range(7)]
    off_attach = len(pick(off, lambda l: l.startswith("N attach ")))
    require(total["x"] == len(gone) == off_attach - len(actual),
            f"{flavour}: dropped counter {total['x']} vs rule {len(gone)}")
    require(total["d"] == total["x"] + sum(flushed),
            f"{flavour}: deferred != dropped + flushed")
    off_status = len(pick(off, lambda l: l == "N status"))
    on_status = len(pick(on, lambda l: l == "N status"))
    require(off_status - on_status == total["s"],
            f"{flavour}: status_local {total['s']} vs {off_status - on_status}")
    require(len(pick(on, lambda l: l.startswith("B "))) == 1 and
            not pick(off, lambda l: l.startswith("B ") or l.startswith("D ")),
            f"{flavour}: banner/receipt presence")
    per_scenario = {}
    names = [l[2:] for l in on if l.startswith("S ")]
    for name, c in zip(names, counters):
        per_scenario[name] = c
    return dict(off_attach=off_attach, on_attach=len(actual), total=total,
                flushed=flushed, scenarios=per_scenario,
                consumers=len(off_c))


def check_oracle(work: Path) -> None:
    import build_all as BA

    env, compiler = BA.msvc_env()
    for flavour in FLAVOURS:
        off = build_and_run(env, compiler, work, flavour, False)
        on = build_and_run(env, compiler, work, flavour, True)
        r = compare(flavour, off, on)
        s = r["scenarios"]
        # Scenario-specific expectations, so a rule that defers nothing or
        # flushes everywhere cannot pass by agreeing with itself.
        require(s["attach-viewport-draw"]["x"] == 1, "viewport must not flush")
        require(s["attach-swap"]["f"][4] == 1, "present must flush")
        require(s["detach"]["f"][6] >= 3, "detach/other shapes flush first")
        require(s["attach-delete-texture"]["f"][5] >= 3, "texture consumers")
        require(s["attach-bind-other-fbo"]["x"] == 1 and
                s["attach-bind-other-fbo"]["f"][6] >= 3, "bind / other fbo")
        require(s["attach-a-draw-attach-b-draw"]["x"] == 0,
                "a consumed attach is never dropped")
        owed, scissor = s["attach-owed-clear"], s["attach-owed-scissor"]
        shutdown = s["shutdown-native-destroy"]
        if flavour == "plain":
            # No elision: every clear is native, nothing is ever owed.
            require(r["flushed"][2] == 0, "plain: no owed flush")
            require(shutdown["f"][3] == 1 and shutdown["f"][6] == 0,
                    "plain: the native read consumed the shutdown attach")
        else:
            # The owed quad forwards the held attach first: detach in the
            # attach wrapper, a read bound away, the scissor poison.
            require(scissor["f"][2] == 1, "owed flush at the scissor poison")
            require(shutdown["f"][6] == 1 and shutdown["x"] == 0,
                    "the native destroy forwards the held attach")
        if flavour == "elision":
            # Without DEPTH_DROP the re-attach materialises too: consumed.
            require(owed["f"][2] == 3 and owed["x"] == 0, "elision owed")
        if flavour == "release-fbo":
            # DEPTH_DROP: the re-attach drops the debt, the attach with it.
            require(owed["f"][2] == 2 and owed["x"] == 1, "release owed")
        if flavour == "release-fbo":
            # The backend serves GetTexel: its attach never reaches vitaGL.
            require(s["read-only-attach-then-reattach"]["x"] == 3 and
                    s["read-only-attach-then-reattach"]["f"][3] == 0,
                    "backend-served reads must not flush")
            require(s["fable-s1-dry-frames"]["x"] == 2 and
                    s["fable-s1-dry-frames"]["f"][3] == 0, "S1 frames")
            require(s["attach-native-readpixels"]["f"][3] == 1,
                    "a read vitaGL serves must flush")
        if flavour == "plain":
            require(s["read-only-attach-then-reattach"]["f"][3] == 3 and
                    s["read-only-attach-then-reattach"]["x"] == 0,
                    "native reads consume the attach")
        t = r["total"]
        print(f"fbo deferred attach oracle [{flavour}]: PASS "
              f"(native attaches {r['off_attach']} -> {r['on_attach']}, "
              f"deferred {t['d']} dropped {t['x']} status-local {t['s']}, "
              f"flush dr,cl,ow,rd,pr,tx,ot="
              f"{','.join(map(str, r['flushed']))}, "
              f"{r['consumers']} consumer states identical)")
    # Mutants: the comparison must reject a backend whose owed quad does not
    # forward the held attach, and a native destroy that does not flush.
    for flavour, mutant in (("release-fbo", "no-owed-flush"),
                            ("elision", "no-owed-flush"),
                            ("release-fbo", "late-owed-flush"),
                            ("release-fbo", "no-external-flush")):
        off = (work / f"trace-{flavour}-off.txt").read_text(
            encoding="utf-8").splitlines()
        on = build_and_run(env, compiler, work, flavour, True, mutant)
        try:
            compare(flavour, off, on)
        except AssertionError as error:
            first = str(error).splitlines()[0]
            print(f"fbo deferred attach mutant [{flavour} {mutant}]: "
                  f"rejected ({first})")
        else:
            raise AssertionError(
                f"mutant {flavour} {mutant} passed the comparison")


# ---- 2b. scenario 14: the attach across a bind packet and an attribute batch -------
#
# The oracle above drives gl_vita_backend.c directly.  On the device the
# backend sits behind the GL recorder, and since RENDER60 S4 / W-pkt two
# record types replay several guest calls from one record:
# REC_ATTRIBS_ENABLE/_DISABLE (79/80, gl_record_backend.c
# record_replay_attribs) and REC_KAGE_BIND_PACKET (81,
# record_replay_bind_packet).  host_tests/gl_record_scenario.c run_defer
# issues the deferred-attach patterns with a Shader::Bind and an attribute
# replay between an attach and what follows it; this leg builds it direct,
# with the deferred recorder and with the render worker (both with the two
# knobs), without and with the deferred attach.

SCENARIO_C = HERE / "host_tests" / "gl_record_scenario.c"
# test_gl_vita_backend.ps1:271 (the recorder units of the layer-2 scenario).
RECORD_SOURCES = ("gl_record_backend.c", "gl_record_ring.c",
                  "gl_record_locations.c")
# guest_gl_installed_backend() (what the Bind seam hands the recorder,
# host_vita_kage_bind_packet.c:466) exists under these two (gl_bridge.c:705).
PACKED = ["/DISAAC_VITA_GL_SHIM_FASTDISPATCH=1",
          "/DISAAC_VITA_SHADER_ATTRIB_FASTPATH=1",
          "/DISAAC_VITA_GL_THREAD_RECORD=1",
          "/DISAAC_VITA_GL_RECORD_ATTRIB_BATCH=1",
          "/DISAAC_VITA_KAGE_BIND_PACKET=1"]
RECORD_VARIANTS = {
    "direct": [],
    "record": PACKED,
    # test_gl_vita_backend.ps1:323 (worker = a real consumer thread, jitter).
    "worker": PACKED + ["/DISAAC_VITA_GL_THREAD_WORKER=1",
                        "/DISAAC_GL_RECORD_WORKER_JITTER=1"],
}
# One flavour.  ISAAC_VITA_FBO_READBACK_FIX is left to the oracle above
# (its elision flavours): the scenario harness prints its inner
# glGetIntegerv as a hash of 16 bytes at a 4-byte GLint
# (gl_record_scenario.c rt_glGetIntegerv), i.e. of the caller's stack, which
# is not comparable between a direct, a packed and a worker build.
# ISAAC_VITA_FBO_CLEAR_ELISION (+ DEPTH_CARRY) without READBACK_FIX was tried
# here and is INERT in run_defer: its framebuffers 9 and 10 are never
# glGenFramebuffers'ed, so gl_vita_fbo_clear_defer finds no record and every
# clear stays native (measured: the same 3593 native calls and ow = 0 as
# "plain").  A flavour that changes nothing is not coverage; the elision
# paths are exercised by the oracle above only.
RECORD_FLAVOURS = {
    "plain": [],
}
# run_defer (gl_record_scenario.c): 5 Binds a frame x 48 frames + 1 closing
# Bind + 8 of section f (frames 2, 8, ..., 44) + 2 of section g; the two of
# section g take the bind packet's cold path.
SCN_BINDS = 48 * 5 + 1 + 8 + 2
SCN_COLD = 2
DRAW_FLUSH = ("    gl_vita_defer_attach_flush("
              "ISAAC_VITA_FBO_DEFER_FLUSH_DRAW);\n")
BIND_TEXTURE_HEAD = ("static void vita_glBindTexture("
                     "guest_gl_enum target, guest_gl_uint texture)\n{\n")
KNOB_FLUSH_OTHER = (f"#if defined({KNOB})\n"
                    "    gl_vita_defer_attach_flush("
                    "ISAAC_VITA_FBO_DEFER_FLUSH_OTHER);\n#endif\n")
# Backend text mutants of the scenario leg: name -> (anchor, replacement).
SCENARIO_TEXT_MUTANTS = {
    # a call every bind packet replays forwards the held attach
    "packet-op-flushes": (BIND_TEXTURE_HEAD,
                          BIND_TEXTURE_HEAD + KNOB_FLUSH_OTHER),
    # the draw does not forward it
    "no-draw-flush": (DRAW_FLUSH, ""),
    # glGetIntegerv does not forward it (run_defer section f)
    "no-getint-flush": (
        "    /* GL_ALPHA_BITS and friends read the bound framebuffer's "
        "attachment. */\n"
        "    gl_vita_defer_attach_flush(ISAAC_VITA_FBO_DEFER_FLUSH_OTHER);\n",
        ""),
    # a held attach of ANOTHER framebuffer is dropped, not forwarded
    # (run_defer section f)
    "cross-fb-drop": (
        "        if (s_defer_attach.framebuffer != s_draw_framebuffer)\n"
        "            gl_vita_defer_attach_flush("
        "ISAAC_VITA_FBO_DEFER_FLUSH_OTHER);\n"
        "        else\n"
        "            ++s_defer_attach_counters.dropped;",
        "            ++s_defer_attach_counters.dropped;"),
}
# Native calls (trace heads of gl_record_scenario.c rt_*) that neither read
# nor change what an attach sets in vitaGL: everything a bind packet and an
# attribute batch replay, and the calls the oracle above lists as NEUTRAL.
RT_NEUTRAL = ("useProgram", "activeTexture", "bindTexture",
              "getUniformLocation", "uniform1i", "uniformMatrix4fv",
              "enableAttrib", "disableAttrib", "viewport", "createProgram",
              "linkProgram")
# The flush class each consumer must report (index into flush(...)).
RT_CONSUMER = {"drawElements": 0, "clear": 1, "readPixels": 3, "swap": 4,
               "getIntegerv": 6}


def scenario_build(env, compiler, work: Path, generated: Path, flavour: str,
                   variant: str, on: bool, mutant: str = "") -> Path:
    """mutant: "" | a key of SCENARIO_TEXT_MUTANTS (the backend with one
    edit) | "late-packet" (the script publishes each bind packet behind the
    records that followed it) | "cold-no-flush" (a cold Bind asks vitaGL for
    its location without publishing the staged half of its packet first)."""
    tag = (f"scn-{flavour}-{variant}-{'on' if on else 'off'}" +
           (f"-{mutant}" if mutant else ""))
    backend = RUNTIME / "gl_vita_backend.c"
    extra = []
    if mutant in SCENARIO_TEXT_MUTANTS:
        text = backend.read_bytes().decode("utf-8")
        anchor, replacement = SCENARIO_TEXT_MUTANTS[mutant]
        require(text.count(anchor) == 1, f"scenario mutant anchor: {mutant}")
        text = text.replace(anchor, replacement)
        backend = work / f"mutant-{tag}" / "gl_vita_backend.c"
        backend.parent.mkdir()
        backend.write_bytes(text.encode("utf-8"))
    elif mutant == "late-packet":
        extra = ["/DSCN_DEFER_MUTATE_LATE_PACKET=1"]
    elif mutant == "cold-no-flush":
        extra = ["/DSCN_DEFER_MUTATE_COLD_NO_FLUSH=1"]
    else:
        require(not mutant, f"unknown scenario mutant {mutant}")
    exe = work / f"fbo-defer-{tag}.exe"
    objdir = work / f"obj-{tag}"
    objdir.mkdir()
    # test_gl_vita_backend.ps1:331-344 (the layer-2 scenario command).
    command = [
        compiler, "/nologo", "/std:c11", "/O2", "/W4", "/WX", "/wd4310",
        "/D_CRT_SECURE_NO_WARNINGS", "/DISAAC_GL_VITA_BACKEND_ORACLE=1",
        "/DISAAC_GL_RECORD_SCENARIO=1", "/DISAAC_VITA_IO_PROFILE=1",
    ] + RECORD_FLAVOURS[flavour] + RECORD_VARIANTS[variant] + (
        [f"/D{KNOB}=1"] if on else []) + extra + [
        "/I", str(generated), "/DGUEST_IMAGE_BASE=0x98000000u",
        "/I", str(RUNTIME),
        str(RUNTIME / "gl_bridge.c"), str(backend),
    ] + ([str(RUNTIME / unit) for unit in RECORD_SOURCES]
         if variant != "direct" else []) + [
        str(RUNTIME / "guest_stack_legacy_oracle_stub.c"),
        str(RUNTIME / "gl_vita_backend_oracle.c"),
        str(SCENARIO_C),
        "/Fe:" + str(exe), "/Fo:" + str(objdir) + os.sep,
    ]
    built = subprocess.run(command, env=env, cwd=work, stdout=subprocess.PIPE,
                           stderr=subprocess.STDOUT)
    log = built.stdout.decode("cp866", errors="replace")
    (work / f"build-{tag}.log").write_text(log, encoding="utf-8")
    if built.returncode != 0 or not exe.is_file():
        print(log[-6000:])
        raise AssertionError(f"scenario build failed: {tag}")
    return exe


def scenario_run(work: Path, exe: Path, poison: int):
    """(trace lines, stdout lines, return code)."""
    trace = work / f"trace-{exe.stem[len('fbo-defer-'):]}-p{poison}.txt"
    ran = subprocess.run([str(exe), str(trace), str(poison), "defer"],
                         cwd=work, stdout=subprocess.PIPE,
                         stderr=subprocess.STDOUT)
    out = ran.stdout.decode("ascii", errors="replace").replace("\r\n", "\n")
    lines = (trace.read_text(encoding="ascii").splitlines()
             if trace.is_file() else [])
    return lines, out.splitlines(), ran.returncode


def scenario_split(lines: list[str], tag: str):
    """native call groups (a call line and its indented detail lines), the
    guest-visible results, the trailer (phase/typed dumps)."""
    require("results" in lines, f"{tag}: truncated trace")
    cut = lines.index("results")
    groups = []
    for line in lines[:cut]:
        if line.startswith(" "):
            require(groups, f"{tag}: detail line without a call")
            groups[-1].append(line)
        else:
            groups.append([line])
    tail = lines[cut + 1:]
    results = [l for l in tail if l.startswith("ret ")]
    trailer = [l for l in tail if not l.startswith("ret ")]
    return groups, results, trailer


def scenario_expected(groups):
    """The knob's rule stated on the OFF native trace, independently of the
    backend: a tracked attach is held; calls in RT_NEUTRAL and framebuffer
    binds pass it; a draw-side status query of its framebuffer is answered
    without vitaGL; a tracked attach of the same framebuffer replaces it;
    anything else is a consumer and receives it first, bracketed by a bind
    pair when its framebuffer is not the bound one.  Returns the expected ON
    call groups, the number of replaced attaches, the number of locally
    answered status queries and the flushes per class."""
    out = []
    pending = None  # (group, framebuffer)
    bound = 0
    dropped = local = 0
    flushed = [0] * 7

    def flush(cls):
        nonlocal pending
        if pending is None:
            return
        group, framebuffer = pending
        pending = None
        if framebuffer != bound:
            out.append([f"bindFramebuffer 8ca9 {framebuffer}"])
        out.append(group)
        if framebuffer != bound:
            out.append([f"bindFramebuffer 8ca9 {bound}"])
        flushed[cls] += 1

    for group in groups:
        fields = group[0].split()
        head = fields[0]
        if head == "bindFramebuffer":
            if fields[1] in FRAMEBUFFER_TARGETS:
                bound = int(fields[2])
        elif head in RT_NEUTRAL:
            pass
        elif head == "checkFramebufferStatus":
            if pending is not None:
                if fields[1] not in FRAMEBUFFER_TARGETS:
                    flush(6)
                elif pending[1] == bound:
                    local += 1
                    continue
        elif head == "framebufferTexture2D":
            # framebufferTexture2D <target> <attachment> <textarget> <tex> <level>
            if (fields[1] in FRAMEBUFFER_TARGETS and fields[2] == "8ce0" and
                    fields[3] == "de1" and fields[4] != "0" and
                    fields[5] == "0" and bound != 0):
                if pending is not None:
                    if pending[1] == bound:
                        dropped += 1
                        pending = None
                    else:
                        flush(6)
                pending = (group, bound)
                continue
            flush(6)
        else:
            require(head in RT_CONSUMER,
                    f"scenario 14: unclassified native call '{head}'")
            flush(RT_CONSUMER[head])
        out.append(group)
    require(pending is None, "scenario 14 must end on a consumer")
    return out, dropped, local, flushed


def scenario_counters(stdout: list[str], tag: str):
    line = [l for l in stdout if l.startswith("defer counters ")]
    require(len(line) == 1, f"{tag}: one 'defer counters' line")
    d, x, s = map(int, re.findall(r" [dxs]=(\d+)", line[0]))
    return d, x, s, [int(v) for v in line[0].split("f=")[1].split(",")]


def scenario_compare(tag: str, off: list[str], on: list[str],
                     on_stdout: list[str]) -> dict:
    off_groups, off_results, _ = scenario_split(off, tag + " off")
    on_groups, on_results, _ = scenario_split(on, tag + " on")
    require(off_results == on_results,
            f"{tag}: values returned to the guest differ")
    expected, dropped, local, flushed = scenario_expected(off_groups)
    if expected != on_groups:
        for index, (a, b) in enumerate(zip(expected, on_groups)):
            if a != b:
                raise AssertionError(
                    f"{tag}: native call {index} differs\n"
                    f"  expected {a[0]}\n  ON       {b[0]}")
        raise AssertionError(f"{tag}: native call count {len(on_groups)} "
                             f"vs {len(expected)} expected")
    d, x, s, f = scenario_counters(on_stdout, tag)
    require(x == dropped and s == local and f == flushed and
            d == x + sum(f),
            f"{tag}: counters d={d} x={x} s={s} f={f} vs rule "
            f"x={dropped} s={local} f={flushed}")
    attaches = sum(1 for g in off_groups if g[0].startswith("framebufferTex"))
    return dict(attaches=attaches, dropped=dropped, local=local,
                flushed=flushed, calls=len(off_groups))


def check_record(work: Path) -> None:
    import build_all as BA

    env, compiler = BA.msvc_env()
    # gl_vita_backend_oracle.c includes the attribute owner extraction: a
    # generated file, generated here (test_gl_vita_backend.ps1:13).
    generated = work / "obj-generated"
    generated.mkdir()
    made = subprocess.run(
        [sys.executable, "-B", str(HERE / "test_vita_attrib_boundaries.py"),
         "--output", str(generated)], cwd=RECOMP, stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT)
    require(made.returncode == 0 and
            (generated / "gl_vita_attrib_boundaries_generated.inc").is_file(),
            "attribute owner extraction failed:\n" +
            made.stdout.decode("utf-8", errors="replace")[-2000:])
    for flavour in RECORD_FLAVOURS:
        traces = {}
        stdout = {}
        batches = set()
        for on in (False, True):
            for variant in RECORD_VARIANTS:
                exe = scenario_build(env, compiler, work, generated, flavour,
                                     variant, on)
                for poison in (0, 1):
                    lines, out, rc = scenario_run(work, exe, poison)
                    tag = f"{flavour} {variant} {'on' if on else 'off'} p{poison}"
                    require(rc == 0, f"scenario 14 run failed: {tag} rc={rc}\n"
                            + "\n".join(out[-20:]))
                    traces[on, variant, poison] = lines
                    stdout[on, variant, poison] = out
                    stats = [l for l in out if l.startswith("defer binds=")]
                    require(len(stats) == 1, f"{tag}: stats line")
                    binds = int(re.search(r"binds=(\d+)", stats[0]).group(1))
                    require(binds == SCN_BINDS, f"{tag}: {stats[0]}")
                    cold = int(re.search(r" cold=(\d+)", stats[0]).group(1))
                    require(cold == (0 if variant == "direct" else SCN_COLD),
                            f"{tag}: {stats[0]}")
                    if variant != "direct":
                        # Every Bind went out as ONE record 81 answered from
                        # the location cache, every attribute set as one
                        # record 79/80: the packed path really ran.
                        packed = re.search(
                            rf" packets={binds} staged={binds * 7} memo=\d+ "
                            rf"cache=\d+ packet-cold=0 batches=(\d+),(\d+)$",
                            stats[0])
                        # the two Binds of section g went cold: the packet
                        # was published in the middle (one record more each)
                        # and the location was asked synchronously (one
                        # staged call less each).
                        packed = re.search(
                            rf" packets={binds + SCN_COLD} "
                            rf"staged={binds * 7 - SCN_COLD} memo=\d+ "
                            rf"cache={SCN_COLD} packet-cold={SCN_COLD} "
                            rf"batches=(\d+),(\d+)$", stats[0])
                        require(packed and
                                int(packed.group(1)) >= binds // 2 and
                                packed.group(1) == packed.group(2),
                                f"{tag}: {stats[0]}")
                        batches.add(packed.group(1))
        require(len(batches) == 1,
                f"scenario 14 [{flavour}]: batch counts differ {batches}")
        # Same knob state: per-call, packed and worker replays are identical
        # byte for byte (trace, results and typed-state dump), poisoned or
        # not.
        for on in (False, True):
            reference = traces[on, "direct", 0]
            for key, lines in traces.items():
                require(key[0] != on or lines == reference,
                        f"scenario 14 [{flavour}]: trace of {key} differs "
                        f"from the direct one")
        # Knob ON = OFF with the rule applied; nothing else moves.
        r = None
        for variant in RECORD_VARIANTS:
            r = scenario_compare(
                f"scenario 14 [{flavour} {variant}]",
                traces[False, variant, 0], traces[True, variant, 0],
                stdout[True, variant, 0])
        # Every class the script aims at really occurred: replaced attaches,
        # locally answered status queries, and an attach forwarded by a
        # draw, a clear, a read and a present (run_defer holds one into the
        # present on frames 1, 5, ... of its 48: 12 of them).
        # ... and class OTHER twice on each of the 8 frames of section f:
        # the attach of another framebuffer, and glGetIntegerv.
        require(r["dropped"] > 0 and r["local"] > 0 and
                all(r["flushed"][i] > 0 for i in (0, 1, 3)) and
                r["flushed"][4] == 12 and r["flushed"][6] == 16,
                f"scenario 14 [{flavour}]: thin coverage {r}")
        native_reads = sum(1 for l in traces[False, "direct", 0]
                           if l.startswith("readPixels "))
        print(f"fbo deferred attach scenario 14 [{flavour}]: PASS "
              f"(direct = packed = worker x poison 0/1, OFF and ON; "
              f"{r['calls']} native calls, attaches {r['attaches']} -> "
              f"{r['attaches'] - r['dropped']}, status-local {r['local']}, "
              f"flush dr,cl,ow,rd,pr,tx,ot="
              f"{','.join(map(str, r['flushed']))}, native reads "
              f"{native_reads}; {SCN_BINDS} Binds = {SCN_BINDS + SCN_COLD} "
              f"bind packets, {SCN_COLD} cold; 2 x {min(batches)} attribute "
              f"batches)")
    # Mutants.  Each must fail the comparison it is built for.
    off = (work / "trace-scn-plain-record-off-p0.txt").read_text(
        encoding="ascii").splitlines()
    for variant, mutant in (("record", "packet-op-flushes"),
                            ("worker", "packet-op-flushes"),
                            ("record", "no-draw-flush"),
                            ("record", "no-getint-flush"),
                            ("record", "cross-fb-drop")):
        exe = scenario_build(env, compiler, work, generated, "plain", variant,
                             True, mutant)
        lines, out, rc = scenario_run(work, exe, 0)
        require(rc == 0, f"scenario mutant {mutant} did not run: rc={rc}")
        try:
            scenario_compare(f"plain {variant}", off, lines, out)
        except AssertionError as error:
            first = " | ".join(s.strip() for s in str(error).splitlines())
            print(f"fbo deferred attach scenario mutant [plain {variant} "
                  f"{mutant}]: rejected ({first})")
        else:
            raise AssertionError(f"scenario mutant {variant} {mutant} passed")
    for on, script_mutant in ((False, "late-packet"), (True, "late-packet"),
                              (False, "cold-no-flush"),
                              (True, "cold-no-flush")):
        exe = scenario_build(env, compiler, work, generated, "plain", "record",
                             on, script_mutant)
        lines, out, rc = scenario_run(work, exe, 0)
        name = f"plain record {'on' if on else 'off'} {script_mutant}"
        reference = (work / f"trace-scn-plain-direct-{'on' if on else 'off'}"
                     "-p0.txt").read_text(encoding="ascii").splitlines()
        require(rc != 0 or lines != reference,
                f"scenario mutant {name} passed")
        first = next((f"line {i + 1}: direct '{a}' vs '{b}'"
                      for i, (a, b) in enumerate(zip(reference, lines))
                      if a != b), f"rc={rc}")
        print(f"fbo deferred attach scenario mutant [{name}]: rejected "
              f"({'rc=' + str(rc) if rc else first})")


# ---- 3. render-target lifecycle model ----------------------------------------------

def load_sim():
    path = HERE / "host_tests" / "a01_rt_reuse_sim.py"
    spec = importlib.util.spec_from_file_location("a01_rt_reuse_sim", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def check_model() -> None:
    sim = load_sim()

    class Deferred(sim.VitaGL):
        """The knob's rule on the model's three events: attach is held,
        forwarded before a draw and before a swap, replaced by a later
        attach.  (sim.VitaGL(lazy=True) forwards only before a draw.)"""

        def __init__(self):
            super().__init__(False)
            self.held = None
            self.dropped = 0

        def attach(self, size):
            self.c["guest_att"] += 1
            if self.held is not None:
                self.dropped += 1
            self.held = size

        def flush(self):
            if self.held is not None:
                self._attach(self.held)
                self.held = None

        def draw(self):
            self.flush()
            super().draw()

        def swap(self):
            self.flush()
            super().swap()

    def rc(gl):
        return gl.c["rcA"] + gl.c["rcB"] + gl.c["rcO"]

    # S1: dry steady state, 100 presents, one read per 10 (FABLE main()).
    results = []
    for make in (lambda: sim.VitaGL(False), Deferred):
        gl = make()
        sim.dry(gl, 200, 10)
        gl.reset()
        sim.dry(gl, 100, 10)
        results.append((rc(gl), gl.c["att"], gl.c["rd"], gl.c["gd"]))
    require(results[0] == (10, 510, 10, 10),
            f"S1 OFF model drifted from FABLE's figures: {results[0]}")
    require(results[1][0] == 0 and results[1][2] == 0 and results[1][3] == 0,
            f"S1 ON: {results[1]}")
    print(f"fbo deferred attach model S1 (100 presents, 10 reads): "
          f"rc {results[0][0]} -> {results[1][0]}, native attaches "
          f"{results[0][1]} -> {results[1][1]}, depth retires "
          f"{results[0][2]} -> {results[1][2]}, rt destroys "
          f"{results[0][3]} -> {results[1][3]}")

    # S4: the read storm after N presents without a size switch, 20 presents.
    worst_off = worst_on = 0
    rows = 0
    for idle in (3, 4, 7, 8):
        for tog, push in ((3, 0), (0, 3), (2, 2), (1, 2)):
            pair = []
            for make in (lambda: sim.VitaGL(False), Deferred):
                gl = make()
                sim.wet(gl, 200, idle_swaps_at=(199, idle))
                gl.reset()
                sim.wet(gl, 20, toggles=tog, pushes=push)
                pair.append(rc(gl))
            require(pair[1] <= 1,
                    f"S4 idle={idle} toggles={tog} pushes={push}: ON rc {pair[1]}")
            require(pair[1] <= pair[0], "ON must not create more targets")
            worst_off = max(worst_off, pair[0])
            worst_on = max(worst_on, pair[1])
            rows += 1
    require(worst_off >= 20, f"S4 OFF no longer reproduces the storm: {worst_off}")
    print(f"fbo deferred attach model S4 ({rows} storms of 20 presents): "
          f"worst rc {worst_off} -> {worst_on} (gate <= 1 per 20 presents)")
    print("fbo deferred attach model: PASS")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--base", default=DEFAULT_BASE,
                        help="revision whose runtime files the OFF surface "
                             "must equal (default: the lane base 22327fa)")
    parser.add_argument("--merged-with", default=DEFAULT_MERGED,
                        help="a file whose OFF surface is not --base must "
                             "equal the clean three-way merge of --base "
                             "into this revision (empty: refuse)")
    parser.add_argument("--keep", type=Path, default=None,
                        help="work directory to keep (traces, build logs)")
    args = parser.parse_args()
    check_sources(args.base, args.merged_with)
    check_model()
    if args.keep:
        args.keep.mkdir(parents=True, exist_ok=True)
        for child in args.keep.iterdir():
            require(child.name.startswith(("fbo-defer-", "obj-", "build-",
                                           "trace-", "mutant-")),
                    f"--keep directory holds foreign files: {child.name}")
        import shutil
        for child in args.keep.iterdir():
            shutil.rmtree(child) if child.is_dir() else child.unlink()
        check_oracle(args.keep)
        check_record(args.keep)
    else:
        with tempfile.TemporaryDirectory(prefix="fbo-defer-") as tmp:
            check_oracle(Path(tmp))
            check_record(Path(tmp))
    print("fbo deferred attach: PASS")


if __name__ == "__main__":
    main()
