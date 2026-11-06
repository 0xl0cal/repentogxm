#!/usr/bin/env python3
"""Frozen-PE host/codegen oracle for the original Vita effects profile."""

from __future__ import annotations

import argparse
import hashlib
from pathlib import Path
import re
import sys


HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import build_one as B  # noqa: E402
import gen_all as G  # noqa: E402
from gpr_locals import bracket_seam_regions, legacy_text  # noqa: E402
import gpr_flush_mask  # noqa: E402
from image import DEFAULT_BASE, Image  # noqa: E402


VITA_BASE = 0x98000000
FENCE = (
    "#if defined(__vita__) && "
    "defined(ISAAC_VITA_ORIGINAL_EFFECTS_PROFILE)"
)
PROFILE = frozenset({"__vita__", "ISAAC_VITA_ORIGINAL_EFFECTS_PROFILE"})
KEEP_WATER = "ISAAC_VITA_EFFECTS_PROFILE_KEEP_WATER"
KEEP_WATER_FENCE = "#if !defined(%s)" % KEEP_WATER
KEEP_CM = "ISAAC_VITA_EFFECTS_PROFILE_KEEP_COLOR_MODIFIER"
KEEP_CM_FENCE = "#if !defined(%s)" % KEEP_CM
STORE = "    st8((uint32_t)(c->ebx + 0x%02xU), (uint8_t)(0U));"
# gpr_locals seam brackets as they appear in the generated owner.
FLUSH = "    GUEST_GPR_FLUSH(c);"
RELOAD = "    GUEST_GPR_RELOAD(c);"
# The injected block before either knob existed, copied from
# 2bbbc4d^:recomp/gen_all.py (render_vita_original_effects_profile).
PRE_KNOB_BLOCK = (
    FENCE,
    "    /* PCSB00676 effects plus no Repentance water/ColorModifier paths. */",
    STORE % 0x7F,
    STORE % 0x7E,
    STORE % 0x80,
    STORE % 0x83,
    STORE % 0x84,
    STORE % 0x85,
    "#endif",
)
# The exact statement list of the injected block before the water knob
# existed (2026-09-20): what the preprocessed owner must still contain with
# the profile ON and the knob OFF.
LEGACY_PROFILE_STATEMENTS = (
    "    /* PCSB00676 effects plus no Repentance water/ColorModifier paths. */",
    STORE % 0x7F,
    STORE % 0x7E,
    STORE % 0x80,
    STORE % 0x83,
    STORE % 0x84,
    STORE % 0x85,
)
KEEP_WATER_PROFILE_STATEMENTS = (
    "    /* PCSB00676 effects plus no Repentance water/ColorModifier paths. */",
    STORE % 0x7F,
    STORE % 0x7E,
    STORE % 0x80,
    STORE % 0x85,
)
# ISAAC_VITA_EFFECTS_PROFILE_KEEP_COLOR_MODIFIER (issue #5 item 15-i): the
# EnableColorModifier store (+0x85) is dropped, alone and with KEEP_WATER,
# and one receipt per seam reads the six profile bytes back.
KEEP_CM_PROFILE_STATEMENTS = (
    "    /* PCSB00676 effects plus no Repentance water/ColorModifier paths. */",
    STORE % 0x7F,
    STORE % 0x7E,
    STORE % 0x80,
    STORE % 0x83,
    STORE % 0x84,
)
KEEP_BOTH_PROFILE_STATEMENTS = (
    "    /* PCSB00676 effects plus no Repentance water/ColorModifier paths. */",
    STORE % 0x7F,
    STORE % 0x7E,
    STORE % 0x80,
)
RECEIPT_FORMAT = (
    "KAGE VITA EFFECTS PROFILE: color_modifier=kept shockwave=%u "
    "caustics=%u filter=%u water=%u latch=%u color_modifier=%u")
RECEIPT_CALL = (
    '            isaac_vita_log("' + RECEIPT_FORMAT + '", ' +
    ", ".join(
        "(unsigned)ld8((uint32_t)(c->ebx + 0x%02xU))" % offset
        for offset in (0x7E, 0x7F, 0x80, 0x83, 0x84, 0x85)) + ");")
