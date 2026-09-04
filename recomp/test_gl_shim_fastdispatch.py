"""Host oracles for ISAAC_VITA_GL_SHIM_FASTDISPATCH (wf/opt-glshim) and its
two per-crossing trims ISAAC_VITA_GL_SHIM_TABLE_TOKENS /
ISAAC_VITA_GL_SHIM_RAW_ARGS (wf/opt-gltok).

1. Default-OFF cleanliness: guest.c, gl_bridge.c and host_vita_gl.c
   preprocessed without the option contain none of the fast-path symbols;
   preprocessed with the fast dispatcher (and the dispatch table) but without
   the two trims they contain none of the trim symbols.
2. Dispatch differential: gl_shim_fastdispatch_oracle.c (the real guest.c,
   gl_bridge.c and host_vita_gl.c) built as six legs -- legacy, fast, fast +
   dispatch table, + table tokens, fast + raw args, and everything -- and
   every trace line before the build-specific marker must be identical across
   the legs (guest CPU, stdcall cleanup, EAX, fault text, stack-fault
   kind/address/size at every argument position of every token, censuses,
   recorded backend call stream, and the phase-10 hostile shapes: esp
   outside the bound stack pointing at host statics shaped like a frame,
   mid-word floor/ceiling cuts, return words straddling a bound, exact-fit
   and unaligned whole frames, backends that move esp or rebind the stack
   under the adapter, and typed calls while the dispatch table is off).  The
   fast legs' exhaustive 2^32 membership sweep must agree with the linear
   scan; the token legs must have routed every eligible GL dispatch through
   the table and rebuilt the table without tokens while a hostile import
   table held the family; the raw legs must have taken the whole-frame fast
   arm.  The one documented divergence -- a backend that rebinds the stack so
   it excludes the return word while esp stays put -- is printed after the
   marker and pinned per leg (legacy retirement: stack pop fault; RAW_ARGS
   retirement: retires without a read).
3. Location cache: gl_vita_location_cache_oracle.c against the fake vitaGL
   model in three variants (cache OFF, ON, ON+VERIFY).
"""

from __future__ import annotations

from pathlib import Path
import os
import re
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import build_all as BA  # noqa: E402

RUNTIME = HERE / "runtime"
VITA_CMAKE = HERE / "vita" / "CMakeLists.txt"
MARKER = "=== build-specific ===\n"
TABLE_LINE = "guest: dispatch table "
PASS_MARKER = (
    "Vita GL shim fast dispatch oracle: PASS "
    "(default OFF; legacy/fast/table/tokens/raw traces identical; exhaustive "
    "token sweep; short-frame faults identical at every argument position; "
    "hostile frames/backends/table-off identical; rebind-exclude divergence "
    "pinned per leg; location cache off/on/verify model-exact)"
)

COMMON = [
    "/nologo", "/W4", "/WX", "/O2", "/Gy", "/std:c11",
    "/wd4310", "/wd4702", "/wd4996",
    "/DGUEST_IMAGE_BASE=0x98000000u",
]
DISPATCH_DEFINES = [
    "/DISAAC_GL_SHIM_FASTDISPATCH_ORACLE=1",
    "/DGUEST_STACK_REQUIRED=1",
    "/DISAAC_VITA_IMPORT_ID_DISPATCH=1",
    "/DISAAC_VITA_GUEST_LOOKUP_CACHE=1",
    "/DISAAC_VITA_PHASE_PROFILE=1",
]
FAST = "/DISAAC_VITA_GL_SHIM_FASTDISPATCH=1"
TABLE = "/DISAAC_VITA_GUEST_DISPATCH_TABLE=1"
TOKENS = "/DISAAC_VITA_GL_SHIM_TABLE_TOKENS=1"
RAW = "/DISAAC_VITA_GL_SHIM_RAW_ARGS=1"
LEGS = (
    ("legacy", ()),
    ("fast", (FAST,)),
    ("table", (FAST, TABLE)),
    ("tokens", (FAST, TABLE, TOKENS)),
    ("raw", (FAST, RAW)),
    ("tokens-raw", (FAST, TABLE, TOKENS, RAW)),
)
LOCATION_DEFINES = [
    "/DISAAC_GL_VITA_BACKEND_ORACLE=1",
    "/DISAAC_GL_VITA_LOCATION_ORACLE=1",
    "/DISAAC_VITA_PHASE_PROFILE=1",
    "/DISAAC_VITA_GL_REDUNDANCY_CACHE=1",
    "/DISAAC_VITA_GL_SHIM_FASTDISPATCH=1",
    "/D_CRT_SECURE_NO_WARNINGS",
]


