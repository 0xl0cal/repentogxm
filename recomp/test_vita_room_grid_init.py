#!/usr/bin/env python3
"""Fresh full-constructor differential test for the exact grid init prefix.

The opaque constructor children are deterministic models; this is a seam/ABI
test of the actual regenerated owner, not a replacement model of that owner.
"""
from __future__ import annotations

import argparse
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
PE_SHA = "31846486979cfa07c8c968221553052c3ff603518681ca11912d445f96ca9404"
BODY_SHA = "c572ee5def8a685d34973ac3812f4da70ccc678dbc7cb13898858559561a5371"


def require(value, message):
    if not value:
        raise AssertionError(message)


def run(argv):
    p = subprocess.run([str(v) for v in argv], capture_output=True, text=True)
    require(p.returncode == 0, f"{argv!r}\n{p.stdout}\n{p.stderr}")
    return p.stdout.strip()


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--pe", type=Path, required=True)
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--cc")
    ap.add_argument("--rezero-real-children", action="store_true",
                    help="Test repeated-zero elision with fresh actual reserve/tree bodies")
    ap.add_argument("--native-prefix-off", action="store_true",
                    help="With --rezero-real-children, retain the original first loop")
    ap.add_argument("--emit-only", action="store_true",
                    help="Emit fresh verified owner/children for an early target-code gate")
    a = ap.parse_args()
    require(not a.native_prefix_off or a.rezero_real_children,
            "--native-prefix-off requires --rezero-real-children")
    require(hashlib.sha256(a.pe.read_bytes()).hexdigest() == PE_SHA,
            "requires exact frozen PE")
    cc = a.cc or shutil.which("clang") or shutil.which("gcc")
    if not cc and os.name == "nt":
        path = Path(os.environ.get("ProgramFiles", "C:/Program Files")) / "LLVM/bin/clang.exe"
        if path.is_file():
            cc = str(path)
    require(cc, "pass installed GCC-compatible compiler via --cc")
    a.out.mkdir(parents=True, exist_ok=True)
    os.environ["REPENTOGXM_PE"] = str(a.pe.resolve())
    sys.path.insert(0, str(HERE))
    import gen_all
    from image import Image, DEFAULT_BASE
    pin = Image(str(a.pe), DEFAULT_BASE)
    raw = Image(str(a.pe), pin.orig_base)
    require(hashlib.sha256(raw.code_at(0x2C4260, 0x2E9)).hexdigest() == BODY_SHA,
            "frozen constructor body changed")
    selector = gen_all.vita_room_grid_init_for_body
    rezero_selector = gen_all.vita_room_grid_rezero_for_body
    captured = []

    def capture(*args):
        captured.append(args)
        return selector(*args)

    gen_all.vita_room_grid_init_for_body = capture
    gen_all._translate_function(pin, {"rva": 0x2C4260}, None, {}, pin_img=pin)
    gen_all.vita_room_grid_init_for_body = selector
    shape = captured[0]
    require(selector(pin, 0x2C4550, None, None, None, ()) is False,
            "seam escaped exact owner")
    for rva in (0x2C4260, 0x2C4314, 0x2C4330, 0x2C4359, 0x2C436A, 0x2C4548):
        class ChangedImage:
            def __init__(self, *unused):
                pass

            def code_at(self, start, count):
                b = bytearray(raw.code_at(start, count))
                if start <= rva < start + count:
                    b[rva - start] ^= 1
                return bytes(b)
        original_image = gen_all.Image
        gen_all.Image = ChangedImage
        try:
            try:
                selector(*shape)
            except RuntimeError:
                pass
            else:
                raise AssertionError(f"accepted mutated frozen byte {rva:08x}")
        finally:
            gen_all.Image = original_image
    try:
        selector(*shape[:-1], (0x2C4330,))
    except RuntimeError:
        pass
    else:
        raise AssertionError("accepted extra/interior entry")
    if a.rezero_real_children:
        require(rezero_selector(*shape), "rezero selector rejected frozen constructor")
        require(not rezero_selector(pin, 0x2C4550, None, None, None, ()),
                "rezero selector escaped exact owner")
        for start, end, unused in gen_all.VITA_ROOM_GRID_REZERO_PINS:
            for changed_rva in (start, end - 1):
                class ChangedCalleeImage:
                    def __init__(self, *unused):
                        pass

                    def code_at(self, rva, count):
                        b = bytearray(raw.code_at(rva, count))
                        if rva <= changed_rva < rva + count:
                            b[changed_rva - rva] ^= 1
                        return bytes(b)
                original_image = gen_all.Image
                gen_all.Image = ChangedCalleeImage
                try:
                    try:
                        rezero_selector(*shape)
                    except RuntimeError:
                        pass
                    else:
                        raise AssertionError(f"accepted rezero pin mutation {changed_rva:08x}")
                finally:
                    gen_all.Image = original_image
        try:
            rezero_selector(*shape[:-1], (0x2C4510,))
        except RuntimeError:
            pass
        else:
            raise AssertionError("accepted rezero interior entry")
    results = {"pe_sha256": PE_SHA, "raw_body_sha256": BODY_SHA,
               "compiler": run([cc, "--version"]), "variants": [],
               "rezero_real_children": a.rezero_real_children,
               "native_prefix_off": a.native_prefix_off}
    for base in (DEFAULT_BASE, 0x98000000):
        image = Image(str(a.pe), base)
        fast = gen_all._translate_function(image, {"rva": 0x2C4260}, None, {}, pin_img=pin)
        gen_all.vita_room_grid_init_for_body = lambda *unused: False
        gen_all.vita_room_grid_rezero_for_body = lambda *unused: False
        try:
            original = gen_all._translate_function(image, {"rva": 0x2C4260}, None, {}, pin_img=pin)
        finally:
            gen_all.vita_room_grid_init_for_body = selector
            gen_all.vita_room_grid_rezero_for_body = rezero_selector
        require(fast["stub"] is None and original["stub"] is None, "owner became stub")
        from gpr_locals import legacy_text
        require(len(legacy_text(fast["text"])) + fast["legacy_size_delta"] ==
                len(legacy_text(original["text"])) + original["legacy_size_delta"],
                "new seam changes legacy unit packing")
        fence_pattern = (r"#if defined\(__vita__\) && defined\(ISAAC_VITA_ROOM_GRID_(?:INIT_NATIVE|REZERO_ELISION)\)"
                         r"[^\n]*\n.*?#endif\n")
        disabled = re.sub(fence_pattern, "", fast["text"], flags=re.S)
        require(disabled == original["text"], "OFF body is not byte-identical original")
        require(fast["text"].count("isaac_vita_room_grid_init_try(c,") == 1 and
                fast["text"].index("isaac_vita_room_grid_init_try(c,") <
                fast["text"].index("L_002c4330:"), "seam executes on backedge")
        prototypes = "\n".join(f"void sub_{rva:08x}(CPU *__restrict c);"
                               for rva in (0x5EACF8, 0x2DA150, 0xC36D0, 0x20480,
                                           0xC3C30, 0xC3B30, 0xC0300, 0x5EB08E, 0x1F50))
        reference = original["text"].replace("void sub_002c4260(",
                                            "void grid_original_constructor(")
        prefix = '#include "guest.h"\n#define __vita__ 1\n' + prototypes + "\n"
        if a.rezero_real_children:
            prefix += ("void room_grid_rezero_oracle_st32(uint32_t, uint32_t);\n"
                       "#if defined(ROOM_GRID_REZERO_ORACLE_COUNT_STORES)\n"
                       "#define st32(a, v) room_grid_rezero_oracle_st32((a), (v))\n"
                       "#endif\n")
        prefix_control = ("#undef ISAAC_VITA_ROOM_GRID_INIT_NATIVE\n"
                          "#define ISAAC_VITA_ROOM_GRID_INIT_NATIVE 0\n"
                          if a.native_prefix_off else "")
        children = ""
        if a.rezero_real_children:
            require(fast["text"].count("GR(ecx) = 0x37e0U;") == 1 and
                    fast["text"].index("GR(ecx) = 0x37e0U;") <
                    fast["text"].index("L_002c4510:"), "rezero seam must precede backedge label")
            for child in (0x2DA150, 0xC3C30, 0xC3B30, 0x20480):
                body = gen_all._translate_function(image, {"rva": child}, None, {}, pin_img=pin)
                require(body["stub"] is None, f"actual child {child:08x} became stub")
                children += "\n" + body["text"]
        path = a.out / f"grid_generated_{base:08x}.c"
        path.write_text(prefix + reference + "\n" + prefix_control + fast["text"] + children, encoding="utf-8")
        (a.out / f"grid_owner_{base:08x}.c").write_text(prefix + prefix_control + fast["text"], encoding="utf-8")
        (a.out / f"grid_original_{base:08x}.c").write_text(prefix + original["text"], encoding="utf-8")
        if a.emit_only:
            continue
        for flags, gpr in ((0, 0), (1, 0), (0, 1), (1, 1)):
            for checked in (0, 1):
                exe = a.out / f"grid_{base:08x}_{flags}{gpr}_{checked}.exe"
                command = [cc, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                           "-fno-strict-aliasing", "-DGUEST_STACK_REQUIRED=1",
                           "-DGUEST_GENERATED_STACK_GUARD=1",
                           "-DISAAC_VITA_ROOM_GRID_INIT_NATIVE=1",
                           "-DISAAC_VITA_HEAP_RANGE_LEASE=1",
                           f"-DGUEST_IMAGE_BASE=0x{base:08x}U",
                           f"-DGUEST_FLAGS_LOCAL={flags}", f"-DGUEST_GPR_LOCAL={gpr}"]
                if "clang" in Path(cc).name:
                    command.append("-Wno-parentheses-equality")
                if checked:
                    command.append("-DGUEST_CHECKED_MEMORY=1")
                if a.rezero_real_children:
                    command += ["-DISAAC_VITA_ROOM_GRID_REZERO_ELISION=1",
                                "-DROOM_GRID_REZERO_ORACLE_REAL_CHILDREN=1",
                                "-DROOM_GRID_REZERO_ORACLE_COUNT_STORES=1"]
                if a.native_prefix_off:
                    command.append("-DROOM_GRID_REZERO_ORACLE_PREFIX_OFF=1")
                command += ["-I", RUNTIME, path, RUNTIME / "host_vita_room_grid_init.c",
                            RUNTIME / "host_vita_room_grid_init_oracle.c", "-o", exe]
                run(command)
                output = run([exe])
                require(output.startswith("Room grid init oracle: PASS;"), output)
                record = {"base": f"{base:08x}", "flags_local": flags, "gpr_local": gpr,
                          "checked": checked, "result": output,
                          "command": [str(v) for v in command]}
                results["variants"].append(record)
                print(f"base={base:08x} flags={flags} gpr={gpr} checked={checked}: {output}", flush=True)
    results["sources"] = {str(p.relative_to(HERE)): hashlib.sha256(p.read_bytes()).hexdigest()
                          for p in (Path(__file__), RUNTIME / "host_vita_room_grid_init.c",
                                    RUNTIME / "host_vita_room_grid_init.h",
                                    RUNTIME / "host_vita_room_grid_init_oracle.c",
                                    HERE / "gen_all.py", RUNTIME / "guest.h")}
    (a.out / "results.json").write_text(json.dumps(results, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