RECEIPT_LINES = (
    "    {",
    "        static uint8_t s_effects_profile_receipt;",
    "        if (!s_effects_profile_receipt) {",
    "            extern void isaac_vita_log(const char *format, ...);",
    "            s_effects_profile_receipt = 1u;",
    RECEIPT_CALL,
    "        }",
    "    }",
)
RECEIPT_BRACKETED = (
    RECEIPT_LINES[:5] +
    ("            GUEST_GPR_FLUSH(c);", RECEIPT_CALL,
     "            GUEST_GPR_RELOAD(c);") +
    RECEIPT_LINES[6:])
# The same lists after gpr_locals.bracket_seam_regions(): one seam per
# preprocessor block.  The knob-OFF shape is copied from the base corpus
# owner (guest_0138.c at 97eae1d, 2026-09-26 regeneration).
LEGACY_PROFILE_BRACKETED = (
    (LEGACY_PROFILE_STATEMENTS[0], FLUSH) +
    LEGACY_PROFILE_STATEMENTS[1:] + (RELOAD,))
KEEP_WATER_PROFILE_BRACKETED = (
    (KEEP_WATER_PROFILE_STATEMENTS[0], FLUSH) +
    KEEP_WATER_PROFILE_STATEMENTS[1:] + (RELOAD,))
KEEP_CM_PROFILE_BRACKETED = (
    (KEEP_CM_PROFILE_STATEMENTS[0], FLUSH) +
    KEEP_CM_PROFILE_STATEMENTS[1:] + (RELOAD,) + RECEIPT_BRACKETED)
KEEP_BOTH_PROFILE_BRACKETED = (
    (KEEP_BOTH_PROFILE_STATEMENTS[0], FLUSH) +
    KEEP_BOTH_PROFILE_STATEMENTS[1:] + (RELOAD,) + RECEIPT_BRACKETED)
# (defined macros on top of the profile) -> expected preprocessed block.
KNOB_CASES = (
    (frozenset(), LEGACY_PROFILE_STATEMENTS, LEGACY_PROFILE_BRACKETED),
    (frozenset({KEEP_WATER}), KEEP_WATER_PROFILE_STATEMENTS,
     KEEP_WATER_PROFILE_BRACKETED),
    (frozenset({KEEP_CM}), KEEP_CM_PROFILE_STATEMENTS + RECEIPT_LINES,
     KEEP_CM_PROFILE_BRACKETED),
    (frozenset({KEEP_WATER, KEEP_CM}),
     KEEP_BOTH_PROFILE_STATEMENTS + RECEIPT_LINES,
     KEEP_BOTH_PROFILE_BRACKETED),
)


def _preprocess(lines, defined):
    """Evaluate the block's own #if/#else/#endif lines for defined macros.

    Only the condition shapes the block emits are accepted
    (`defined(A) && defined(B)`, `!defined(A)` and a bare `#else`); anything
    else fails so a new directive shape cannot pass unexamined.
    """
    kept = []
    stack = []
    for line in lines:
        if line.startswith("#if "):
            value = True
            for term in line[len("#if "):].split(" && "):
                match = re.fullmatch(r"(!?)defined\((\w+)\)", term)
                assert match is not None, line
                truth = match.group(2) in defined
                value = value and (truth != bool(match.group(1)))
            stack.append(value)
        elif line == "#else":
            stack[-1] = not stack[-1]
        elif line == "#endif":
            stack.pop()
        else:
            assert not line.startswith("#"), line
            if all(stack):
                kept.append(line)
    assert stack == []
    return tuple(kept)


def _raw_blocks(text):
    """The injected blocks of a generated owner, FENCE to its #endif, raw
    (seam brackets included)."""
    lines = text.split("\n")
    blocks = []
    index = 0
    while index < len(lines):
        if lines[index] != FENCE:
            index += 1
            continue
        depth = 0
        for end in range(index, len(lines)):
            if lines[end].startswith("#if"):
                depth += 1
            elif lines[end] == "#endif":
                depth -= 1
                if depth == 0:
                    break
        else:
            raise AssertionError("unterminated effects profile block")
        blocks.append(tuple(lines[index:end + 1]))
        index = end + 1
    return blocks


