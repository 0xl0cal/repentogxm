#!/usr/bin/env python3
"""Proofs for the native Userdata::getClass / getExact seam
(ISAAC_VITA_LUA_NATIVE_GETCLASS, recomp/runtime/host_vita_lua_getclass.[ch]).

1. frozen PE pins: the two prologues, the identity-key push, the plain `ret`,
   a caller of each (`call; add esp, 8|4`), the five .rdata strings, and the
   RVAs the seam header hard-codes;
2. generated corpus pins: both bodies owned by guest_0120.c with their
   coverage ids, ZERO in-TU callers (so --wrap catches every one of the
   917 + 65 direct sites and the guest_table.c dispatch slot), the
   GUEST_IMPORT_CALL multiset of each body (the algorithm the seam replays);
3. a 32-bit MSVC host oracle (raw lua_State/userdata pointers cross the
   bridge; the bodies address the PE's IAT/.rdata pages absolutely, so the
   oracle maps those pages at their guest addresses): the generated bodies
   extracted from the corpus + the production bridge host_vita_lua.c + the
   seam + pristine Lua 5.3.3, translated body vs __wrap_ on one CPU and one
   Lua state -- plain and VERIFY builds;
4. optional --arm-cc leg: eboot flags, -Werror, nm contract.
"""

from __future__ import annotations

import argparse
from collections import Counter
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parent
RUNTIME = ROOT / "runtime"
sys.path.insert(0, str(ROOT))

import test_vita_lua_runtime as lua_runtime          # noqa: E402  (vcvars, Lua source pins)
import test_vita_native_inflate as inflate           # noqa: E402  (frozen PE class/pins, ARM flags)

GUEST_BASE = 0x98000000
GETCLASS, GETEXACT = "sub_003f8e30", "sub_003f8c80"
OWNER_UNIT = "guest_0120.c"

# --- frozen PE pins ------------------------------------------------------------

CODE_PINS = [
    (0x3f8e30, "55 8b ec 83 ec 0c 53 56 57 ff 75 08 8b f9 8b f2 68 d8 b9 f0 ff 57 89 75 f8 33 db ff 15 cc 61 a0",
     "getClass prologue: edi=L(ecx) esi=index(edx) push [ebp+8]=classKey; rawgetp(REGISTRY, key)"),
    (0x3f8c80, "55 8b ec 51 53 56 57 8b f9 6a 01 57 ff 15 20 62 a0 00 ff 75 08 8b 35 cc 61 a0 00 8b d8 33 c0 68",
     "getExact prologue: edi=L(ecx) absindex(L, 1) push [ebp+8]=classKey; rawgetp"),
    (0x3f8e6c, "68 40 41 c0 00", "getClass: push identity key 0xc04140 for rawgetp(mt, key)"),
    (0x3f8cc5, "68 40 41 c0 00", "getExact: push identity key 0xc04140"),
    (0x3f8fa0, "c3", "getClass: plain ret on the match path (caller does add esp, 8)"),
    (0x3f9070, "c3", "getClass: plain ret after the error path"),
    (0x40cc22, "e8 09 c2 fe ff 83 c4 08", "a getClass caller: call 0x3f8e30; add esp, 8 (return word 0x40cc27)"),
    (0x45d47b, "e8 00 b8 f9 ff 83 c4 04", "a getExact caller: call 0x3f8c80; add esp, 4 (return word 0x45d480)"),
]
MSG_PINS = [
    (0x74f298, "__type"),
    (0x74f2a0, "%s expected, got %s"),
    (0x74f2b4, "__const"),
    (0x74f314, "cannot be const"),
    (0x74f324, "__parent"),
]
HEADER_PINS = {
    "ISAAC_LGC_GETCLASS_RVA": 0x3f8e30, "ISAAC_LGC_GETEXACT_RVA": 0x3f8c80,
    "ISAAC_LGC_GETCLASS_COVERAGE": 4588, "ISAAC_LGC_GETEXACT_COVERAGE": 4587,
    "ISAAC_LGC_IDENTITY_KEY_RVA": 0x804140,
    "ISAAC_LGC_STR_TYPE_RVA": 0x74f298, "ISAAC_LGC_STR_FORMAT_RVA": 0x74f2a0,
    "ISAAC_LGC_STR_CONST_RVA": 0x74f2b4, "ISAAC_LGC_STR_CANNOT_CONST_RVA": 0x74f314,
    "ISAAC_LGC_STR_PARENT_RVA": 0x74f324,
}

