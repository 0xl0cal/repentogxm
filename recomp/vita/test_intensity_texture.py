#!/usr/bin/env python3
"""Fresh pinned native bodies + lossless-storage differential/failure oracle.

SDK descriptor/allocation/render-target calls are doubles, not a device PASS.
No generated native input is reused; stock archive hash is checked every run.
"""
import argparse
import hashlib
import io
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile
import shutil

from test_vitagl_stock_patch_chain import (SOURCE_ARCHIVE_SHA256, INTENSITY_CHAINS,
                                           materialize_stock, apply_patch)

HERE = Path(__file__).resolve().parent
RECIPE = HERE / "vitagl-stock-reference"
GUARD = "HAVE_ISAAC_INTENSITY_TEXTURE"
GIT_APPLY = ["git", "-c", "core.autocrlf=false", "-c", "core.eol=lf", "apply", "--unidiff-zero"]


def run(command, cwd=None):
    p = subprocess.run([str(x) for x in command], cwd=cwd, capture_output=True, text=True)
    if p.returncode:
        raise RuntimeError(f"{command}\nexit={p.returncode}\n{p.stdout}{p.stderr}")
    return p.stdout


def body(text, name):
    # These pinned C functions close with an unindented brace; no regex guesses
    # about nested blocks. Refuse a duplicate/absent signature.
    hits = list(re.finditer(r"(?m)^[^\n]*\b" + re.escape(name) + r"\([^;\n]*\) \{", text))
    assert len(hits) == 1, (name, len(hits))
    start = hits[0].start()
    return text[start:text.index("\n}", start) + 2] + "\n"


def static_body(text, name, rename):
    """The external glGenTextures/glDeleteTextures as a static host function
    (an extracted external inline must not need a second definition)."""
    head, rest = body(text, name).split(name + "(", 1)
    assert head in ("inline __attribute__((always_inline)) void ", "void "), head
    return "static " + head + rename + "(" + rest


def strip_optional(text):
    result, depth = [], 0
    for line in text.splitlines(keepends=True):
        if not depth and line.startswith("#ifdef " + GUARD):
            depth = 1
        elif depth:
            if re.match(r"#\s*if", line): depth += 1
            elif re.match(r"#\s*endif", line): depth -= 1
        else:
            result.append(line)
    assert not depth
    return "".join(result)