def run(command: list[str], env: dict[str, str], quiet: bool = False
        ) -> subprocess.CompletedProcess:
    completed = subprocess.run(
        command, cwd=HERE, env=env,
        stdout=subprocess.PIPE, stderr=subprocess.PIPE,
    )
    if completed.returncode != 0 or not quiet:
        out = completed.stdout.decode("cp866", errors="replace")
        err = completed.stderr.decode("cp866", errors="replace")
        if not quiet:
            print(out, end="")
        if completed.returncode != 0:
            print(out[-4000:], end="")
            print(err[-4000:], end="", file=sys.stderr)
            raise subprocess.CalledProcessError(completed.returncode, command)
    return completed


def verify_build_gate() -> None:
    cmake = VITA_CMAKE.read_text(encoding="utf-8")
    for option in ("ISAAC_VITA_GL_SHIM_FASTDISPATCH",
                   "ISAAC_VITA_GL_LOCATION_CACHE_VERIFY",
                   "ISAAC_VITA_GL_SHIM_TABLE_TOKENS",
                   "ISAAC_VITA_GL_SHIM_RAW_ARGS"):
        if re.search(
            r"option\(" + option + r"\s+\"[^\"]+\"\s+OFF\)", cmake,
        ) is None:
            raise AssertionError(f"{option} is not default OFF")
    scope = re.search(
        r"if\(ISAAC_VITA_GL_SHIM_FASTDISPATCH\)\s+.*?set_property\(SOURCE\s+"
        r'"\$\{ISAAC_RUNTIME\}/guest\.c"\s+'
        r'"\$\{ISAAC_RUNTIME\}/gl_bridge\.c"\s+'
        r'"\$\{ISAAC_RUNTIME\}/host_vita_gl\.c"\s+'
        r'"\$\{ISAAC_RUNTIME\}/gl_vita_backend\.c"\s+'
        r"APPEND PROPERTY COMPILE_DEFINITIONS\s+"
        r"ISAAC_VITA_GL_SHIM_FASTDISPATCH=1\)",
        cmake, re.DOTALL,
    )
    if scope is None or "guest_0" in scope.group(0):
        raise AssertionError("fast dispatch definition scope changed")
    # The two trims live inside the fast-dispatch block, require the dispatch
    # table, and reach exactly their owners.
    for option in ("ISAAC_VITA_GL_SHIM_TABLE_TOKENS",
                   "ISAAC_VITA_GL_SHIM_RAW_ARGS"):
        if re.search(
            r"if\(" + option + r" AND\s+NOT \(ISAAC_VITA_GL_SHIM_FASTDISPATCH "
            r"AND ISAAC_VITA_DISPATCH_TABLE\)\)\s+message\(FATAL_ERROR",
            cmake,
        ) is None:
            raise AssertionError(f"{option} does not require fast dispatch "
                                 "and the dispatch table")
    tokens = re.search(
        r"if\(ISAAC_VITA_GL_SHIM_TABLE_TOKENS\)\s+.*?set_property\(SOURCE\s+"
        r'"\$\{ISAAC_RUNTIME\}/guest\.c"\s+'
        r'"\$\{ISAAC_RUNTIME\}/gl_bridge\.c"\s+'
        r'"\$\{ISAAC_RUNTIME\}/kage_vita_phase_profile\.c"\s+'
        r"APPEND PROPERTY COMPILE_DEFINITIONS\s+"
        r"ISAAC_VITA_GL_SHIM_TABLE_TOKENS=1\).*?endif\(\)",
        cmake, re.DOTALL,
    )
    if tokens is None or "guest_0" in tokens.group(0):
        raise AssertionError("table tokens definition scope changed")
    raw = re.search(
        r"if\(ISAAC_VITA_GL_SHIM_RAW_ARGS\)\s+.*?set_property\(SOURCE\s+"
        r'"\$\{ISAAC_RUNTIME\}/gl_bridge\.c"\s+'
        r"APPEND PROPERTY COMPILE_DEFINITIONS\s+"
        r"ISAAC_VITA_GL_SHIM_RAW_ARGS=1\).*?endif\(\)",
        cmake, re.DOTALL,
    )
    if raw is None or "guest_0" in raw.group(0) or "guest.c" in raw.group(0):
        raise AssertionError("raw args definition scope changed")
    fast_block = cmake[scope.start():]
    fast_block = fast_block[:fast_block.index("elseif(ISAAC_VITA_GL_LOCATION_CACHE)")]
    if "if(ISAAC_VITA_GL_SHIM_TABLE_TOKENS)" not in fast_block or \
            "if(ISAAC_VITA_GL_SHIM_RAW_ARGS)" not in fast_block:
        raise AssertionError("the trims left the fast-dispatch block")


