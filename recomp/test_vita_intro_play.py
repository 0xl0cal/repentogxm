#!/usr/bin/env python3
"""Codegen/host oracle for ISAAC_VITA_INTRO_PLAY (GitHub issue #14).

The generator injects, after IsActionTriggered(ACTION_MENUCONFIRM) in the
cutscene update sub_004b78d0, a forced "confirm pressed" for cutscene 1
(Intro).  The guard of that seam is spelled guest_kage_intro_skip_ready();
guest.h aliases it to guest_kage_pc_handoff_ready unless the knob defines a
real function, so OFF keeps today's auto-skip with the same tokens.

1. Generator pins: the Intro seam is the only emitted spelling of the new
   name, its length equals the handoff predicate's (legacy_text packing),
   and the other nine emitted handoff guards are untouched.
2. Runtime and build pins: the guest.h alias, the three knob-fenced
   definitions, the CMake option (default OFF), the scaffold guard, one
   definition site; the v0.1.1-alpha release recipe (which shipped the
   auto-skip) does not mention the knob.  Later recipes are the owner's.
3. CMake owner census under `cmake -P` with the real owner block: a fake
   corpus selects exactly the owner plus the two KAGE boundary sources, a
   corpus without the owner and one with the pre-rename spelling fail.
4. Fresh translation (--pe, required): the frozen PE (size and SHA-256 as
   tools/build_vita.py pins them) is translated by this tree's generator,
   sub_004b78d0 only, at the Vita guest base and at the PE base; the
   generator must classify it "native_skip" and emit the same seam at both
   bases.  Nothing below consumes a corpus the test did not generate.
5. Host oracle (clang or gcc; neither is a FAIL): the seam block of that
   fresh translation is compiled with the guest.h alias block (OFF) or the
   real manual_kage_vita.c function (ON) and run on a fake Manager/cutscene:
   OFF forces the confirm exactly when the handoff is ready and every Intro
   condition holds, ON never does and logs one receipt.
6. Corpus contract (--generated-dir, optional): exactly one unit spells the
   new name, once, and it is the owner of sub_004b78d0; its seam equals the
   fresh seam modulo the GUEST_GPR_FLUSH_MASK post-pass
   (gpr_flush_mask.legacy_text).  With --base-generated-dir the corpora
   have the same file set and function->unit map, and the only differing
   unit is the owner, whose text differs only by the rename (guest_table.c
   and the coverage header may differ in their generation id lines only).
"""

from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile


HERE = Path(__file__).resolve().parent
RUNTIME = HERE / "runtime"
VITA_CMAKE = HERE / "vita" / "CMakeLists.txt"
GEN_ALL = HERE / "gen_all.py"
BUILD_VITA = HERE.parent / "tools" / "build_vita.py"
RELEASE_V011 = HERE.parent / "release" / "v0.1.1-alpha.cmake"
OLD_NAME = "guest_kage_pc_handoff_ready"
NEW_NAME = "guest_kage_intro_skip_ready"
OWNER_NAME = "sub_004b78d0"
FENCE = "#if defined(ISAAC_VITA_INTRO_PLAY) && ISAAC_VITA_INTRO_PLAY"
SEAM_COMMENT = "    /* explicit PC automatic-Intro action; original test follows */"
# Frozen facts copied from gen_all.py (INTRO_PC_NATIVE_SKIP_MACHINE and the
# emitted seam): Manager global RVA, cutscene-manager offset, entry field.
MANAGER_GLOBAL_RVA = 0x007FD680
CUTSCENE_OFFSET = 0x20A00
RECEIPT = "KAGE VITA INTRO: autoskip=0 backend_ready=%d"


def require(condition, message):
    if not condition:
        raise AssertionError(message)


# ---- 1. generator pins -----------------------------------------------------------

