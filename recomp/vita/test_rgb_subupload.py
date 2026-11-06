#!/usr/bin/env python3
"""Regenerate patched native bodies, compare stock pixels, execute real ARM NEON."""
import argparse
import hashlib
import os
from pathlib import Path
import re
import struct
import subprocess
import sys
import tempfile

from test_vitagl_stock_patch_chain import (
    SOURCE_ARCHIVE_SHA256, materialize_stock, apply_patch,
)

HERE = Path(__file__).resolve().parent
RECIPE = HERE / "vitagl-stock-reference"
GUARD = "HAVE_ISAAC_RGB_SUBUPLOAD"


def run(command):
    result = subprocess.run([str(x) for x in command], capture_output=True, text=True)
    if result.returncode:
        raise RuntimeError(f"{command}\n{result.stdout}{result.stderr}")
    return result.stdout


def function(text, name):
    start = text.index("static inline __attribute__((always_inline)) void " + name + "(")
    return text[start:text.index("\n}", start) + 2] + "\n"


def strip_optional(text):
    output, depth = [], 0
    for line in text.splitlines(keepends=True):
        if not depth and line.startswith("#ifdef " + GUARD):
            depth = 1
        elif depth:
            if re.match(r"#\s*if", line): depth += 1
            elif re.match(r"#\s*endif", line): depth -= 1
        else:
            output.append(line)
    assert not depth
    return "".join(output)


def host_bodies(archive, clang, work):
    assert hashlib.sha256(archive.read_bytes()).hexdigest() == SOURCE_ARCHIVE_SHA256
    patches = {int(p.name[:4]): p for p in RECIPE.glob("[0-9][0-9][0-9][0-9]-*.patch")}
    # Same supported recipes as the existing scene-guard regression suite.
    chains = {
        "bare": (1, 28, 29, 30),
        "control": (1,2,3,8,4,12,13,14,15,7,10,11,16,17,18,21,24,33,22,23,26,28,29,30,34),
        "diagnostic": (1,2,3,8,4,12,13,14,15,5,6,7,10,9,11,16,17,18,20,21,24,33,22,23,26,25,27,28,29,31,32,34),
    }
    for label, chain in chains.items():
        source = materialize_stock(work/label, archive, None)
        for n in chain:
            apply_patch(source, patches[n])
        before = (source/"source/textures.c").read_text()
        apply_patch(source, patches[35])
        after = (source/"source/textures.c").read_text()
        assert strip_optional(after) == before, label
        assert "vglIsaacRgbSubuploadSnapshot" not in (source/"source/vitaGL.h").read_text()
        baseline = function(before, "_glTexSubImage2D").replace("_glTexSubImage2D(", "reference_subupload(", 1)
        candidate = function(after, "_glTexSubImage2D").replace("_glTexSubImage2D(", "candidate_subupload(", 1)
        prelude = after[after.index("#ifdef " + GUARD):after.index("static inline __attribute__((always_inline)) void _glTexSubImage2D(")]
        callbacks = (source/"source/texture_callbacks.c").read_text()
        callbacks = re.sub(r"(?m)^#include[^\n]*\n", "", callbacks)
        generated = work/"rgb-subupload-generated.h"
        generated.write_text(callbacks + "\n" + baseline + "\n#define " + GUARD + " 1\n" + prelude + candidate)
        # Real GL constants are copied, not recalled. GXM values are deliberately
        # opaque labels in the SDK double: this is a control/data test, not ABI decoding.
        gl = (source/"source/vitaGL.h").read_text()
        defines = "\n".join(re.findall(r"^#define\s+GL_\w+\s+[^\n]+", gl, re.M))
        names = set(re.findall(r"SCE_GXM_\w+", after[after.index("static inline __attribute__((always_inline)) void _glTexSubImage2D("):]))
        names |= {"SCE_GXM_TEXTURE_LINEAR", "SCE_GXM_TEXTURE_SWIZZLED"}
        enums = "enum {\n" + ",\n".join(f"{name} = {i+1000}" for i, name in enumerate(sorted(names))) + "\n};\n"
        (work/"rgb-subupload-enums.h").write_text(defines + "\n" + enums)
        exe = work/(label+".exe")
        flags = ["-DHAVE_ISAAC_P8_SAFE_UPLOAD=1", "-DSKIP_ERROR_HANDLING=1"] if 10 in chain else []
        # Unchanged upstream: callback pointer declarations discard const, and
        # COW+level>0 has an uninitialized mip_stride (that undefined combination
        # is not an equality oracle). Level-zero COW and defined mip paths run.
        run([clang, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
             "-Wno-incompatible-function-pointer-types", "-Wno-sign-compare", "-Wno-unused-function",
             "-Wno-sometimes-uninitialized",
             "-fno-strict-aliasing", *flags, "-I", work, "-I", RECIPE,
             HERE/"host_tests/rgb_subupload_oracle.c", "-o", exe])
        print(label + ": " + run([exe]).strip())
        print(label + ": freshly patched textures.c OFF-stripped byte-identical to its prior recipe")


