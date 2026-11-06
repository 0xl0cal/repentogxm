#!/usr/bin/env python3
"""GUEST_GPR_FLUSH_MASK (recomp/gpr_flush_mask.py): analysis unit checks, an
execution oracle and in-test mutations.

1. Analysis: hand-written bodies with known dirty sets -- straight line,
   branch union, loop back edge, backward goto, leaf-inline dual spelling,
   switch, host seam (HOST forces a full FLUSH until a RELOAD), refusals.
2. Execution: the cases of test_gpr_locals_semantics.py plus mask-specific
   ones (a register written on one branch only, a call inside a loop whose
   body writes after the call, partial-register and implicit writes before a
   call, two calls with no write between them, the C stub) are rendered once
   through the real decoder/emitter and built three ways:
     mode 0  registers in memory
     mode 1  registers in locals, every FLUSH publishes all eight
     mode 2  mode 1 + the masked text + GUEST_GPR_FLUSH_MASKED=1
   All three must print the same registers, records and guest-memory hash.
   Two hand-written bodies with a host helper that writes CPU fields behind
   the locals compare modes 1 and 2 (memory mode legitimately differs there).
3. Mutations (the oracle must catch each): every mask forced to 0x00; the
   HOST rule disabled; ebx writes ignored by the analysis.

usage: test_gpr_flush_mask.py [--cc gcc] [--pe isaac-ng.exe.unpacked.exe]
Without --cc only the analysis checks run.  Needs Linux x86-64 for --cc.
"""
from __future__ import annotations

import argparse
import os
import pathlib
import subprocess
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, os.fspath(HERE))

import gpr_flush_mask as M                                   # noqa: E402

FAILURES = []


def check(cond, what):
    if not cond:
        FAILURES.append(what)
        print("FAIL:", what)


def body(*lines):
    return "".join("    %s\n" % line for line in ("GUEST_FLAGS_DECL;", "GUEST_GPR_DECL;") + lines)


def masks_of(text):
    """Masks of every FLUSH in textual order (None = kept full)."""
    out, pos = [], 0
    masks = M.analyse_body(text)
    import re
    for m in re.finditer(r"\bGUEST_GPR_FLUSH\(c\);", text):
        mask = masks.get(m.start())
        out.append(None if mask is None or mask & M.HOST or mask == M.FULL else mask)
    return out