def verify_generator() -> str:
    require(len(OLD_NAME) == len(NEW_NAME),
            "rename changes legacy_text length: %d != %d" % (
                len(OLD_NAME), len(NEW_NAME)))
    lines = GEN_ALL.read_text(encoding="utf-8").split("\n")
    emitted_new = [i for i, line in enumerate(lines)
                   if "lines.append" in line and NEW_NAME + "()" in line]
    require(len(emitted_new) == 1,
            "gen_all emits %s at %d sites" % (NEW_NAME, len(emitted_new)))
    site = emitted_new[0]
    window = "\n".join(lines[max(0, site - 12):site])
    require("if ins.address == INTRO_PC_NATIVE_SKIP_SITE_RVA:" in window and
            SEAM_COMMENT.strip() in window,
            "the new guard is not the Intro native-skip seam")
    require(lines[site].strip() ==
            'lines.append("        if (%s() &&")' % NEW_NAME,
            "Intro guard statement drifted: %r" % lines[site])
    old_guards = [line for line in lines
                  if OLD_NAME + "()" in line and
                  ('"' in line.split(OLD_NAME)[0])]
    require(len(old_guards) == 9,
            "other emitted handoff guards changed: %d" % len(old_guards))
    return "generator: PASS (1 Intro guard renamed, %d-char names, 9 handoff " \
           "guards untouched)" % len(NEW_NAME)


# ---- 2. runtime and build pins ---------------------------------------------------

ALIAS_BLOCK = (
    "#if defined(ISAAC_VITA_INTRO_PLAY) && ISAAC_VITA_INTRO_PLAY\n"
    "int guest_kage_intro_skip_ready(void);\n"
    "#else\n"
    "#define guest_kage_intro_skip_ready guest_kage_pc_handoff_ready\n"
    "#endif\n"
)


def fenced_function(path: Path) -> str:
    text = path.read_text(encoding="utf-8")
    require(text.count(FENCE) == 1, "%s: expected one knob fence" % path.name)
    start = text.index(FENCE) + len(FENCE) + 1
    end = text.index("\n#endif", start)
    body = text[start:end]
    require("int %s(void)\n{" % NEW_NAME in body,
            "%s: fenced block does not define %s" % (path.name, NEW_NAME))
    require(text.count(NEW_NAME) == 1,
            "%s: %s escapes the fence" % (path.name, NEW_NAME))
    return body


def verify_runtime_and_build() -> list[str]:
    guest_h = (RUNTIME / "guest.h").read_text(encoding="utf-8")
    require(guest_h.count(ALIAS_BLOCK) == 1, "guest.h alias block drifted")
    require(guest_h.count(NEW_NAME) == 2, "guest.h spells the name elsewhere")

    vita = fenced_function(RUNTIME / "manual_kage_vita.c")
    require("const int autoskip = 0;" in vita and
            "return autoskip;" in vita and
            '"KAGE VITA INTRO: autoskip=%d backend_ready=%d"' in vita and
            "static int s_intro_receipt;" in vita,
            "manual_kage_vita.c: ON body must return 0 and log once")
    disabled = fenced_function(RUNTIME / "manual_kage_disabled.c")
    require("return 0;" in disabled, "manual_kage_disabled.c must return 0")
    pc = fenced_function(RUNTIME / "manual_kage.c")
    require("return %s();" % OLD_NAME in pc,
            "manual_kage.c must keep the PC handoff behaviour")

    cmake = VITA_CMAKE.read_text(encoding="utf-8")
    option = re.search(r"option\(ISAAC_VITA_INTRO_PLAY\s+.*?\s+(ON|OFF)\)",
                       cmake, flags=re.S)
    require(option is not None and option.group(1) == "OFF",
            "ISAAC_VITA_INTRO_PLAY must default OFF")
    require("ISAAC_VITA_INTRO_PLAY requires the translated guest runtime"
            in cmake, "scaffold guard missing")
    require(cmake.count("ISAAC_VITA_INTRO_PLAY=1") == 1,
            "the definition must be set in exactly one place")
    block = owner_block()
    require("ISAAC_VITA_INTRO_PLAY=1" in block and
            '"^void %s\\\\(CPU \\\\*__restrict c\\\\)$"' % OWNER_NAME in block,
            "definition/owner regex escaped the owner block")
    # v0.1.1-alpha shipped the automatic Intro skip (issue #14) before the
    # knob existed.  Whether a later recipe ships ISAAC_VITA_INTRO_PLAY=ON is
    # the owner's call after the device check, so later recipes are not
    # pinned here (same split as test_backdrop_watergfx_guard.py's recipe
    # pin on the release-recipe line).
    require(RELEASE_V011.is_file(), "missing %s" % RELEASE_V011)
    require("INTRO_PLAY" not in RELEASE_V011.read_text(encoding="utf-8"),
            "the v0.1.1-alpha recipe predates the knob: %s" %
            RELEASE_V011.name)
    return ["runtime: PASS (guest.h alias, 3 fenced definitions: vita 0 + "
            "receipt, disabled 0, PC handoff)",
            "build switch: PASS (default OFF, scaffold guard, one definition "
            "site, v0.1.1-alpha recipe predates the knob)"]