def preprocess(env, compiler, root: Path, name: str, tag: str,
               extra: list[str]) -> str:
    preprocessed = root / f"{name}.{tag}.i"
    run([
        compiler, "/nologo", "/P", "/std:c11", "/wd4310", "/wd4996",
        "/DGUEST_IMAGE_BASE=0x98000000u",
        "/DISAAC_VITA_IMPORT_ID_DISPATCH=1", *extra,
        "/I", str(RUNTIME), str(RUNTIME / name),
        "/Fi:" + str(preprocessed),
    ], env, quiet=True)
    return preprocessed.read_text(encoding="utf-8", errors="replace")


def verify_default_off(env: dict[str, str], compiler: str, root: Path) -> None:
    forbidden = {
        "guest.c": ("g_gl_dynamic_first", "isaac_vita_gl_dynamic_counted",
                    "guest_gl_table_tokens", "guest_dispatch_gl_token_of",
                    "gl_tokens"),
        "gl_bridge.c": ("s_guest_gl_owner_cpu", "s_guest_gl_token_index",
                        "guest_gl_run_owned",
                        "guest GL dispatch from a foreign CPU/thread",
                        "guest_gl_table_dispatch", "guest_gl_frame_ok",
                        "s_guest_gl_table_functions",
                        "guest_gl_stdcall_return_raw"),
        "host_vita_gl.c": ("guest_gl_dispatch_counted(c, token, call_count)",),
    }
    for name, tokens in forbidden.items():
        text = preprocess(env, compiler, root, name, "off", [])
        leaked = [token for token in tokens if token in text]
        if leaked:
            raise AssertionError(f"default-OFF {name} retained: {leaked}")
    # Fast dispatcher (and, for guest.c, the dispatch table) ON, trims OFF:
    # the production perf configuration before wf/opt-gltok.
    trims = {
        "guest.c": (["/DISAAC_VITA_GL_SHIM_FASTDISPATCH=1",
                     "/DISAAC_VITA_GUEST_DISPATCH_TABLE=1",
                     "/DISAAC_VITA_GUEST_LOOKUP_CACHE=1",
                     "/DISAAC_VITA_PHASE_PROFILE=1"],
                    ("guest_gl_table_tokens", "guest_dispatch_gl_token_of",
                     "guest_dispatch_item_key", "gl_tokens", "dispatch_gl",
                     "GL tokens")),
        "gl_bridge.c": (["/DISAAC_VITA_GL_SHIM_FASTDISPATCH=1"],
                        ("guest_gl_table_dispatch", "guest_gl_frame_ok",
                         "guest_gl_frame_bound", "s_guest_gl_table_functions",
                         "_slow(",
                         "s_guest_gl_table_tokens", "guest_gl_table_",
                         "guest_gl_stdcall_return_raw",
                         "guest_gl_f64_from_bits", "dispatch_gl")),
        "kage_vita_phase_profile.c": (
            ["/DISAAC_VITA_GUEST_DISPATCH_TABLE=1",
             "/DISAAC_VITA_PHASE_PROFILE=1",
             "/DISAAC_KAGE_VITA_PHASE_PROFILE_ORACLE=1",
             "/DISAAC_VITA_PHASE_PROFILE_BUILD_ID=\"x\"",
             "/I", str(HERE / "vita")],
            ("dispatch_gl", "d(c,s,sf,g)")),
    }
    for name, (extra, tokens) in trims.items():
        text = preprocess(env, compiler, root, name, "trims-off", extra)
        leaked = [token for token in tokens if token in text]
        if leaked:
            raise AssertionError(f"trims-OFF {name} retained: {leaked}")


