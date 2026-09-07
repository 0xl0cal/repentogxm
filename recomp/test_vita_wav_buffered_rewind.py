#!/usr/bin/env python3
"""Regenerate the frozen Seek/Read/RIFF bodies and test the narrow WAV rewind.

The oracle executes those generated bodies. Reset/refill are a stateful block
source model, not a claim to execute every archive codec or native FILE API.
"""
from __future__ import annotations

import argparse
import collections
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import sys

HERE = Path(__file__).resolve().parent
RUNTIME = HERE / "runtime"
PE_SHA256 = "31846486979cfa07c8c968221553052c3ff603518681ca11912d445f96ca9404"
SPANS = (
    (0x59C160, 0x59C167, "38847314767bf203d742ddbd0a5b1f11fe227170034c80c53423b77f4ce768da"),
    (0x59C690, 0x59C73E, "91095e529bc24877fbf6c3d20a6e5a9a8931e80ba265e8cffda1e9f1755aba71"),
    (0x59C750, 0x59C812, "471cdaadfdf5e785c68739245fddd914d005d8925a7bbb2483e94005987f5982"),
    (0x5A3B30, 0x5A3C3E, "7a6e319dbcb25b3a3f5233abea2de8a8ba1ef01fefd56d8687257e0bd1e9728e"),
    (0x5BF4E0, 0x5BF60D, "6ca5abb747643ac6e16085be78ec389b74fc16f26018aa47af62bd18ee6b1cfe"),
)
ADMISSION_SPANS = SPANS + (
    (0x59C170, 0x59C26A, "d05ec40aa360d8b9896562906d285767974883cdfcfd258104f58b9fd0a919a9"),
    (0x59C270, 0x59C68C, "6253e8e1fb1f34e09994f3bcc9174d19ddfe57593168abe58fa6ef6f424b29b6"),
    (0x765128, 0x765148, "62cfc4a6e8905ba7f14ddba042e3866d7f2fc2c5a2992cd25ade8867a00904c9"),
)


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def run(command: list[str]) -> str:
    result = subprocess.run(command, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True, check=False)
    require(result.returncode == 0,
            f"command failed ({result.returncode}): {command!r}\n{result.stdout}")
    return result.stdout.strip()


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pe", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--cc")
    args = parser.parse_args()
    require(args.pe.stat().st_size == 8_650_240 and
            hashlib.sha256(args.pe.read_bytes()).hexdigest() == PE_SHA256,
            "test requires the exact frozen PE")
    cc = args.cc or shutil.which("clang") or shutil.which("gcc")
    if not cc and os.name == "nt":
        installed = Path(os.environ.get("ProgramFiles", r"C:\Program Files")) / "LLVM/bin/clang.exe"
        if installed.is_file():
            cc = str(installed)
    require(bool(cc), "pass --cc with a GCC-compatible host compiler")
    args.out.mkdir(parents=True, exist_ok=True)
    os.environ["REPENTOGXM_PE"] = str(args.pe.resolve())
    sys.path.insert(0, str(HERE))
    import gen_all
    from image import DEFAULT_BASE, Image
    import test_flags_local_corpus_contract as contract

    pin = Image(str(args.pe), DEFAULT_BASE)
    for start, end, expected in ADMISSION_SPANS:
        require(hashlib.sha256(pin.code_at(start, end - start)).hexdigest() == expected,
                f"frozen body changed at {start:08x}")
    require(gen_all.vita_wav_buffered_rewind_prologue(pin, 0x59C750) == (),
            "WAV seam escaped its exact Seek owner")
    for start, _, _ in ADMISSION_SPANS:
        if start == 0x59C160:
            continue
        class ChangedImage:
            def code_at(self, address, count):
                original = pin.code_at(address, count)
                return bytes((original[0] ^ 1,)) + original[1:] if address == start else original
        try:
            gen_all.vita_wav_buffered_rewind_prologue(ChangedImage(), 0x59C690)
        except RuntimeError:
            pass
        else:
            raise AssertionError(f"seam accepted changed bytes at {start:08x}")
    try:
        gen_all.vita_wav_buffered_rewind_prologue(pin, 0x59C690, (0x59C691,))
    except RuntimeError:
        pass
    else:
        raise AssertionError("seam accepted an interior entry")
    summaries = []
    for base in (DEFAULT_BASE, 0x98000000):
        image = Image(str(args.pe), base)
        bodies = []
        seam_body = ""
        # A fresh reference is generated EVERY run. It is compiled with only
        # the entry seam disabled (=0); no guest instruction is replaced.
        for start, _, _ in SPANS:
            if start == 0x5A3B30:  # authenticated caller, not exercised here
                continue
            generated = gen_all._translate_function(
                image, {"rva": start}, None, {}, pin_img=pin)
            require(generated["stub"] is None, f"body {start:08x} became a stub")
            body = generated["text"]
            if start == 0x59C690:
                require(body.count("isaac_vita_wav_buffered_rewind_try(c)") == 1 and
                        body.index("isaac_vita_wav_buffered_rewind_try(c)") < body.index("GUEST_FLAGS_DECL"),
                        "entry seam absent/duplicated or moved after prologue")
                require("!g_guest_coverage_functions" in body and "!g_guest_coverage_cases" in body,
                        "coverage fallback disappeared")
                prefix = body[:body.index("GUEST_FLAGS_DECL")]
                require("GUEST_GPR_FLUSH" not in prefix and "GUEST_GPR_RELOAD" not in prefix,
                        "entry seam used local registers before their declaration")
                match = contract.BODY_RE.search(body)
                require(match and match.group(1) == "sub_0059c690", "missing fresh Seek body")
                name, checked_body = match.groups()

                def contract_problems(test_name, test_body):
                    problems = []
                    stats = collections.Counter()
                    contract.check_flags(test_name, test_body, stats, problems)
                    contract.check_gpr(test_name, test_body, stats, problems)
                    return problems

                require(not contract_problems(name, checked_body), "fresh WAV contract failed")
                selector = gen_all.vita_wav_buffered_rewind_prologue
                gen_all.vita_wav_buffered_rewind_prologue = lambda *unused: ()
                try:
                    original = gen_all._translate_function(
                        image, {"rva": start}, None, {}, pin_img=pin)
                finally:
                    gen_all.vita_wav_buffered_rewind_prologue = selector
                original_body = contract.BODY_RE.search(original["text"]).group(2)
                require(contract.wav_buffered_rewind_entry_original(name, checked_body) ==
                        original_body and not contract_problems(name, original_body),
                        "WAV entry normalization differs from fresh original")
                mutations = (
                    ("isaac_vita_wav_buffered_rewind_try(c)", "another_helper(c)"),
                    ("!g_guest_coverage_cases", "1"),
                    ("&& ISAAC_VITA_WAV_BUFFERED_REWIND\n", "&& 1\n"),
                    ("    if (!g_guest_coverage_functions", "    observer(c);\n    if (!g_guest_coverage_functions"),
                    ("    if (!g_guest_coverage_functions", "L_incoming:\n    if (!g_guest_coverage_functions"),
                    ("isaac_vita_wav_buffered_rewind_try(c)) return;", "isaac_vita_wav_buffered_rewind_try(c));\n    return;"),
                    ("#endif\n    GUEST_FLAGS_DECL;", "#endif\n    observer(c);\n    GUEST_FLAGS_DECL;"),
                    ("    GUEST_GPR_DECL;\n", "    GUEST_GPR_DECL;\n    observer(c);\n"),
                )
                for old, new in mutations:
                    changed = checked_body.replace(old, new, 1)
                    require(changed != checked_body and contract_problems(name, changed),
                            "WAV contract accepted changed guard/helper/order/observer")
                for changed in ("L_incoming:\n" + checked_body,
                                "    GUEST_GPR_DECL;\n" + checked_body,
                                checked_body + "    isaac_vita_wav_buffered_rewind_try(c);\n"):
                    require(contract_problems(name, changed),
                            "WAV contract accepted incoming label/early locals/duplicate try")
                require(contract_problems("sub_0059c750", checked_body),
                        "WAV contract accepted another owner")
                summary = (f"base={base:08x} WAV entry corpus contract: PASS; "
                           "fresh native/original; 12 invalid owner/guard/order/observer cases refused")
                summaries.append(summary)
                print(summary, flush=True)
                seam_body = body
                definition = "void sub_0059c690(CPU *__restrict c)"
                require(body.count(definition) == 1, "Seek definition changed")
                body = body.replace(definition, "void wav_original_seek(CPU *__restrict c)")
            bodies.append(body)
        prototypes = "\n".join(
            f"void sub_{rva:08x}(CPU *__restrict c);"
            for rva in (0x59C160, 0x59C170, 0x59C270, 0x59C690,
                        0x59C750, 0x5BF4E0, 0x55E330, 0x5EACD7, 0x5EC14C))
        generated_path = args.out / f"wav_rewind_generated_{base:08x}.c"
        generated_path.write_text(
            '#include "guest.h"\n#undef ISAAC_VITA_WAV_BUFFERED_REWIND\n'
            '#define __vita__ 1\n#define ISAAC_VITA_WAV_BUFFERED_REWIND 0\n'
            + prototypes + "\n" + "\n".join(bodies) +
            '\n/* Enable only the real generated seam after host headers. */\n'
            '#undef ISAAC_VITA_WAV_BUFFERED_REWIND\n#define ISAAC_VITA_WAV_BUFFERED_REWIND 1\n' + seam_body,
            encoding="utf-8")
        for flags_local, gpr_local in ((0, 0), (1, 0), (0, 1), (1, 1)):
            binary = args.out / f"wav_rewind_{base:08x}_{flags_local}{gpr_local}.exe"
            generated_warning_flags = (["-Wno-parentheses-equality"]
                                       if "clang" in Path(str(cc)).name else [])
            run([
                str(cc), "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                *generated_warning_flags,
                "-DGUEST_STACK_REQUIRED=1", "-DGUEST_GENERATED_STACK_GUARD=1",
                "-DISAAC_VITA_WAV_BUFFERED_REWIND=1", "-DISAAC_VITA_HEAP_RANGE_LEASE=1",
                f"-DGUEST_IMAGE_BASE=0x{base:08x}U",
                f"-DGUEST_FLAGS_LOCAL={flags_local}", f"-DGUEST_GPR_LOCAL={gpr_local}",
                "-I", str(RUNTIME), str(generated_path),
                str(RUNTIME / "host_vita_wav_buffered_rewind.c"),
                str(RUNTIME / "host_vita_wav_buffered_rewind_oracle.c"),
                "-o", str(binary),
            ])
            output = run([str(binary)])
            require(output.startswith("WAV buffered rewind oracle: PASS;"), output)
            summary = f"base={base:08x} flags-local={flags_local} gpr-local={gpr_local}: {output}"
            summaries.append(summary)
            print(summary, flush=True)
    (args.out / "results.txt").write_text("\n".join(summaries) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
