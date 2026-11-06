#!/usr/bin/env python3
"""Host test for recomp/vita/vita_render_watch.py and the census hook it feeds
(ISAAC_VITA_PROFILE_FUNCTION_ENTRY_CENSUS).

  python test_vita_render_watch.py [--coverage GENERATED/guest_coverage.json]

1. Generates guest_entry_census_ids.h from a synthetic corpus id space (the
   real watch list plus unwatched functions; ids = sorted positions, the
   gen_all.py rule) and checks the header: count, corpus identity, RVA table
   in watch order, one switch case per entry with the id of that RVA.
2. A second run with unchanged inputs keeps the file's mtime (no corpus-wide
   rebuild); --check writes nothing.
3. Each refusal: missing macro, duplicate entry, more than 64 entries, an RVA
   that is not a coverage function, a function list that does not hash to its
   own function_rvas_sha256, ids that are not sorted positions, a
   function_count that disagrees, id 0 watched.  Each must exit 1 with a FAIL
   line and leave no output file.
4. Compiles one host TU per hook through guest.h with the generated header
   (clang/gcc/cc from PATH, -O2): a watched id's entry hook references the
   census rows and the sampler phase word, an unwatched id's and the runtime
   seams' empty-argument hook reference neither (the hook folds to nothing).
5. With --coverage: the real corpus's guest_coverage.json passes --check and
   prints the id range (the configure-time run CMake does).
6. With --coverage, against the generated units next to it: every watched
   generated body opens with guest_coverage_function(<its json id>U), and a
   watched manual body's id is a GUEST_COVERAGE_MANUAL_*_ID of the corpus
   header that a runtime manual_*.c source passes to the hook.
The sampler-side ph120.fe records are pinned by scenario 9 of
kage_vita_guest_sampler_oracle.c (test_kage_vita_guest_sampler.py).
"""
from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

VITA = Path(__file__).resolve().parent
RUNTIME = VITA.parent / "runtime"
WATCH = RUNTIME / "kage_vita_render_watch.h"
SCRIPT = VITA / "vita_render_watch.py"

sys.path.insert(0, str(VITA))
import vita_render_watch as watch  # noqa: E402


def compiler() -> str:
    for name in ("cc", "gcc", "clang"):
        found = shutil.which(name)
        if found:
            return found
    raise AssertionError("no host C compiler (cc/gcc/clang) on PATH")


def generate(watch_path: Path, coverage: Path, output: Path | None,
             check: bool = False) -> subprocess.CompletedProcess:
    command = [sys.executable, str(SCRIPT), "--watch", str(watch_path),
               "--coverage", str(coverage)]
    if output is not None:
        command += ["--output", str(output)]
    if check:
        command.append("--check")
    return subprocess.run(command, capture_output=True, text=True, check=False)


def coverage_doc(rvas: list[int]) -> dict:
    ordered = sorted(rvas)
    return {
        "function_count": len(ordered),
        "function_rvas_sha256": watch.rows_sha256(ordered),
        "generation_id": "gen:render-watch-test",
        "functions": [{"id": i, "rva": rva} for i, rva in enumerate(ordered)],
    }


def write_json(path: Path, data: dict) -> Path:
    path.write_text(json.dumps(data), encoding="utf-8")
    return path