def build_dispatch(env, compiler, root: Path, tag: str,
                   defines: tuple[str, ...]) -> str:
    exe = root / f"gl-shim-fastdispatch-{tag}.exe"
    objects = root / f"obj-{tag}"
    objects.mkdir()
    command = [compiler] + COMMON + DISPATCH_DEFINES + list(defines) + [
        "/I", str(RUNTIME),
        str(RUNTIME / "gl_shim_fastdispatch_oracle.c"),
        str(RUNTIME / "gl_bridge.c"),
        str(RUNTIME / "host_vita_gl.c"),
        "/Fe:" + str(exe),
        "/Fo:" + str(objects) + os.sep,
        "/link", "/OPT:REF", "/INCREMENTAL:NO",
    ]
    run(command, env, quiet=True)
    ran = run([str(exe)], env, quiet=True)
    return ran.stdout.decode("cp866", errors="replace").replace("\r\n", "\n")


def split_trace(trace: str) -> tuple[str, list[str], str]:
    """Common section without the dispatch-table boot lines (which only the
    table legs print), those boot lines, and the build-specific tail."""
    if MARKER not in trace:
        raise AssertionError("trace marker missing")
    common, tail = trace.split(MARKER, 1)
    kept = []
    boot = []
    for line in common.splitlines(keepends=True):
        (boot if line.startswith(TABLE_LINE) else kept).append(line)
    return "".join(kept), [line.rstrip("\n") for line in boot], tail


def check_rebind_exclude(tail: str, tag: str, raw: bool) -> None:
    """The documented RAW_ARGS divergence: a backend that rebinds the stack so
    the binding excludes the return word while esp stays put.  Legacy
    retirement (gpop + adjust) faults as a stack pop (kind 7); the raw
    retirement re-checks only that esp is still the validated frame and
    retires without reading anything."""
    outcome = re.search(r"^  hostile outcome fault=(.*) sk=(\d+) sa=[-+]\d+$",
                        tail, re.M)
    expectation = re.search(r"^rebind-exclude expectation=(\S+)$", tail, re.M)
    if outcome is None or expectation is None:
        raise AssertionError(f"{tag}: rebind-exclude case missing from the tail")
    if raw:
        if expectation.group(1) != "raw-retires" or outcome.group(2) != "0" or \
                outcome.group(1) != "-":
            raise AssertionError(f"{tag}: raw retirement changed: {outcome.group(0)}")
    elif expectation.group(1) != "legacy-pop-fault" or outcome.group(2) != "7":
        raise AssertionError(f"{tag}: legacy retirement changed: {outcome.group(0)}")


def compare_common(reference: str, other: str, tag: str) -> None:
    if reference == other:
        return
    reference_lines = reference.splitlines()
    other_lines = other.splitlines()
    for index, (a, b) in enumerate(zip(reference_lines, other_lines)):
        if a != b:
            raise AssertionError(
                f"trace divergence ({tag}) at line {index + 1}:\n"
                f"  legacy: {a}\n  {tag}: {b}")
    raise AssertionError(
        f"trace length differs ({tag}): legacy {len(reference_lines)} "
        f"{tag} {len(other_lines)}")