def asset_inputs(args, work, exe):
    """No sampling: decode all eight archive pages, premultiply ALL pixels
    with the frozen PE's actual table bytes, then execute the native candidate.
    This is a reconstructed upload, not a device capture of current flags.
    """
    import sys
    import numpy as np
    import pefile
    from PIL import Image
    sys.path.insert(0, str(HERE.parent.parent / "tools"))
    import isaac_vita_repack as rp
    census = json.loads(args.font_census.read_text())
    assert hashlib.sha256(args.font_archive.read_bytes()).hexdigest() == census["archive_sha256"]
    # Copy table locations from the actual native premultiply source.
    premul = (HERE.parent / "runtime/host_vita_png_premultiply.c").read_text()
    pe = pefile.PE(str(args.pe))
    import gl_surface
    gl_surface.verify_pe(str(args.pe), gl_surface.commands())
    tables = {}
    for mode in ("LINEAR", "GAMMA"):
        rva = int(re.search(r"PREMUL_" + mode + r"_TABLE\s*=\s*0x([0-9a-f]+)U", premul)[1], 16)
        raw = pe.get_data(rva, 65536)
        tables[mode.lower()] = np.frombuffer(raw, dtype=np.uint8).reshape(256,256)
        print(mode + " premultiply actual PE table SHA256=" + hashlib.sha256(raw).hexdigest())
    budget = {"rgba": 0, "linear": 0, "gamma": 0}
    with rp.Archive(args.font_archive) as archive:
        limits = archive.payload_limits()
        for row in census["pngs"]:
            entry = next(e for e in archive.entries if e.offset == row["archive_offset"])
            png, _ = rp.decode_entry(rp.shape_of(rp.entry_pieces(archive,entry,limits[entry.offset])), entry, True)
            assert png is not None and hashlib.sha256(png).hexdigest() == row["sha256"]
            image = Image.open(io.BytesIO(png)).convert("RGBA")
            raw = np.array(image)
            assert image.size == (row["width"], row["height"])
            alpha = raw[:,:,3]
            visible = alpha != 0
            hidden_color = int(np.count_nonzero(np.any(raw[:,:,:3] != 0,axis=2) & ~visible))
            colors = int(np.count_nonzero(np.any(raw[:,:,:3] != 255,axis=2) & visible))
            budget["rgba"] += raw.size
            print(Path(row["resource_path"]).name + f": all pixels={alpha.size} visible-nonwhite={colors} transparent-nonzero-RGB={hidden_color} alpha-values={len(np.unique(alpha))}")
            for mode, table in tables.items():
                upload = raw.copy()
                for c in range(3): upload[:,:,c] = table[alpha,raw[:,:,c]]
                upload[:,:,:3][alpha==0] = 0
                upload[:,:,:3][alpha==255] = raw[:,:,:3][alpha==255]
                mismatches = int(np.count_nonzero(np.any(upload[:,:,:3] != alpha[:,:,None],axis=2)))
                colored = int(np.count_nonzero(np.any(upload[:,:,:3] != upload[:,:,0,None],axis=2)))
                data = upload.tobytes()
                path = work / "font-upload.rgba" # ephemeral, never committed
                path.write_bytes(data)
                receipt = run([exe,path,row["width"],row["height"]]).strip()
                print(mode + f": RGB-vs-alpha-mismatch-pixels={mismatches} colored-pixels={colored}; " + receipt)
                assert path.read_bytes() == data
                expected_bytes = alpha.size * (1 if mismatches == 0 else 2 if colored == 0 else 4)
                assert int(re.search(r"GPU payload=(\d+) B",receipt)[1]) == expected_bytes
                budget[mode] += expected_bytes
    print("Eight-page GPU payload budget (not process RAM, no driver/display/scratch attribution): " + json.dumps(budget,sort_keys=True))