def expect_fail(result: subprocess.CompletedProcess, needle: str,
                output: Path, label: str) -> None:
    if result.returncode != 1 or "vita_render_watch: FAIL" not in result.stderr \
            or needle not in result.stderr:
        raise AssertionError(
            f"{label}: expected FAIL with {needle!r}, got rc={result.returncode}\n"
            f"{result.stdout}{result.stderr}")
    if output.exists():
        raise AssertionError(f"{label}: a refused run left {output}")
    print(f"refusal {label}: {result.stderr.strip()}")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--coverage", type=Path,
                    help="a real corpus guest_coverage.json to check as well")
    args = ap.parse_args()

    entries = watch.read_watch(WATCH)
    if not 0 < len(entries) <= watch.WATCH_MAX:
        raise AssertionError(f"watch list has {len(entries)} entries")
    # Unwatched functions on both sides of the watched ones; 0x1000 is id 0.
    extra = [0x00001000, 0x000023f0, 0x00100000, 0x005a0de0, 0x00605ff0]
    rvas = sorted(set(entries) | set(extra))
    by_rva = {rva: i for i, rva in enumerate(rvas)}

    with tempfile.TemporaryDirectory(prefix="isaac-render-watch-") as value:
        tmp = Path(value)
        coverage = write_json(tmp / "guest_coverage.json", coverage_doc(rvas))
        header = tmp / "out" / "guest_entry_census_ids.h"

        # 1. the generated header
        result = generate(WATCH, coverage, header)
        if result.returncode != 0:
            raise AssertionError(f"generation failed:\n{result.stderr}")
        text = header.read_text(encoding="utf-8")
        doc = json.loads(coverage.read_text(encoding="utf-8"))
        for needle in (f"#define GUEST_ENTRY_CENSUS_COUNT {len(entries)}U",
                       f"#define GUEST_ENTRY_CENSUS_FUNCTION_COUNT {len(rvas)}U",
                       f'#define GUEST_ENTRY_CENSUS_FUNCTION_RVAS_SHA256 '
                       f'"{doc["function_rvas_sha256"]}"',
                       '#define GUEST_ENTRY_CENSUS_GENERATION_ID '
                       '"gen:render-watch-test"',
                       "    default: return UINT32_MAX;"):
            if needle not in text:
                raise AssertionError(f"generated header lacks {needle!r}")
        cases = re.findall(r"    case (\d+)U: return (\d+)U;  /\* 0x([0-9a-f]{8}) \*/",
                           text)
        if [(int(c), int(s), int(r, 16)) for c, s, r in cases] != \
                [(by_rva[rva], slot, rva) for slot, rva in enumerate(entries)]:
            raise AssertionError("switch cases are not watch order -> sorted ids")
        table = re.search(r"#define GUEST_ENTRY_CENSUS_RVAS \{ \\\n(.*?)\n\}", text,
                          re.S)
        if table is None or [int(v, 16) for v in re.findall(
                r"0x([0-9a-f]{8})U", table[1])] != entries:
            raise AssertionError("RVA table is not the watch list in order")
        print(f"generated: {result.stdout.strip()}")

        # 2. unchanged inputs keep the mtime; --check writes nothing
        old = header.stat().st_mtime_ns - 10_000_000_000
        os.utime(header, ns=(old, old))
        result = generate(WATCH, coverage, header)
        if result.returncode != 0 or header.stat().st_mtime_ns != old:
            raise AssertionError("an unchanged regeneration rewrote the header")
        check_out = tmp / "check" / "guest_entry_census_ids.h"
        result = generate(WATCH, coverage, None, check=True)
        if result.returncode != 0 or check_out.exists() or (tmp / "check").exists():
            raise AssertionError(f"--check failed or wrote output:\n{result.stderr}")
        print("idempotent: unchanged inputs keep the mtime; --check writes nothing")

        # 3. refusals
        lines = WATCH.read_text(encoding="utf-8").splitlines(keepends=True)
        entry_at = [i for i, line in enumerate(lines) if watch.RE_ENTRY.match(line)]
        first, second = entry_at[0], entry_at[1]
        # The real list may be full (64 = WATCH_MAX since the watch list
        # grew to 64), so each variant below replaces an entry instead of adding one
        # and only the rule under test can fire; "too many" adds exactly
        # WATCH_MAX + 1 - len(entries) entries (the boundary).

        def watch_variant(name: str, mutate) -> Path:
            path = tmp / f"watch-{name}.h"
            path.write_text("".join(mutate(list(lines))), encoding="utf-8")
            return path

        refused = tmp / "refused" / "guest_entry_census_ids.h"
        expect_fail(generate(watch_variant(
            "nomacro", lambda ls: [line.replace("KAGE_VITA_RENDER_WATCH(X)",
                                                "KAGE_VITA_RENDER_LIST(X)")
                                   for line in ls]), coverage, refused),
            "no '#define KAGE_VITA_RENDER_WATCH(X)'", refused, "missing macro")
        expect_fail(generate(watch_variant(
            "dup", lambda ls: ls[:second] + [ls[first]] + ls[second + 1:]),
            coverage, refused), "duplicate entries", refused, "duplicate")
        many = [f"    X(0x{0x00700000 + 16 * i:08x}u) \\\n"
                for i in range(watch.WATCH_MAX + 1 - len(entries))]
        expect_fail(generate(watch_variant(
            "many", lambda ls: ls[:first] + many + ls[first:]), coverage, refused),
            f"{watch.WATCH_MAX + 1} entries, at most {watch.WATCH_MAX}", refused,
            f"{watch.WATCH_MAX + 1} entries")
        expect_fail(generate(watch_variant(
            "absent", lambda ls: ls[:first] + ["    X(0x00123456u) \\\n"] + ls[first + 1:]),
            coverage, refused), "not coverage functions in this corpus: 0x00123456",
            refused, "absent RVA")
        expect_fail(generate(watch_variant(
            "id0", lambda ls: ls[:first] + ["    X(0x00001000u) \\\n"] + ls[first + 1:]),
            coverage, refused), "coverage id 0 cannot be watched", refused, "id 0")
        bad = coverage_doc(rvas)
        bad["function_rvas_sha256"] = "0" * 64
        expect_fail(generate(WATCH, write_json(tmp / "badhash.json", bad), refused),
                    "sorted function RVAs hash to", refused, "hash mismatch")
        bad = coverage_doc(rvas)
        bad["functions"][1]["id"], bad["functions"][2]["id"] = 2, 1
        expect_fail(generate(WATCH, write_json(tmp / "badids.json", bad), refused),
                    "not positions in the sorted RVA list", refused, "ids")
        bad = coverage_doc(rvas)
        bad["function_count"] += 1
        expect_fail(generate(WATCH, write_json(tmp / "badcount.json", bad), refused),
                    "function_count", refused, "count")

        # 4. the hook folds: watched -> census rows, unwatched/empty -> nothing
        cc = compiler()
        watched = by_rva[entries[0]]
        unwatched = by_rva[0x000023f0]
        for name, body, expect in (
                ("watched", f"guest_coverage_function({watched}U);", True),
                ("unwatched", f"guest_coverage_function({unwatched}U);", False),
                ("empty", "GUEST_PROFILE_NOTE_FUNCTION_ENTRY();", False)):
            source = tmp / f"hook_{name}.c"
            source.write_text('#include "guest.h"\n'
                              f"void hook_{name}(void) {{ {body} }}\n",
                              encoding="utf-8")
            asm = tmp / f"hook_{name}.s"
            result = subprocess.run(
                [cc, "-std=c11", "-O2", "-S", "-DGUEST_IMAGE_BASE=0x98000000u",
                 "-DGUEST_COVERAGE_HOOKS=0",
                 "-DISAAC_VITA_PROFILE_FUNCTION_ENTRY_CENSUS=1",
                 f"-I{RUNTIME}", f"-I{VITA}", f"-I{header.parent}",
                 str(source), "-o", str(asm)],
                capture_output=True, text=True, check=False)
            if result.returncode != 0:
                raise AssertionError(f"hook TU {name} failed:\n{result.stderr}")
            code = asm.read_text(encoding="utf-8", errors="replace")
            refs = ("g_guest_entry_census" in code,
                    "g_kage_vita_guest_sampler_phase" in code)
            if refs != (expect, expect):
                raise AssertionError(
                    f"hook {name}: census/phase references {refs}, expected "
                    f"{(expect, expect)}")
            print(f"hook {name}: census and phase word "
                  f"{'referenced' if expect else 'absent (folded)'}")

        # 5. the real corpus, when given
        if args.coverage is not None:
            result = generate(WATCH, args.coverage, None, check=True)
            if result.returncode != 0:
                raise AssertionError(f"real corpus refused:\n{result.stderr}")
            print(f"real corpus {args.coverage}: {result.stdout.strip()}")
            verify_corpus_hooks(args.coverage, entries)
    print("render watch generator + census hook: PASS")
    return 0