def _pre_knob_text(text, block):
    """The text with every injected block replaced by the pre-knob block:
    the spelling the unit packing budget is frozen to."""
    rendered = "\n".join(block)
    assert text.count(rendered) == 2
    return text.replace(rendered, "\n".join(PRE_KNOB_BLOCK))


def _verify_knob_block() -> None:
    block = G.render_vita_original_effects_profile()
    # Two nested fences, KEEP_COLOR_MODIFIER outside, KEEP_WATER inside each
    # of its branches; every leaf is a complete store list.  gpr_locals
    # brackets each preprocessor block as its own seam, so a fence inside
    # the store run would split it (checked on the bracketed text below).
    assert block.count(KEEP_CM_FENCE) == 1
    assert sum(KEEP_CM in line for line in block) == 1
    assert block.count(KEEP_WATER_FENCE) == 2
    assert sum(KEEP_WATER in line for line in block) == 2
    assert block[0] == FENCE
    assert block[1] == LEGACY_PROFILE_STATEMENTS[0]
    assert block[2] == KEEP_CM_FENCE
    assert block[3] == KEEP_WATER_FENCE
    assert block[4:10] == LEGACY_PROFILE_STATEMENTS[1:]
    assert block[10] == "#else"
    assert block[11:15] == KEEP_WATER_PROFILE_STATEMENTS[1:]
    assert block[15:18] == ("#endif", "#else", KEEP_WATER_FENCE)
    assert block[18:23] == KEEP_CM_PROFILE_STATEMENTS[1:]
    assert block[23] == "#else"
    assert block[24:27] == KEEP_BOTH_PROFILE_STATEMENTS[1:]
    assert block[27] == "#endif"
    assert block[28:36] == RECEIPT_LINES
    assert block[36:] == ("#endif", "#endif")
    assert block.count("#else") == 3 and block.count("#endif") == 4
    assert G.VITA_ORIGINAL_EFFECTS_PROFILE_WATER_FIELDS == (0x83, 0x84)
    assert G.VITA_ORIGINAL_EFFECTS_PROFILE_COLOR_MODIFIER_FIELDS == (0x85,)
    assert G.VITA_ORIGINAL_EFFECTS_PROFILE_COLOR_MODIFIER_RECEIPT == (
        RECEIPT_FORMAT)
    assert G.VITA_ORIGINAL_EFFECTS_PROFILE_PRE_KNOB_BLOCK == PRE_KNOB_BLOCK
    water_stores = (STORE % 0x83, STORE % 0x84)
    cm_stores = (STORE % 0x85,)
    for statements, dropped in (
            (KEEP_WATER_PROFILE_STATEMENTS, water_stores),
            (KEEP_CM_PROFILE_STATEMENTS, cm_stores),
            (KEEP_BOTH_PROFILE_STATEMENTS, water_stores + cm_stores)):
        assert tuple(
            statement for statement in LEGACY_PROFILE_STATEMENTS
            if statement not in dropped) == statements
    # The receipt reads the six bytes the game sees, in the printed order.
    assert RECEIPT_FORMAT.count("%u") == 6
    assert RECEIPT_CALL.count("ld8(") == 6
    assert "st8(" not in "\n".join(RECEIPT_LINES)

    assert _preprocess(block, set()) == ()
    assert _preprocess(block, {"__vita__"}) == ()
    assert _preprocess(block, {"__vita__", KEEP_WATER, KEEP_CM}) == ()
    # Packing: the knob bytes are exactly what the block gained over the
    # pre-knob text, and the pre-knob block is the knob-OFF path.
    assert G.vita_original_effects_profile_knob_bytes() == (
        sum(len(line) + 1 for line in block) -
        sum(len(line) + 1 for line in PRE_KNOB_BLOCK))
    assert _preprocess(PRE_KNOB_BLOCK, PROFILE) == LEGACY_PROFILE_STATEMENTS

    # The seam pass output, not only the emitted text: with both knobs OFF
    # the bracketed block must be the pre-knob bracketed block (one FLUSH,
    # six stores, one RELOAD), with KEEP_WATER alone the 2026-09-20 water
    # block, and each KEEP_COLOR_MODIFIER leaf its stores plus the bracketed
    # receipt.
    bracketed = tuple(bracket_seam_regions(list(block), "effects knob block"))
    assert _preprocess(bracketed, set()) == ()
    for extra, statements, expected in KNOB_CASES:
        assert _preprocess(block, PROFILE | extra) == statements, extra
        assert _preprocess(bracketed, PROFILE | extra) == expected, extra
    kept = "\n".join(_preprocess(block, PROFILE | {KEEP_CM}))
    assert "0x85U), (uint8_t)(0U)" not in kept
    kept = "\n".join(_preprocess(block, PROFILE | {KEEP_WATER}))
    assert "0x83U" not in kept and "0x84U" not in kept
    _verify_flush_masks(bracketed)