def owner_block() -> str:
    lines = VITA_CMAKE.read_text(encoding="utf-8").split("\n")
    start = lines.index("  if(ISAAC_VITA_INTRO_PLAY)")
    depth = 0
    for index in range(start, len(lines)):
        stripped = lines[index].strip()
        if stripped.startswith("if("):
            depth += 1
        elif stripped.startswith("endif("):
            depth -= 1
            if depth == 0:
                return "\n".join(lines[start:index + 1])
    raise AssertionError("owner block has no matching endif")


# ---- 3. CMake owner census -------------------------------------------------------

def _cmake() -> str:
    found = shutil.which("cmake")
    if found:
        return found
    fallback = "C:/Program Files/CMake/bin/cmake.exe"
    require(Path(fallback).exists(), "cmake is required for the owner census")
    return fallback


def census(work: Path, tag: str, units: list[Path]) -> tuple[int, str]:
    script = work / ("census_%s.cmake" % tag)
    script.write_text("\n".join([
        "cmake_minimum_required(VERSION 3.16)",
        'include("%s")' % (VITA_CMAKE.parent /
                           "generated_definitions.cmake").as_posix(),
        "function(set_property)",
        '  message(STATUS "SETPROP ${ARGN}")',
        "endfunction()",
        "set(ISAAC_VITA_INTRO_PLAY ON)",
        'set(ISAAC_RUNTIME "%s")' % RUNTIME.as_posix(),
        'set(ISAAC_GENERATED_C "%s")' % ";".join(u.as_posix() for u in units),
        owner_block(),
        "",
    ]), encoding="utf-8", newline="\n")
    completed = subprocess.run([_cmake(), "-P", str(script)],
                               stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT)
    out = completed.stdout.decode("utf-8", "replace")
    return completed.returncode, re.sub(r"\s+", " ", out)


def _source_props(out: str) -> list[str]:
    return re.findall(r"SETPROP SOURCE;(.*?);APPEND;PROPERTY;"
                      r"COMPILE_DEFINITIONS;ISAAC_VITA_INTRO_PLAY=1", out)


def verify_census(work: Path, units: list[Path] | None) -> list[str]:
    results = []
    fake = work / "fake"
    fake.mkdir()
    owner = fake / "guest_0001.c"
    other = fake / "guest_0002.c"
    owner.write_text(
        "void %s(CPU *__restrict c)\n{\n        if (%s() &&\n}\n" % (
            OWNER_NAME, NEW_NAME), encoding="utf-8", newline="\n")
    other.write_text(
        "void sub_004b7890(CPU *__restrict c)\n{\n    if (%s()) {}\n}\n" %
        OLD_NAME, encoding="utf-8", newline="\n")
    expected = "%s;%s/manual_kage_vita.c;%s/manual_kage_disabled.c" % (
        owner.as_posix(), RUNTIME.as_posix(), RUNTIME.as_posix())
    rc, out = census(work, "fake", [owner, other])
    require(rc == 0 and _source_props(out) == [expected],
            "census on the fake corpus: %s" % out[-1500:])
    results.append("census fake corpus: PASS (owner + 2 KAGE boundaries)")

    rc, out = census(work, "no_owner", [other])
    require(rc != 0 and ("root %s must own exactly one generated source; "
                         "found 0" % OWNER_NAME) in out,
            "census accepted a corpus without the owner: %s" % out[-1500:])
    stale = fake / "guest_0003.c"
    stale.write_text(
        "void %s(CPU *__restrict c)\n{\n        if (%s() &&\n}\n" % (
            OWNER_NAME, OLD_NAME), encoding="utf-8", newline="\n")
    rc, out = census(work, "stale", [stale, other])
    require(rc != 0 and ("must call %s() exactly once (found 0)" % NEW_NAME)
            in out, "census accepted a pre-rename corpus: %s" % out[-1500:])
    results.append("census negatives: PASS (no owner: found 0; pre-rename "
                   "corpus: found 0 guards)")

    if units is not None:
        rc, out = census(work, "real", units)
        props = _source_props(out)
        require(rc == 0 and len(props) == 1 and
                props[0].endswith("/manual_kage_vita.c;%s/manual_kage_"
                                  "disabled.c" % RUNTIME.as_posix()),
                "census on the real corpus: %s" % out[-1500:])
        results.append("census real corpus: PASS (owner %s)" %
                       Path(props[0].split(";")[0]).name)
    return results