def verify_dispatch(env, compiler, root: Path) -> tuple[int, int, int]:
    traces = {}
    for tag, defines in LEGS:
        traces[tag] = build_dispatch(env, compiler, root, tag, defines)
        (root / f"{tag}.trace").write_text(traces[tag], encoding="utf-8")
    legacy_common, legacy_boot, legacy_tail = split_trace(traces["legacy"])
    if legacy_boot:
        raise AssertionError("the legacy leg printed dispatch-table lines")
    for tag, defines in LEGS[1:]:
        common, boot, tail = split_trace(traces[tag])
        compare_common(legacy_common, common, tag)
        if "fast build\n" not in tail:
            raise AssertionError(f"{tag}: build-specific tail mislabelled")
        if "full sweep registered=73 mismatch=0\n" not in tail:
            raise AssertionError(f"{tag}: exhaustive sweep failed")
        if ("foreign fault=guest GL dispatch from a foreign CPU/thread"
                not in tail):
            raise AssertionError(f"{tag}: foreign-CPU trap did not fire")
        if ("foreign after unknown fault=guest GL dispatch from a foreign "
                "CPU/thread backend=2" not in tail):
            raise AssertionError(
                f"{tag}: unknown token from a foreign CPU moved the pin")
        if "GL SHIM FASTDISPATCH ORACLE DONE" not in tail:
            raise AssertionError(f"{tag}: oracle did not finish")
        if TABLE in defines:
            # Five rebuilds (2 registered functions: one in-image, one under
            # a registry token -> skipped): guest_register (flag still down:
            # no tokens), the import table (tokens in), the hostile import
            # table of phase 6 (flag down: tokens out), the restored table
            # (tokens back), and the phase-10 re-registration after the
            # table was off (0 functions: "no eligible functions", no boot
            # line).  Every printed rebuild ends ready.
            ready = [line for line in boot if " ready: 1 keys, 1 skipped" in line]
            if len(ready) != 5 or len(ready) != len(boot):
                raise AssertionError(f"{tag}: dispatch-table boot lines: {boot}")
            if TOKENS in defines:
                counts = [re.search(r"skipped, (\d+) GL tokens", line)
                          for line in ready]
                if any(match is None for match in counts):
                    raise AssertionError(f"{tag}: GL token census missing: {boot}")
                counts = [int(match.group(1)) for match in counts]
                if counts != [0, 73, 0, 73, 73]:
                    raise AssertionError(f"{tag}: GL token census: {counts}")
                if not re.search(r"^table gl hits=[1-9]\d* dyn=\d+$", tail, re.M):
                    raise AssertionError(f"{tag}: no GL dispatch went through the table")
            elif any("GL tokens" in line for line in ready):
                raise AssertionError(f"{tag}: GL tokens without the option")
        elif boot:
            raise AssertionError(f"{tag}: dispatch-table lines without the table")
        if RAW in defines:
            match = re.search(r"^raw frames=(\d+)$", tail, re.M)
            if match is None or int(match.group(1)) < 200:
                raise AssertionError(f"{tag}: whole-frame fast arm not exercised")
        elif "raw frames=" in tail:
            raise AssertionError(f"{tag}: raw frames without the option")
        check_rebind_exclude(tail, tag, raw=RAW in defines)
    if "legacy build\n" not in legacy_tail or \
            "GL SHIM FASTDISPATCH ORACLE DONE" not in legacy_tail:
        raise AssertionError("legacy tail mislabelled")
    check_rebind_exclude(legacy_tail, "legacy", raw=False)
    hostile = re.findall(r"^hostile (\S+) .* sk=(\d+) sa=([-+]\d+) .* backend=([-+]\d+) .*$",
                         legacy_common, re.M)
    if len(hostile) != 17:
        raise AssertionError(f"hostile frame sweep drifted: {len(hostile)} cases")
    hostile_by = {name: (int(kind), int(sa), int(backend))
                  for name, kind, sa, backend in hostile
                  if name != "midword-ceiling"}
    # esp outside the stack: the first argument word (or the return word of a
    # 0-arg call, popped after the backend ran) faults before/after exactly
    # as the legacy validators do; no build read the host statics.
    expected = {
        "host-esp-4arg": (5, 4, 0), "host-esp-1arg": (5, 4, 0),
        "host-esp-0arg": (7, 0, 1), "esp-below-floor": (5, 4, 0),
        "esp-at-ceiling": (5, 4, 0), "midword-floor-arg0": (5, 4, 0),
        "one-byte-short": (5, 16, 0), "ret-straddles-ceiling-0arg": (7, 0, 1),
        "ret-straddles-floor-0arg": (7, 0, 1),
        "ret-straddles-ceiling-1arg": (5, 4, 0),
        "exact-fit": (0, 0, 1), "exact-fit-0arg": (0, 0, 1),
        "unaligned-whole": (0, 0, 1),
    }
    if hostile_by != expected:
        raise AssertionError(f"hostile frame outcomes drifted: {hostile_by}")
    midword = [(int(kind), int(sa), int(backend))
               for name, kind, sa, backend in hostile if name == "midword-ceiling"]
    if midword != [(5, 4 + 4 * k, 0) for k in range(4)]:
        raise AssertionError(f"mid-word ceiling cuts drifted: {midword}")
    phase10 = legacy_common.split("=== phase 10")[1]
    if phase10.count("  be glViewport(") != 5 or \
            phase10.count("  be glCreateProgram()") != 4:
        raise AssertionError("phase 10 whole frames did not reach the backend")
    outcomes = re.findall(r"^  hostile outcome fault=(.*) sk=(\d+) sa=([-+]\d+)$",
                          legacy_common, re.M)
    if len(outcomes) != 5:
        raise AssertionError(f"hostile backend cases drifted: {outcomes}")
    # push2 x2 and rebind-tight retire; esp parked at the ceiling faults the
    # pop (kind 7), one word under it faults the adjust (kind 4), exactly the
    # legacy gpop + guest_stack_adjust pair.
    kinds = [int(kind) for _, kind, _ in outcomes]
    if kinds != [0, 0, 7, 4, 0]:
        raise AssertionError(f"hostile backend fault kinds drifted: {kinds}")
    # call() reports esp relative to the pre-push ceiling: a retired stdcall
    # frame nets +0.
    if not re.search(r"^call table-off-cull .*\n(?:  be .*\n)*  stop=0 esp=\+0 "
                     r"eax=deadbeef fault=- ", legacy_common, re.M):
        raise AssertionError("typed call with the dispatch table off did not retire")
    calls = legacy_common.count("\ncall ") + legacy_common.startswith("call ")
    backend = legacy_common.count("\n  be ")
    direct = legacy_common.count("\ndirect ")
    shorts = re.findall(r"^short .* sk=(\d+) .*$", legacy_common, re.M)
    if calls < 500 or backend < 100 or direct != 3:
        raise AssertionError(
            f"trace too small: calls={calls} be={backend} direct={direct}")
    # Phase 9: every token at every argument position without a backend
    # (sum of words + 1 over the registry), the pointer-free subset again
    # with the recording backend, plus one floor cut per pointer-free token
    # with arguments.  Cut frames fault as stack access (kind 5) on their
    # first outside word; floor cuts reach the backend and fault in the
    # retirement as stack overflow (kind 7, the legacy gpop).
    if len(shorts) < 300 or shorts.count("5") < 200 or shorts.count("7") != 13:
        raise AssertionError(
            f"short-frame sweep drifted: {len(shorts)} cases, "
            f"{shorts.count('5')} access faults, {shorts.count('7')} pop faults")
    if not re.search(r"^short floorcut .* backend=\+1 ", legacy_common, re.M):
        raise AssertionError("floor cut did not reach the backend")
    if not re.search(r"^short backend token=\S+ name=glViewport words=4 inside=4 "
                     r"floor_cut=0 stop=\d+ esp=\+20 ", legacy_common, re.M):
        raise AssertionError("whole glViewport frame did not retire by 20 bytes")
    # Direct bridge dispatch: the known token ran, both unknown tokens
    # returned zero without a fault or a backend call.
    direct_lines = re.findall(r"^direct (\S+) .*\n(?:  be .*\n)*  stop=\d+ "
                              r"rc=(-?\d+) fault=(\S+) .*backend=([+-]\d+)",
                              legacy_common, re.M)
    if direct_lines != [
            ("known", "1", "-", "+1"), ("unknown-7e", "0", "-", "+0"),
            ("unknown-7f", "0", "-", "+0")]:
        raise AssertionError(f"direct dispatch lines drifted: {direct_lines}")
    if "families registered=73 mismatch=0\n" not in legacy_common:
        raise AssertionError("family sweep did not agree with the scan")
    # The recorded backend stream must contain the KAGE per-draw shape: two
    # draws x 7 attributes (bind + unbind) plus the two shadow-precedence
    # glGetAttribLocation dispatches of phases 5 and 6.
    kage = legacy_common.split("=== phase 9")[0]
    shape = (kage.count("  be glVertexAttribPointer("),
             kage.count("  be glGetAttribLocation("),
             kage.count("  be glDrawElements("))
    if shape != (14, 30, 2):
        raise AssertionError(f"KAGE replay shape drifted: {shape}")
    return calls + direct, backend, len(shorts)