def _verify_flush_masks(bracketed) -> None:
    """GUEST_GPR_FLUSH_MASK=1 corpora: the block must leave the owner's
    other masks alone.  gpr_flush_mask merges every preprocessor branch, so
    host code on any path (the receipt's declaration once sat at block
    level) leaks a HOST bit past the block and turns the next call site's
    mask into a full FLUSH in every build, knob OFF included (seen at
    0x47fc6f 0x3b and 0x47fe43 0x13 before the declaration moved).  A
    synthetic body: dirty ebx, the bracketed block, then a push and a call;
    the call's FLUSH must publish exactly ebx (still dirty on the path with
    the profile off) and esp (the push), with no HOST bit."""
    body = "\n".join(
        ("    GUEST_GPR_DECL;",
         "    GR(ebx) = (uint32_t)(GR(esp));") + tuple(bracketed) +
        ("    GPUSH(0x1U); GUEST_STACK_CALLSITE_BARRIER(); "
         "GUEST_GPR_FLUSH(c); sub_00000001(c); GUEST_GPR_RELOAD(c);",
         "    return;"))
    unit = "void sub_00000000(CPU *__restrict c)\n{\n" + body + "\n}\n"
    masked = gpr_flush_mask.mask_unit(unit)
    assert gpr_flush_mask.legacy_text(masked) == unit
    # Every leaf FLUSH publishes ebx (dirty) and nothing unknown; the
    # receipt's own FLUSH stays full (cold, after the declaration); the
    # call after the block publishes ebx|esp, not FULL.
    leaf_mask = "    GUEST_GPR_FLUSH_MASK(c, 0x%02xU);" % gpr_flush_mask.BIT["ebx"]
    assert masked.count(leaf_mask) == 4, masked
    assert masked.count("            GUEST_GPR_FLUSH(c);") == 1, masked
    assert masked.count(
        "GUEST_GPR_FLUSH_MASK(c, 0x%02xU); sub_00000001(c);" %
        (gpr_flush_mask.BIT["ebx"] | gpr_flush_mask.BIT["esp"])) == 1, masked


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _raw_translation(img: Image):
    result = B.translate(
        img,
        G.VITA_ORIGINAL_EFFECTS_PROFILE_ROOT_RVA,
        "sub_0047fbe0",
        None,
    )
    em, insns, order, body, unsupported, indirect, _members = result
    assert unsupported == []
    assert indirect == []
    return em, insns, order, body