# ---- 4. fresh translation --------------------------------------------------------

def extract_seam(owner_text: str) -> str:
    lines = owner_text.split("\n")
    require(lines.count(SEAM_COMMENT) == 1, "seam comment count != 1")
    start = lines.index(SEAM_COMMENT)
    require(lines[start + 1] == "    {", "seam block does not open")
    end = start + 2
    while lines[end] != "    }":
        end += 1
    require(lines[end + 1].startswith("    /* 004b78fb"),
            "seam block is not followed by 004b78fb")
    block = "\n".join(lines[start:end + 1])
    require(block.count(NEW_NAME + "()") == 1, "seam guard spelling")
    require(OLD_NAME not in block, "seam still spells %s" % OLD_NAME)
    return block


def _build_pins() -> tuple[int, int, str]:
    """The frozen PE identity and the Vita guest base, read from the build's
    own definitions (tools/build_vita.py GUEST_BASE / EXPECTED_PE_SIZE /
    EXPECTED_PE_SHA256, recomp/vita/CMakeLists.txt ISAAC_GUEST_BASE), not
    restated here."""
    text = BUILD_VITA.read_text(encoding="utf-8")
    base = re.search(r"^GUEST_BASE = (0x[0-9A-Fa-f]+)$", text, flags=re.M)
    size = re.search(r"^EXPECTED_PE_SIZE = ([0-9_]+)$", text, flags=re.M)
    sha = re.search(r'^EXPECTED_PE_SHA256 = \(\n\s+"([0-9a-f]{64})"\n\)$',
                    text, flags=re.M)
    require(base is not None and size is not None and sha is not None,
            "tools/build_vita.py PE/base pins not found")
    cmake = re.search(r'^set\(ISAAC_GUEST_BASE "(0x[0-9A-Fa-f]+)" CACHE STRING',
                      VITA_CMAKE.read_text(encoding="utf-8"), flags=re.M)
    require(cmake is not None and int(cmake.group(1), 16) ==
            int(base.group(1), 16),
            "ISAAC_GUEST_BASE default disagrees with tools/build_vita.py")
    return int(base.group(1), 16), int(size.group(1).replace("_", "")), \
        sha.group(1)


def translate_owner(pe_path: Path) -> tuple[str, list[str]]:
    """Translate sub_004b78d0 from the frozen PE with this tree's generator
    and return its Intro seam block (identical at both image bases)."""
    guest_base, pe_size, pe_sha = _build_pins()
    pe_path = pe_path.resolve()
    require(pe_path.is_file(), "missing PE %s" % pe_path)
    data = pe_path.read_bytes()
    require(len(data) == pe_size and
            hashlib.sha256(data).hexdigest() == pe_sha,
            "%s is not the frozen PE (size %d, sha256 %s)" % (
                pe_path, len(data), hashlib.sha256(data).hexdigest()))
    os.environ["REPENTOGXM_PE"] = str(pe_path)
    sys.path.insert(0, str(HERE))
    import gen_all as G  # noqa: E402
    from image import DEFAULT_BASE, Image  # noqa: E402

    require(G.INTRO_PC_NATIVE_SKIP_ROOT == int(OWNER_NAME[4:], 16),
            "INTRO_PC_NATIVE_SKIP_ROOT is not %s" % OWNER_NAME)
    pin = Image(str(pe_path), DEFAULT_BASE)
    seams = {}
    for base in (guest_base, DEFAULT_BASE):
        emit = pin if base == DEFAULT_BASE else Image(str(pe_path), base)
        try:
            result = G._translate_function(
                emit, {"rva": G.INTRO_PC_NATIVE_SKIP_ROOT}, None, {},
                pin_img=pin)
        except RuntimeError as exc:  # the generator's own fail-closed pins
            raise AssertionError("generator refused %s at 0x%08x: %s" % (
                OWNER_NAME, base, exc)) from exc
        require(result["stub"] is None,
                "%s translated to a stub: %r" % (OWNER_NAME, result["stub"]))
        require(result["intro_pc_native_skip"] == "native_skip",
                "generator classified %s as %r, not 'native_skip'" % (
                    OWNER_NAME, result["intro_pc_native_skip"]))
        require(result["text"].count(
                    "\nvoid %s(CPU *__restrict c)\n{\n" % OWNER_NAME) == 1,
                "fresh text does not define %s once" % OWNER_NAME)
        seams[base] = extract_seam(result["text"])
    require(seams[guest_base] == seams[DEFAULT_BASE],
            "the seam depends on the image base")
    seam = seams[guest_base]
    return seam, [
        "frozen PE: PASS (%d bytes, sha256 %s..., pins from %s)" % (
            pe_size, pe_sha[:16], BUILD_VITA.name),
        "fresh translation: PASS (%s -> native_skip at 0x%08x and 0x%08x; "
        "same seam, %d lines, guard %s())" % (
            OWNER_NAME, guest_base, DEFAULT_BASE, seam.count("\n") + 1,
            NEW_NAME),
    ]