def analysis_checks():
    B = M.BIT
    # straight line: eax written, then a call boundary, then esi
    t = body("GR(eax) = 1U;",
             "GPUSH(0x10U); GUEST_STACK_CALLSITE_BARRIER(); GUEST_GPR_FLUSH(c); sub_00001200(c); GUEST_GPR_RELOAD(c);",
             "GR(esi) = GR(eax);",
             "(void)GPOP();", "GUEST_STACK_CALLSITE_BARRIER();", "GUEST_GPR_FLUSH(c); return;")
    check(masks_of(t) == [B["eax"] | B["esp"], B["esi"] | B["esp"]], "straight line %r" % masks_of(t))
    # branch: ebx only on one path
    t = body("if (((GR(ecx)) == (0U))) goto L_1;", "GR(ebx) = 5U;", "L_1:",
             "GUEST_GPR_FLUSH(c); return;")
    check(masks_of(t) == [B["ebx"]], "branch union %r" % masks_of(t))
    # loop: edi written after the call reaches the call's FLUSH via the back edge
    t = body("L_5:", "GR(esi) += 1U;",
             "GUEST_GPR_FLUSH(c); sub_00001200(c); GUEST_GPR_RELOAD(c);",
             "GR(edi) = GR(eax);", "GR(ecx) = GR(ecx) - 1U;",
             "if (GR(ecx) != 0U) goto L_5;", "GUEST_GPR_FLUSH(c); return;")
    check(masks_of(t) == [B["esi"] | B["edi"] | B["ecx"], B["edi"] | B["ecx"]],
          "loop back edge %r" % masks_of(t))
    # leaf dual spelling: the inline copy's writes survive the #else RELOAD
    t = body("GPUSH(0x20U); GUEST_STACK_CALLSITE_BARRIER();",
             "#if GUEST_LEAF_INLINE",
             "{ GR(eax) = (uint32_t)(GR(ecx)); (void)GPOP(); GUEST_STACK_CALLSITE_BARRIER(); }",
             "#else",
             "GUEST_GPR_FLUSH(c); sub_00001f30(c); GUEST_GPR_RELOAD(c);",
             "#endif",
             "GUEST_GPR_FLUSH(c); return;")
    check(masks_of(t) == [B["esp"], B["eax"] | B["esp"]], "leaf dual spelling %r" % masks_of(t))
    # switch with default: cases join the entry state
    t = body("GR(edx) = 1U;",
             "{ uint32_t _target = GR(eax);", "switch (_target) {",
             "case 0x1U: { goto L_7; }", "default: GR(ebx) = 2U; break;", "} }",
             "L_7:", "GUEST_GPR_FLUSH(c); return;")
    check(masks_of(t) == [B["edx"] | B["ebx"]], "switch %r" % masks_of(t))
    # host seam: full FLUSH until a RELOAD, masked again after it
    t = body("GR(eax) = 1U;", "GUEST_GPR_FLUSH(c);", "seam_helper(c);",
             "GR(edx) = 5U;", "GUEST_GPR_FLUSH(c);", "GUEST_GPR_RELOAD(c);",
             "GR(edx) = 6U;", "GUEST_GPR_FLUSH(c); return;")
    check(masks_of(t) == [B["eax"], None, B["edx"]], "host seam %r" % masks_of(t))
    # a GPR field named directly is host too
    t = body("c->ecx = 3U;", "GR(edx) = 5U;", "GUEST_GPR_FLUSH(c); return;")
    check(masks_of(t) == [None], "c->ecx host %r" % masks_of(t))
    # any preprocessor branch is a possible path
    t = body("#if defined(SOMETHING)", "GR(ebp) = 1U;", "#elif defined(OTHER)",
             "GR(esi) = 1U;", "#endif", "GUEST_GPR_FLUSH(c); return;")
    check(masks_of(t) == [B["ebp"] | B["esi"]], "pp branches %r" % masks_of(t))
    # partial-register and compound writes count
    t = body("GR(ebx) = (GR(ebx) & ~0xffU) | 7U;", "GR(edi) += 4U;", "GR(ecx)--;",
             "GUEST_GPR_FLUSH(c); return;")
    check(masks_of(t) == [B["ebx"] | B["edi"] | B["ecx"]], "compound writes %r" % masks_of(t))
    # refusals: unpublished boundary, boundary not followed by RELOAD, alias
    for name, t in (
            ("unpublished boundary", body("GR(eax) = 1U;", "sub_00001200(c);", "GUEST_GPR_RELOAD(c);")),
            ("boundary without reload", body("GUEST_GPR_FLUSH(c);", "sub_00001200(c);", "GR(eax) = 1U;")),
            ("alias", body("CPU *d = c;", "GUEST_GPR_FLUSH(c); return;"))):
        try:
            M.analyse_body(t)
            check(False, "refusal expected: " + name)
        except M.Refuse:
            pass
    # the unit rewriter round-trips to the input
    unit = "void sub_00001000(CPU *__restrict c)\n{\n" + body(
        "GR(eax) = 1U;", "GUEST_GPR_FLUSH(c); return;") + "}\n"
    masked = M.mask_unit(unit)
    check("GUEST_GPR_FLUSH_MASK(c, 0x01U);" in masked, "unit rewrite")
    check(M.legacy_text(masked) == unit, "legacy round trip")


# --------------------------------------------------------------- execution ---
CALLEE = ("89d8"          # mov eax, ebx
          "01f0"          # add eax, esi
          "01d0"          # add eax, edx
          "01f8"          # add eax, edi
          "bb34120000"    # mov ebx, 0x1234   (breaks the convention: reload must see it)
          "46"            # inc esi
          "c3")