def _verify_generator(pe_path: Path) -> None:
    pin = Image(str(pe_path), DEFAULT_BASE)
    em, insns, order, body = _raw_translation(pin)
    assert G.vita_original_effects_profile_for_body(
        pin,
        G.VITA_ORIGINAL_EFFECTS_PROFILE_ROOT_RVA,
        em,
        insns,
        order,
        body,
    )

    expected_block = "\n".join(
        G.render_vita_original_effects_profile()
    )
    blocks = {}
    for base in (DEFAULT_BASE, VITA_BASE):
        emit = Image(str(pe_path), base)
        result = G._translate_function(
            emit,
            {"rva": G.VITA_ORIGINAL_EFFECTS_PROFILE_ROOT_RVA},
            None,
            {},
            pin_img=pin,
        )
        assert result["stub"] is None
        assert result["vita_original_effects_profile"] is True
        source = legacy_text(result["text"])
        # One block after the last default store (reached on every path) and
        # one after the last GetBool store (reached only with config.ini).
        assert source.count(FENCE) == 2
        assert source.count(expected_block) == 2
        # The knob fences exist only inside those two blocks (KEEP_COLOR_
        # MODIFIER once per seam, KEEP_WATER once per branch of it) and
        # nowhere else in the owner function.
        assert source.count(KEEP_CM_FENCE) == 2
        assert source.count(KEEP_CM) == 2
        assert source.count(KEEP_WATER_FENCE) == 4
        assert source.count(KEEP_WATER) == 4
        assert source.count(RECEIPT_FORMAT) == 2
        # legacy_text() strips the seam brackets; the raw owner is the build
        # input.  Each injected block preprocessed with both knobs OFF must
        # be the pre-knob bracketed block, and every knob combination its
        # own bracketed leaf.
        raw_blocks = _raw_blocks(result["text"])
        assert len(raw_blocks) == 2
        for raw_block in raw_blocks:
            for extra, _statements, expected in KNOB_CASES:
                assert _preprocess(raw_block, PROFILE | extra) == expected, (
                    extra)
        # Unit membership is frozen: the owner's packing budget
        # (unit_packing_size + legacy_size_delta) must be that of the
        # pre-knob text, i.e. with each block replaced by the pre-knob one.
        pre_knob = _pre_knob_text(
            source, G.render_vita_original_effects_profile())
        assert pre_knob.count(KEEP_WATER) == 0
        assert pre_knob.count(KEEP_CM) == 0
        assert pre_knob.count("\n".join(PRE_KNOB_BLOCK)) == 2
        assert len(source) - len(pre_knob) == (
            2 * G.vita_original_effects_profile_knob_bytes())
        assert G.unit_packing_size(result["text"]) + (
            result["legacy_size_delta"]) == len(pre_knob)

        default_comment = (
            "    /* %08x" % G.VITA_ORIGINAL_EFFECTS_PROFILE_DEFAULT_SITE_RVA
        )
        default_next_comment = "    /* 0047fc2b"
        default_at = source.index(default_comment)
        default_store_at = source.index(
            "    " + G.VITA_ORIGINAL_EFFECTS_PROFILE_DEFAULT_SITE_STATEMENT,
            default_at,
        )
        default_block_at = source.index(expected_block, default_store_at)
        default_next_at = source.index(default_next_comment, default_block_at)
        assert default_at < default_store_at < default_block_at < default_next_at

        site_comment = "    /* %08x" % G.VITA_ORIGINAL_EFFECTS_PROFILE_SITE_RVA
        next_comment = "    /* 0047fe37"
        site_at = source.index(site_comment)
        original_at = source.index(
            "    " + G.VITA_ORIGINAL_EFFECTS_PROFILE_SITE_STATEMENT,
            site_at,
        )
        block_at = source.index(expected_block, original_at)
        next_at = source.index(next_comment, block_at)
        assert site_at < original_at < block_at < next_at
        assert default_block_at < site_at
        blocks[base] = expected_block
    assert blocks[DEFAULT_BASE] == blocks[VITA_BASE]

    # Machine, owner and source-name drift must each fail closed even if a
    # caller accidentally reuses the original decoded instruction objects.
    pin.mem = bytearray(pin.mem)
    mutations = (
        G.VITA_ORIGINAL_EFFECTS_PROFILE_SITE_RVA,
        G.VITA_ORIGINAL_EFFECTS_PROFILE_DEFAULT_SITE_RVA,
        G.VITA_ORIGINAL_EFFECTS_PROFILE_EARLY_EXIT_RVA,
        G.VITA_ORIGINAL_EFFECTS_PROFILE_ROOT_RVA,
        G.VITA_ORIGINAL_EFFECTS_PROFILE_STRING_RVAS[
            G.VITA_ORIGINAL_EFFECTS_PROFILE_FILTER_OFFSET
        ][0],
        G.VITA_ORIGINAL_EFFECTS_PROFILE_STRING_RVAS[
            G.VITA_ORIGINAL_EFFECTS_PROFILE_WATER_OFFSET
        ][0],
        G.VITA_ORIGINAL_EFFECTS_PROFILE_STRING_RVAS[
            G.VITA_ORIGINAL_EFFECTS_PROFILE_COLOR_MODIFIER_OFFSET
        ][0],
    )
    for rva in mutations:
        original = pin.mem[rva]
        pin.mem[rva] ^= 0x01
        try:
            G.vita_original_effects_profile_for_body(
                pin,
                G.VITA_ORIGINAL_EFFECTS_PROFILE_ROOT_RVA,
                em,
                insns,
                order,
                body,
            )
        except RuntimeError as exc:
            assert "original-Vita" in str(exc)
        else:
            raise AssertionError(
                "mutated original-Vita effects owner passed at 0x%08x" % rva
            )
        finally:
            pin.mem[rva] = original