# ---- 5. host oracle --------------------------------------------------------------

HARNESS = r"""
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

typedef struct CPU { uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi; } CPU;
#define GUEST_IMAGE_BASE 0x98000000u
/* The fake CPU has no register locals: publishing and reloading are no-ops. */
#define GUEST_GPR_FLUSH(c) ((void)(c))
#define GUEST_GPR_RELOAD(c) ((void)(c))

static uint32_t g_addr[16];
static uint32_t g_val[16];
static int g_n;
static void poke(uint32_t a, uint32_t v) { g_addr[g_n] = a; g_val[g_n] = v; g_n++; }
static uint32_t ld32(uint32_t a)
{
    for (int i = 0; i < g_n; i++)
        if (g_addr[i] == a)
            return g_val[i];
    return 0u;
}
static uint8_t ld8(uint32_t a) { return (uint8_t)ld32(a); }

static int g_handoff;
static int g_log_lines;
static char g_last_log[256];
int guest_kage_pc_handoff_ready(void) { return g_handoff; }
int kage_vita_backend_ready(void) { return g_handoff; }
void isaac_vita_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_last_log, sizeof g_last_log, fmt, ap);
    va_end(ap);
    g_log_lines++;
    printf("LOG %s\n", g_last_log);
}

/* guest.h knob block, verbatim */
@ALIAS@
#if defined(ISAAC_VITA_INTRO_PLAY) && ISAAC_VITA_INTRO_PLAY
/* manual_kage_vita.c knob body, verbatim */
@VITA@
#endif

static void seam(CPU *__restrict c)
{
@SEAM@
}

#define MANAGER 0x10000000u
static void world(uint32_t cutscene, uint32_t state, uint32_t flag834,
                  uint32_t esi48)
{
    g_n = 0;
    poke(GUEST_IMAGE_BASE + @MGR@u, MANAGER);
    poke(MANAGER + @CUT@u, 1u);              /* cutscene manager active */
    poke(MANAGER + @CUT@u + 0x79cu, cutscene);
    poke(MANAGER + 0x8u, state);
    poke(MANAGER + 0xcu, 0u);
    poke(MANAGER + 0x1ea834u, flag834);
    poke(MANAGER + 0x1ea838u, 1u);
    poke(0x20000000u + 0x48u, esi48);
}

static int fails;
static int cases;
static void check(const char *name, int handoff, uint32_t eax_in,
                  uint32_t expect)
{
    CPU c;
    memset(&c, 0, sizeof c);
    c.edi = MANAGER + @CUT@u;
    c.esi = 0x20000000u;
    c.eax = eax_in;
    g_handoff = handoff;
    cases++;
    seam(&c);
    if (c.eax != expect) {
        printf("FAIL %s handoff=%d eax=%08x expect=%08x\n", name, handoff,
               (unsigned)c.eax, (unsigned)expect);
        fails++;
    }
}

int main(void)
{
    const uint32_t idle = 0x12345600u, pressed = 0x12345601u;
#if defined(ISAAC_VITA_INTRO_PLAY) && ISAAC_VITA_INTRO_PLAY
    const int on = 1;
#else
    const int on = 0;
#endif
    for (int h = 0; h <= 1; h++) {
        world(1u, 3u, 1u, 0u);
        check("intro-idle", h, idle, (!on && h) ? pressed : idle);
        check("intro-pressed", h, pressed, pressed);
        world(2u, 3u, 1u, 0u);
        check("other-cutscene", h, idle, idle);
        world(1u, 2u, 1u, 0u);
        check("manager-state", h, idle, idle);
        world(1u, 3u, 0u, 0u);
        check("startup-flag", h, idle, idle);
        world(1u, 3u, 1u, 1u);
        check("entry-skip-latch", h, idle, idle);
    }
    if (on && g_log_lines != 1) {
        printf("FAIL receipt lines=%d\n", g_log_lines);
        fails++;
    }
    if (!on && g_log_lines != 0) {
        printf("FAIL OFF logged %d lines\n", g_log_lines);
        fails++;
    }
    printf("intro oracle play=%d: cases=%d receipts=%d last='%s' failures=%d\n",
           on, cases, g_log_lines, g_last_log, fails);
    return fails != 0;
}
"""