# --- corpus pins -----------------------------------------------------------------

# (IAT slot RVA, dense import id) -> count of GUEST_IMPORT_CALL sites in the body
GETCLASS_IMPORTS = {
    (0x606220, 133): 5, (0x6061ac, 104): 5, (0x606194, 98): 5, (0x60620c, 128): 4,
    (0x606214, 130): 3, (0x606170, 89): 3, (0x6061d4, 114): 2, (0x6061cc, 112): 2,
    (0x606188, 95): 2, (0x60622c, 136): 1, (0x60621c, 132): 1, (0x606210, 129): 1,
    (0x6061d8, 115): 1, (0x6061b4, 106): 1, (0x6061b0, 105): 1, (0x606160, 85): 1,
}
GETEXACT_IMPORTS = {
    (0x606220, 133): 4, (0x606170, 89): 4, (0x6061ac, 104): 3, (0x606194, 98): 3,
    (0x606210, 129): 2, (0x60620c, 128): 2, (0x6061d4, 114): 2, (0x6061b0, 105): 2,
    (0x60621c, 132): 1, (0x606214, 130): 1, (0x6061d8, 115): 1, (0x6061cc, 112): 1,
    (0x6061b4, 106): 1, (0x606188, 95): 1, (0x606160, 85): 1,
}
SLOT_NAMES = {
    0x606160: "lua_typename", 0x606170: "lua_settop", 0x606188: "luaL_argerror",
    0x606194: "lua_rawget", 0x6061ac: "lua_pushstring", 0x6061b0: "lua_touserdata",
    0x6061b4: "lua_pushfstring", 0x6061cc: "lua_rawgetp", 0x6061d4: "lua_tolstring",
    0x6061d8: "lua_isuserdata", 0x60620c: "lua_type", 0x606210: "lua_rawequal",
    0x606214: "lua_rotate", 0x60621c: "lua_getmetatable", 0x606220: "lua_absindex",
    0x60622c: "lua_copy",
}
UNTYPED_CALLS = {GETCLASS: 5, GETEXACT: 2}   # `call ebx` x5 (settop) / `call esi` x2 (rawgetp, settop)
GETCLASS_SITES, GETEXACT_SITES = 917, 65
GETCLASS_CALLER = ("guest_0122.c", "GPUSH(0x40cc27U); GUEST_STACK_CALLSITE_BARRIER(); GUEST_GPR_FLUSH(c); "
                                   "sub_003f8e30(c); GUEST_GPR_RELOAD(c);")
GETEXACT_CALLER = ("guest_0190.c", "GPUSH(0x45d480U); GUEST_STACK_CALLSITE_BARRIER(); GUEST_GPR_FLUSH(c); "
                                   "sub_003f8c80(c); GUEST_GPR_RELOAD(c);")

# guest.h configuration of the production corpus / runtime, shared by every
# guest.h TU of the oracle so the CPU struct and the stack helpers agree.
GUEST_DEFS = ["/DGUEST_IMAGE_BASE=0x98000000u", "/DGUEST_GPR_LOCAL=1", "/DGUEST_FLAGS_LOCAL=1",
              "/DGUEST_IMPORT_DIRECT=0", "/DGUEST_COVERAGE_HOOKS=1"]
SEAM_DEFS = ["/DISAAC_VITA_LUA_NATIVE_GETCLASS=1", "/DISAAC_VITA_LUA_NATIVE_GETCLASS_WRAP=1"]

require = inflate.require
quote = lua_runtime.quote


# --- 1. frozen PE ----------------------------------------------------------------

def verify_pe(path: Path) -> None:
    require(path.stat().st_size == inflate.PE_SIZE, "frozen PE size changed")
    require(inflate.sha256(path) == inflate.PE_SHA256, "frozen PE hash changed")
    pe = inflate.PE(path)
    for rva, hexbytes, what in CODE_PINS:
        want = bytes.fromhex(hexbytes.replace(" ", ""))
        got = pe.read(rva, len(want))
        require(got == want, f"PE code pin at 0x{rva:x} ({what}): {got.hex(' ')} != {want.hex(' ')}")
    for rva, text in MSG_PINS:
        want = text.encode("ascii") + b"\0"
        got = pe.read(rva, len(want))
        require(got == want, f"PE string at 0x{GUEST_BASE + rva:08x} is {got!r}, expected {want!r}")
    data = [s for s in pe.sections if s[0] == ".data"]
    require(len(data) == 1 and data[0][1] <= 0x804140 < data[0][1] + data[0][2],
            "identity key 0xc04140 is not in .data")
    header = (RUNTIME / "host_vita_lua_getclass.h").read_text()
    for macro, value in HEADER_PINS.items():
        found = re.search(rf"#define\s+{macro}\s+(0x[0-9a-fA-F]+|\d+)U?", header)
        require(found is not None and int(found.group(1), 0) == value,
                f"{macro} in host_vita_lua_getclass.h != 0x{value:x}")
    print(f"frozen PE: {len(CODE_PINS)} code pins, {len(MSG_PINS)} strings, identity key in .data, "
          f"{len(HEADER_PINS)} header RVAs pinned")