def _verify_host_oracle() -> None:
    before = bytearray(range(0x90))
    before[0x7C] = 1  # Color correction differs across Vita revisions.
    before[0x7D] = 1  # Lighting remains enabled on both revisions.
    before[0x7E] = 1  # Shockwave: PC default, disabled on Vita.
    before[0x7F] = 1  # Caustics: PC default, disabled on Vita.
    before[0x80] = 1  # Filter: PC default, disabled on Vita.
    before[0x81] = 1  # Pixelation differs across Vita revisions.
    before[0x83] = 1  # Repentance water surface has no Rebirth equivalent.
    before[0x84] = 1  # Loader's copied water-surface latch.
    before[0x85] = 1  # Repentance whole-room ColorModifier surface.
    after = G.vita_original_effects_profile_host_oracle(before)

    changed = {
        index for index, (old, new) in enumerate(zip(before, after))
        if old != new
    }
    assert changed == set(G.VITA_ORIGINAL_EFFECTS_PROFILE_FIELDS)
    assert after[0x7C] == 1
    assert after[0x7D] == 1
    assert after[0x7E] == 0
    assert after[0x7F] == 0
    assert after[0x80] == 0
    assert after[0x81] == 1
    assert after[0x83] == 0
    assert after[0x84] == 0
    assert after[0x85] == 0

    # ISAAC_VITA_EFFECTS_PROFILE_KEEP_WATER: the two water bytes stay as the
    # loader stored them; the other four still move, nothing else does.
    kept = G.vita_original_effects_profile_host_oracle(before, keep_water=True)
    kept_changed = {
        index for index, (old, new) in enumerate(zip(before, kept))
        if old != new
    }
    assert kept_changed == (
        set(G.VITA_ORIGINAL_EFFECTS_PROFILE_FIELDS) -
        set(G.VITA_ORIGINAL_EFFECTS_PROFILE_WATER_FIELDS))
    assert kept_changed == {0x7E, 0x7F, 0x80, 0x85}
    assert kept[0x83] == 1
    assert kept[0x84] == 1
    assert kept[0x85] == 0

    # ISAAC_VITA_EFFECTS_PROFILE_KEEP_COLOR_MODIFIER: EnableColorModifier
    # stays as the loader stored it, alone and with KEEP_WATER.
    for keep_water, expected in (
            (False, {0x7E, 0x7F, 0x80, 0x83, 0x84}),
            (True, {0x7E, 0x7F, 0x80})):
        cm = G.vita_original_effects_profile_host_oracle(
            before, keep_water=keep_water, keep_color_modifier=True)
        cm_changed = {
            index for index, (old, new) in enumerate(zip(before, cm))
            if old != new
        }
        assert cm_changed == expected
        assert cm[0x85] == 1
        assert cm[0x7C] == 1 and cm[0x7D] == 1 and cm[0x81] == 1

    try:
        G.vita_original_effects_profile_host_oracle(b"short")
    except ValueError:
        pass
    else:
        raise AssertionError("short OptionsConfig image was accepted")