def _compiler() -> list[str] | None:
    clang = shutil.which("clang") or "C:/Program Files/LLVM/bin/clang.exe"
    if Path(clang).exists():
        return [clang]
    gcc = shutil.which("gcc")
    return [gcc] if gcc else None


def verify_oracle(seam: str, work: Path) -> list[str]:
    compiler = _compiler()
    require(compiler is not None,
            "host oracle needs clang or gcc (none found); the seam oracle "
            "is not optional")
    vita = fenced_function(RUNTIME / "manual_kage_vita.c")
    source = (HARNESS
              .replace("@ALIAS@", ALIAS_BLOCK)
              .replace("@VITA@", vita)
              .replace("@SEAM@", seam)
              .replace("@MGR@", "0x%08x" % MANAGER_GLOBAL_RVA)
              .replace("@CUT@", "0x%x" % CUTSCENE_OFFSET))
    harness = work / "intro_oracle.c"
    harness.write_text(source, encoding="utf-8", newline="\n")
    results = []
    for on in (0, 1):
        exe = work / ("intro_oracle_%d%s" % (on, ".exe" if os.name == "nt"
                                              else ""))
        command = compiler + ["-std=c11", "-O1", "-Wall", "-Werror",
                              "-Wno-unused-function", str(harness),
                              "-o", str(exe)]
        if on:
            command.insert(1, "-DISAAC_VITA_INTRO_PLAY=1")
        built = subprocess.run(command, stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT)
        require(built.returncode == 0, "oracle build failed:\n%s" %
                built.stdout.decode("utf-8", "replace")[-3000:])
        ran = subprocess.run([str(exe)], stdout=subprocess.PIPE,
                             stderr=subprocess.STDOUT)
        out = ran.stdout.decode("utf-8", "replace").strip().splitlines()
        require(ran.returncode == 0 and out and
                out[-1].endswith("failures=0") and
                not any(line.startswith("FAIL") for line in out),
                "oracle failed (play=%d):\n%s" % (on, "\n".join(out)))
        if on:
            require(("LOG " + RECEIPT % 0) in out,
                    "ON receipt text drifted: %s" % out)
        results.append("host oracle: %s" % out[-1])
    return results


# ---- 6. corpus contract ----------------------------------------------------------

DEF_RE = re.compile(r"^void (sub_[0-9a-f]+)\(CPU \*__restrict c\)$", re.M)


def _units(directory: Path) -> list[Path]:
    units = sorted(directory.glob("guest_[0-9][0-9][0-9][0-9].c"))
    require(units, "no generated units in %s" % directory)
    return units


