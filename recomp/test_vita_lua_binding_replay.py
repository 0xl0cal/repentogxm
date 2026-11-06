#!/usr/bin/env python3
"""Proofs for the LuaBridge binding replays (ISAAC_VITA_LUA_BINDING_REPLAY,
recomp/runtime/host_vita_lua_binding_replay.[ch] + the generated
host_vita_lua_binding_table.h / host_vita_lua_binding_mirror.h).

1. generator: `lua_binding_replay_table.py --write` into the work directory
   (both headers are game-derived and not tracked; the build generates them
   at configure time the same way), then `--check` and `--check-corpus` (the
   configure-time gate the build runs) -- all as subprocesses, so the test
   fails when either generated header drifts from its sha256 pin; plus a
   negative control: one comment byte changed in a replayed body of a copied
   corpus must fail generation at the pin;
2. inputs regenerated on every run from the PE and the corpus: the loader
   image (sections at their RVAs, .reloc applied for 0x98000000, every one of
   the 413 IAT words tagged with its own VA as guest.c tag_imports does), the
   413-row import table, and a TU with the generated bodies -- the 54
   replayed bodies as lbr_t_sub_<rva> (the reference), the 3f8820 / 3f8840 /
   280cc0 helpers, getClass / getExact as __real_sub_003f8e30 /
   __real_sub_003f8c80;
3. a 32-bit MSVC host oracle per build leg (production generated-unit modes:
   GUEST_STACK_REQUIRED=1 with guest.c's five validators copied verbatim
   into the oracle, GUEST_GPR_LOCAL=1, GUEST_FLAGS_LOCAL=1,
   GUEST_GPR_FLUSH_MASKED=1, raw generated stack; GUEST_IMPORT_DIRECT=1 with
   the typed fast endpoints, and the generic-route leg) that forks every Lua
   chunk: translated bodies vs
   __wrap_ replays from one snapshot of the whole world, byte for byte
   (VERIFY legs: result, faults, owner and CPU only -- VERIFY reruns a body
   by design).  The replay TU is compiled with REPLAY_TAP_DEFS, which routes
   its one lua_pushstring call (the emulated crossing) through the oracle's
   tap, so the fixture can prove that finalizers ran inside an emulated
   pushstring; negative controls: two define mutants and one source mutant
   (SOURCE_MUTANTS);
4. optional --arm-cc leg (an ARM build host): arm-vita-eabi-gcc with the
   native inflate test's ARM flags (inflate.ARM_FLAGS) plus a hand-written
   GUEST_*/knob define list -- not the definitions CMake derives for the TU,
   which only a real configure shows -- -Werror, nm contract.

The frozen PE and the corpus come from --pe / --generated or the environment
(ISAAC_FROZEN_PE or ISAAC_PE_PATH, ISAAC_GENERATED_DIR); without them the
test prints SKIP (`--wiring-only` still runs the static CMake/README pins).
"""

from __future__ import annotations

import argparse
import concurrent.futures
import hashlib
import os
from pathlib import Path
import re
import shlex
import shutil
import struct
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parent
RUNTIME = ROOT / "runtime"
VITA = ROOT / "vita"
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(VITA))

import test_vita_lua_runtime as lua_runtime          # noqa: E402  (vcvars, Lua source pins)
import test_vita_native_inflate as inflate           # noqa: E402  (ARM flags, run)
import lua_binding_replay_table as table             # noqa: E402  (the generator)

GENERATOR = VITA / "lua_binding_replay_table.py"
GUEST_BASE = 0x98000000
GETCLASS, GETEXACT = "sub_003f8e30", "sub_003f8c80"
IMPORT_COUNT = 413
BELOW_NORMAL = 0x00004000 if os.name == "nt" else 0

require = inflate.require
quote = lua_runtime.quote

# guest.h configuration of the production generated units, shared by every
# guest.h TU of the oracle (the CPU struct and the stack helpers agree).
GUEST_DEFS = ["/DGUEST_IMAGE_BASE=0x98000000u", "/DGUEST_STACK_REQUIRED=1", "/DGUEST_GPR_LOCAL=1",
              "/DGUEST_FLAGS_LOCAL=1", "/DGUEST_GPR_FLUSH_MASKED=1", "/DGUEST_GENERATED_STACK_GUARD=0"]

# leg -> (description, defines for every TU of the leg)
LEGS = {
    "prod": ("production: direct IAT route, typed endpoints, no coverage, entry census",
             ["/DGUEST_IMPORT_DIRECT=1", "/DGUEST_COVERAGE_HOOKS=0", "/DISAAC_VITA_PROFILE_FUNCTION_ENTRIES=1",
              "/DISAAC_VITA_LUA_IMPORT_FASTDISPATCH=1"]),
    "cov": ("production + function coverage bytes compared",
            ["/DGUEST_IMPORT_DIRECT=1", "/DGUEST_COVERAGE_HOOKS=1", "/DISAAC_VITA_PROFILE_FUNCTION_ENTRIES=1",
             "/DISAAC_VITA_LUA_IMPORT_FASTDISPATCH=1"]),
    "generic": ("guest_call route only, generic endpoints (FASTDISPATCH off), coverage",
                ["/DGUEST_IMPORT_DIRECT=0", "/DGUEST_COVERAGE_HOOKS=1"]),
    "smallwin": ("production with a 0x40-byte window: deep frames cross for real",
                 ["/DGUEST_IMPORT_DIRECT=1", "/DGUEST_COVERAGE_HOOKS=0", "/DISAAC_VITA_PROFILE_FUNCTION_ENTRIES=1",
                  "/DISAAC_VITA_LUA_IMPORT_FASTDISPATCH=1", "/DISAAC_LBR_WINDOW_BELOW=0x40U"]),
    "scalar": ("production, FAMILIES=scalar only",
               ["/DGUEST_IMPORT_DIRECT=1", "/DGUEST_COVERAGE_HOOKS=0", "/DISAAC_VITA_PROFILE_FUNCTION_ENTRIES=1",
                "/DISAAC_VITA_LUA_IMPORT_FASTDISPATCH=1", "/DISAAC_VITA_LUA_BINDING_REPLAY_FAMILY_MASK=0x1U"]),
    "singles": ("production, FAMILIES=getdata;boolii;index2",
                ["/DGUEST_IMPORT_DIRECT=1", "/DGUEST_COVERAGE_HOOKS=0", "/DISAAC_VITA_PROFILE_FUNCTION_ENTRIES=1",
                 "/DISAAC_VITA_LUA_IMPORT_FASTDISPATCH=1", "/DISAAC_VITA_LUA_BINDING_REPLAY_FAMILY_MASK=0xeU"]),
    "verify": ("production + VERIFY (const scalar getters re-run translated, results compared)",
               ["/DGUEST_IMPORT_DIRECT=1", "/DGUEST_COVERAGE_HOOKS=0", "/DISAAC_VITA_PROFILE_FUNCTION_ENTRIES=1",
                "/DISAAC_VITA_LUA_IMPORT_FASTDISPATCH=1", "/DISAAC_VITA_LUA_BINDING_REPLAY_VERIFY=1"]),
    # release/v0.1.1-alpha.cmake pins TRANSLATED_CPU_GPR_FLUSH_MASK OFF: the
    # same corpus compiles to full publication (a leg define overrides GUEST_DEFS)
    "nomask": ("production with full GPR publication (GUEST_GPR_FLUSH_MASKED=0, the release pin)",
               ["/DGUEST_IMPORT_DIRECT=1", "/DGUEST_COVERAGE_HOOKS=0", "/DISAAC_VITA_PROFILE_FUNCTION_ENTRIES=1",
                "/DISAAC_VITA_LUA_IMPORT_FASTDISPATCH=1", "/DGUEST_GPR_FLUSH_MASKED=0"]),
    # negative controls (third item: the diagnostic the oracle must print)
    "mutant-result": ("NEGATIVE CONTROL: the replay's lua_pushboolean pushes the inverted value",
                      ["/DGUEST_IMPORT_DIRECT=1", "/DGUEST_COVERAGE_HOOKS=0",
                       "/DISAAC_VITA_PROFILE_FUNCTION_ENTRIES=1", "/DISAAC_VITA_LUA_IMPORT_FASTDISPATCH=1",
                       "/DISAAC_LBR_ORACLE_MUTANT=1"], "] result: T status"),
    "mutant-cpu": ("NEGATIVE CONTROL: the replay's lua_type leaves EDX different from the endpoint",
                   ["/DGUEST_IMPORT_DIRECT=1", "/DGUEST_COVERAGE_HOOKS=0",
                    "/DISAAC_VITA_PROFILE_FUNCTION_ENTRIES=1", "/DISAAC_VITA_LUA_IMPORT_FASTDISPATCH=1",
                    "/DISAAC_LBR_ORACLE_MUTANT=2"], "] CPU differs"),
    # this mutant passed the whole suite before the
    # run_gc_in_emulated_pushstring fixture existed
    "mutant-observe": ("NEGATIVE CONTROL: no lbr_observed() after the emulated lua_pushstring",
                       ["/DGUEST_IMPORT_DIRECT=1", "/DGUEST_COVERAGE_HOOKS=0",
                        "/DISAAC_VITA_PROFILE_FUNCTION_ENTRIES=1", "/DISAAC_VITA_LUA_IMPORT_FASTDISPATCH=1"],
                       "[gc in emulated pushstring: owner] translated and replayed worlds differ"),
}
# Source negative controls: the leg compiles a copy of the replay TU with
# exactly one statement replaced (old must occur once, from the current
# source, so the mutant can never silently stop being one).
SOURCE_MUTANTS = {
    "mutant-observe": ("    lbr_observed(c, x, s_import_typed[LBR_K_PUSHSTRING]); } while (0)",
                       "    (void)x; } while (0)"),
}
REPLAY_DEFS = ["/DISAAC_VITA_LUA_BINDING_REPLAY=1", "/DISAAC_VITA_LUA_NATIVE_GETCLASS=1",
               "/DISAAC_VITA_LUA_BINDING_REPLAY_ORACLE=1",
               '/DISAAC_VITA_LUA_BINDING_REPLAY_BUILD_ID=\\"oracle\\"']