def _verify_build_switch() -> None:
    cmake = (HERE / "vita/CMakeLists.txt").read_text(encoding="utf-8")
    build_tool = (HERE.parent / "tools/build_vita.py").read_text(
        encoding="utf-8"
    )
    option = re.search(
        r"option\(ISAAC_VITA_ORIGINAL_EFFECTS_PROFILE\s+.*?\s+(ON|OFF)\)",
        cmake,
        flags=re.S,
    )
    assert option is not None and option.group(1) == "OFF"
    assert "ISAAC_VITA_ORIGINAL_EFFECTS_PROFILE requires the translated" in cmake
    assert cmake.count("ISAAC_VITA_ORIGINAL_EFFECTS_PROFILE=1") == 1
    assert cmake.count("sub_0047fbe0") == 1
    # ISAAC_VITA_EFFECTS_PROFILE_WATER: default OFF, refused without the
    # profile, and its define lands on the same single owner unit only.
    water = re.search(
        r"option\(ISAAC_VITA_EFFECTS_PROFILE_WATER\s+.*?\s+(ON|OFF)\)",
        cmake,
        flags=re.S,
    )
    assert water is not None and water.group(1) == "OFF"
    assert (
        "ISAAC_VITA_EFFECTS_PROFILE_WATER requires "
        "ISAAC_VITA_ORIGINAL_EFFECTS_PROFILE=ON"
    ) in cmake
    assert cmake.count(KEEP_WATER + "=1") == 1
    assert cmake.count(KEEP_WATER) == 1
    define_at = cmake.index(KEEP_WATER + "=1")
    owner_at = cmake.index("ISAAC_VITA_ORIGINAL_EFFECTS_PROFILE=1")
    assert owner_at < define_at
    assert cmake.count(
        "${ISAAC_VITA_ORIGINAL_EFFECTS_PROFILE_OWNER}", owner_at, define_at
    ) == 2
    # ISAAC_VITA_EFFECTS_PROFILE_KEEP_COLOR_MODIFIER: the same shape, its
    # define after the water one on the same owner.
    color = re.search(
        r"option\(%s\s+.*?\s+(ON|OFF)\)" % KEEP_CM,
        cmake,
        flags=re.S,
    )
    assert color is not None and color.group(1) == "OFF"
    assert (
        "ISAAC_VITA_EFFECTS_PROFILE_KEEP_COLOR_MODIFIER requires "
        "ISAAC_VITA_ORIGINAL_EFFECTS_PROFILE=ON"
    ) in cmake
    assert cmake.count(KEEP_CM + "=1") == 1
    color_at = cmake.index(KEEP_CM + "=1")
    assert define_at < color_at
    assert cmake.count(
        "${ISAAC_VITA_ORIGINAL_EFFECTS_PROFILE_OWNER}", define_at, color_at
    ) == 1
    assert build_tool.count(
        '"-DISAAC_VITA_ORIGINAL_EFFECTS_PROFILE=OFF"'
    ) == 2
    assert '"ISAAC_VITA_ORIGINAL_EFFECTS_PROFILE": False' in build_tool


CM_CHECK_BEGIN = "      # keep-color-modifier corpus check begin\n"
CM_CHECK_END = "      # keep-color-modifier corpus check end\n"


def _cmake_line_regex(line: str) -> str:
    """The file(STRINGS REGEX) spelling of one exact generated line."""
    return '"^%s$"' % line.replace("(", "\\\\(").replace(")", "\\\\)")


