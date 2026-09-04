"""White-box host oracle for guest.c's sampler word (ISAAC_VITA_GUEST_SAMPLER).

guest_sampler_word_oracle.c includes guest.c, so the production guest_call
wrapper, the dispatcher's entry store, guest_try_direct_sync_import_call,
guest_call's validated sync branch and the direct IAT route guest_import_call
(ISAAC_VITA_IMPORT_DIRECT) drive g_kage_guest_last_indirect_target; the
oracle only sets the top-level "enclosing" values and reads.  guest.c
is a Win32 host file (VirtualAlloc, Interlocked*, file mapping), so this runs
under MSVC like test_vita_sync_import_fastpath.py.  The portable half of the
sampler proof (kage_vita_guest_sampler.c + scheduler) lives in
vita/test_kage_vita_guest_sampler.py.
"""

from __future__ import annotations

from pathlib import Path
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE / "vita"))

import build_all as BA  # noqa: E402


RUNTIME = HERE / "runtime"
PASS_MARKER = "Vita guest sampler word oracle: PASS"
IMAGE_BASE = 0x98000000
NEEDLES = (
    # A: idle enclosing value, publish on entry, restore on every return.
    # F/G inside the body: direct IAT route (malloc, slot 0x606504) and its
    # fail-closed fallback (TryEnterCriticalSection's token with malloc's ID).
    "scenario A idle: enclosing=00000000 outer=98562e00 inner=985a0de0 "
    "leaf=980023f0 inner.after=985a0de0 outer.after=98562e00 "
    "sync.inside=986060fc sync.after=98562e00 branch.inside=986060f8 "
    "branch.after=98562e00 generic.inside=98606100 generic.after=98562e00 "
    "rejected.after=98562e00 direct.inside=98606504 direct.after=98562e00 "
    "fallback.inside=98606100 fallback.after=98562e00 final=00000000",
    # B: a stale finished callee as the enclosing value survives the tree.
    "scenario B stale: enclosing=98123456 outer=98562e00 inner=985a0de0 "
    "leaf=980023f0 inner.after=985a0de0 outer.after=98562e00 "
    "sync.inside=986060fc sync.after=98562e00 branch.inside=986060f8 "
    "branch.after=98562e00 generic.inside=98606100 generic.after=98562e00 "
    "rejected.after=98562e00 direct.inside=98606504 direct.after=98562e00 "
    "fallback.inside=98606100 fallback.after=98562e00 final=98123456",
    # A again through the dispatch cache's hit path.
    "scenario A cached: enclosing=00000000 outer=98562e00 inner=985a0de0 "
    "leaf=980023f0 inner.after=985a0de0 outer.after=98562e00",
    # C: top-level fast path, RVA and VA target forms.
    "scenario C rva: target=006060fc enclosing=00000000 inside=006060fc "
    "after=00000000",
    "scenario C va: target=986060f8 enclosing=98123456 inside=986060f8 "
    "after=98123456",
    # F: top-level guest_import_call (never enters guest_call): the family
    # endpoint sees the slot VA, the enclosing value comes back.
    "scenario F idle: target=98606504 enclosing=00000000 inside=98606504 "
    "after=00000000",
    "scenario F stale: target=98606504 enclosing=98123456 inside=98606504 "
    "after=98123456",
    PASS_MARKER,
)


def run(command: list[str], env: dict[str, str]) -> str:
    completed = subprocess.run(
        command,
        cwd=HERE,
        env=env,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )
    output = completed.stdout.decode("cp866", errors="replace")
    print(output, end="")
    if completed.returncode != 0:
        raise subprocess.CalledProcessError(completed.returncode, command)
    return output


# Two builds of the same oracle: the complete-classification guest_call
# (no dispatch table) and the ISAAC_VITA_GUEST_DISPATCH_TABLE inline probe,
# where the sampler wrapper sits around the probe and the sync branch lives in
# guest_call_slow.  Every scenario line must be identical.
VARIANTS = (
    ("classify", ()),
    ("table", ("/DISAAC_VITA_GUEST_DISPATCH_TABLE=1",)),
)


def build_and_run(env: dict[str, str], compiler: str, root: Path,
                  name: str, extra: tuple[str, ...]) -> str:
    executable = root / f"guest-sampler-word-oracle-{name}.exe"
    run(
        [
            compiler,
            "/nologo",
            "/W4",
            "/WX",
            "/O2",
            "/Gy",
            "/std:c11",
            "/wd4310",
            "/wd4702",
            "/wd4996",
            f"/DGUEST_IMAGE_BASE=0x{IMAGE_BASE:08x}u",
            "/DISAAC_VITA_GUEST_SAMPLER=1",
            "/DISAAC_VITA_IMPORT_ID_DISPATCH=1",
            "/DISAAC_VITA_IMPORT_DIRECT=1",
            "/DISAAC_VITA_SYNC_IMPORT_FASTPATH=1",
            "/DISAAC_VITA_GUEST_LOOKUP_CACHE=1",
            "/DISAAC_VITA_PHASE_PROFILE=1",
            "/DISAAC_VITA_PROFILE_IMPORT_KINDS=1",
            *extra,
            "/I",
            str(RUNTIME),
            str(RUNTIME / "guest_sampler_word_oracle.c"),
            "/Fe:" + str(executable),
            "/Fo:" + str(root / f"guest-sampler-word-oracle-{name}.obj"),
            "/link",
            "/OPT:REF",
            "/INCREMENTAL:NO",
        ],
        env,
    )
    return run([str(executable)], env)


def main() -> int:
    env, compiler = BA.msvc_env()
    outputs = {}
    with tempfile.TemporaryDirectory(
            prefix="isaac-guest-sampler-word-") as temporary:
        root = Path(temporary)
        for name, extra in VARIANTS:
            outputs[name] = build_and_run(env, compiler, root, name, extra)
    for name, output in outputs.items():
        for needle in NEEDLES:
            if needle not in output:
                raise AssertionError(
                    f"word oracle ({name}) output lacks {needle!r}")
    scenarios = {
        name: [line for line in output.splitlines()
               if line.startswith("scenario ")]
        for name, output in outputs.items()
    }
    if scenarios["classify"] != scenarios["table"] or not scenarios["table"]:
        raise AssertionError(
            "sampler word scenarios differ between the complete-classification "
            "and dispatch-table builds")
    print("guest sampler word oracle (guest.c white-box, MSVC; "
          f"{len(scenarios['table'])} scenarios identical with and without "
          "the dispatch table): PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
