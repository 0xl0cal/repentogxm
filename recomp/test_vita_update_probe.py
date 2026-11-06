#!/usr/bin/env python3
"""ISAAC_VITA_UPDATE_PROBE (profiling-only --wrap timers): host oracle,
mutations that must fail it, the build gate and the ph120.ut length bound.

usage: python test_vita_update_probe.py [--pe isaac-ng.exe.unpacked.exe]

--pe defaults to $REPENTOGXM_PE; with neither, the import IDs are not
re-derived from the frozen PE and the other legs still run.
"""
from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
RUNTIME = HERE / "runtime"
VITA_CMAKE = HERE / "vita" / "CMakeLists.txt"
PROFILE_C = RUNTIME / "kage_vita_phase_profile.c"
PROBE_C = RUNTIME / "host_vita_update_probe.c"
sys.path.insert(0, str(HERE))
import build_all  # noqa: E402

RESULT = "Vita update probe oracle: PASS; probes=62"
# (description, old, new): each must make the oracle fail.
MUTATIONS = (
    ("first-window bound", "ord < 64U", "ord < 63U"),
    ("sampling period", "(ord & 63U) == 0U", "(ord & 31U) == 0U"),
    ("re-entry guard", "if (!p->open && (", "if ((p->open || 1) && ("),
    ("nested count", "p->in += s_spans - spans0;", "p->in += s_spans;"),
    ("span count", "        ++s_spans;\n        return;", "        return;"),
    ("max", "if (dt > p->mx)", "if (dt < p->mx)"),
    ("bracket residue", "& 1023U) == 512U", "& 1023U) == 511U"),
    ("body once", "    body(c);\n}", "    body(c);\n    if (ord == 70U) body(c);\n}"),
    ("take zeroes", "memset(s_slot, 0, sizeof s_slot);", ";"),
    ("rva table", "out->row[i].rva = k_rva[i];", "out->row[i].rva = 0;"),
    ("lua key offset", "ld32(c->ecx + 4U)", "ld32(c->ecx + 8U)"),
    ("init variant", "(ld32(c->esp + 8U) << 16)", "(ld32(c->esp + 4U) << 16)"),
    ("init subtype mask", "& 0xffffU);", "& 0xfffffU);"),
    ("keyed always timed", "keyed >= 0 || ord < 64U", "keyed > 0 || ord < 64U"),
    ("key entry count", "            ++e->n;\n", "            e->n = 1U;\n"),
    ("key ranking", "o->top[j - 1U].us < e->us", "o->top[j - 1U].us > e->us"),
    ("key empty slot rank", "o->top[j - 1U].n == 0U ||", "0 ||"),
    ("key overflow", "    ++t->overflow;\n", "\n"),
    ("keys reset", "    memset(s_keys, 0, sizeof s_keys);\n", "\n"),
    ("census heap group", "n[305] + n[306] + n[307] + n[309]", "n[305] + n[307] + n[309]"),
    ("census access id", "g[ISAAC_VITA_UPDATE_PROBE_GROUP_ACCESS] = n[303];", "g[ISAAC_VITA_UPDATE_PROBE_GROUP_ACCESS] = n[302];"),
    ("census delta", "o->calls[k] += g1[k] - g0[k];", "o->calls[k] += g1[k];"),
    ("composed timing", "            o->us += dt;\n", "\n"),
    ("composed reset", "    memset(s_composed, 0, sizeof s_composed);\n", "\n"),
)


def require(ok: bool, msg: str) -> None:
    if not ok:
        raise SystemExit("FAIL: " + msg)


def run_oracle(env, compiler: str, work: Path, probe_source: str, tag: str) -> tuple[int, str]:
    src = work / f"probe_{tag}.c"
    src.write_text(probe_source, encoding="utf-8", newline="\n")
    exe = work / f"probe_{tag}.exe"
    build = subprocess.run(
        [compiler, "/nologo", "/W4", "/WX", "/O2", "/std:c11", "/wd4310", "/wd4702",
         "/DGUEST_IMAGE_BASE=0x98000000u", "/DISAAC_VITA_PHASE_PROFILE=1",
         "/DISAAC_VITA_GUEST_DISPATCH_TABLE=1", "/DISAAC_VITA_UPDATE_PROBE_CENSUS=1",
         "/I", str(RUNTIME),
         str(src), str(RUNTIME / "host_vita_update_probe_oracle.c"),
         "/Fe:" + str(exe), "/Fo:" + str(work) + "\\"],
        env=env, capture_output=True, text=True)
    if build.returncode != 0:
        return -1, build.stdout + build.stderr
    done = subprocess.run([str(exe)], capture_output=True, text=True)
    return done.returncode, done.stdout