# The replay TU's one lua_pushstring call is the emulated crossing
# (LBR_EMU_PUSHSTRING); in the oracle only, lua.h declares -- and that call
# reaches -- the oracle's tap (vita_lua_binding_replay_oracle.c
# lbr_oracle_tap_pushstring), which calls the real lua_pushstring.  The
# production source is untouched; no Vita build sees this definition.
REPLAY_TAP_DEFS = ["/Dlua_pushstring=lbr_oracle_tap_pushstring"]
SEAM_DEFS = ["/DISAAC_VITA_LUA_NATIVE_GETCLASS=1", "/DISAAC_VITA_LUA_NATIVE_GETCLASS_WRAP=1"]


# --- 1. generator ------------------------------------------------------------------

def generated_headers(work: Path) -> Path:
    """Where the test generates both headers (the build uses its own build tree)."""
    return work / "lbr-generated"


def run_generator(pe: Path, generated: Path, work: Path) -> None:
    out_dir = generated_headers(work)
    for name in (table.TABLE_NAME, table.MIRROR_NAME):
        require(not (RUNTIME / name).exists(),
                f"{RUNTIME / name} exists: it would shadow the generated header (delete it)")
    for mode in (["--write", "--pe", str(pe)], ["--check", "--pe", str(pe)], ["--check-corpus"]):
        completed = subprocess.run([sys.executable, str(GENERATOR), *mode, "--generated", str(generated),
                                    "--out-dir", str(out_dir)],
                                   stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        require(completed.returncode == 0, f"generator {mode[0]} failed:\n{completed.stdout}")
        print(f"generator {mode[0]}: {completed.stdout.strip()}")
    for name, digest in table.EXPECTED_SHA256.items():
        actual = hashlib.sha256((out_dir / name).read_bytes()).hexdigest()
        require(actual == digest, f"generated {name} sha256 {actual} != pinned {digest}")


def check_pin_refusal(pe: Path, generated: Path, corpus: dict, work: Path) -> None:
    """Negative control for EXPECTED_SHA256: a copy of the owner units with one
    comment byte added inside the replayed helper 3f8820 passes every
    structural check of the generator and must still fail -- at the pin --
    while the unmodified copy generates the pinned headers."""
    units = {r["unit"] for r in corpus["rows"]} | {i["unit"] for i in corpus["helpers"].values()}
    units |= {i["unit"] for i in corpus["singles"].values()} | {corpus["getclass_unit"]}
    helper_unit = corpus["helpers"][table.HELPER_SIGNED]["unit"]
    old, new = "    /* 003f8820  push ebp */\n", "    /* 003f8820  push  ebp */\n"
    for name, mutate in (("pin-control", False), ("pin-mutant", True)):
        copy = work / name
        copy.mkdir(exist_ok=True)
        for unit in sorted(units) + ["guest_table.c"]:
            data = (generated / unit).read_bytes()
            if mutate and unit == helper_unit:
                text = data.decode("utf-8")
                require(text.count(old) == 1, f"{unit}: pin mutation anchor occurs {text.count(old)} times")
                data = text.replace(old, new).encode("utf-8")
            (copy / unit).write_bytes(data)
        try:
            table.generate(pe, copy)
        except table.GenError as error:
            require(mutate and "pinned" in str(error), f"[{name}] generation failed:\n{error}")
            print(f"generator pin: one-byte comment change in {helper_unit} refused as expected "
                  f"({str(error)[:96]}...)")
            continue
        require(not mutate, "[pin-mutant] a changed corpus body generated without tripping the sha256 pin")
        print(f"generator pin: {len(units)} owner units + guest_table.c copied, pinned headers regenerated")


# --- 2. regenerated inputs -----------------------------------------------------------

def rename(body: str, symbol: str, new: str) -> str:
    header = f"void {symbol}(CPU *__restrict c)\n{{"
    require(body.startswith(header), f"{symbol}: body does not start with its definition")
    return f"void {new}(CPU *__restrict c)\n{{" + body[len(header):]


def write_inputs(pe: Path, generated: Path, work: Path) -> dict:
    text, mirror, _pe_scan, corpus, image = table.generate(pe, generated)
    out_dir = generated_headers(work)
    require(text == (out_dir / table.TABLE_NAME).read_bytes().decode("utf-8"),
            "table header differs from a fresh generation")
    require(mirror == (out_dir / table.MIRROR_NAME).read_bytes().decode("utf-8"),
            "mirror header differs from a fresh generation")

    guest_table = (generated / "guest_table.c").read_text(errors="replace")
    start = guest_table.index("static const guest_import s_imports[] = {")
    lines = re.findall(r'^\s*(\{ 0x[0-9A-Fa-f]+U, "[^"]+" \},)$', guest_table[start:], re.M)
    slots = [int(re.match(r"\{ (0x[0-9A-Fa-f]+)U", line).group(1), 16) for line in lines]
    require(len(lines) == IMPORT_COUNT, f"{len(lines)} import rows, expected {IMPORT_COUNT}")
    require(slots == sorted(slots) and len(set(slots)) == len(slots), "import rows not sorted/unique by slot")
    for slot, ident in corpus["imports"].items():
        require(slots[ident] == slot, f"dense id {ident} is not slot {slot:06x}")
    (work / "lbr_oracle_imports.inc").write_bytes(
        ("/* guest_table.c s_imports, dense order (generated by the test) */\n" + "\n".join(lines) + "\n")
        .encode("utf-8"))

    mapped = bytearray(image.mapped)
    for slot in slots:
        struct.pack_into("<I", mapped, slot, GUEST_BASE + slot)
    (work / "lbr_image.bin").write_bytes(bytes(mapped))

    owner = generated / corpus["getclass_unit"]
    owner_text = owner.read_text(errors="replace")
    parts = []
    names = []
    for row in corpus["rows"]:
        parts.append(rename(row["body"], row["symbol"], "lbr_t_" + row["symbol"]))
        names.append(row["symbol"])
    for rva, info in sorted(corpus["singles"].items()):
        parts.append(rename(info["body"], info["symbol"], "lbr_t_" + info["symbol"]))
        names.append(info["symbol"])
    helpers = []
    for rva, info in sorted(corpus["helpers"].items()):
        parts.append(info["body"])
        helpers.append(info["symbol"])
    for symbol in (GETCLASS, GETEXACT):
        parts.append(rename(table.extract_body(owner_text, symbol), symbol, "__real_" + symbol))
    referenced = set()
    for part in parts:
        referenced |= set(re.findall(r"(?<![_A-Za-z0-9])(sub_[0-9a-f]{8})\(c\)", part))
    unknown = referenced - set(helpers) - {GETCLASS}
    require(not unknown, f"bodies call symbols the oracle does not provide: {sorted(unknown)}")
    require(len(names) == 54, f"{len(names)} replayed bodies, expected 54")
    declarations = "".join(f"void {s}(CPU *__restrict c);\n" for s in sorted(referenced | set(helpers)))
    source = ("/* Extracted by recomp/test_vita_lua_binding_replay.py from the generated corpus "
              f"({generated}); compiled with the corpus flags.  Bodies renamed: the 54 replayed ones "
              "lbr_t_sub_<rva>, getClass/getExact __real_sub_<rva>. */\n"
              "#include <intrin.h>\n#include \"guest.h\"\n\n" + declarations + "\n" + "\n".join(parts))
    (work / "lbr_oracle_bodies.c").write_bytes(source.encode("utf-8"))
    print(f"inputs: image {len(mapped)} bytes ({len(slots)} IAT words tagged), {len(names)} bodies + "
          f"{len(helpers)} helpers + getClass/getExact from {len({r['unit'] for r in corpus['rows']})} units, "
          f"calls {sorted(referenced)}")
    return corpus


# The replays reach Userdata::getClass through the native seam's wrap; with
# ISAAC_VITA_LUA_GETCLASS_DIRECT that wrap runs the direct path, so this leg
# is production with the direct seam (defined after LEGS: it extends the prod
# leg's defines).
LEGS["direct"] = ("production + ISAAC_VITA_LUA_GETCLASS_DIRECT (the getClass wrap without Lua C API calls)",
                  [*LEGS["prod"][1], "/DISAAC_VITA_LUA_GETCLASS_DIRECT=1"])


# --- 3. host oracle --------------------------------------------------------------------

def run_capture(command: str, cwd: Path) -> str:
    # VSLANG=1033: MSVC diagnostics in English, so the "warning" scans below
    # cannot be defeated by a localized toolset
    completed = subprocess.run(command, cwd=cwd, text=True, shell=True, creationflags=BELOW_NORMAL,
                               env={**os.environ, "VSLANG": "1033"},
                               stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if completed.returncode:
        raise AssertionError(f"command failed ({completed.returncode}): {command}\n{completed.stdout}")
    return completed.stdout


SEED_HEADER = b"""/* test_vita_lua_binding_replay.py: a reproducible Lua string-hash seed.
 * lstate.c makeseed() mixes luai_makeseed() (time(NULL) by default) with
 * four addresses; with the oracle linked at a fixed base (/DYNAMICBASE:NO)
 * the seed is a function of the oracle's argv[3] alone, so a failing T/R
 * fork reproduces run after run. */
unsigned int lbr_oracle_lua_seed(void);
#define luai_makeseed() lbr_oracle_lua_seed()
"""


def build_lua(vcvars: Path, lua_source: Path, lua_sources: list[Path], work: Path) -> list[Path]:
    lua_dir = work / "lua-seeded"
    lua_dir.mkdir(exist_ok=True)
    seed_header = lua_dir / "lbr_oracle_luaseed.h"
    if not seed_header.is_file() or seed_header.read_bytes() != SEED_HEADER:
        seed_header.write_bytes(SEED_HEADER)
    objects = [lua_dir / f"{p.stem}.obj" for p in lua_sources]
    # --keep reuse: the sources are pristine (validated above); any newer input rebuilds all
    newest = max([s.stat().st_mtime for s in lua_sources] + [seed_header.stat().st_mtime])
    if all(o.is_file() and o.stat().st_mtime > newest for o in objects):
        return objects
    common = ["cl", "/nologo", "/std:c11", "/O2", "/D_CRT_SECURE_NO_WARNINGS", f"/I{quote(lua_source / 'src')}",
              f"/FI{quote(seed_header)}"]
    out = run_capture(f"call {quote(vcvars)} x86 >nul && " +
                      " ".join([*common, "/W3", "/c", *[quote(p) for p in lua_sources]]), lua_dir)
    require("warning" not in out.lower(), f"Lua compile warned:\n{out}")
    return objects


def input_digest(work: Path) -> str:
    """Every input a compile step can read: the runtime headers, the four
    runtime sources, the two generated binding headers and the generated
    oracle inputs (bodies, import rows)."""
    digest = hashlib.sha256()
    files = sorted(RUNTIME.glob("*.h")) + [RUNTIME / name for name in (
        "host_vita_lua.c", "host_vita_lua_getclass.c", "host_vita_lua_binding_replay.c",
        "vita_lua_binding_replay_oracle.c")] + [generated_headers(work) / name for name in (
        table.TABLE_NAME, table.MIRROR_NAME)] + [work / "lbr_oracle_bodies.c", work / "lbr_oracle_imports.inc"]
    for path in files:
        digest.update(path.name.encode() + b"\0" + path.read_bytes() + b"\0")
    return digest.hexdigest()


def build_leg(vcvars: Path, lua_source: Path, lua_objects: list[Path], work: Path, leg: str,
              digest: str) -> Path:
    # a negative control's define reaches only the replay TU (the other objects stay shared)
    mutant = [d for d in LEGS[leg][1] if d.startswith("/DISAAC_LBR_ORACLE_MUTANT=")]
    defs = [d for d in LEGS[leg][1] if d not in mutant]
    overridden = {d.split("=", 1)[0] for d in defs}
    guest_defs = [d for d in GUEST_DEFS if d.split("=", 1)[0] not in overridden]
    build = work / leg
    build.mkdir(exist_ok=True)
    cache = work / "objcache"
    cache.mkdir(exist_ok=True)
    includes = [f"/I{quote(lua_source / 'src')}", f"/I{quote(RUNTIME)}", f"/I{quote(work)}",
                f"/I{quote(generated_headers(work))}"]
    common = ["cl", "/nologo", "/std:c11", "/O2", "/D_CRT_SECURE_NO_WARNINGS", *includes, *guest_defs, *defs]
    replay_source = RUNTIME / "host_vita_lua_binding_replay.c"
    mutation = ""
    if leg in SOURCE_MUTANTS:
        old, new = SOURCE_MUTANTS[leg]
        text = replay_source.read_bytes().decode("utf-8")
        require(text.count(old) == 1, f"[{leg}] the mutated statement occurs {text.count(old)} times, expected 1")
        mutated = text.replace(old, new)
        require(new in mutated and old not in mutated, f"[{leg}] mutation not applied")
        # the copy's quoted includes resolve through /I RUNTIME and the generated
        # header directory (MSVC tries the including file's directory first,
        # which holds no header)
        replay_source = build / "host_vita_lua_binding_replay.c"
        replay_source.write_bytes(mutated.encode("utf-8"))
        mutation = hashlib.sha256(mutated.encode("utf-8")).hexdigest()
    steps = [
        # the corpus bodies: exactly the generated units' warning level
        ("lbr_oracle_bodies", [*common, "/W3", "/c", quote(work / "lbr_oracle_bodies.c")]),
        ("host_vita_lua", [*common, "/W4", "/WX", "/wd4310", SEAM_DEFS[0], "/c",
                           quote(RUNTIME / "host_vita_lua.c")]),
        ("host_vita_lua_getclass", [*common, "/W4", "/WX", "/wd4310", *SEAM_DEFS, "/c",
                                    quote(RUNTIME / "host_vita_lua_getclass.c")]),
        ("host_vita_lua_binding_replay", [*common, "/W4", "/WX", "/wd4310", *REPLAY_DEFS, *REPLAY_TAP_DEFS,
                                          *mutant, "/c", quote(replay_source)]),
        ("vita_lua_binding_replay_oracle", [*common, "/W4", "/WX", "/wd4310", *REPLAY_DEFS, *SEAM_DEFS[1:],
                                            "/c", quote(RUNTIME / "vita_lua_binding_replay_oracle.c")]),
    ]
    for stem, step in steps:
        # legs whose command for this object is identical share one compile
        # (the key covers the command and every input file's bytes, and a
        # source mutant's mutated text)
        extra = mutation if stem == "host_vita_lua_binding_replay" else ""
        key = hashlib.sha256((digest + "\0" + " ".join(step) + "\0" + extra).encode()).hexdigest()[:24]
        cached = cache / f"{stem}-{key}.obj"
        target = build / f"{stem}.obj"
        if not cached.is_file():
            out = run_capture(f"call {quote(vcvars)} x86 >nul && " + " ".join(step), build)
            require("warning" not in out.lower(), f"[{leg}] MSVC warned:\n{' '.join(step)}\n{out}")
            shutil.copyfile(target, cached)
        else:
            shutil.copyfile(cached, target)
    exe = build / f"lbr-oracle-{leg}.exe"
    objects = [*map(str, lua_objects), "lbr_oracle_bodies.obj", "host_vita_lua.obj", "host_vita_lua_getclass.obj",
               "host_vita_lua_binding_replay.obj", "vita_lua_binding_replay_oracle.obj"]
    # LARGEADDRESSAWARE: the guest image (0x98xxxxxx) and the Lua arena (0xa0xxxxxx) live above 2 GB
    run_capture(f"call {quote(vcvars)} x86 >nul && " +
                " ".join(["link", "/nologo", "/LARGEADDRESSAWARE", "/DYNAMICBASE:NO", f"/out:{quote(exe)}",
                          *[quote(o) for o in objects]]), build)
    return exe


def run_leg(exe: Path, image: Path, rounds: int, leg: str, seeds: list[int]) -> str:
    outs = []
    expect = LEGS[leg][2] if len(LEGS[leg]) > 2 else None
    for seed in (seeds if expect is None else seeds[:1]):
        completed = subprocess.run([str(exe), str(image), str(rounds), f"0x{seed:08x}"], cwd=exe.parent,
                                   text=True, creationflags=BELOW_NORMAL,
                                   stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        out = completed.stdout
        (exe.parent / f"transcript-{seed:08x}.txt").write_bytes(out.encode("utf-8", "replace"))
        if expect is None:
            require(completed.returncode == 0 and "binding replay oracle: PASS" in out,
                    f"[{leg} seed 0x{seed:08x}] oracle failed (rc={completed.returncode}):\n{out[-6000:]}")
        else:
            # negative control: a deliberately wrong replay must be caught, by the check named
            require(completed.returncode != 0 and "binding replay oracle: PASS" not in out and
                    "translated and replayed worlds differ" in out and expect in out,
                    f"[{leg}] the mutant was NOT caught as expected ({expect!r}, rc={completed.returncode}):\n"
                    f"{out[-6000:]}")
            first = next((line for line in out.splitlines() if expect in line), "")
            out = f"  mutant: caught as expected: {first.strip()[:140]}"
        outs.append(out)
    return "\n".join(outs)


def summarize(out: str) -> list[str]:
    keep = [line for line in out.splitlines()
            if line.startswith(("fixture:", "getClass", "  count", "  stats", "  stub", "  gcerr", "  gcown",
                                "  gcps", "  vgc", "  branches", "  mutant", "binding replay oracle"))]
    return keep


# --- 3b. build wiring (CMake option/FATAL blocks run under cmake -P, static pins) ----

VITA_CMAKE = VITA / "CMakeLists.txt"


def cmake_block(text: str, start: str, end: str) -> str:
    require(text.count(start) == 1, f"CMakeLists.txt: block start {start!r} not unique")
    begin = text.index(start)
    stop = text.index(end, begin)
    return text[begin:stop]


def run_cmake_script(cmake: str, work: Path, name: str, body: str, defs: dict) -> tuple[int, str]:
    script = work / f"wiring-{name}.cmake"
    script.write_bytes(body.encode())
    completed = subprocess.run([cmake, *[f"-D{k}={v}" for k, v in defs.items()], "-P", str(script)],
                               text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    # cmake wraps message() text at word boundaries: compare on single spaces
    return completed.returncode, " ".join(completed.stdout.split())


def check_wiring(work: Path, generated: Path | None = None, pe: Path | None = None) -> None:
    text = VITA_CMAKE.read_bytes().decode("utf-8")
    cmake = shutil.which("cmake")
    require(cmake is not None, "cmake not found (the wiring check runs the real option/FATAL blocks)")
    # (1) options, the getClass FATAL and the family mask, exactly as configure runs them
    options = cmake_block(text, "option(ISAAC_VITA_LUA_BINDING_REPLAY\n", "option(ISAAC_VITA_OPENAL_POOL")
    for knob in ("ISAAC_VITA_LUA_BINDING_REPLAY", "ISAAC_VITA_LUA_BINDING_REPLAY_VERIFY"):
        option = re.search(rf"option\({knob}\n\s+\"(?:[^\"\\]|\\.)*\" (ON|OFF)\)", options)
        require(option is not None and option.group(1) == "OFF", f"{knob} must default OFF")
    require('set(ISAAC_VITA_LUA_BINDING_REPLAY_FAMILIES "scalar;getdata;boolii;index2" CACHE STRING' in options,
            "FAMILIES default")
    probe = options + '\nmessage(STATUS "LBR_MASK=${ISAAC_VITA_LUA_BINDING_REPLAY_FAMILY_MASK}")\n'
    on = {"ISAAC_VITA_LUA_BINDING_REPLAY": "ON", "ISAAC_VITA_LUA_NATIVE_GETCLASS": "ON"}
    cases = (
        ("default", {}, None, "LBR_MASK=0"),
        ("on", on, None, "LBR_MASK=15"),
        ("singles", {**on, "ISAAC_VITA_LUA_BINDING_REPLAY_FAMILIES": "getdata;boolii;index2"}, None, "LBR_MASK=14"),
        ("scalar", {**on, "ISAAC_VITA_LUA_BINDING_REPLAY_FAMILIES": "scalar"}, None, "LBR_MASK=1"),
        ("verify", {**on, "ISAAC_VITA_LUA_BINDING_REPLAY_VERIFY": "ON"}, None, "LBR_MASK=15"),
        ("no-getclass", {"ISAAC_VITA_LUA_BINDING_REPLAY": "ON"},
         "ISAAC_VITA_LUA_BINDING_REPLAY requires ISAAC_VITA_LUA_NATIVE_GETCLASS=ON", None),
        ("verify-alone", {"ISAAC_VITA_LUA_NATIVE_GETCLASS": "ON", "ISAAC_VITA_LUA_BINDING_REPLAY_VERIFY": "ON"},
         "ISAAC_VITA_LUA_BINDING_REPLAY_VERIFY requires ISAAC_VITA_LUA_BINDING_REPLAY=ON", None),
        ("bad-family", {**on, "ISAAC_VITA_LUA_BINDING_REPLAY_FAMILIES": "scalar;getclass"}, "unknown family", None),
        ("twice", {**on, "ISAAC_VITA_LUA_BINDING_REPLAY_FAMILIES": "scalar;scalar"}, "listed twice", None),
        ("empty", {**on, "ISAAC_VITA_LUA_BINDING_REPLAY_FAMILIES": ""}, "FAMILIES is empty", None),
    )
    for name, defs, fatal, status in cases:
        rc, out = run_cmake_script(cmake, work, f"options-{name}", probe, defs)
        if fatal:
            require(rc != 0 and fatal in out, f"options[{name}]: expected FATAL {fatal!r}, rc={rc}:\n{out}")
        else:
            require(rc == 0 and status in out, f"options[{name}]: expected {status!r}, rc={rc}:\n{out}")
    # (2) the translated-CPU prerequisites
    prereq = cmake_block(text, "if(ISAAC_VITA_LUA_BINDING_REPLAY AND\n   (ISAAC_VITA_SCAFFOLD_ONLY", "endif()\n")
    prereq += "endif()\nmessage(STATUS LBR_PREREQ_OK)\n"
    good = {"ISAAC_VITA_LUA_BINDING_REPLAY": "ON", "ISAAC_VITA_SCAFFOLD_ONLY": "OFF",
            "ISAAC_VITA_TRANSLATED_CPU": "ON", "ISAAC_VITA_TRANSLATED_CPU_GPR_LOCAL": "ON",
            "ISAAC_VITA_TRANSLATED_CPU_FLAGS_LOCAL": "ON", "ISAAC_VITA_GUEST_STACK_GUARD": "OFF",
            "ISAAC_VITA_STALL_PROBE": "OFF"}
    rc, out = run_cmake_script(cmake, work, "prereq-good", prereq, good)
    require(rc == 0 and "LBR_PREREQ_OK" in out, f"prerequisites refused a good configuration:\n{out}")
    rc, out = run_cmake_script(cmake, work, "prereq-off", prereq, {**good, "ISAAC_VITA_LUA_BINDING_REPLAY": "OFF",
                                                                   "ISAAC_VITA_GUEST_STACK_GUARD": "ON"})
    require(rc == 0 and "LBR_PREREQ_OK" in out, f"prerequisites fired with the knob OFF:\n{out}")
    for key, bad in (("ISAAC_VITA_SCAFFOLD_ONLY", "ON"), ("ISAAC_VITA_TRANSLATED_CPU", "OFF"),
                     ("ISAAC_VITA_TRANSLATED_CPU_GPR_LOCAL", "OFF"), ("ISAAC_VITA_TRANSLATED_CPU_FLAGS_LOCAL", "OFF"),
                     ("ISAAC_VITA_GUEST_STACK_GUARD", "ON"), ("ISAAC_VITA_STALL_PROBE", "ON")):
        rc, out = run_cmake_script(cmake, work, f"prereq-{key}", prereq, {**good, key: bad})
        require(rc != 0 and "ISAAC_VITA_LUA_BINDING_REPLAY requires the generated guest runtime" in out,
                f"prerequisites accepted {key}={bad}:\n{out}")
    # (3) static pins of the wiring the configure-only checks above cannot reach
    # (anchors are code lines: the comments around these blocks may be reworded)
    wiring = cmake_block(text, "\n    if(ISAAC_VITA_LUA_BINDING_REPLAY)\n", "  if(ISAAC_VITA_CRT_ATOF_SMALLINT)")
    for needle in ('"${ISAAC_LBR_TABLE_SCRIPT}"\n          --write --pe "${ISAAC_PE_PATH}" --generated "${ISAAC_GENERATED_DIR}"\n'
                   '          --out-dir "${ISAAC_LBR_GENERATED_INCLUDE}"',
                   '"${CMAKE_CURRENT_BINARY_DIR}/isaac-lua-binding-replay"',
                   'APPEND PROPERTY INCLUDE_DIRECTORIES "${ISAAC_LBR_GENERATED_INCLUDE}")',
                   '"${ISAAC_LBR_TABLE_SCRIPT}" "${ISAAC_PE_PATH}"\n'
                   '        "${ISAAC_GENERATED_DIR}/guest_table.c" ${ISAAC_GENERATED_CANONICAL_C})',
                   '"${ISAAC_LBR_TABLE_SCRIPT}"\n          --check-corpus --generated "${ISAAC_GENERATED_DIR}"',
                   '"-Wl,--wrap=${ISAAC_LBR_SYMBOL}"', "NOT ISAAC_LBR_ROW_COUNT EQUAL 51",
                   "list(APPEND ISAAC_LBR_WRAPPED sub_00446d90)", "list(APPEND ISAAC_LBR_WRAPPED sub_00433870)",
                   "list(APPEND ISAAC_LBR_WRAPPED sub_003f90b0)",
                   '"${ISAAC_RUNTIME}/host_vita_lua_binding_replay.c")',
                   "ISAAC_VITA_LUA_BINDING_REPLAY_BUILD_ID=\\\"${ISAAC_VITA_GUEST_LINK_ID}\\\")",
                   "ISAAC_VITA_LUA_IMPORT_FASTDISPATCH=1)", "ISAAC_VITA_PROFILE_FUNCTION_ENTRIES=1)",
                   "ISAAC_VITA_LUA_BINDING_REPLAY_VERIFY=1)"):
        require(needle in wiring, f"CMake wiring lacks {needle!r}")
    # the knob and the generated-header directory reach the new TU only: no
    # existing source gains a definition or an include directory
    scoped = re.findall(r"set_property\(SOURCE\s+((?:\"[^\"]+\"\s+)+)APPEND PROPERTY", wiring)
    require(len(scoped) == 5, f"replay block: {len(scoped)} source-scoped settings (expected 4 + 1 include)")
    require(wiring.count("INCLUDE_DIRECTORIES") == 1, "replay block: exactly one INCLUDE_DIRECTORIES setting")
    for sources in scoped:
        require(re.findall(r"\"([^\"]+)\"", sources) == ["${ISAAC_RUNTIME}/host_vita_lua_binding_replay.c"],
                f"a set_property(SOURCE ...) in the replay block names another unit: {sources.strip()}")
    copy_start = "\n  if(ISAAC_VITA_LUA_BINDING_REPLAY)\n"
    copy = cmake_block(text, copy_start, 'set_property(SOURCE "${ISAAC_RUNTIME}/guest.c" APPEND PROPERTY')
    for needle in ('list(FILTER ISAAC_LBR_OWNER_DEFS INCLUDE REGEX "^GUEST_")', "owner units disagree",
                   "GUEST_GPR_LOCAL=1 GUEST_FLAGS_LOCAL=1\n            GUEST_GENERATED_STACK_GUARD=0",
                   'set_property(SOURCE "${ISAAC_RUNTIME}/host_vita_lua_binding_replay.c"\n'
                   '      APPEND PROPERTY COMPILE_DEFINITIONS ${ISAAC_LBR_GUEST_DEFS})'):
        require(needle in copy, f"CMake GUEST_* copy lacks {needle!r}")
    require(text.index("  if(ISAAC_VITA_TRANSLATED_CPU)\n    if(ISAAC_VITA_TRANSLATED_CPU_COVERAGE_HOOKS_OFF)") <
            text.index(copy_start),
            "the GUEST_* copy must run after the corpus code generation settings")
    # (4) the replay block and the GUEST_* copy themselves, run under cmake -P
    # with the target/source commands stubbed (they only record), against the
    # corpus: the configure gate passes, the --wrap list per family mask, the
    # owner units, the copied definitions and their refusals
    stubs = ("function(target_link_options)\n  message(STATUS \"LBR_TLO ${ARGN}\")\nendfunction()\n"
             "function(set_property)\n  message(STATUS \"LBR_SP ${ARGN}\")\nendfunction()\n"
             "function(get_source_file_property var file prop)\n  get_filename_component(n \"${file}\" NAME_WE)\n"
             "  set(${var} \"${LBR_DEFS_${n}}\" PARENT_SCOPE)\nendfunction()\n")
    copy_block = copy[:copy.rindex("  endif()") + len("  endif()\n")]
    if generated is not None and (generated / "guest_table.c").is_file() and pe is not None and pe.is_file():
        gen = generated.resolve().as_posix()
        binary = work / "cmake-bin"
        include_dir = (binary / "isaac-lua-binding-replay").resolve().as_posix()
        units = sorted(p.name for p in generated.glob("guest_*.c") if p.name != "guest_table.c")
        owners_expected = ["guest_0120.c", "guest_0128.c", "guest_0129.c", "guest_0131.c", "guest_0132.c",
                           "guest_0133.c", "guest_0134.c", "guest_0190.c"]
        prod_defs = "GUEST_GENERATED_STACK_GUARD=0;GUEST_COVERAGE_HOOKS=0;GUEST_FLAGS_LOCAL=1;GUEST_GPR_LOCAL=1;" \
                    "GUEST_IMPORT_DIRECT=1;GUEST_GPR_FLUSH_MASKED=1;ISAAC_VITA_OWNER_ONLY=1"
        preamble = (stubs + f'set(CMAKE_CURRENT_SOURCE_DIR "{VITA.as_posix()}")\n'
                    f'set(CMAKE_CURRENT_BINARY_DIR "{binary.resolve().as_posix()}")\n'
                    f'set(ISAAC_PE_PATH "{pe.resolve().as_posix()}")\n'
                    f'set(Python3_EXECUTABLE "{Path(sys.executable).as_posix()}")\n'
                    f'set(ISAAC_RUNTIME "{RUNTIME.as_posix()}")\nset(ISAAC_GENERATED_DIR "{gen}")\n'
                    "set(ISAAC_GENERATED_CANONICAL_C " + " ".join(f'"{gen}/{u}"' for u in units) + ")\n"
                    'set(ISAAC_VITA_DIRECT_DEFAULT_SOURCE_NAME "guest_0144.c")\n'
                    'set(ISAAC_VITA_GUEST_LINK_ID "rel:test")\n')
        block = wiring[:wiring.rindex("    endif()") + len("    endif()\n")]
        for mask, families, wraps in ((15, "scalar;getdata;boolii;index2", 54), (1, "scalar", 51),
                                      (14, "getdata;boolii;index2", 3), (8, "index2", 1)):
            defs = {"ISAAC_VITA_LUA_BINDING_REPLAY": "ON", "ISAAC_VITA_LUA_BINDING_REPLAY_FAMILY_MASK": str(mask),
                    "ISAAC_VITA_LUA_BINDING_REPLAY_FAMILIES": families, "ISAAC_VITA_DIRECT_DEFAULT": "ON",
                    "ISAAC_VITA_LUA_IMPORT_FASTDISPATCH": "ON", "ISAAC_VITA_PROFILE_FUNCTION_ENTRIES": "OFF",
                    "ISAAC_VITA_LUA_BINDING_REPLAY_VERIFY": "OFF"}
            body = (preamble + "set(ISAAC_VITA_RUNTIME_SOURCES)\n" + block +
                    "".join(f'set(LBR_DEFS_{Path(o).stem} "{prod_defs}")\n' for o in owners_expected) +
                    copy_block + 'message(STATUS "LBR_OWNERS=${ISAAC_LBR_OWNER_UNITS}")\n'
                    'message(STATUS "LBR_RUNTIME=${ISAAC_VITA_RUNTIME_SOURCES}")\n')
            rc, out = run_cmake_script(cmake, work, f"block-{mask}", body, defs)
            require(rc == 0, f"replay block (mask {mask}) failed:\n{out[-3000:]}")
            wrapped = re.findall(r"LBR_TLO isaac_first_arm_fault;PRIVATE;-Wl,--wrap=(sub_[0-9a-f]{8})", out)
            require(len(wrapped) == wraps == len(set(wrapped)), f"mask {mask}: {len(wrapped)} --wrap (expected {wraps})")
            require(("sub_003f90b0" in wrapped) == bool(mask & 8) and ("sub_00446d90" in wrapped) == bool(mask & 2)
                    and ("sub_00433870" in wrapped) == bool(mask & 4), f"mask {mask}: wrong single bodies {wrapped}")
            require("LBR_OWNERS=" + ";".join(owners_expected) in out, f"mask {mask}: owner units differ:\n{out}")
            require(f"LBR_RUNTIME={RUNTIME.as_posix()}/host_vita_lua_binding_replay.c" in out, "runtime source")
            require(f"ISAAC_VITA_LUA_BINDING_REPLAY_FAMILY_MASK={mask}U" in out, f"mask {mask}: mask define")
            copied = re.search(r"LBR_SP SOURCE;[^ ]*/host_vita_lua_binding_replay\.c;APPEND;PROPERTY;"
                               r"COMPILE_DEFINITIONS;(GUEST_[^ ]*)", out)
            require(copied is not None and copied.group(1).split(";") == sorted(
                d for d in prod_defs.split(";") if d.startswith("GUEST_")), f"mask {mask}: GUEST_* copy {copied}")
            # the configure generated both headers, pinned, into the build tree,
            # and only the replay TU got that directory
            include = re.findall(r"LBR_SP SOURCE;([^ ]*);APPEND;PROPERTY;INCLUDE_DIRECTORIES;([^ ]*)", out)
            require(include == [(f"{RUNTIME.as_posix()}/host_vita_lua_binding_replay.c", include_dir)],
                    f"mask {mask}: generated include directory {include}")
            for name, digest in table.EXPECTED_SHA256.items():
                actual = hashlib.sha256((binary / "isaac-lua-binding-replay" / name).read_bytes()).hexdigest()
                require(actual == digest, f"mask {mask}: configure-generated {name} sha256 {actual} != {digest}")
            depends = re.search(r"LBR_SP DIRECTORY;APPEND;PROPERTY;CMAKE_CONFIGURE_DEPENDS;([^ ]*)", out)
            depend_list = depends.group(1).split(";") if depends else []
            require(depend_list[:3] == [f"{VITA.as_posix()}/lua_binding_replay_table.py", pe.resolve().as_posix(),
                                        f"{gen}/guest_table.c"] and f"{gen}/guest_0120.c" in depend_list,
                    f"mask {mask}: configure dependencies {depend_list[:4]}")
        # refusals: owners that disagree, an owner without GPR locals; then a
        # foreign corpus, a missing PE and a stray header next to the replay TU
        for name, owner_defs, needle in (
                ("disagree", {"guest_0190": prod_defs.replace("GUEST_GPR_FLUSH_MASKED=1", "GUEST_GPR_FLUSH_MASKED=0")},
                 "owner units disagree"),
                ("no-gpr", {o[:-2]: prod_defs.replace("GUEST_GPR_LOCAL=1;", "") for o in owners_expected},
                 "the owner units lack GUEST_GPR_LOCAL=1")):
            merged = {Path(o).stem: prod_defs for o in owners_expected}
            merged.update(owner_defs)
            body = (stubs + 'set(ISAAC_RUNTIME "/r")\nset(ISAAC_VITA_LUA_BINDING_REPLAY ON)\n'
                    "set(ISAAC_LBR_OWNER_SOURCES " + " ".join(f'"/g/{o}"' for o in owners_expected) + ")\n" +
                    "".join(f'set(LBR_DEFS_{k} "{v}")\n' for k, v in merged.items()) + copy_block)
            rc, out = run_cmake_script(cmake, work, f"copy-{name}", body, {})
            require(rc != 0 and needle in out, f"GUEST_* copy [{name}]: expected FATAL {needle!r}:\n{out}")
        empty = work / "empty-corpus"
        empty.mkdir(exist_ok=True)
        (empty / "guest_table.c").write_bytes(b"/* no bodies */\n")
        stray = work / "stray-runtime"
        stray.mkdir(exist_ok=True)
        (stray / table.TABLE_NAME).write_bytes(b"/* stale copy */\n")
        knob = {"ISAAC_VITA_LUA_BINDING_REPLAY": "ON", "ISAAC_VITA_LUA_BINDING_REPLAY_FAMILY_MASK": "15",
                "ISAAC_VITA_DIRECT_DEFAULT": "OFF"}
        refusals = (
            ("foreign-corpus", f'set(ISAAC_GENERATED_DIR "{gen}")', f'set(ISAAC_GENERATED_DIR "{empty.as_posix()}")',
             ("could not generate the binding headers", "no generated units in")),
            ("missing-pe", f'set(ISAAC_PE_PATH "{pe.resolve().as_posix()}")',
             f'set(ISAAC_PE_PATH "{(work / "no-such-pe.exe").resolve().as_posix()}")',
             ("could not generate the binding headers", "no-such-pe.exe")),
            ("stray-header", f'set(ISAAC_RUNTIME "{RUNTIME.as_posix()}")', f'set(ISAAC_RUNTIME "{stray.as_posix()}")',
             ("would shadow the header this configure generates",)))
        for name, old, new, needles in refusals:
            require(preamble.count(old) == 1, f"refusal [{name}]: preamble line {old!r} is not unique")
            rc, out = run_cmake_script(cmake, work, f"block-{name}", preamble.replace(old, new) + block, knob)
            require(rc != 0 and all(needle in out for needle in needles),
                    f"replay block [{name}]: expected FATAL {needles!r}, rc={rc}:\n{out[-2000:]}")
        print(f"build wiring: replay block under cmake -P against {generated}: masks 15/1/14/8 -> "
              f"54/51/3/1 --wrap, owners {len(owners_expected)}, pinned headers generated into the build tree "
              f"for the replay TU only, GUEST_* copy, {2 + len(refusals)} refusals: OK")
    else:
        print("build wiring: replay block run skipped (needs --generated and --pe)")
    gate_text = (VITA / "vita_raw_allocator_gate.py").read_bytes().decode("utf-8")
    require('runtime.add("host_vita_lua_binding_replay.c")' in gate_text, "raw allocator gate registration")
    readme = (VITA / "README.md").read_bytes().decode("utf-8")
    for knob in ("ISAAC_VITA_LUA_BINDING_REPLAY", "ISAAC_VITA_LUA_BINDING_REPLAY_VERIFY",
                 "ISAAC_VITA_LUA_BINDING_REPLAY_FAMILIES"):
        require(f"`{knob}`" in readme, f"README knob table lacks {knob}")
    print(f"build wiring: options/FATAL blocks under cmake -P ({len(cases)} + 8 cases), --wrap/defines/"
          f"GUEST_* copy pins, allocator gate, README: OK")


# --- 4. ARM cross-compile (optional) ----------------------------------------------------

def arm_cross(arm_cc: str, lua_source: Path, corpus: dict, work: Path) -> None:
    cc = shlex.split(arm_cc)
    require(shutil.which(cc[0]) is not None, f"ARM compiler not found: {cc[0]}")
    tool = Path(shutil.which(cc[0]))
    nm = str(tool.with_name(tool.name[:tool.name.rindex("gcc")] + "nm"))
    common = [*inflate.ARM_FLAGS, f"-I{RUNTIME}", f"-I{VITA}", f"-I{generated_headers(work)}",
              f"-I{lua_source / 'src'}", "-Werror",
              "-DGUEST_STACK_REQUIRED=1", "-DGUEST_GPR_LOCAL=1", "-DGUEST_FLAGS_LOCAL=1",
              "-DGUEST_GPR_FLUSH_MASKED=1", "-DGUEST_GENERATED_STACK_GUARD=0", "-DGUEST_IMPORT_DIRECT=1", "-DGUEST_COVERAGE_HOOKS=0",
              "-DISAAC_VITA_LUA_IMPORT_FASTDISPATCH=1", "-DISAAC_VITA_LUA_BINDING_REPLAY=1",
              "-DISAAC_VITA_LUA_NATIVE_GETCLASS=1", '-DISAAC_VITA_LUA_BINDING_REPLAY_BUILD_ID="test"']
    poison = VITA / "isaac_vita_raw_allocator_poison.h"
    if poison.exists():
        common += ["-include", str(poison)]
    rows = [r["symbol"] for r in corpus["rows"]]
    singles = {1: "sub_00446d90", 2: "sub_00433870", 3: "sub_003f90b0"}
    for mask, extra in ((0xf, []), (0x1, []), (0xe, []), (0xf, ["-DISAAC_VITA_LUA_BINDING_REPLAY_VERIFY=1"])):
        obj = work / f"arm_replay_{mask:x}{'_verify' if extra else ''}.o"
        out = inflate.run([*cc, "-c", *common, *extra, f"-DISAAC_VITA_LUA_BINDING_REPLAY_FAMILY_MASK={mask:#x}U",
                           str(RUNTIME / "host_vita_lua_binding_replay.c"), "-o", str(obj)])
        require("warning" not in out, f"ARM compile (mask {mask:#x}) warned:\n{out}")
        symbols = inflate.run([nm, str(obj)])
        wrapped = rows if mask & 1 else []
        wrapped += [s for bit, s in singles.items() if mask & (1 << bit)]
        for symbol in wrapped:
            require(re.search(rf"\bT __wrap_{symbol}\b", symbols), f"mask {mask:#x}: no T __wrap_{symbol}")
            require(re.search(rf"\bU __real_{symbol}\b", symbols), f"mask {mask:#x}: no U __real_{symbol}")
        defined_wraps = re.findall(r"\bT (__wrap_sub_[0-9a-f]{8})\b", symbols)
        require(len(defined_wraps) == len(wrapped), f"mask {mask:#x}: {len(defined_wraps)} wraps defined")
        require(re.search(r"\bU sub_003f8e30\b", symbols), "getClass must stay an undefined reference (its --wrap)")
        require(not re.search(r"\bT sub_[0-9a-f]{8}\b", symbols), "the replay TU must not define a sub_")
    print("ARM cross-compile (inflate ARM flags + GUEST_* list, -Werror, masks 0xf/0x1/0xe/0xf+VERIFY): OK; "
          "nm contract OK")


# --- main -------------------------------------------------------------------------------

def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--pe", type=Path, default=table.env_path("ISAAC_FROZEN_PE", "ISAAC_PE_PATH"),
                        help="frozen isaac-ng.exe.unpacked.exe (or ISAAC_FROZEN_PE / ISAAC_PE_PATH)")
    parser.add_argument("--generated", type=Path, default=table.env_path("ISAAC_GENERATED_DIR"),
                        help="masked-flush corpus, guest_*.c / guest_table.c (or ISAAC_GENERATED_DIR)")
    parser.add_argument("--lua-source", type=Path, help="pristine lua-5.3.3 (or ISAAC_LUA53_SOURCE_DIR)")
    parser.add_argument("--legs", default=",".join(LEGS), help="comma list of " + ",".join(LEGS))
    parser.add_argument("--rounds", type=int, default=10)
    parser.add_argument("--seeds", default="0x15aac5ee,0x0badf00d",
                        help="Lua string-hash seeds; every leg runs once per seed (comma list)")
    parser.add_argument("--jobs", type=int, default=4)
    parser.add_argument("--arm-cc")
    parser.add_argument("--keep", type=Path, help="work directory to keep (default: temp)")
    parser.add_argument("--skip-oracle", action="store_true")
    parser.add_argument("--wiring-only", action="store_true",
                        help="only the CMake/gate/README wiring checks (no compiler; the replay-block run "
                             "also needs --pe and --generated)")
    args = parser.parse_args()
    if args.wiring_only:
        with tempfile.TemporaryDirectory(prefix="isaac-lua-binding-replay-wiring-") as scratch:
            check_wiring(Path(scratch), args.generated, args.pe)
        print("Vita LuaBridge binding replay wiring: PASS")
        return 0

    if args.pe is None or args.generated is None:
        missing = [what for what, value in (("--pe (or ISAAC_FROZEN_PE / ISAAC_PE_PATH)", args.pe),
                                            ("--generated (or ISAAC_GENERATED_DIR)", args.generated))
                   if value is None]
        print(f"Vita LuaBridge binding replay: SKIP (no frozen PE / generated corpus; pass {' and '.join(missing)}; "
              "--wiring-only runs the static wiring checks without them)")
        return 0
    require(args.pe.is_file(), f"frozen PE not found: {args.pe}")
    require((args.generated / "guest_table.c").is_file(), f"no corpus at {args.generated}")
    legs = [leg.strip() for leg in args.legs.split(",") if leg.strip()]
    seeds = [int(seed, 0) for seed in args.seeds.split(",") if seed.strip()]
    require(bool(seeds), "--seeds is empty")
    require(all(leg in LEGS for leg in legs), f"unknown leg in {legs}")
    lua_source = lua_runtime.discover_source(args.lua_source)
    lua_sources = lua_runtime.validate_source(lua_source, lua_runtime.records())
    lua_sources = [p for p in lua_sources if p.name not in ("lua.c", "luac.c")]

    tmp = None
    if args.keep:
        work = args.keep.resolve()
        work.mkdir(parents=True, exist_ok=True)
    else:
        tmp = tempfile.TemporaryDirectory(prefix="isaac-lua-binding-replay-")
        work = Path(tmp.name)
    try:
        run_generator(args.pe, args.generated, work)
        check_wiring(work, args.generated, args.pe)
        corpus = write_inputs(args.pe, args.generated, work)
        check_pin_refusal(args.pe, args.generated, corpus, work)
        if not args.skip_oracle:
            vcvars = lua_runtime.find_vcvars()
            lua_objects = build_lua(vcvars, lua_source, lua_sources, work)
            with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, min(args.jobs, 6))) as pool:
                digest = input_digest(work)
                built = {leg: pool.submit(build_leg, vcvars, lua_source, lua_objects, work, leg, digest)
                         for leg in legs}
                exes = {leg: future.result() for leg, future in built.items()}
            print(f"host oracle: built {len(exes)} legs ({', '.join(exes)})")
            with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, min(args.jobs, 6))) as pool:
                futures = {leg: pool.submit(run_leg, exe, work / "lbr_image.bin", args.rounds, leg, seeds)
                           for leg, exe in exes.items()}
                failures = []
                for leg, future in futures.items():
                    try:
                        out = future.result()
                    except AssertionError as error:
                        failures.append(str(error))
                        continue
                    print(f"host oracle [{leg}] {LEGS[leg][0]}:")
                    for line in summarize(out):
                        print("  " + line)
                require(not failures, "\n\n".join(failures))
        else:
            print("host oracle: skipped (--skip-oracle)")
        if args.arm_cc:
            arm_cross(args.arm_cc, lua_source, corpus, work)
        else:
            print("ARM cross-compile: skipped (no --arm-cc)")
    finally:
        if tmp is not None:
            tmp.cleanup()
    print("Vita LuaBridge binding replay: PASS")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except AssertionError as error:
        print(f"FAIL: {error}", file=sys.stderr)
        raise SystemExit(1)