def off_identity(before, after, gpu, callbacks, clang, work):
    """Compile the real pre/post native bodies with the feature ABSENT, using
    the same absolute source path. Compare preprocessed tokens and object bytes,
    and execute the post-patch OFF image path with exactly preserved RGBA.
    This is a host extraction identity, not a complete ARM archive identity.
    """
    harness = (HERE / "host_tests/intensity_texture_oracle.c").read_text()
    prefix = harness.split('#include "intensity-generated.h"')[0]
    # Exact pinned utils/debug_utils.h behavior with LOG_ERRORS absent. This
    # removes __FILE__/__LINE__ arguments at preprocessing, like production,
    # rather than treating diagnostic line offsets as changed program tokens.
    prefix += '\n#define vgl_log(...)\n'
    calls = ("_glTexImage2D_FlatIMPL", "_glTexSubImage2D", "_glTexParameteri")
    main = '''
static void gpu_free_texture(texture *t) { gpu_free_texture_data(t); t->status=TEX_UNUSED; }
void force_sub(texture *t,const void *p) { _glTexSubImage2D(t,GL_TEXTURE_2D,0,0,0,1,1,GL_RGBA,GL_UNSIGNED_BYTE,p); }
void force_param(texture *t) { _glTexParameteri(t,GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR); }
void force_attach(framebuffer *f,texture *t) { _glFramebufferTexture2D(f,GL_COLOR_ATTACHMENT0,t,1); }
int main(void) {
    size_t n=512U*512U*4U; uint8_t *p=malloc(n); assert(p);
    for(size_t i=0;i<n;++i) p[i]=(uint8_t)(i/4U);
    texture *t=&texture_slots[1];t->status=TEX_UNINITIALIZED;t->last_frame=OBJ_NOT_USED;
    _glTexImage2D_FlatIMPL(t,0,GL_RGBA,512,512,GL_RGBA,GL_UNSIGNED_BYTE,p);
    assert(t->format==SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR);
    assert(live_bytes==n && !memcmp(t->data,p,n));gpu_free_texture_data(t);
    GLuint ids[2]={0,0};glGenTextures(2,ids);
    assert(ids[0]==2&&ids[1]==3&&texture_slots[2].status==TEX_UNINITIALIZED);
    assert(live_bytes==0);free(p);puts("OFF native RGBA execution PASS");return 0;
}
'''
    source = work / "off-identity.c"
    objects, tokens = [], []
    for version in (before, after):
        native = callbacks + body(version["utils/gpu_utils.h"], "gpu_free_texture_data") + body(gpu, "gpu_alloc_texture")
        native += "\n".join(body(version["textures.c"], n) for n in calls)
        native += static_body(version["textures.c"], "glGenTextures", "glGenTextures")
        native += body(version["shared.h"], "_glFramebufferTexture2D")
        source.write_text(prefix + native + '\n#line 1 "off-oracle-main"\n' + main)
        flags = [clang,"-std=c11","-O2","-D_CRT_SECURE_NO_WARNINGS","-fno-strict-aliasing",
                 "-Wno-incompatible-function-pointer-types","-I",work,"-I",RECIPE]
        preprocessed = run([*flags,"-E","-P",source])
        # Preprocessing preserves different blank lines where guarded blocks
        # disappear; compare lexical tokens, preserving quoted literal bytes.
        tokens.append(re.findall(r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|\w+|[^\s]', preprocessed))
        obj = work / "off-identity.o"
        run([*flags,"-c",source,"-o",obj])
        # Normalize ONLY the COFF timestamp (external object-container metadata).
        data = bytearray(obj.read_bytes())
        data[4:8] = b"\0" * 4
        objects.append(bytes(data))
    if tokens[0] != tokens[1]:
        index = next(i for i,(a,b) in enumerate(zip(tokens[0],tokens[1])) if a != b)
        raise AssertionError(f"OFF tokens changed at {index}: {tokens[0][index-6:index+6]} != {tokens[1][index-6:index+6]}")
    assert objects[0] == objects[1], "OFF native extracted object changed"
    exe = work / "off-identity.exe"
    run([*flags,source,"-o",exe])
    print(run([exe]).strip() + "; preprocessed tokens + host object identical (COFF timestamp normalized)")


RUNTIME_GUARD = "#if defined(ISAAC_VITA_VITAGL_INTENSITY_TEXTURE) && ISAAC_VITA_VITAGL_INTENSITY_TEXTURE\n"
RUNTIME_WRAPPERS = ("vita_glFramebufferTexture2D", "vita_glTexImage2D", "vita_glTexParameteri", "vita_glTexSubImage2D")


def wrapper_span(text, name):
    # The four wrappers open with a multi-line signature and close with an
    # unindented brace; refuse a duplicate/absent definition.
    head = "\nstatic void " + name + "(\n"
    assert text.count(head) == 1, name
    start = text.index(head)
    return text[start:text.index("\n}\n", start) + 3]


def c_strings(text):
    return re.findall(r'"((?:\\.|[^"\\])*)"', text)


def runtime_reader():
    """The guarded native.log reader from gl_vita_backend.c, verbatim, plus the
    static pins that tie its tables and word indices to the 0037 header."""
    runtime = (HERE.parent / "runtime/gl_vita_backend.c").read_text()
    header = (RECIPE / "isaac_intensity_texture.h").read_text()
    # Five guards (the reader + four call sites), nothing else sees the knob.
    assert runtime.count("ISAAC_VITA_VITAGL_INTENSITY_TEXTURE") == 10
    assert runtime.count(RUNTIME_GUARD) == 5
    start = runtime.index(RUNTIME_GUARD + "/* Private 0037 archive endpoint")
    end = runtime.index("\n}\n#endif\n", start) + 3
    block = runtime[start + len(RUNTIME_GUARD):end]
    directives = re.findall(r"(?m)^\s*#\s*(\w+)", block)
    assert directives == ["define"], directives
    assert block.count("gl_vita_intensity_receipt(void)") == 1
    call = RUNTIME_GUARD + "    gl_vita_intensity_receipt();\n#endif\n"
    assert runtime.count("gl_vita_intensity_receipt();") == 4
    for name in RUNTIME_WRAPPERS:
        span = wrapper_span(runtime, name)
        # One read per wrapper, and no return that could skip it.
        assert span.count(call) == 1 and not re.search(r"\breturn\b", span), name
    # The word layout the reader indexes by number (w[1], w[2..24]).
    layout = dict((k, int(v)) for k, v in re.findall(r"ISAAC_INTENSITY_(W_\w+|WORDS) = (\d+)", header))
    assert layout == {"W_GEN": 0, "W_LAST": 1, "W_UPLOAD_BASE": 1, "W_FAILED_BASE": 5, "W_SKIP_BASE": 9,
                      "W_REJECT_BASE": 13, "W_U8": 18, "W_A8L8": 19, "W_DEOPT": 20, "W_DEOPT_FAILED": 21,
                      "W_REDEFINE_REJECTED": 22, "W_LIVE_PAGES": 23, "W_LIVE_BYTES": 24, "WORDS": 25}, layout
    assert "#define GL_VITA_INTENSITY_WORDS 25u\n" in block
    args = re.findall(r"\(unsigned\)w\[(\d+)\]", block)
    assert [int(a) for a in args] == list(range(2, 25)), args
    first = block.index('"KAGE VITA INTENSITY: gen=')
    fmt = "".join(c_strings(block[first:block.index("(unsigned)gen,", first)]))
    labels = re.findall(r" (\w+)=%u", fmt)
    assert labels == ["gen", "up", "fail", "skip", "rej", "u8", "a8l8", "deopt", "dfail", "redef", "live"], labels
    assert fmt.count("%u") == 1 + 23 and fmt.count("%s") == 3
    # Tables: phase names = the header phase enum = the tty "phase=" spellings;
    # details = the header detail strings; shapes = isaac_intensity_shape order.
    def table(name):
        m = re.search(r"static const char \*const " + name + r"\[\] = \{(.*?)\};", block, re.S)
        return c_strings(m[1])
    phase_enum = re.search(r"enum \{ ISAAC_INTENSITY_ARMED = 1,(.*?)\};", header, re.S)
    phase_names = ["armed"] + [n.lower().replace("_", "-") for n in re.findall(r"ISAAC_INTENSITY_(\w+)", phase_enum[1])]
    assert table("phases") == ["none"] + phase_names, table("phases")
    for phase in phase_names[1:]:
        assert ('sceClibPrintf("Isaac intensity: phase=' + phase + " ") in header, phase
    detail_strings = c_strings(re.search(r"isaac_intensity_details\[\] = \{(.*?)\};", header, re.S)[1])
    assert table("details") == [""] + [":" + d for d in detail_strings[1:]], table("details")
    detail_enum = re.search(r"enum \{ ISAAC_INTENSITY_D_U8 = 1,(.*?)\};", header, re.S)
    assert len(re.findall(r"ISAAC_INTENSITY_D_\w+", detail_enum[0])) == len(detail_strings) - 1
    head = "\nstatic unsigned isaac_intensity_shape(GLsizei width, GLsizei height)\n{\n"
    assert header.count(head) == 1
    shape_fn = header[header.index(head):header.index("\n}\n", header.index(head))]
    shapes = [f":{w}x{h}" for w, h in re.findall(r"width == (\d+) && height == (\d+)", shape_fn)]
    assert re.findall(r"return (\d+);", shape_fn) == ["1", "2", "3", "4", "0"]
    assert table("shapes") == [""] + shapes, table("shapes")
    # Exactly one word-0 bump per printed tty line.
    assert header.count('sceClibPrintf("Isaac intensity: ') == 8
    assert len(re.findall(r"\bisaac_intensity_line\(ISAAC_INTENSITY_", header)) == 8
    # Worst case (every word saturated, longest names) fits platform.c
    # isaac_vita_log body[384] (383 characters + NUL), so nothing truncates.
    longest = [max(table(n), key=len) for n in ("phases", "shapes", "details")]
    worst = fmt.replace("%u", str(2**32 - 1))
    for s in longest: worst = worst.replace("%s", s, 1)
    assert "%" not in worst and len(worst) <= 383, (len(worst), worst)
    platform = (HERE / "platform.c").read_text()
    assert re.search(r"void isaac_vita_log\(const char \*format, \.\.\.\)\n\{\n    char body\[384\];", platform)
    print(f"runtime reader pins: 5 guards, 4 call sites, layout/tables = 0037 header, worst-case line {len(worst)}/383 chars")
    return "#define ISAAC_VITA_VITAGL_INTENSITY_TEXTURE 1\n" + block


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--stock-tar", type=Path, required=True)
    parser.add_argument("--clang", type=Path, required=True)
    parser.add_argument("--font-archive", type=Path)
    parser.add_argument("--font-census", type=Path)
    parser.add_argument("--pe", type=Path)
    parser.add_argument("--sanitize", action="store_true", help="execute ASan host fixtures (not ARM)")
    parser.add_argument("--temp-dir", type=Path, help="parent of the scratch tree (default: system temp)")
    parser.add_argument("--before-patch", type=Path, action="append", default=[],
                        help="another line's vitaGL patch (e.g. 0036) applied before 0037 on every chain "
                             "where it applies; the oracle then runs on that chain too")
    args = parser.parse_args()
    if args.sanitize and os.name == "nt":
        dlls = sorted(args.clang.parent.parent.glob("lib/clang/*/lib/windows/clang_rt.asan_dynamic-x86_64.dll"))
        assert dlls, "Clang ASan DLL was not found in this compiler installation"
        os.environ["PATH"] = str(dlls[-1].parent) + os.pathsep + os.environ.get("PATH", "")
    # Shell syntax only, never invoke the SDK build or touch the shared stage.
    git_bash = Path("C:/Program Files/Git/bin/bash.exe")
    bash = str(git_bash) if os.name == "nt" and git_bash.is_file() else shutil.which("bash")
    assert bash, "bash is required for syntax checking the native recipe"
    run([bash,"-n",(RECIPE / "build.sh").as_posix()])
    assert hashlib.sha256(args.stock_tar.read_bytes()).hexdigest() == SOURCE_ARCHIVE_SHA256
    cmake = (HERE / "CMakeLists.txt").read_text()
    build = (RECIPE / "build.sh").read_text()
    assert re.search(r'option\(ISAAC_VITA_VITAGL_INTENSITY_TEXTURE\s+"[^"]+" OFF\)', cmake)
    for needle in ("ISAAC_INTENSITY_TEXTURE=${ISAAC_VITA_VITAGL_INTENSITY_TEXTURE_MODE}",
                   '"${ISAAC_VITA_VITAGL_INTENSITY_TEXTURE_MODE_FILE}"',
                   "0037-isaac-intensity-texture.patch", "isaac_intensity_texture.h"):
        assert needle in cmake, needle
    for needle in ('intensity_texture=${ISAAC_INTENSITY_TEXTURE:-0}',
                   'extra_cflags="$extra_cflags -DHAVE_ISAAC_INTENSITY_TEXTURE=1"',
                   'apply_source_patch "$intensity_texture_patch"',
                   '"$intensity_texture_patch_sha"', '"$intensity_texture_header_sha"',
                   "for endpoint in isaacIntensityPrepare isaacIntensityRelease vglIsaacIntensitySnapshot; do"):
        assert needle in build, needle
    assert ('set_property(SOURCE "${ISAAC_RUNTIME}/gl_vita_backend.c"\n'
            '          APPEND PROPERTY COMPILE_DEFINITIONS ISAAC_VITA_VITAGL_INTENSITY_TEXTURE=1)') in cmake
    reader = runtime_reader()
    # All measured mutators enter patched native owners. If the guest surface
    # expands, this experimental policy must be reviewed, not silently assumed.
    # No gl_surface_generated file is consumed: use freshly generated output.
    import sys
    sys.path.insert(0, str(HERE.parent))
    import gl_surface
    surface = "\n".join(gl_surface.generated_outputs(gl_surface.commands()).values())
    for name in ("glTexImage2D", "glTexSubImage2D", "glTexParameteri", "glFramebufferTexture2D", "glDeleteTextures"):
        assert name in surface
    for name in ("glCopyTexImage2D", "glCopyTexSubImage2D", "glGenerateMipmap", "glTexStorage2D",
                 "glCompressedTexImage2D", "glCompressedTexSubImage2D", "glTextureView",
                 "glBindSampler", "glGetTexImage"):
        assert '"' + name + '"' not in surface, name
    patches = {int(p.name[:4]): p for p in RECIPE.glob("[0-9][0-9][0-9][0-9]-*.patch")}
    # Shared with test_vitagl_stock_patch_chain.prove_intensity_texture; any
    # --before-patch (e.g. the stall line's 0036) adds a chain variant where it applies.
    variants = [(label, chain, ()) for label, chain in INTENSITY_CHAINS.items()]
    for extra in args.before_patch:
        variants += [(label + "+" + extra.name[:4], chain, (extra,)) for label, chain in INTENSITY_CHAINS.items()]
    temp_parent = str(args.temp_dir) if args.temp_dir else None
    with tempfile.TemporaryDirectory(prefix="isaac-intensity-", dir=temp_parent) as temp:
        work = Path(temp)
        for label, chain, extra in variants:
            src = materialize_stock(work / label.replace("+", "-"), args.stock_tar, None)
            for n in chain: apply_patch(src, patches[n])
            if extra:
                if subprocess.run([*GIT_APPLY, "--check", str(extra[0])], cwd=src, capture_output=True).returncode:
                    print(label + ": " + extra[0].name + " itself does not apply on this chain; skipped")
                    continue
                run([*GIT_APPLY, extra[0]], src)
            paths = ("textures.c", "shared.h", "utils/gpu_utils.h")
            before = {p: (src / "source" / p).read_text() for p in paths}
            # Same zero-context allowance as production, never a fuzzy patch.
            run([*GIT_APPLY, "--check", patches[37]], src)
            run([*GIT_APPLY, patches[37]], src)
            after = {p: (src / "source" / p).read_text() for p in paths}
            for p in paths: assert strip_optional(after[p]) == before[p], (label, p)
            # Deletion is byte-identical ON. glGenTextures gains exactly the
            # guarded name-lifetime reset after its TEX_UNINITIALIZED store.
            assert body(before["textures.c"], "glDeleteTextures") == body(after["textures.c"], "glDeleteTextures")
            gen_on = body(after["textures.c"], "glGenTextures")
            assert strip_optional(gen_on) == body(before["textures.c"], "glGenTextures")
            assert gen_on.count("isaac_intensity_generated(") == 1
            assert ("\t\t\ttexture_slots[i].status = TEX_UNINITIALIZED;\n#ifdef " + GUARD +
                    "\n\t\t\tisaac_intensity_generated(i);\n#endif\n") in gen_on
            callbacks = (src / "source/texture_callbacks.c").read_text()
            callbacks = re.sub(r"(?m)^#include[^\n]*\n", "", callbacks)
            funcs = ("_glTexImage2D_FlatIMPL", "_glTexSubImage2D", "_glTexParameteri")
            reference = "\n".join(body(before["textures.c"], n).replace(n + "(", "reference" + n + "(", 1) for n in funcs)
            candidate = "\n".join(body(after["textures.c"], n).replace(n + "(", "candidate" + n + "(", 1) for n in funcs)
            reference += body(before["shared.h"], "_glFramebufferTexture2D").replace("_glFramebufferTexture2D(", "reference_attach(", 1)
            candidate += body(after["shared.h"], "_glFramebufferTexture2D").replace("_glFramebufferTexture2D(", "candidate_attach(", 1)
            # Real name allocator/deleter: the ON allocator carries the reset.
            candidate += static_body(after["textures.c"], "glGenTextures", "candidate_glGenTextures")
            candidate += static_body(after["textures.c"], "glDeleteTextures", "candidate_glDeleteTextures")
            gpu = (src / "source/utils/gpu_utils.c").read_text()
            generated = callbacks + body(after["utils/gpu_utils.h"], "gpu_free_texture_data") + body(gpu, "gpu_alloc_texture")
            generated += reference + '#define ' + GUARD + ' 1\n#include "isaac_intensity_texture.h"\n' + candidate
            # The common native free is generated with the guard already ON.
            generated = '#define ' + GUARD + ' 1\nvoid isaacIntensityRelease(texture *);\n' + generated
            # The real runtime reader (gl_vita_backend.c, ON), executed by the oracle.
            generated += reader
            (work / "intensity-generated.h").write_text(generated)
            gl = (src / "source/vitaGL.h").read_text()
            # Consumer-declared endpoint: the public header never names it.
            assert "vglIsaacIntensitySnapshot" not in gl and "isaacIntensity" not in gl
            defines = "\n".join(re.findall(r"^#define\s+GL_\w+\s+[^\n]+", gl, re.M))
            opaque = sorted(set(re.findall(r"SCE_GXM_\w+", generated + (RECIPE / "isaac_intensity_texture.h").read_text())))
            defines += "\nenum {" + ",\n".join(f"{n}={i+1000}" for i,n in enumerate(opaque)) + "};\n"
            # Native constants copied from stock, not remembered.
            shared = before["shared.h"]
            for name in ("TEXTURES_NUM", "GXM_TEX_MAX_SIZE", "FRAME_PURGE_FREQ", "OBJ_NOT_USED",
                         "TEXTURE_IMAGE_UNITS_NUM", "COMBINED_TEXTURE_IMAGE_UNITS_NUM"):
                defines += re.search(r"(?m)^#define " + name + r"[^\n]+", shared)[0] + "\n"
            # The texture-status and depth-buffer-state enums, verbatim.
            for first in ("TEX_UNUSED", "DEPTHBUFFER_MISSING"):
                defines += re.search(r"(?m)^enum \{\n\t" + first + r",\n[^}]*\};\n", shared)[0]
            (work / "intensity-enums.h").write_text(defines)
            exe = work / (label + ".exe")
            off_identity(before, after, gpu, callbacks, args.clang, work)
            sanitizer = ["-fsanitize=address"] if args.sanitize else []
            run([args.clang, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", "-D_CRT_SECURE_NO_WARNINGS", *sanitizer,
                 "-Wno-incompatible-function-pointer-types", "-Wno-sign-compare", "-Wno-unused-function",
                 "-Wno-sometimes-uninitialized", "-fno-strict-aliasing", "-I", work, "-I", RECIPE,
                 HERE / "host_tests/intensity_texture_oracle.c", "-o", exe])
            print(label + ": " + run([exe]).strip())
            print(label + ": OFF-stripped full native sources identical; ON glDeleteTextures byte-identical, "
                  "ON glGenTextures = stock + one guarded isaac_intensity_generated(i)")
            if label == "bare" and args.font_archive:
                assert args.font_census and args.pe
                asset_inputs(args, work, exe)
    print("Intensity storage wiring/surface: PASS; ARM/GXM/device gates NOT run")


if __name__ == "__main__": main()