def verify_gate() -> None:
    cmake = VITA_CMAKE.read_text(encoding="utf-8")
    require(re.search(r'option\(ISAAC_VITA_UPDATE_PROBE\s+"[^"]*"\s+OFF\)', cmake) is not None,
            "option missing or not default OFF")
    require("ISAAC_VITA_UPDATE_PROBE needs the generated runtime, TRANSLATED_CPU and PHASE_PROFILE"
            in cmake, "prerequisite gate missing")
    block = re.search(r"\n  if\(ISAAC_VITA_UPDATE_PROBE\)\n(.*?)\n  endif\(\)\n", cmake, re.S)
    require(block is not None, "knob block missing")
    require(cmake.count("host_vita_update_probe.c") ==
            block.group(1).count("host_vita_update_probe.c") >= 1 and
            '"-Wl,--wrap=${_isaac_uprobe_sym}"' in block.group(1) and
            "ISAAC_VITA_UPDATE_PROBE=1" in block.group(1),
            "the seams must be reachable from the knob block only")
    header = (RUNTIME / "host_vita_update_probe.h").read_text(encoding="utf-8")
    items = re.findall(r"X\(([0-9a-f]{8})\)", header)
    require(len(items) == 62 and len(set(items)) == 62, "62 distinct probes")
    # No probe may already carry another seam's --wrap, unless CMake refuses
    # that seam's knob together with the probe: a FATAL_ERROR guard
    # if(<knob> AND ISAAC_VITA_UPDATE_PROBE) naming the symbol
    # (ISAAC_VITA_SFX_FLOOR_PRELOAD wraps Level::Init sub_00311e60).
    others = set(re.findall(r"--wrap=sub_([0-9a-f]{8})", cmake))
    exclusive = set(re.findall(
        r'if\(ISAAC_VITA_\w+ AND ISAAC_VITA_UPDATE_PROBE\)\s*'
        r'message\(FATAL_ERROR\s*"[^"]*both --wrap sub_([0-9a-f]{8})', cmake))
    require(exclusive <= set(items),
            f"exclusion guard for a non-probe: {sorted(exclusive - set(items))}")
    require(not (others & set(items)) - exclusive,
            f"double wrap: {sorted((others & set(items)) - exclusive)}")
    gate = (HERE / "vita" / "vita_raw_allocator_gate.py").read_text(encoding="utf-8")
    require('("ISAAC_VITA_UPDATE_PROBE", "host_vita_update_probe.c")' in gate,
            "raw allocator gate does not know the unit")


def verify_record_bound() -> None:
    """Upper bound of ph120.uz / ph120.ut with every number at UINT32_MAX
    (the phase-profile oracle pins the exact all-ones lengths, 125/351/379)."""
    src = PROFILE_C.read_text(encoding="utf-8")
    fn = re.search(r"static void kage_vita_profile_log_update_probe\((.*?)\n}\n", src, re.S)
    require(fn is not None, "ph120.ut printer missing")
    worst = []
    for fmt in re.findall(r'KVPP_PRINTF\(\s*((?:"[^"]*"\s*)+)', fn.group(1)):
        text = "".join(re.findall(r'"([^"]*)"', fmt)).replace("\\n", "\n")
        text = text.replace("%.32s", "B" * 32).replace("%u", "4294967295").replace("%x", "ffffffff")
        worst.append(len(text))
    require(len(worst) == 4 and max(worst) < 384, f"record bound {worst}")
    print(f"record bound: uz={worst[0]} ut={worst[1]} uc={worst[2]} uk={worst[3]} bytes (< 384)")


def verify_import_ids(frozen_pe: Path) -> None:
    """The census IDs in host_vita_update_probe.c, re-derived from the PE."""
    import pefile
    pe = pefile.PE(str(frozen_pe))
    rows = sorted((i.address - pe.OPTIONAL_HEADER.ImageBase, (i.name or b"").decode())
                  for e in pe.DIRECTORY_ENTRY_IMPORT for i in e.imports)
    want = {305: "free", 306: "calloc", 307: "malloc", 309: "realloc", 303: "_access",
            368: "fopen", 359: "fread", 361: "fseek", 61: "EnterCriticalSection",
            383: "strncpy_s", 385: "strcat_s", 281: "memcpy", 284: "memmove"}
    require(len(rows) == 413, f"{len(rows)} imports")
    for k, name in want.items():
        require(rows[k][1] == name, f"import {k} is {rows[k][1]}, not {name}")
    src = PROBE_C.read_text(encoding="utf-8")
    for k in want:
        require(f"n[{k}]" in src, f"probe does not read import {k}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    env_pe = os.environ.get("REPENTOGXM_PE")
    parser.add_argument("--pe", type=Path, default=Path(env_pe) if env_pe else None,
                        help="frozen isaac-ng.exe.unpacked.exe (default: $REPENTOGXM_PE)")
    args = parser.parse_args()
    verify_gate()
    if args.pe is None:
        print("import IDs: SKIP (no --pe and no REPENTOGXM_PE; not re-derived from the frozen PE)")
    elif not args.pe.is_file():
        print(f"frozen PE not found: {args.pe}", file=sys.stderr)
        return 2
    else:
        verify_import_ids(args.pe)
        print("import IDs: PASS (re-derived from the frozen PE)")
    verify_record_bound()
    env, compiler = build_all.msvc_env()
    base = PROBE_C.read_text(encoding="utf-8")
    with tempfile.TemporaryDirectory() as tmp:
        work = Path(tmp)
        rc, out = run_oracle(env, compiler, work, base, "base")
        require(rc == 0 and out.strip().splitlines()[-1:] == [RESULT], f"oracle: rc={rc}\n{out}")
        killed = 0
        for i, (what, old, new) in enumerate(MUTATIONS):
            require(base.count(old) == 1, f"mutation anchor not unique: {what}")
            rc, out = run_oracle(env, compiler, work, base.replace(old, new), f"m{i}")
            if rc != 0:
                killed += 1
            else:
                print(f"SURVIVED mutation: {what}")
        require(killed == len(MUTATIONS), f"mutations {killed}/{len(MUTATIONS)}")
    print(RESULT)
    print(f"Vita update probe: PASS; mutations {killed}/{len(MUTATIONS)} killed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
