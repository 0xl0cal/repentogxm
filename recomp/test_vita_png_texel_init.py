#!/usr/bin/env python3
"""Narrow no-padding PNG init comparison; regenerate the owner every run."""
from __future__ import annotations
import argparse
import hashlib
import os
from pathlib import Path
import struct
import subprocess
import sys
import zlib

HERE = Path(__file__).resolve().parent
BASE = "fa87783"
PE_SHA = "31846486979cfa07c8c968221553052c3ff603518681ca11912d445f96ca9404"

def run(command, *, data=None):
    p = subprocess.run(command, input=data, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if p.returncode:
        raise RuntimeError(f"failed {command!r}\n{p.stdout.decode(errors='replace')}")
    if p.stdout:
        print(p.stdout.decode(errors="replace"), end="")
    return p.stdout

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--pe", type=Path, required=True)
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--cc", default="C:/Program Files/LLVM/bin/clang.exe")
    args = ap.parse_args()
    if os.name == "nt":
        from setuptools.msvc import msvc14_get_vc_env
        os.environ.update({k.upper():v for k,v in msvc14_get_vc_env("x64").items()})
    assert hashlib.sha256(args.pe.read_bytes()).hexdigest() == PE_SHA
    os.environ["REPENTOGXM_PE"] = str(args.pe)
    sys.path.insert(0, str(HERE))
    import gen_all as G
    from image import Image, DEFAULT_BASE
    img, pin = Image(str(args.pe),0x98000000), Image(str(args.pe),DEFAULT_BASE)
    assert img.code_at(0x5a12de,9) == bytes.fromhex("576a0050e86bae0400")
    record = G._translate_function(img,{"rva":0x5a0e10},None,{},pin_img=pin)
    assert record["stub"] is None
    source = record["text"]
    args.out.mkdir(parents=True,exist_ok=True)
    (args.out/"image-png.fresh.c").write_text(source,encoding="utf-8")
    span = source[source.index("L_005a12de:"):source.index("    /* 005a13d0 ")]
    emitted = ("static void row_loop(CPU *c) {\nGUEST_FLAGS_DECL; GUEST_GPR_DECL;\n"+span+
        "GUEST_GPR_FLUSH(c);return;\nL_guest_setjmp_cleanup_005a0e10: GUEST_GPR_FLUSH(c);return;\n}\n")
    fixture = args.out/"png-texel-init-fixture.c"
    fixture.write_text((HERE/"vita/host_tests/kage_vita_png_texel_init.c").read_text().replace(
        "/* FRESH_ROW_LOOP */",emitted),encoding="utf-8")
    records=[]
    for w,h,pw,ph in ((8,8,8,8),(16,8,16,8),(24,40,24,40),(256,256,256,256),
                      (7,8,8,8),(8,7,8,8),(15,9,16,16),(256,257,256,264)):
        for gamma in (0,1):
            raw=bytearray(); expected=bytearray()
            for y in range(h):
                raw.append(0)
                for x in range(w):
                    rgba=bytes((x&255,(y*17+x)&255,(255-x)&255,(0,1,127,255)[(x+y)%4]))
                    raw.extend(rgba)
                    expected.extend(bytes(255-v for v in rgba[:3])+rgba[3:] if gamma else rgba)
            compressed=zlib.compress(raw,6)
            records.append(struct.pack("<6I",w,h,pw,ph,gamma,len(compressed))+compressed+expected)
    cases=args.out/"valid-idat-cases.bin"
    cases.write_bytes(struct.pack("<I",len(records))+b"".join(records))
    runtime=HERE/"runtime"
    baseline=subprocess.check_output(["git","show",f"{BASE}:recomp/runtime/host_vita_memory.c"],cwd=HERE)
    common=[args.cc,"-std=gnu11","-O2","-fno-strict-aliasing","-Wall","-Wextra","-Werror",
            "-Wno-unused-variable","-Wno-unused-label","-D_CRT_SECURE_NO_WARNINGS",
            "-DGUEST_IMAGE_BASE=0x98000000U","-I"+str(runtime),"-I"+str(HERE/"vita"),
            "-I"+str(HERE/"vita/anm2_scratch_oracle_include")]
    # Same stdin filename: comparison is not polluted by COFF file symbols.
    old_off=args.out/"memory-old-off.obj"; new_off=args.out/"memory-new-off.obj"
    run([*common,"-x","c","-","-c","-o",str(old_off)],data=baseline)
    run([*common,"-x","c","-","-c","-o",str(new_off)],data=(runtime/"host_vita_memory.c").read_bytes())
    before,after=bytearray(old_off.read_bytes()),bytearray(new_off.read_bytes())
    if os.name == "nt":
        # COFF TimeDateStamp, not machine code or source-level differences.
        before[4:8]=after[4:8]=bytes(4)
    assert before==after, "default-OFF memory object changed beyond COFF timestamp"
    print("default-OFF memory object identity excluding COFF timestamp PASS",hashlib.sha256(after).hexdigest())
    for gpr,flags,guard in ((0,0,0),(1,1,0),(1,1,1)):
        packing=[f"-DGUEST_GPR_LOCAL={gpr}",f"-DGUEST_FLAGS_LOCAL={flags}",f"-DGUEST_GENERATED_STACK_GUARD={guard}"]
        old=args.out/f"memory-original-{gpr}{flags}{guard}.obj"
        renames=["-Disaac_vita_memory_import=original_memory_import",
                 "-Disaac_vita_memory_import_name=original_memory_import_name",
                 "-Disaac_vita_memory_import_counted=original_memory_import_counted",
                 "-Disaac_vita_memory_import_indexed=original_memory_import_indexed"]
        run([*common,*packing,*renames,"-x","c","-","-c","-o",str(old)],data=baseline)
        exe=args.out/f"png-init-{gpr}{flags}{guard}.exe"
        run([*common,*packing,"-DISAAC_VITA_PNG_TEXEL_INIT_ELISION=1",
             "-DISAAC_VITA_HEAP_RANGE_LEASE=1","-DISAAC_VITA_TEXEL_SCRATCH_ORACLE=1",
             str(fixture),str(old),str(runtime/"host_vita_memory.c"),
             str(runtime/"host_vita_png_texel_init.c"),str(runtime/"host_vita_texel_scratch.c"),
             str(runtime/"host_vita_native_png.c"),str(runtime/"host_vita_archive_miniz_native.c"),
             "-o",str(exe)])
        run([str(exe),str(cases)])
    print("PNG texel init focused comparison PASS; no hardware performance claim")
    return 0

if __name__=="__main__":
    raise SystemExit(main())