def verify_location(env, compiler, root: Path) -> list[str]:
    results = []
    variants = {
        "no-cache": [],
        "cache": ["/DISAAC_VITA_GL_LOCATION_CACHE=1"],
        "cache+verify": ["/DISAAC_VITA_GL_LOCATION_CACHE=1",
                         "/DISAAC_VITA_GL_LOCATION_CACHE_VERIFY=1"],
    }
    for tag, extra in variants.items():
        safe = tag.replace("+", "-")
        exe = root / f"gl-vita-location-cache-{safe}.exe"
        objects = root / f"obj-loc-{safe}"
        objects.mkdir()
        command = [compiler] + COMMON + LOCATION_DEFINES + extra + [
            "/I", str(RUNTIME),
            str(RUNTIME / "gl_vita_location_cache_oracle.c"),
            str(RUNTIME / "gl_bridge.c"),
            str(RUNTIME / "gl_vita_backend.c"),
            str(RUNTIME / "guest_stack_legacy_oracle_stub.c"),
            "/Fe:" + str(exe),
            "/Fo:" + str(objects) + os.sep,
            "/link", "/OPT:REF", "/INCREMENTAL:NO",
        ]
        run(command, env, quiet=True)
        ran = run([str(exe)], env, quiet=True)
        out = ran.stdout.decode("cp866", errors="replace")
        expected = f"Vita GL location cache oracle: PASS ({tag};"
        if expected not in out:
            raise AssertionError(f"location cache {tag}: {out!r}")
        results.append(out.strip())
    return results


def main() -> int:
    verify_build_gate()
    env, compiler = BA.msvc_env()
    with tempfile.TemporaryDirectory(prefix="isaac-gl-shim-fastdispatch-") as temporary:
        root = Path(temporary)
        verify_default_off(env, compiler, root)
        calls, backend, shorts = verify_dispatch(env, compiler, root)
        print(f"dispatch differential: {len(LEGS)} legs "
              f"({', '.join(tag for tag, _ in LEGS)}), {calls} guest_call cases, "
              f"{shorts} short-frame cases, {backend} recorded backend calls, "
              "traces identical")
        for line in verify_location(env, compiler, root):
            print(line)
    print(PASS_MARKER)
    return 0


if __name__ == "__main__":
    sys.exit(main())