def arm_code(clang, work):
    source = work/"rgb-arm.c"
    source.write_text('''#include "isaac_rgb_subupload.h"
int isaac_test_rgb_rows(uint8_t *d, uint32_t ds, const uint8_t *s, uint32_t sl, int32_t w, int32_t h) {
    return isaac_rgb_subupload_rows(d, ds, s, sl, w, h);
}
int isaac_test_rgb_replace(const uint32_t *a) {
    return isaac_rgb_subupload_full_replace(a[0], (int32_t)a[1], (int32_t)a[2], (int32_t)a[3],
        (int32_t)a[4], (int32_t)a[5], a[6], a[7], a[8], a[9], (const void *)(uintptr_t)a[10]);
}
''')
    obj, elf, binary = (work/("rgb-arm."+s) for s in ("o", "elf", "bin"))
    run([clang, "--target=armv7-none-eabi", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
         "-ffreestanding", "-fno-builtin", "-ffunction-sections", "-fdata-sections",
         "-mcpu=cortex-a9", "-mfpu=neon", "-mfloat-abi=softfp", "-mthumb", "-D__vita__=1",
         "-I", RECIPE, "-c", source, "-o", obj])
    tools = clang.parent
    suffix = ".exe" if clang.suffix == ".exe" else ""
    run([tools/("ld.lld"+suffix), "--gc-sections", "--entry=isaac_test_rgb_rows",
         "--undefined=isaac_test_rgb_replace", "-Ttext=0x10000", obj, "-o", elf])
    symbols = run([tools/("llvm-nm"+suffix), elf])
    entries = {name: next(int(l.split()[0], 16) for l in symbols.splitlines() if l.endswith(" " + name))
               for name in ("isaac_test_rgb_rows", "isaac_test_rgb_replace")}
    asm = run([tools/("llvm-objdump"+suffix), "-d", elf])
    assert "vld3" in asm and "vst4" in asm, "actual NEON conversion was not emitted"
    run([tools/("llvm-objcopy"+suffix), "-O", "binary", "--only-section=.text", elf, binary])
    return binary.read_bytes(), entries, asm, run([tools/("llvm-objdump"+suffix), "-h", elf])