MASK_CASES = [
    ("mask_branch_taken",
     "85c9" "7405" "bb78563412" "e8{callee}" "8906" "895e04" "897604" "c3",
     CALLEE, {"ecx": "1", "esi": "BUF + 0x3000"}),
    ("mask_branch_skipped",
     "85c9" "7405" "bb78563412" "e8{callee}" "8906" "895e04" "c3",
     CALLEE, {"ecx": "0", "esi": "BUF + 0x3040"}),
    ("mask_loop_calls",
     "b903000000" "46" "e8{callee}" "01c7" "49" "75f5" "893e" "c3",
     CALLEE, {"esi": "BUF + 0x3100"}),
    ("mask_two_calls",
     "e8{callee}" "e8{callee}" "01d8" "8906" "c3",
     CALLEE, {"esi": "BUF + 0x3200"}),
    ("mask_partial_then_call",
     "b307" "e8{callee}" "8906" "c3",
     CALLEE, {"esi": "BUF + 0x3300"}),
    ("mask_implicit_then_call",
     "b800000080" "99" "ad" "e8{callee}" "8906" "c3",
     CALLEE, {"esi": "BUF + 0x3400"}),
    ("mask_c_stub",
     "6a05" "e8{stub}" "83c404" "8906" "895e04" "897e08" "c3",
     None, {"esi": "BUF + 0x3500"}),
]

HAND = r'''
/* Host helper writing CPU fields behind the locals (a seam's shape). */
static void hand_poke(CPU *__restrict c) { c->ecx = 0xabcdef01u; c->ebx ^= 1u; }
void hand_host(CPU *__restrict c)
{
    GUEST_FLAGS_DECL;
    GUEST_GPR_DECL;
    GR(eax) = GR(eax) + 1U;
    GUEST_GPR_FLUSH(c);
    hand_poke(c);
    GR(edx) = 5U;
    (void)GPOP();
    GUEST_GPR_FLUSH(c); return;
}
void hand_host_reload(CPU *__restrict c)
{
    GUEST_FLAGS_DECL;
    GUEST_GPR_DECL;
    GUEST_GPR_FLUSH(c);
    hand_poke(c);
    GUEST_GPR_RELOAD(c);
    GR(edx) += 1U;
    (void)GPOP();
    GUEST_GPR_FLUSH(c); return;
}
static void setup_hand(CPU *c) { (void)c; }
'''
HAND_RUNS = ('    puts("-- hand (locals vs masked only) --");\n'
             '    prime(0x99u);\n    run("hand_host", hand_host, 1, setup_hand);\n'
             '    prime(0x9au);\n    run("hand_host_reload", hand_host_reload, 1, setup_hand);\n')


def build_source(pe):
    import test_gpr_locals_semantics as S
    S.CASES = list(S.CASES) + MASK_CASES
    base_count = len(S.CASES) - len(MASK_CASES)
    cases = []
    for i, case in enumerate(S.CASES):
        fn_name, text, em = S.build_synthetic(i, *case)
        if i >= base_count:
            # one translated callee per case (the harness image has one slot)
            text = text.replace("sub_%08x" % S.CALLEE, "mask_callee_%d" % i)
        cases.append((fn_name, text, em))
    pe_functions = []
    if pe is not None:
        from image import Image
        img = Image(os.fspath(pe))
        calls = set()
        text, _ = S.render_function(img, S.RNG_NEXT, "sub_%08x" % S.RNG_NEXT, None, calls)
        pe_functions.append(("sub_%08x" % S.RNG_NEXT, S.logger_stub() + text))
    source = S.build_harness(cases, pe_functions)
    anchor = '    puts("gpr oracle: done");'
    assert source.count(anchor) == 1
    source = source.replace(anchor, HAND_RUNS + anchor)
    head_end = source.index("static uint32_t rng_state;")
    source = source[:head_end] + HAND + source[head_end:]
    return source, len(cases)


class RunFailed(Exception):
    pass


def compile_run(cc, work, name, source, defines, timeout=600):
    src = work / (name + ".c")
    src.write_text(source, encoding="ascii", newline="\n")
    binary = work / name
    cmd = [cc, "-std=gnu11", "-O2", "-Wall", "-Wextra", "-Wno-unused-variable",
           "-Wno-unused-but-set-variable", "-Wno-unused-label",
           "-Wno-unused-parameter", "-Wno-unused-function",
           "-DGUEST_GENERATED_STACK_GUARD=0", "-DGUEST_STACK_REQUIRED=0",
           *defines, "-I", os.fspath(HERE / "runtime"), os.fspath(src),
           "-o", os.fspath(binary), "-lm"]
    built = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if built.returncode:
        print(built.stdout)
        raise SystemExit("compile failed: " + name)
    ran = subprocess.run([os.fspath(binary)], stdout=subprocess.PIPE,
                         stderr=subprocess.STDOUT, text=True, timeout=timeout)
    if ran.returncode:
        if name == "mut":
            raise RunFailed(name)
        print(ran.stdout)
        raise SystemExit("run failed: " + name)
    return ran.stdout


