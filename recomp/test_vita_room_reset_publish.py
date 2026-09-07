#!/usr/bin/env python3
"""Fresh whole-00520160 publication differential; opaque callees are models.

Reuse the installed host compiler and guest.h, not frozen generated fixtures.
The original is freshly emitted with only this selector disabled. Every run
also pins the input and checks fail-closed selection/render/contract behavior.
"""
from __future__ import annotations
import argparse
import collections
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

HERE = Path(__file__).resolve().parent
RUNTIME = HERE / "runtime"
ORACLE = HERE / "vita/host_tests/kage_vita_room_reset_publish.c"
PE_SHA = "31846486979cfa07c8c968221553052c3ff603518681ca11912d445f96ca9404"


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def run(argv):
    p = subprocess.run([str(x) for x in argv], text=True, capture_output=True)
    require(p.returncode == 0, f"exit={p.returncode} {argv!r}\n{p.stdout}\n{p.stderr}")
    return p.stdout.strip()


def refused(call, label):
    try:
        call()
    except (ValueError, RuntimeError):
        return
    raise AssertionError("accepted changed " + label)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--pe", type=Path, required=True)
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--cc")
    ap.add_argument("--emit-only", action="store_true")
    a = ap.parse_args()
    a.out.mkdir(parents=True, exist_ok=True)
    # Invalidate any previous PASS before input validation or generation.
    results = {"status": "in_progress", "variants": [], "generated": []}
    (a.out / "results.json").write_text(json.dumps(results, indent=2) + "\n", encoding="utf-8")
    require(hashlib.sha256(a.pe.read_bytes()).hexdigest() == PE_SHA, "wrong frozen PE")
    cc = a.cc or shutil.which("clang") or shutil.which("gcc")
    if not cc and os.name == "nt":
        cc = "C:/Program Files/LLVM/bin/clang.exe"
    require(cc and Path(cc).is_file(), "provide installed compiler via --cc")
    os.environ["REPENTOGXM_PE"] = str(a.pe.resolve())
    # Match the production emission switches. Compile-time variants below
    # exercise register/flag/stack/observer guards independently.
    for key in ("GUEST_SSE_LOWER", "GUEST_FLOOR_THUNK_FASTPATH", "GUEST_LEAF_INLINE"):
        os.environ[key] = "1"
    sys.path.insert(0, str(HERE))
    import gen_all
    from image import Image, DEFAULT_BASE
    from gpr_locals import legacy_text
    import test_flags_local_corpus_contract as contract
    pin = Image(str(a.pe), DEFAULT_BASE)
    require(len(pin.mem) == 0x85f000, "oracle image mapping size changed")
    raw = Image(str(a.pe), pin.orig_base)
    selector = gen_all.vita_room_reset_publish_for_body
    captured = []

    def capture(*args):
        if args[1] == 0x520160:
            captured.append(args)
        return selector(*args)

    gen_all.vita_room_reset_publish_for_body = capture
    gen_all._translate_function(pin, {"rva": 0x520160}, None, {}, pin_img=pin)
    gen_all.vita_room_reset_publish_for_body = selector
    require(len(captured) == 1, "missing selector receipt")
    shape = captured[0]
    require(not selector(pin, 0x520ac0, None, None, None, ()), "owner escaped")
    for rva in (0x520160, 0x52037a, 0x520385, 0x52038d, 0x52039e,
                0x5203a8, 0x5203b3, 0x5203bb, 0x5203f2, 0x520ab8):
        class ChangedImage:
            def __init__(self, *unused):
                pass

            def code_at(self, start, size):
                b = bytearray(raw.code_at(start, size))
                if start <= rva < start + size:
                    b[rva - start] ^= 1
                return bytes(b)
        original_image = gen_all.Image
        gen_all.Image = ChangedImage
        try:
            refused(lambda: selector(*shape), f"frozen byte {rva:08x}")
        finally:
            gen_all.Image = original_image
    refused(lambda: selector(*shape[:-1], (0x52038a,)), "extra entry")
    shape[2].labels.add(0x52038a)
    try:
        refused(lambda: selector(*shape), "interior label")
    finally:
        shape[2].labels.remove(0x52038a)
    results.update(pe_sha256=PE_SHA, compiler=run([cc, "--version"]))
    for base in (DEFAULT_BASE, 0x98000000):
        image = Image(str(a.pe), base)
        fast = gen_all._translate_function(image, {"rva": 0x520160}, None, {}, pin_img=pin)
        gen_all.vita_room_reset_publish_for_body = lambda *unused: False
        try:
            original = gen_all._translate_function(image, {"rva": 0x520160}, None, {}, pin_img=pin)
        finally:
            gen_all.vita_room_reset_publish_for_body = selector
        require(fast["stub"] is None and original["stub"] is None, "stub owner")
        require(len(legacy_text(fast["text"])) + fast["legacy_size_delta"] ==
                len(legacy_text(original["text"])) + original["legacy_size_delta"],
                "legacy unit packing changed")
        require(contract.room_reset_publication_original("sub_00520160", fast["text"]) ==
                original["text"], "OFF body is not exact original")
        # Exercise the actual _main accounting path, not just owner emission:
        # both preprocessor arms are present lexically, but represent six
        # original pushes and six explicitly authenticated CPU publications.
        accounting = gen_all.guest_stack_codegen_accounting
        census = accounting(0x520160, fast["text"])
        require(census == accounting(0x520160, original["text"]) and
                census[1:] == (0, 0), "full-generation stack accounting changed")
        require(gen_all.guest_stack_accounting_text(0x520160, fast["text"]) ==
                original["text"], "stack normalizer differs from independent contract")
        raw_pushes = sum(legacy_text(fast["text"]).count(needle)
                         for needle in ("gpush_generated(c,", "GPUSH("))
        require(raw_pushes == census[0]["push_generated"] + 6 and
                len(gen_all.GUEST_STACK_DIRECT_WRITER_RE.findall(fast["text"])) == 6,
                "failed full-generation regression shape is not exercised")
        refused(lambda: accounting(0x520ac0, fast["text"]), "stack census owner")
        for site in (0x520385, 0x52038d, 0x52039e, 0x5203a8, 0x5203b3, 0x5203bb):
            start = fast["text"].index(f"    /* ROOM_RESET_DIRTY_PUBLISH site {site:08x}:")
            stop = fast["text"].index("#else", start)
            block = fast["text"][start:stop]
            require("c->esp = GR(esp);" in block, "missing ESP publication")
            for replacement in ("c->esp = GR(eax);",
                                "c->esp = GR(esp); c->esp += 4U;"):
                changed = fast["text"][:start] + block.replace(
                    "c->esp = GR(esp);", replacement, 1) + fast["text"][stop:]
                refused(lambda: accounting(0x520160, changed), "stack census mask/write")
        # An unrelated write outside the pinned span is NOT normalized away.
        # The canonical _main gate rejects the returned nonzero raw count.
        for text in (fast["text"], original["text"]):
            changed = text.replace("    /* 00520160 ",
                                   "    c->esp += 4U;\n    /* 00520160 ", 1)
            require(changed != text and accounting(0x520160, changed)[1:] == (1, 0),
                    "extra raw ESP write escaped full-generation gate")
        require(accounting(0x520ac0, "c->esp = 0U;\ngpush(c, 1U);\n")[1:] == (1, 1),
                "generic raw-writer/legacy-stack prohibition changed")
        results.setdefault("stack_accounting", []).append({
            "base": f"{base:08x}", "counts": census[0],
            "direct_writers": census[1], "legacy_calls": census[2],
            "lexical_extra_pushes": 6, "lexical_partial_writers": 6,
            "mask_or_extra_write_refusals": 12, "external_writes_detected": 2})
        problems = []
        body = contract.BODY_RE.search(fast["text"]).group(2)
        contract.check_gpr("sub_00520160", body, collections.Counter(), problems)
        require(not problems, "\n".join(problems))
        for old, new in (("c->esp = GR(esp);", "c->esp = GR(eax);"),
                         ("GUEST_GPR_LOCAL &&", "1 &&"),
                         ("sub_003063a0(c); GUEST_GPR_RELOAD(c);",
                          "sub_003063a0(c);"),
                         ("    /* 0052038a ", "    guest_fault(c, 0, 0);\n    /* 0052038a ")):
            refused(lambda: contract.room_reset_publication_original(
                "sub_00520160", fast["text"].replace(old, new, 1)), "contract mutation")
            refused(lambda: accounting(0x520160, fast["text"].replace(old, new, 1)),
                    "stack accounting mutation")
        for old, new in (("GR(ecx) = (uint32_t)(GR(eax));", "GR(ecx) = 0U;"),
                         ("    /* 0052038a ", "L_unexpected:\n    /* 0052038a "),
                         ("sub_002c4260(c); GUEST_GPR_RELOAD(c);",
                          "sub_002c4260(c);")):
            refused(lambda: gen_all.vita_room_reset_publish_render(
                original["text"].replace(old, new, 1)), "rendered span")
        callees = sorted(set(re.findall(r"\bsub_([0-9a-f]{8})\(c\)", original["text"])))
        prototypes = "\n".join(f"void sub_{x}(CPU *__restrict c);" for x in callees)
        prefix = '#include "guest.h"\n' + prototypes + "\n"
        # Defining __vita__ after guest.h enables the narrow generated seam
        # without making a host test include Vita platform headers.
        prefix += "#if ROOM_RESET_TEST_VITA\n#define __vita__ 1\n#endif\n"
        paths = {}
        for kind, text in (("owner", fast["text"]), ("original", original["text"])):
            path = a.out / f"reset_{kind}_{base:08x}.c"
            path.write_text(prefix + text, encoding="utf-8")
            paths[kind] = path
        pair = a.out / f"reset_pair_{base:08x}.c"
        trace_prefix = ""
        for name, kind in (("st8", "uint8_t"), ("st16", "uint16_t"), ("st32", "uint32_t"),
                           ("st64", "uint64_t"), ("stf", "float"), ("std_", "double"),
                           ("stx", "xmm_t")):
            trace_prefix += f"void reset_{name}(uint32_t, {kind});\n#define {name} reset_{name}\n"
        for name, kind in (("ld8", "uint8_t"), ("ld16", "uint16_t"), ("ld32", "uint32_t"),
                           ("ld64", "uint64_t"), ("ldf", "float"), ("ldd", "double"),
                           ("ldx", "xmm_t")):
            trace_prefix += f"{kind} reset_{name}(uint32_t);\n#define {name} reset_{name}\n"
        pair.write_text(prefix + trace_prefix + original["text"].replace("void sub_00520160(",
                        "void room_reset_original(") + fast["text"], encoding="utf-8")
        results["generated"].append({"base": f"{base:08x}", "calls": callees,
            "paths": {k: str(v.resolve()) for k, v in paths.items()},
            "sha256": {k: hashlib.sha256(v.read_bytes()).hexdigest() for k, v in paths.items()}})
        if a.emit_only:
            continue
        for flags in (0, 1):
            # ON supported, explicit OFF, then each independent refusal mode.
            for feature, gpr, guard, checked, vita in (
                    (1, 1, 0, 0, 1), (0, 1, 0, 0, 1), (1, 0, 0, 0, 1),
                    (1, 1, 1, 0, 1), (1, 1, 0, 1, 1), (1, 1, 0, 0, 0)):
                tag = f"{base:08x}_{flags}{feature}{gpr}{guard}{checked}{vita}"
                command = [cc, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                           "-fno-strict-aliasing", "-DGUEST_STACK_REQUIRED=1",
                           f"-DGUEST_IMAGE_BASE=0x{base:08x}U",
                           f"-DGUEST_FLAGS_LOCAL={flags}", f"-DGUEST_GPR_LOCAL={gpr}",
                           f"-DGUEST_GENERATED_STACK_GUARD={guard}",
                           f"-DISAAC_VITA_ROOM_RESET_DIRTY_PUBLISH={feature}",
                           f"-DROOM_RESET_TEST_VITA={vita}", "-I", RUNTIME]
                if "clang" in Path(cc).name:
                    command.append("-Wno-parentheses-equality")
                if checked:
                    command.append("-DGUEST_CHECKED_MEMORY=1")
                exe = a.out / f"reset_{tag}.exe"
                run(command + [pair, ORACLE, "-o", exe])
                output = run([exe])
                require(output.startswith("Room reset publication: PASS;"), output)
                record = {"base": f"{base:08x}", "flags": flags, "feature": feature,
                          "gpr": gpr, "guard": guard, "checked": checked, "vita": vita,
                          "result": output, "command": [str(v) for v in command]}
                if base == DEFAULT_BASE and not flags and (feature, gpr, guard, checked, vita) == (1, 1, 0, 0, 1):
                    bad_pair = a.out / "reset_deliberately_wrong.c"
                    bad_pair.write_text(pair.read_text().replace("c->ecx = GR(ecx); c->esp = GR(esp);",
                                        "c->ecx = GR(eax); c->esp = GR(esp);", 1), encoding="utf-8")
                    bad_exe = a.out / "reset_deliberately_wrong.exe"
                    run(command + [bad_pair, ORACLE, "-o", bad_exe])
                    bad = subprocess.run([str(bad_exe)], text=True, capture_output=True)
                    require(bad.returncode == 1 and "pass=1 call=4" in bad.stderr and
                            "memcmp(&event" in bad.stderr, "negative control missed wrong publication")
                    record["negative_control"] = bad.stderr.strip()
                if not (feature and gpr and not guard and not checked and vita):
                    assemblies = []
                    for kind in ("owner", "original"):
                        asm = a.out / f"reset_{tag}_{kind}.s"
                        run(command + ["-S", paths[kind], "-o", asm])
                        assemblies.append(re.sub(r'^\s*\.file[^\n]*\n', '', asm.read_text(), flags=re.M))
                    require(assemblies[0] == assemblies[1], f"unsupported/OFF assembly changed: {tag}")
                    record["off_assembly_identical"] = True
                results["variants"].append(record)
                (a.out / "results.json").write_text(json.dumps(results, indent=2) + "\n", encoding="utf-8")
                print(tag + ": " + output, flush=True)
    results["sources"] = {str(p.relative_to(HERE)): hashlib.sha256(p.read_bytes()).hexdigest()
                          for p in (Path(__file__), HERE / "gen_all.py", RUNTIME / "guest.h",
                                    ORACLE,
                                    HERE / "test_flags_local_corpus_contract.py") if p.exists()}
    results["status"] = "emit_only_complete" if a.emit_only else "pass"
    (a.out / "results.json").write_text(json.dumps(results, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