# --- 2. generated corpus ---------------------------------------------------------

def extract_body(text: str, symbol: str) -> str:
    header = f"void {symbol}(CPU *__restrict c)\n{{"
    require(text.count(header) == 1, f"{symbol} must be defined exactly once in its unit")
    start = text.index(header)
    end = text.find("\n/* sub_", start)
    return text[start:end if end >= 0 else len(text)].rstrip() + "\n"


def import_sites(body: str) -> Counter:
    return Counter((int(slot, 16), int(ident))
                   for slot, ident in re.findall(r"GUEST_IMPORT_CALL\(_target, (0x[0-9a-fA-F]+)U, (\d+)U\)", body))


def verify_generated(generated: Path) -> tuple[str, str]:
    sources = sorted(generated.glob("guest_*.c"))
    require(sources, f"no generated units in {generated}")
    owner_text = (generated / OWNER_UNIT).read_text(errors="replace")
    bodies = {}
    for symbol, coverage, imports, sites in ((GETCLASS, 4588, GETCLASS_IMPORTS, GETCLASS_SITES),
                                             (GETEXACT, 4587, GETEXACT_IMPORTS, GETEXACT_SITES)):
        body = extract_body(owner_text, symbol)
        require(f"guest_coverage_function({coverage}U);" in body[:400], f"{symbol} coverage id != {coverage}")
        require(f"{symbol}(c)" not in owner_text, f"{symbol} has an in-TU caller: --wrap would miss it")
        require(re.search(rf"\b{symbol}\(", body[len(symbol) + 6:]) is None, f"{symbol} is recursive?")
        got = import_sites(body)
        require(got == Counter(imports), f"{symbol} GUEST_IMPORT_CALL multiset drifted:\n{sorted(got.items())}")
        # register-indirect `call ebx/esi` through a cached IAT word the emitter
        # could not type: still import crossings (lua_settop), dispatched by guest_call
        untyped = body.count("guest_call(c, _target)")
        require(untyped == UNTYPED_CALLS[symbol], f"{symbol}: {untyped} untyped register calls")
        require("GUEST_IMPORT_JMP(" not in body and body.count("guest_call(") == untyped,
                f"{symbol} must only reach imports through GUEST_IMPORT_CALL / guest_call")
        callers = 0
        for source in sources:
            if source.name == OWNER_UNIT:
                continue
            text = source.read_text(errors="replace")
            require(f"void {symbol}(CPU" not in text, f"{symbol} also defined in {source.name}")
            callers += len(re.findall(rf"\b{symbol}\(c\)", text))
        require(callers == sites, f"{symbol}: {callers} translated call sites, expected {sites}")
        bodies[symbol] = body
    for unit, line in (GETCLASS_CALLER, GETEXACT_CALLER):
        require(line in (generated / unit).read_text(errors="replace"), f"pinned caller in {unit} drifted")
    table = (generated / "guest_table.c").read_text(errors="replace")
    for symbol in (GETCLASS, GETEXACT):
        rva = symbol[4:]
        require(re.search(rf"\b0x{rva}U\b", table) and re.search(rf"\b{symbol}\b", table),
                f"{symbol} is not in the guest_table.c dispatch table")
    start = table.index("static const guest_import s_imports[] = {")
    entries = dict((int(rva, 16), name) for rva, name in
                   re.findall(r"^\s*\{ (0x[0-9A-Fa-f]+)U, \"([^\"]+)\" \},", table[start:], re.M))
    for slot, name in SLOT_NAMES.items():
        require(entries.get(slot) == f"Lua5.3.3r.dll!{name}", f"IAT slot 0x{slot:x} is {entries.get(slot)}")
    print(f"generated corpus: {GETCLASS}/{GETEXACT} owned by {OWNER_UNIT} (cov 4588/4587), 0 in-TU callers, "
          f"{GETCLASS_SITES}/{GETEXACT_SITES} translated call sites + guest_table.c dispatch slots, "
          f"{sum(GETCLASS_IMPORTS.values())}/{sum(GETEXACT_IMPORTS.values())} GUEST_IMPORT_CALL sites pinned "
          f"({len(SLOT_NAMES)} Lua imports named)")
    return bodies[GETCLASS], bodies[GETEXACT]