RE_BODY = re.compile(r"^void (sub_[0-9a-f]{8})\(CPU \*__restrict c\)\n\{\n", re.M)
RE_FIRST_INSN = re.compile(r"    /\* [0-9a-f]{8}  ")
RE_LABEL = re.compile(r"L_[0-9a-f]{8}:")


def body_prologue(text: str, start: int) -> str:
    """The lines of a generated body before its first instruction or label (declarations,
    seam #if blocks, the entry hook).  A label starts other code: sub_002dbe40 is
    'goto L_002dbe40;' then a cross-root copy of sub_00006dd0 under L_00006dd0 whose own
    hook (6dd0's id) precedes 6dd0's first instruction (guest_0075.c:9380-9387)."""
    out = []
    for line in text[start:start + 4096].splitlines():
        if line.startswith("}") or RE_FIRST_INSN.match(line) or RE_LABEL.match(line):
            break
        out.append(line)
    return "\n".join(out)
RE_HOOK = re.compile(r"guest_coverage_function\((\d+)U\);")
RE_MANUAL_ID = re.compile(r"#define (GUEST_COVERAGE_MANUAL_[A-Z0-9_]+_ID) (\d+)U")


def verify_corpus_hooks(coverage: Path, entries: list[int]) -> None:
    """6. Each watched id is the id its body's entry hook passes: a generated
    body sub_XXXXXXXX opens with guest_coverage_function(<json id>U); a manual body has its id
    in guest_coverage_generated.h as GUEST_COVERAGE_MANUAL_*_ID and a runtime source passes that
    macro to the hook."""
    corpus = coverage.parent
    data = json.loads(coverage.read_text(encoding="utf-8"))
    by_rva = {int(f["rva"]): f for f in data["functions"]}
    want = {by_rva[r]["emitted_symbol"]: int(by_rva[r]["id"]) for r in entries if not by_rva[r].get("manual")}
    manual = {r: int(by_rva[r]["id"]) for r in entries if by_rva[r].get("manual")}
    found: dict[str, list[int]] = {}
    units = sorted(corpus.glob("guest_[0-9][0-9][0-9][0-9].c"))
    if not units:
        raise AssertionError(f"{corpus}: no generated guest_NNNN.c units next to the coverage file")
    for unit in units:
        text = unit.read_text(encoding="utf-8", errors="replace")
        for m in RE_BODY.finditer(text):
            if m.group(1) in want:
                found.setdefault(m.group(1), []).extend(
                    int(x) for x in RE_HOOK.findall(body_prologue(text, m.end())))
    bad = [f"{sym}: body hook {found.get(sym)} != json id {i}" for sym, i in sorted(want.items())
           if found.get(sym) != [i]]
    header = (corpus / "guest_coverage_generated.h").read_text(encoding="utf-8", errors="replace")
    macros = {int(v): k for k, v in RE_MANUAL_ID.findall(header)}
    runtime_text = "\n".join(p.read_text(encoding="utf-8", errors="replace")
                             for p in sorted(RUNTIME.glob("manual_*.c")))
    for rva, i in sorted(manual.items()):
        name = macros.get(i)
        if name is None:
            bad.append(f"0x{rva:08x}: manual id {i} has no GUEST_COVERAGE_MANUAL_*_ID in the corpus header")
        elif f"guest_coverage_function({name});" not in runtime_text:
            bad.append(f"0x{rva:08x}: no runtime manual body passes {name} to the hook")
    if bad:
        raise AssertionError("watched ids disagree with the corpus hooks:\n  " + "\n  ".join(bad))
    print(f"corpus hooks: {len(want)} generated bodies open with their json id, "
          f"{len(manual)} manual ({', '.join(f'{r:x}={macros[i]}' for r, i in manual.items())}); "
          f"{len(units)} units scanned")


if __name__ == "__main__":
    sys.exit(main())