def arm_checks(clang, work):
    from unicorn import Uc, UC_ARCH_ARM, UC_MODE_THUMB, UC_HOOK_MEM_READ, UC_HOOK_MEM_WRITE, UC_MEM_READ
    from unicorn.arm_const import (UC_ARM_REG_C1_C0_2, UC_ARM_REG_FPEXC, UC_ARM_REG_LR,
        UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3, UC_ARM_REG_SP)
    # CPACR/FPEXC and Thumb invocation follow test_vita_native_png_rgba_neon.py.
    code, entries, asm, sections = arm_code(clang, work)
    uc = Uc(UC_ARCH_ARM, UC_MODE_THUMB)
    CODE, SRC, DST, STACK, STOP = 0x10000, 0x100000, 0x500000, 0x900000, 0xa00000
    uc.mem_map(CODE, 0x10000); uc.mem_write(CODE, code)
    uc.mem_map(SRC, 0x400000); uc.mem_map(DST, 0x400000)
    uc.mem_map(STACK, 0x10000); uc.mem_map(STOP, 0x1000)
    uc.reg_write(UC_ARM_REG_C1_C0_2, 0xF << 20)
    uc.reg_write(UC_ARM_REG_FPEXC, 0x40000000)
    allowed = []
    def access(u, kind, address, size, value, user):
        if STACK <= address and address+size <= STACK+0x10000: return
        if kind == UC_MEM_READ and CODE <= address and address+size <= CODE+0x10000: return
        for base, stride, rowbytes, height, reading in allowed:
            if (kind == UC_MEM_READ) != reading or address < base: continue
            row, offset = divmod(address-base, stride)
            if row < height and offset+size <= rowbytes: return
        raise AssertionError(f"ARM non-logical pixel access {address:x}+{size}, kind={kind}")
    uc.hook_add(UC_HOOK_MEM_READ | UC_HOOK_MEM_WRITE, access)
    def invoke(name, args):
        sp = STACK+0x8000
        uc.reg_write(UC_ARM_REG_SP, sp); uc.reg_write(UC_ARM_REG_LR, STOP|1)
        for register, value in zip((UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3), args):
            uc.reg_write(register, value & 0xffffffff)
        if len(args)>4: uc.mem_write(sp, struct.pack("<"+"I"*(len(args)-4), *(x & 0xffffffff for x in args[4:])))
        uc.emu_start(entries[name]|1, STOP, count=10000000)
        return uc.reg_read(UC_ARM_REG_R0)
    cases = 0
    for width in list(range(1,66)) + [127,128,129,255,256,257,960,1024,2048]:
        for alignment in range(16):
            height = 3
            s, d = SRC+128+alignment, DST+128+((alignment*7)&15)
            rowlen = width + (alignment%3)*5
            ss, ds = rowlen*3, width*4+((alignment%4)*7)
            raw = bytes((i*37+alignment*19)&255 for i in range(ss*height))
            uc.mem_write(s, raw); uc.mem_write(d-16, b"\xa5"*(ds*height+32))
            allowed[:] = [(s,ss,width*3,height,True),(d,ds,width*4,height,False)]
            assert invoke("isaac_test_rgb_rows", [d,ds,s,rowlen,width,height]) == 1
            expected = bytearray(b"\xa5"*(ds*height))
            for y in range(height):
                for x in range(width): expected[y*ds+x*4:y*ds+x*4+4] = raw[y*ss+x*3:y*ss+x*3+3] + b"\xff"
            assert bytes(uc.mem_read(d,ds*height)) == expected
            assert bytes(uc.mem_read(d-16,16)) == b"\xa5"*16
            assert bytes(uc.mem_read(d+ds*height,16)) == b"\xa5"*16
            assert bytes(uc.mem_read(s,len(raw))) == raw
            cases += 1
    # 32-bit range wrap, aliases, short stride, negative/empty dimensions:
    # rejection must perform NO client-memory read or write.
    allowed[:] = []
    for args in ([DST,128,0xfffffff0,32,32,2], [DST,128,SRC,32,32,0x7fffffff],
                 [SRC+4,128,SRC,32,32,2], [DST,128,SRC,16,32,2],
                 [DST,127,SRC,32,32,2], [DST,128,SRC,32,-1,2],
                 [DST,128,SRC,32,32,0], [DST,128,0,32,32,2]):
        assert invoke("isaac_test_rgb_rows", args) == 0
    predicate = [1,0,0,0,960,544,960,544,1,0,SRC]
    for index, value in ((-1,0),(0,0),(1,1),(2,1),(3,1),(4,959),(5,543),(8,2),(9,959),(10,0)):
        a = predicate[:]
        if index>=0: a[index]=value
        uc.mem_write(STACK+64,struct.pack("<11I",*a))
        answer = invoke("isaac_test_rgb_replace",[STACK+64])
        assert answer == (index<0), (index, a, answer, sections, asm)
    print(f"ARM NEON RGB rows: PASS {cases} strided/aligned/tail cases + 8 rejection + 10 overwrite gates; "
          f"real vld3/vst4, exact logical bounds, code={len(code)} B (not a timing estimate)")