def verify_corpus(generated: Path, base: Path | None,
                  fresh_seam: str) -> list[str]:
    from gpr_flush_mask import legacy_text as unmask  # noqa: E402

    units = _units(generated)
    owners = [u for u in units
              if "\nvoid %s(CPU *__restrict c)\n" % OWNER_NAME
              in u.read_text(encoding="utf-8")]
    require(len(owners) == 1, "%s owners: %s" % (OWNER_NAME, owners))
    owner = owners[0]
    spelling = {u.name: u.read_text(encoding="utf-8").count(NEW_NAME)
                for u in units}
    spelling = {k: v for k, v in spelling.items() if v}
    require(spelling == {owner.name: 1},
            "%s spelled outside the owner: %s" % (NEW_NAME, spelling))
    results = ["corpus: %d units; %s in %s; the new guard only there, once"
               % (len(units), OWNER_NAME, owner.name)]
    corpus_seam = extract_seam(owner.read_text(encoding="utf-8"))
    # gen_all.py applies GUEST_GPR_FLUSH_MASK after packing; its own inverse
    # (gpr_flush_mask.legacy_text) maps every mask token back to FLUSH.
    require(unmask(corpus_seam) == fresh_seam,
            "corpus seam in %s is not this generator's seam (modulo "
            "GUEST_GPR_FLUSH_MASK); regenerate the corpus" % owner.name)
    results.append("corpus seam == fresh seam modulo GUEST_GPR_FLUSH_MASK: "
                   "PASS (%s, %d mask token(s))" % (
                       owner.name, corpus_seam.count("GUEST_GPR_FLUSH_MASK(")))
    if base is not None:
        def files(d):
            return sorted(p.name for p in d.iterdir() if p.is_file())
        require(files(base) == files(generated),
                "file sets differ: %s" % sorted(
                    set(files(base)) ^ set(files(generated))))
        moved = 0
        base_map = {}
        for unit in _units(base):
            for name in DEF_RE.findall(unit.read_text(encoding="utf-8")):
                base_map[name] = unit.name
        fix_map = {}
        for unit in units:
            for name in DEF_RE.findall(unit.read_text(encoding="utf-8")):
                fix_map[name] = unit.name
        require(set(base_map) == set(fix_map), "function sets differ")
        moved = sum(1 for k in base_map if base_map[k] != fix_map[k])
        require(moved == 0, "%d functions moved between units" % moved)
        differing = []
        for name in files(generated):
            a = (base / name).read_bytes()
            b = (generated / name).read_bytes()
            if a != b:
                differing.append(name)
        # Every other differing file may differ only in SHA-256 digests: the
        # generation/build id (gen_all.py is a recipe input) and, in
        # manifest.json, the owner's output hash, the gen_all.py input hash
        # and the output-set id.
        digest = re.compile(r"[0-9a-f]{64}")
        allowed_generation = set()
        for name in differing:
            if name == owner.name:
                continue
            a = (base / name).read_text(encoding="utf-8")
            b = (generated / name).read_text(encoding="utf-8")
            require(digest.sub("<H>", a) == digest.sub("<H>", b),
                    "%s differs outside SHA-256 digests" % name)
            allowed_generation.add(name)
        require(owner.name in differing, "owner unit did not change")
        a = (base / owner.name).read_text(encoding="utf-8")
        b = (generated / owner.name).read_text(encoding="utf-8")
        require(a.count(OLD_NAME + "()") == b.count(OLD_NAME + "()") + 1 and
                a.replace(OLD_NAME + "() &&\n                _intro_manager",
                          NEW_NAME + "() &&\n                _intro_manager",
                          1) == b,
                "%s differs by more than the rename" % owner.name)
        results.append(
            "corpus vs base: PASS (%d functions, 0 moved; differing: %s "
            "(rename only)%s)" % (
                len(fix_map), owner.name,
                "; SHA-256 digests only: " +
                ", ".join(sorted(allowed_generation))
                if allowed_generation else ""))
    return results


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pe", type=Path, required=True,
                        help="the frozen isaac-ng.exe.unpacked.exe")
    parser.add_argument("--generated-dir", type=Path)
    parser.add_argument("--base-generated-dir", type=Path)
    parser.add_argument("--keep", type=Path,
                        help="keep the oracle work directory here")
    args = parser.parse_args()
    if args.base_generated_dir and not args.generated_dir:
        parser.error("--base-generated-dir needs --generated-dir")

    print(verify_generator())
    for line in verify_runtime_and_build():
        print(line)
    work_parent = args.keep
    if work_parent:
        work_parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="isaac-intro-play-",
                                     dir=work_parent) as value:
        work = Path(value)
        units = _units(args.generated_dir) if args.generated_dir else None
        for line in verify_census(work, units):
            print(line)
        seam, lines = translate_owner(args.pe)
        for line in lines:
            print(line)
        for line in verify_oracle(seam, work):
            print(line)
        if args.generated_dir:
            for line in verify_corpus(args.generated_dir,
                                      args.base_generated_dir, seam):
                print(line)
        else:
            print("corpus contract: not run (no --generated-dir); the oracle "
                  "above ran on this generator's fresh seam")
    print("Intro play (issue #14) codegen/host oracle: PASS")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except AssertionError as exc:
        print("FAIL: %s" % exc)
        sys.exit(1)