def _verify_color_modifier_corpus_check() -> None:
    """KEEP_COLOR_MODIFIER=ON must refuse a corpus without the knob fence.

    The CMake block is cut out of CMakeLists.txt and executed with cmake -P
    against owners built from the generator's own lines, so the regexes are
    checked against what gen_all.py emits, not against a copy of it."""
    import shutil
    import subprocess
    import tempfile

    cmake_text = (HERE / "vita/CMakeLists.txt").read_text(encoding="utf-8")
    assert cmake_text.count(CM_CHECK_BEGIN) == 1
    assert cmake_text.count(CM_CHECK_END) == 1
    begin = cmake_text.index(CM_CHECK_BEGIN)
    end = cmake_text.index(CM_CHECK_END)
    # Inside the KEEP_COLOR_MODIFIER branch, after its define.
    assert cmake_text.index(KEEP_CM + "=1") < begin < end
    knob_if = cmake_text.rindex("if(%s)" % KEEP_CM, 0, begin)
    assert cmake_text.count("endif()", knob_if, begin) == 0
    check = cmake_text[begin:end]
    block = list(G.render_vita_original_effects_profile())
    fence_line = block[0]
    assert fence_line == FENCE
    assert G.VITA_ORIGINAL_EFFECTS_PROFILE_PRE_KNOB_BLOCK[0] == FENCE
    assert _cmake_line_regex(FENCE) in check
    assert _cmake_line_regex(KEEP_CM_FENCE) in check
    assert KEEP_CM_FENCE == G.VITA_ORIGINAL_EFFECTS_PROFILE_KEEP_COLOR_MODIFIER_FENCE
    assert "message(FATAL_ERROR" in check

    cmake = shutil.which("cmake")
    assert cmake, "cmake is required to execute the corpus check"
    filler = ["static void sub_00000000(CPU *__restrict c) {", "}"]
    old = list(G.VITA_ORIGINAL_EFFECTS_PROFILE_PRE_KNOB_BLOCK)
    cases = (
        # name, owner lines, configure must pass, expected counts
        ("generated", filler + block + filler + block + filler, True, None),
        ("pre-knob", filler + old + filler + old, False, (0, 2)),
        ("mixed", filler + block + filler + old, False, (1, 2)),
        ("no-profile", filler, False, (0, 0)),
        ("indented-fence", filler + ["  " + l if l == KEEP_CM_FENCE else l
                                     for l in block], False, (0, 1)),
    )
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        for name, lines, ok, counts in cases:
            owner = root / ("owner-%s.c" % name)
            owner.write_text("\n".join(lines) + "\n", encoding="utf-8",
                             newline="\n")
            script = root / ("check-%s.cmake" % name)
            script.write_text(
                'set(ISAAC_VITA_ORIGINAL_EFFECTS_PROFILE_OWNER "%s")\n%s'
                'message(STATUS "keep-color-modifier corpus check: ok")\n'
                % (owner.as_posix(), check),
                encoding="utf-8", newline="\n")
            run_ = subprocess.run([cmake, "-P", str(script)],
                                  capture_output=True, text=True)
            out = " ".join((run_.stdout + run_.stderr).split())
            if ok:
                assert run_.returncode == 0, (name, out)
                assert "keep-color-modifier corpus check: ok" in out, (name, out)
            else:
                assert run_.returncode != 0, (name, out)
                assert ("ISAAC_VITA_EFFECTS_PROFILE_KEEP_COLOR_MODIFIER needs "
                        "a corpus generated with its fence") in out, (name, out)
                assert ("has %d fence(s) for %d profile block(s)" % counts
                        in out), (name, out)
                assert "corpus check: ok" not in out, (name, out)


def run(pe_path: Path) -> None:
    pe_path = pe_path.resolve()
    assert pe_path.stat().st_size == G.VITA_ORIGINAL_EFFECTS_PROFILE_PE_SIZE
    assert _sha256(pe_path) == G.VITA_ORIGINAL_EFFECTS_PROFILE_PE_SHA256
    _verify_generator(pe_path)
    _verify_knob_block()
    _verify_host_oracle()
    _verify_build_switch()
    _verify_color_modifier_corpus_check()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pe", type=Path, required=True)
    args = parser.parse_args()
    run(args.pe)
    print("original Vita effects host/codegen oracle: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