def split(out):
    head, _, hand = out.partition("-- hand (locals vs masked only) --\n")
    return head, hand


def mask_source(source, stats=None):
    return M.mask_unit(source, stats, functions=r"\w+")


def execution(cc, pe):
    source, ncases = build_source(pe)
    stats = {}
    masked = mask_source(source, stats)
    partial = masked.count("GUEST_GPR_FLUSH_MASK(c, 0x")
    print("masked harness: %r" % {k: v for k, v in stats.items() if k != "reasons"})
    check(stats.get("refused", 0) == 0 or set(stats.get("reasons", {})) <= {
        "body does not open with GUEST_GPR_DECL"}, "harness bodies refused: %r" % stats.get("reasons"))
    check(partial >= 40, "too few masked FLUSHes in the harness: %d" % partial)
    check("GUEST_GPR_FLUSH(c); return;\n}" in masked.split("void hand_host(")[1][:400],
          "hand_host final FLUSH must stay full")
    with tempfile.TemporaryDirectory(prefix="isaac-flush-mask-") as raw:
        work = pathlib.Path(raw)
        out0 = compile_run(cc, work, "m0", source, ["-DGUEST_GPR_LOCAL=0", "-DGUEST_FLAGS_LOCAL=0"])
        out1 = compile_run(cc, work, "m1", source, ["-DGUEST_GPR_LOCAL=1", "-DGUEST_FLAGS_LOCAL=1"])
        out2 = compile_run(cc, work, "m2", masked, ["-DGUEST_GPR_LOCAL=1", "-DGUEST_FLAGS_LOCAL=1",
                                                    "-DGUEST_GPR_FLUSH_MASKED=1"])
        # the masked text with the header knob off is the unmasked program
        out3 = compile_run(cc, work, "m3", masked, ["-DGUEST_GPR_LOCAL=1", "-DGUEST_FLAGS_LOCAL=1"])
        print(out1, end="")
        h0, _ = split(out0)
        h1, k1 = split(out1)
        h2, k2 = split(out2)
        check(h0 == h1, "memory vs locals modes differ (pre-existing contract)")
        check(out1 == out2, "locals vs masked modes differ")
        check(out1 == out3, "masked text with GUEST_GPR_FLUSH_MASKED=0 differs")
        check(sum(1 for l in k1.splitlines() if l.startswith("hand_")) == 2, "hand runs missing")

        # mutations: each must change the output (a hang or crash counts too:
        # an unpublished loop counter can spin forever)
        def mutant(label, text):
            try:
                out = compile_run(cc, work, "mut", text, ["-DGUEST_GPR_LOCAL=1", "-DGUEST_FLAGS_LOCAL=1",
                                                          "-DGUEST_GPR_FLUSH_MASKED=1"], timeout=60)
            except (subprocess.TimeoutExpired, RunFailed):
                out = None
            caught = out != out1
            print("mutation %-28s %s" % (label, "caught" if caught else "SURVIVED"))
            check(caught, "mutation survived: " + label)

        import re
        mutant("all masks 0x00", re.sub(r"GUEST_GPR_FLUSH_MASK\(c, 0x[0-9a-f]{2}U\)",
                                        "GUEST_GPR_FLUSH_MASK(c, 0x00U)", masked))
        real_uses = M._cpu_uses
        M._cpu_uses = lambda toks: (real_uses(toks)[0], False)
        try:
            mutant("HOST rule disabled", mask_source(source))
        finally:
            M._cpu_uses = real_uses
        real_writes = M._writes
        M._writes = lambda toks: real_writes(toks) & ~M.BIT["ebx"]
        try:
            mutant("ebx writes ignored", mask_source(source))
        finally:
            M._writes = real_writes
    return ncases


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cc")
    parser.add_argument("--pe", type=pathlib.Path)
    args = parser.parse_args()
    analysis_checks()
    ncases = None
    if args.cc:
        ncases = execution(args.cc, args.pe)
    if FAILURES:
        print("gpr flush mask: %d failure(s)" % len(FAILURES))
        return 1
    print("gpr flush mask: analysis checks PASS%s" % (
        "; execution oracle %d cases + 2 hand bodies%s, 3 mutations caught: PASS"
        % (ncases, " + RNG::Next" if args.pe else "") if ncases else " (no --cc, not executed)"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