# --- 3. host oracle ---------------------------------------------------------------

def run_capture(command: str, cwd: Path) -> str:
    completed = subprocess.run(command, cwd=cwd, text=True, shell=True,
                               stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if completed.returncode:
        raise AssertionError(f"command failed ({completed.returncode}): {command}\n{completed.stdout}")
    return completed.stdout


def build_oracle(vcvars: Path, lua_source: Path, lua_sources: list[Path], build: Path,
                 getclass_body: str, getexact_body: str) -> dict[str, Path]:
    generated = build / "lgc_oracle_generated.c"
    generated.write_text(
        "/* Extracted by recomp/test_vita_lua_getclass.py from the generated corpus "
        f"({OWNER_UNIT}); compiled with the corpus flags. */\n"
        "#include <intrin.h>\n#include \"guest.h\"\n\n" + getclass_body + "\n" + getexact_body,
        encoding="utf-8")
    includes = [f"/I{quote(lua_source / 'src')}", f"/I{quote(RUNTIME)}"]
    common = ["cl", "/nologo", "/std:c11", "/O2", "/D_CRT_SECURE_NO_WARNINGS", *includes]
    vc = f"call {quote(vcvars)} x86 >nul && "
    steps = [
        " ".join([*common, "/W3", "/c", *[quote(p) for p in lua_sources]]),
        # the corpus TU: raw generated stack (GUEST_GENERATED_STACK_GUARD=0) exactly like the eboot's units
        " ".join([*common, "/W3", *GUEST_DEFS, "/DGUEST_GENERATED_STACK_GUARD=0", "/c", quote(generated)]),
        # the production bridge with the knob
        " ".join([*common, "/W4", "/WX", "/wd4310", *GUEST_DEFS, SEAM_DEFS[0], "/c",
                  quote(RUNTIME / "host_vita_lua.c")]),
    ]
    variants = {}
    for tag, extra in (("plain", []), ("verify", ["/DISAAC_VITA_LUA_NATIVE_GETCLASS_VERIFY=1"])):
        for source in ("host_vita_lua_getclass.c", "host_vita_lua_getclass_guest_oracle.c"):
            steps.append(" ".join([*common, "/W4", "/WX", "/wd4310", *GUEST_DEFS, *SEAM_DEFS, *extra, "/c",
                                   quote(RUNTIME / source), f"/Fo{quote(build / f'{tag}_{source[:-2]}.obj')}"]))
        exe = build / f"lgc-guest-oracle-{tag}.exe"
        objects = [f"{p.stem}.obj" for p in lua_sources] + ["lgc_oracle_generated.obj", "host_vita_lua.obj",
                                                            f"{tag}_host_vita_lua_getclass.obj",
                                                            f"{tag}_host_vita_lua_getclass_guest_oracle.obj"]
        # LARGEADDRESSAWARE: the guest IAT/.rdata pages live above 2 GB (0x98xxxxxx)
        steps.append(" ".join(["link", "/nologo", "/LARGEADDRESSAWARE", f"/out:{quote(exe)}", *objects]))
        variants[tag] = exe
    for step in steps:
        out = run_capture(vc + step, build)
        require("warning" not in out.lower() or step.startswith("link"), f"MSVC warned:\n{step}\n{out}")
    return variants


def run_oracle(exe: Path, tag: str) -> None:
    out = run_capture(quote(exe), exe.parent)
    require("native getclass guest oracle: PASS" in out, f"{tag} oracle failed:\n{out}")
    print(f"host oracle [{tag}]:")
    print("  " + out.strip().replace("\n", "\n  "))


# --- 4. ARM cross-compile (optional) ---------------------------------------------

def arm_cross(arm_cc: str, lua_source: Path, work: Path) -> None:
    import shlex
    import shutil
    cc = shlex.split(arm_cc)
    require(shutil.which(cc[0]) is not None, f"ARM compiler not found: {cc[0]}")
    tool = Path(shutil.which(cc[0]))
    nm = str(tool.with_name(tool.name[:tool.name.rindex("gcc")] + "nm"))
    common = [*inflate.ARM_FLAGS, f"-I{RUNTIME}", f"-I{lua_source / 'src'}", "-Werror",
              "-DISAAC_VITA_LUA_NATIVE_GETCLASS=1"]
    plan = [("seam", RUNTIME / "host_vita_lua_getclass.c",
             ["-DISAAC_VITA_LUA_NATIVE_GETCLASS_WRAP=1", '-DISAAC_VITA_LUA_NATIVE_GETCLASS_BUILD_ID="test"']),
            ("seam_verify", RUNTIME / "host_vita_lua_getclass.c",
             ["-DISAAC_VITA_LUA_NATIVE_GETCLASS_WRAP=1", "-DISAAC_VITA_LUA_NATIVE_GETCLASS_VERIFY=1"]),
            ("bridge", RUNTIME / "host_vita_lua.c", [])]
    objects = []
    for name, source, flags in plan:
        obj = work / f"arm_{name}.o"
        out = inflate.run([*cc, "-c", *common, *flags, str(source), "-o", str(obj)])
        require("warning" not in out, f"ARM compile of {name} warned:\n{out}")
        objects.append(obj)
    symbols = inflate.run([nm, str(objects[0]), str(objects[2])])
    for want in (r"\bT __wrap_sub_003f8e30\b", r"\bT __wrap_sub_003f8c80\b", r"\bU __real_sub_003f8e30\b",
                 r"\bU __real_sub_003f8c80\b", r"\bT isaac_vita_lua_seam_enter\b", r"\bT isaac_vita_lua_seam_leave\b",
                 r"\bT isaac_vita_lua_getclass_try\b", r"\bT isaac_vita_lua_getexact_try\b"):
        require(re.search(want, symbols, re.M), f"nm contract: {want} missing")
    print("ARM cross-compile (eboot flags, -Werror): OK; nm contract OK (__wrap_/__real_ x2, seam_enter/leave)")


# --- main ------------------------------------------------------------------------

def default_pe() -> Path | None:
    environment = os.environ.get("ISAAC_FROZEN_PE")
    if environment and Path(environment).is_file():
        return Path(environment)
    return None


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--pe", type=Path, default=default_pe(), help="frozen isaac-ng.exe.unpacked.exe")
    parser.add_argument("--generated", type=Path, required=True, help="directory with guest_*.c / guest_table.c")
    parser.add_argument("--lua-source", type=Path, help="pristine lua-5.3.3 (or ISAAC_LUA53_SOURCE_DIR)")
    parser.add_argument("--arm-cc")
    parser.add_argument("--keep", type=Path, help="work directory to keep (default: temp)")
    parser.add_argument("--skip-oracle", action="store_true")
    args = parser.parse_args()

    require(args.pe is not None and args.pe.is_file(), "pass --pe <isaac-ng.exe.unpacked.exe>")
    verify_pe(args.pe)
    getclass_body, getexact_body = verify_generated(args.generated)
    lua_source = lua_runtime.discover_source(args.lua_source)
    lua_sources = lua_runtime.validate_source(lua_source, lua_runtime.records())
    lua_sources = [p for p in lua_sources if p.name not in ("lua.c", "luac.c")]

    tmp = None
    if args.keep:
        work = args.keep.resolve()
        work.mkdir(parents=True, exist_ok=True)
    else:
        tmp = tempfile.TemporaryDirectory(prefix="isaac-lua-getclass-")
        work = Path(tmp.name)
    try:
        if not args.skip_oracle:
            vcvars = lua_runtime.find_vcvars()
            variants = build_oracle(vcvars, lua_source, lua_sources, work, getclass_body, getexact_body)
            for tag, exe in variants.items():
                run_oracle(exe, tag)
        else:
            print("host oracle: skipped (--skip-oracle)")
        if args.arm_cc:
            arm_cross(args.arm_cc, lua_source, work)
        else:
            print("ARM cross-compile: skipped (no --arm-cc)")
    finally:
        if tmp is not None:
            tmp.cleanup()
    print("Vita native getClass/getExact seam: PASS")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except AssertionError as error:
        print(f"FAIL: {error}", file=sys.stderr)
        raise SystemExit(1)