def wiring_checks():
    cmake = (HERE/"CMakeLists.txt").read_text()
    build = (RECIPE/"build.sh").read_text()
    assert re.search(r'option\(ISAAC_VITA_VITAGL_RGB_SUBUPLOAD\s+"[^"]+" OFF\)',cmake)
    for needle in ("ISAAC_RGB_SUBUPLOAD=${ISAAC_VITA_VITAGL_RGB_SUBUPLOAD_MODE}",
                   '"${ISAAC_VITA_VITAGL_RGB_SUBUPLOAD_MODE_FILE}"',
                   "0035-isaac-rgb-subupload.patch", "isaac_rgb_subupload.h",
                   "ISAAC_VITA_VITAGL_RGB_SUBUPLOAD=1"):
        assert needle in cmake, needle
    for needle in ('rgb_subupload=${ISAAC_RGB_SUBUPLOAD:-0}',
                   'extra_cflags="$extra_cflags -DHAVE_ISAAC_RGB_SUBUPLOAD=1"',
                   '"$rgb_subupload" "$rgb_subupload_patch_sha" "$rgb_subupload_header_sha"',
                   'apply_source_patch "$rgb_subupload_patch"',
                   'vglIsaacRgbSubuploadSnapshot',
                   'verify_sha "$source_dir/source/isaac_rgb_subupload.h" "$rgb_subupload_header_sha"'):
        assert needle in build, needle
    assert build.index('apply_source_patch "$rgb_subupload_patch"') > build.index('apply_source_patch "$scene_guard_patch"')
    print("RGB upload wiring: PASS (default OFF, dependency/mode/recipe inputs, real compiler flag, source and archive endpoint guards)")


def verify_unicorn_dll(path, expected_sha):
    """Use the isolated upstream hook fix, never a global package replacement.

    Unicorn 2.1.4 has a continuing-memory-hook Thumb ITSTATE bug (fixed
    upstream by 2bf1098e). The old engine also falsely
    accepts width959/orig_w960 in this predicate. Keep instructions and strict
    hooks unchanged; prove which DLL the binding actually loaded.
    """
    if not path:
        return
    assert os.name == "nt" and expected_sha
    assert "unicorn" not in sys.modules, "DLL must be selected before import"
    path = path.resolve(strict=True)
    assert hashlib.sha256(path.read_bytes()).hexdigest() == expected_sha.lower()
    os.environ["LIBUNICORN_PATH"] = str(path.parent)
    # Import the binding only now, then ask Windows which DLL it mapped.
    import ctypes
    from ctypes import wintypes
    import importlib
    binding = importlib.import_module("unicorn.unicorn_py3.unicorn")
    get_name = ctypes.WinDLL("kernel32", use_last_error=True).GetModuleFileNameW
    get_name.argtypes = (wintypes.HMODULE, wintypes.LPWSTR, wintypes.DWORD)
    get_name.restype = wintypes.DWORD
    buffer = ctypes.create_unicode_buffer(32768)
    n = get_name(binding.uclib._handle, buffer, len(buffer))
    assert 0 < n < len(buffer)
    loaded = Path(buffer.value).resolve(strict=True)
    assert loaded == path, f"Unicorn DLL fallback: {loaded} != {path}"
    assert hashlib.sha256(loaded.read_bytes()).hexdigest() == expected_sha.lower()
    print(f"Verified Unicorn DLL: {loaded}; SHA256={expected_sha}")


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--stock-tar",type=Path,required=True)
    parser.add_argument("--clang",type=Path,required=True)
    parser.add_argument("--arm-only",action="store_true")
    parser.add_argument("--unicorn-dll",type=Path)
    parser.add_argument("--unicorn-sha256")
    args=parser.parse_args()
    verify_unicorn_dll(args.unicorn_dll,args.unicorn_sha256)
    wiring_checks()
    with tempfile.TemporaryDirectory(prefix="isaac-rgb-upload-") as tmp:
        work=Path(tmp)
        if not args.arm_only:
            host_bodies(args.stock_tar,args.clang,work)
        arm_checks(args.clang,work)


if __name__ == "__main__": main()
