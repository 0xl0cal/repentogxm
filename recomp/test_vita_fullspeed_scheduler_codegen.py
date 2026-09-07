#!/usr/bin/env python3
"""Frozen-J835 proof for the Vita 60/30 cadence generated seams."""

from __future__ import annotations

import argparse
import copy
import hashlib
import re
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import gen_all as G  # noqa: E402
from gpr_locals import legacy_text  # noqa: E402
from image import DEFAULT_BASE, Image  # noqa: E402
from vita.test_kage_vita_fullspeed_scheduler import compiler, run  # noqa: E402
from vita.test_kage_vita_phase_profile import function_body  # noqa: E402

PE_SIZE = 8_650_240
PE_SHA256 = "31846486979cfa07c8c968221553052c3ff603518681ca11912d445f96ca9404"
VITA_IMAGE_BASE = 0x98000000
APP_ROOT = 0x0048BC50
MANAGER_ROOT = 0x004B0010
MANAGER_END = 0x004B0600
RENDER_ROOT = 0x004B0600
RENDER_END = 0x004B1090
GAME_UPDATE_ROOT = 0x002CDCF0
GAME_UPDATE_END = 0x002CE750
GAME_PUBLISH_ROOT = 0x004B3FD0

GAME_UPDATE_SWITCH_ENTRIES = (
    0x002CDEE3, 0x002CDF18, 0x002CDF37,
    0x002CDF56, 0x002CDFA9, 0x002CE022,
)
GAME_UPDATE_SWITCH_TARGETS = (
    0x002CDF18, 0x002CDEE3, 0x002CDF37,
    0x002CDF56, 0x002CDFA9, 0x002CE022,
)
GAME_UPDATE_EARLY_RETURNS = (
    0x002CE074, 0x002CE099, 0x002CE0BE, 0x002CE0EA,
    0x002CE121, 0x002CE167, 0x002CE1C7, 0x002CE362,
)
GAME_UPDATE_RETURNS = GAME_UPDATE_EARLY_RETURNS + (0x002CE74F,)

MANAGER_SWITCH_ENTRIES = (
    0x004B0298, 0x004B047F, 0x004B0486, 0x004B0493, 0x004B049E,
)
MANAGER_SWITCH_TARGETS = (
    0x004B047F, 0x004B0298, 0x004B0486, 0x004B049E, 0x004B0493,
)
RENDER_SWITCH_ENTRIES = (
    0x004B07D6, 0x004B07E6, 0x004B07F0, 0x004B09A1, 0x004B09B4,
)
RENDER_SWITCH_TARGETS = (
    0x004B07E6, 0x004B07D6, 0x004B07F0, 0x004B09B4, 0x004B09A1,
)


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def call_target(image: Image, rva: int) -> int:
    call = image.code_at(rva, 5)
    if len(call) != 5 or call[0] != 0xE8:
        raise AssertionError(f"expected direct call at 0x{rva:08x}")
    return (rva + 5 + int.from_bytes(call[1:], "little", signed=True)) & 0xFFFFFFFF


def discover_switch_info(
    image: Image, root: int, owner_end: int,
    expected_entries: tuple[int, ...], site: int,
    expected_targets: tuple[int, ...],
) -> dict[int, dict[str, object]]:
    entries, edges, rejected, tables = G.discover_jump_tables(
        image, root, owner_end=owner_end
    )
    if entries != expected_entries:
        raise AssertionError(
            f"switch entries changed at 0x{root:08x}: {entries!r}"
        )
    expected = {site: expected_targets}
    if edges != expected or tables != expected or rejected:
        raise AssertionError(
            f"switch proof changed at 0x{root:08x}: "
            f"edges={edges!r} rejected={rejected!r} tables={tables!r}"
        )
    return {
        root: {
            "entries": entries,
            "edges": edges,
            "rejected": rejected,
            "tables": tables,
        }
    }


def translate(
    image: Image, pin: Image, root: int,
    switch_info: dict[int, dict[str, object]],
) -> dict[str, object]:
    result = G._translate_function(
        image, {"rva": root}, None, switch_info, pin_img=pin
    )
    if result.get("stub") is not None:
        raise AssertionError(
            f"cadence owner 0x{root:08x} became a generated stub: "
            f"{result.get('stub')}"
        )
    return result


def feature_blocks(text: str) -> list[str]:
    marker = (
        "#if defined(__vita__) && "
        "defined(ISAAC_VITA_FULLSPEED_SCHEDULER)\n"
    )
    blocks = []
    position = 0
    while True:
        begin = text.find(marker, position)
        if begin < 0:
            return blocks
        end = text.find("#endif\n", begin)
        if end < 0:
            raise AssertionError("unterminated cadence feature block")
        blocks.append(text[begin:end + len("#endif\n")])
        position = end + len("#endif\n")


def assert_read_only(blocks: list[str], expected: int) -> None:
    if len(blocks) != expected:
        raise AssertionError(
            f"expected {expected} cadence blocks, found {len(blocks)}"
        )
    for block in blocks:
        if "st8(" in block or "st16(" in block or "st32(" in block or \
                "st64(" in block:
            raise AssertionError("cadence observation writes guest memory")
        if re.search(r"c->[a-zA-Z0-9_]+\s*=", block):
            raise AssertionError("cadence observation writes guest CPU state")
        if "sub_002cdcf0" in block:
            raise AssertionError("cadence feature directly calls Game::Update")


def verify_app(result: dict[str, object]) -> str:
    if result.get("vita_fullspeed_scheduler") is not True:
        raise AssertionError("application cadence owner was not selected")
    if result.get("vita_fullspeed_cadence_seams") != ():
        raise AssertionError("application owner acquired Manager/Render seams")
    text = str(legacy_text(result["text"]))

    # The gate is one complete statement (the GPR seam pass brackets it): a
    # skipped frame notes the skip and jumps over the Render range to the
    # fenced label at the continuation instruction.
    render_query = "if (!kage_pc_backend_fullspeed_plan_render("
    skipped_note = "kage_pc_backend_note_render_skipped();"
    skip_jump = "goto L_vita_fullspeed_render_done;"
    limiter_query = "if (kage_pc_backend_fullspeed_bypass_limiter())"
    for marker in (render_query, skipped_note, skip_jump, limiter_query,
                   "L_vita_fullspeed_render_done:"):
        if text.count(marker) != 1:
            raise AssertionError(
                f"application cadence marker absent or duplicated: {marker}"
            )

    render_gate = text.index(render_query)
    skipped = text.index(skipped_note)
    skip = text.index(skip_jump)
    render_entry = text.index("kage_pc_backend_note_render_entry();")
    render_call = text.index("/* 0048bdfa  call 0x4b0600 */")
    render_return = text.index("kage_pc_backend_note_render_return();")
    resume_label = text.index("L_vita_fullspeed_render_done:")
    continuation = text.index("/* 0048bdff  cmp dword ptr")
    # A skipped frame has no Render entry and therefore no Render return.
    # Its Update sample is already closed by note_render_skipped above.
    if not (
        render_gate < skipped < skip < render_entry < render_call <
        render_return < resume_label < continuation
    ):
        raise AssertionError("render gate no longer brackets only Render")

    limiter_gate = text.index(limiter_query)
    limiter_entry = text.index("kage_pc_backend_note_limiter_entry();")
    limiter_call = text.index("/* 0048be3e  call 0x570040 */")
    limiter_exit = text.index("kage_pc_backend_note_limiter_exit();")
    terminal_backedge = text.index("/* 0048bf0b  jmp 0x48bd40 */")
    if not (
        continuation < limiter_gate < limiter_entry < limiter_call <
        limiter_exit < terminal_backedge
    ):
        raise AssertionError("limiter bypass no longer owns the whole limiter")
    if text.count("goto L_0048bd40;") < 2:
        raise AssertionError("cadence scheduler lost the pinned loop-head edge")
    assert_read_only(feature_blocks(text), 3)
    return text


def execute_app_render_seam(result: dict[str, object]) -> None:
    """Run newly generated C, not a hand-written model of its skip branch.

    Reuse the existing phase oracle's two 120-tick windows and its real
    scheduler/profiler assertions. Only guest memory and Manager::Render are
    stand-ins; the entry/return/skip hooks are extracted from production.
    """
    runtime = HERE / "runtime"
    text = str(result["text"])
    marker = ("#if defined(__vita__) && "
              "defined(ISAAC_VITA_FULLSPEED_SCHEDULER)\n")
    start = text.index(marker, text.index("/* 0048bdf5  call 0x4b0010 */"))
    end = text.index("    /* explicit PC no-Lua main-loop bypass", start)
    fragment = text[start:end]
    if "GUEST_GPR_RELOAD(c); goto L_vita_fullspeed_render_done;" not in fragment:
        raise AssertionError("render skip must reload GPR locals before leaving its seam")

    # Recreate ce33caba's control-flow error as a negative control. With the
    # feature OFF, moving its fenced label must change no generated C token.
    done = marker + "L_vita_fullspeed_render_done:\n#endif\n"
    legacy = fragment.replace(done, "").replace(
        "goto L_vita_fullspeed_render_done;", "goto L_0048bdff;")
    return_heartbeat = "    /* explicit PC Render return heartbeat; original follows */"
    legacy = legacy.replace(return_heartbeat,
                            marker + "L_0048bdff:\n#endif\n" + return_heartbeat)
    def without_feature(value: str) -> str:
        for block in feature_blocks(value):
            value = value.replace(block, "")
        return value
    if without_feature(fragment) != without_feature(legacy):
        raise AssertionError("cadence skip fix changes feature-OFF generated C")

    hooks_source = (runtime / "kage_vita_generated_hooks.c").read_text(encoding="utf-8")
    hooks = []
    for name in ("entry", "return", "skipped"):
        signature = f"void kage_pc_backend_note_render_{name}(void)"
        hooks.append(signature + "\n" + function_body(hooks_source, signature))

    helpers = r'''
#include <assert.h>
#include "kage_vita_stall_probe.h"
static CPU *s_seam_cpu;
static uint32_t s_seam_counter, s_seam_frame, s_seam_render_us;
static uint32_t s_seam_expected_eax;
static int s_seam_rendered;
static uint32_t seam_read32(uint32_t address)
{
    switch (address) {
    case GUEST_IMAGE_BASE + 0x007fd680u: return 0x10000000u;
    case GUEST_IMAGE_BASE + 0x007fd65cu: return 0x20000000u;
    case 0x10000000u + 0x0004a264u: return s_seam_counter;
    case 0x20000000u + 0x001a30dcu: return s_seam_frame;
    default: assert(!"unexpected generated memory read"); return 0u;
    }
}
static uint8_t seam_read8(uint32_t address)
{
    assert(address == 0x10000000u + 0x00029e73u);
    return 0u;
}
int kage_pc_backend_mode(void) { return 1; }
int kage_pc_backend_fullspeed_plan_render(uint32_t manager, uint32_t counter,
    uint32_t interpolation, uint32_t game, uint32_t frame)
{
    int rendered = kage_vita_fullspeed_scheduler_plan_render(
        manager, counter, interpolation, game, frame);
    /* Synthetic seam clobber: missing GPR reload must fail even though the
     * present production observer happens not to modify the CPU. */
    s_seam_cpu->eax = s_seam_expected_eax = 0x12345678u;
    return rendered;
}
static void sub_004b0600(CPU *c)
{
    assert(c->esp == 0x30000000u - 4u);
    s_seam_rendered = 1;
    kage_vita_fullspeed_scheduler_note_render_body(
        0x10000000u, s_seam_counter, 0u);
    s_now += s_seam_render_us;
    kage_vita_fullspeed_scheduler_note_present();
    kage_vita_phase_profile_note_present_enter();
    s_now += 100u;
    kage_vita_phase_profile_note_present_return();
    s_now += 400u;
    c->esp += 4u;
    c->eax = s_seam_expected_eax = 0x87654321u;
}
@HOOKS@
static int generated_render_seam(uint32_t counter, uint32_t frame, uint32_t us)
{
    CPU state = {0};
    CPU *c = &state;
    state.esp = 0x30000000u;
    state.eax = 0x11111111u;
    s_seam_cpu = c;
    s_seam_counter = counter;
    s_seam_frame = frame;
    s_seam_render_us = us;
    s_seam_rendered = 0;
    GUEST_GPR_DECL;
    /* Mock the one synthetic stack store, not GPR FLUSH/RELOAD. */
#undef GPUSH
#define GPUSH(value) do { assert((value) == 0x0048bdffu); GR(esp) -= 4u; } while (0)
#define ld32(address) seam_read32(address)
#define ld8(address) seam_read8(address)
#define __vita__ 1
@FRAGMENT@
L_0048be0d:
#undef __vita__
#undef ld8
#undef ld32
    /* This is reached through the original guest continuation in both arms. */
    assert(GR(eax) == s_seam_expected_eax);
    assert(GR(esp) == 0x30000000u);
    GUEST_GPR_FLUSH(c);
    return s_seam_rendered;
}
'''.replace("@HOOKS@", "\n".join(hooks))
    oracle = (runtime / "kage_vita_fullspeed_phase_oracle.c").read_text(encoding="utf-8")
    begin = oracle.index("        render = kage_vita_fullspeed_scheduler_plan_render(")
    finish = oracle.index("        kage_vita_fullspeed_scheduler_snapshot(&after);", begin)
    oracle = oracle[:begin] + (
        "        render = generated_render_seam(manager_counter, game_frame, render_us);\n"
        "        if (render) ++renders[window];\n"
    ) + oracle[finish:]

    with tempfile.TemporaryDirectory(prefix="isaac-generated-cadence-") as temporary:
        directory = Path(temporary)
        cc = compiler()
        for local, broken in ((0, False), (1, False), (1, True)):
            source = directory / f"render-{local}-{int(broken)}.c"
            binary = source.with_suffix(".exe")
            implementation = helpers.replace("@FRAGMENT@", legacy if broken else fragment)
            source.write_text(oracle.replace("int main(void)", implementation + "\nint main(void)"),
                              encoding="utf-8")
            run([cc, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                 f"-I{runtime}", f"-I{HERE / 'vita'}",
                 "-DISAAC_KAGE_VITA_PHASE_PROFILE_ORACLE=1",
                 "-DISAAC_VITA_PHASE_PROFILE=1", "-DISAAC_VITA_FULLSPEED_SCHEDULER=1",
                 '-DISAAC_VITA_PHASE_PROFILE_BUILD_ID="fullspeed:phase-oracle"',
                 '-DISAAC_VITA_FULLSPEED_SCHEDULER_BUILD_ID="fullspeed:phase-oracle"',
                 f"-DGUEST_GPR_LOCAL={local}", "-DGUEST_GENERATED_STACK_GUARD=0",
                 f"-DGUEST_IMAGE_BASE=0x{VITA_IMAGE_BASE:08x}u",
                 str(runtime / "kage_vita_fullspeed_scheduler.c"),
                 str(runtime / "kage_vita_phase_profile.c"), str(source), "-o", str(binary)])
            try:
                output = run([str(binary)])
            except RuntimeError as error:
                if not broken or "kage_vita_fullspeed_scheduler_bypass_limiter()" not in str(error):
                    raise
            else:
                if broken:
                    raise AssertionError("ce33caba render-return regression was not detected")
                if "Vita cadence30/phase integration oracle: PASS" not in output:
                    raise AssertionError(f"unexpected generated-seam result: {output}")
    print("Vita generated render seam: PASS (GPR memory/locals; real scheduler/profiler; "
          "240 ticks each; skipped Update closed; continuation preserved; "
          "ce33caba negative control rejected; feature-OFF text identical)")


def verify_manager(result: dict[str, object]) -> str:
    expected_seams = (0x004B0027, 0x004B0311, 0x004B0316)
    if result.get("vita_fullspeed_cadence_seams") != expected_seams:
        raise AssertionError("Manager cadence seam inventory changed")
    text = str(legacy_text(result["text"]))
    if text.count("sub_002cdcf0(c);") != 1:
        raise AssertionError("original Game::Update call is absent or duplicated")

    manager_note = text.index(
        "kage_pc_backend_fullspeed_note_manager_entry(c->edi"
    )
    parity_instruction = text.index("/* 004b0027  mov eax, ecx */")
    game_begin = text.index(
        "kage_pc_backend_fullspeed_note_game_update_begin(ld32("
    )
    game_comment = text.index("/* 004b0311  call 0x2cdcf0 */")
    game_call = text.index("sub_002cdcf0(c);")
    game_end = text.index(
        "kage_pc_backend_fullspeed_note_game_update_end(_guest_game"
    )
    return_instruction = text.index("/* 004b0316  jmp 0x4b0326 */")
    if not (
        manager_note < parity_instruction < game_begin < game_comment <
        game_call < game_end < return_instruction
    ):
        raise AssertionError("Manager/Game cadence observation order changed")
    blocks = feature_blocks(text)
    assert_read_only(blocks, 3)
    if sum(block.count("ld32(") for block in blocks) < 5 or \
            sum(block.count("ld8(") for block in blocks) != 1:
        raise AssertionError("Manager/Game counter observations changed")
    return text


def verify_game_publish(result: dict[str, object]) -> str:
    if result.get("vita_fullspeed_cadence_seams") != (
            0x004B4031, 0x004B406D):
        raise AssertionError("Continue lifecycle seam inventory changed")
    text = str(legacy_text(result["text"]))
    if text.count("sub_002c5aa0(c);") != 1 or \
            text.count("sub_002c8c90(c);") != 1:
        raise AssertionError("original Game construction chain changed")

    constructor = text.index("sub_002c5aa0(c);")
    counter_note = text.index(
        "kage_pc_backend_fullspeed_note_manager_counter_rebase(c->edi"
    )
    counter_store = text.index("/* 004b4031  mov dword ptr [")
    publish_note = text.index(
        "kage_pc_backend_fullspeed_note_game_pointer_publish(c->edi"
    )
    store_comment = text.index("/* 004b406d  mov dword ptr [")
    initialize = text.index("sub_002c8c90(c);")
    if not counter_note < counter_store < constructor < publish_note < \
            store_comment < initialize:
        raise AssertionError(
            "Continue lifecycle tokens no longer precede their original stores"
        )
    blocks = feature_blocks(text)
    assert_read_only(blocks, 2)
    if blocks[0].count("ld32(") != 1 or \
            "c->edi + 0x0004a264U" not in blocks[0] or \
            "c->eax" not in blocks[0]:
        raise AssertionError("Manager counter rebase evidence shape changed")
    if blocks[1].count("ld32(") != 2 or \
            "GUEST_IMAGE_BASE + 0x007fd65cU" not in blocks[1] or \
            "c->eax" not in blocks[1]:
        raise AssertionError("Game publication evidence shape changed")
    return text


def verify_game_update(result: dict[str, object]) -> str:
    if result.get("vita_fullspeed_cadence_seams") != \
            GAME_UPDATE_EARLY_RETURNS:
        raise AssertionError("Game::Update early-return seam inventory changed")
    if result.get("insns") != 640 or result.get("switch_entries") != 6 or \
            result.get("switch_sites") != 1:
        raise AssertionError("Game::Update full-switch body shape changed")
    text = str(legacy_text(result["text"]))
    marker = "    kage_pc_backend_fullspeed_note_game_update_early_return("
    if text.count(marker) != len(GAME_UPDATE_EARLY_RETURNS):
        raise AssertionError("Game::Update early-return token census changed")
    blocks = feature_blocks(text)
    assert_read_only(blocks, len(GAME_UPDATE_EARLY_RETURNS))

    frame_increment = text.index(
        "/* 002ce598  mov esi, dword ptr [edi + 0x1a30dc] */"
    )
    normal_return = text.index("/* 002ce74f  ret  */")
    previous_return = -1
    for site in GAME_UPDATE_EARLY_RETURNS:
        token = f"{marker}0x{site:08x}U);"
        if text.count(token) != 1:
            raise AssertionError(
                f"Game::Update return token changed at 0x{site:08x}"
            )
        hook = text.index(token)
        return_instruction = text.index(f"/* {site:08x}  ret  */")
        between = text[hook:return_instruction]
        if not previous_return < hook < return_instruction < frame_increment or \
                between.count("#endif") != 1 or \
                re.search(r"/\* [0-9a-f]{8}  ", between):
            raise AssertionError(
                f"early-return token is not immediately before 0x{site:08x}"
            )
        previous_return = return_instruction
    if not frame_increment < normal_return or \
            f"{marker}0x{GAME_UPDATE_RETURNS[-1]:08x}U);" in text:
        raise AssertionError("normal Game::Update path acquired hook overhead")
    return text


def verify_render(result: dict[str, object]) -> str:
    if result.get("vita_fullspeed_cadence_seams") != (0x004B0614,):
        raise AssertionError("Render cadence seam inventory changed")
    text = str(legacy_text(result["text"]))
    render_note = text.index(
        "kage_pc_backend_fullspeed_note_render_body(c->esi"
    )
    parity_instruction = text.index(
        "/* 004b0614  test byte ptr [esi + 0x4a264], 1 */"
    )
    if render_note >= parity_instruction:
        raise AssertionError("Render phase observation moved past parity test")
    blocks = feature_blocks(text)
    assert_read_only(blocks, 1)
    if blocks[0].count("ld32(") != 1 or blocks[0].count("ld8(") != 1:
        raise AssertionError("Render parity observations changed")
    return text


def expect_pin_failure(
    image: Image, pin: Image, switch_info: dict[int, dict[str, object]],
    root: int, rva: int, needle: str | tuple[str, ...],
) -> None:
    mutated = copy.copy(pin)
    memory = bytearray(pin.mem)
    memory[rva] ^= 1
    mutated.mem = bytes(memory)
    try:
        translate(image, mutated, root, switch_info)
    except RuntimeError as exc:
        needles = (needle,) if isinstance(needle, str) else needle
        if root == GAME_UPDATE_ROOT:
            # The independent simulation-cadence body pin now runs before
            # the fullspeed pin and may reject the same mutated bytes first.
            needles += ("simulation cadence Game::Update body/caller identity changed",)
        if not any(value in str(exc) for value in needles):
            raise AssertionError(
                f"wrong failure for mutation 0x{rva:08x}: {exc}"
            ) from exc
    else:
        raise AssertionError(
            f"mutated cadence seam at 0x{rva:08x} was accepted"
        )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--pe", type=Path, required=True)
    arguments = parser.parse_args()
    path = arguments.pe.resolve()
    if path.stat().st_size != PE_SIZE or sha256(path) != PE_SHA256:
        raise AssertionError("cadence codegen test received a different PE")

    # These are literal J835 facts, deliberately independent of gen_all's
    # names, so moving a generator constant cannot update its own oracle.
    expected_constants = {
        "APPLICATION_MAIN_RVA": APP_ROOT,
        "VITA_FULLSPEED_MANAGER_ROOT_RVA": MANAGER_ROOT,
        "VITA_FULLSPEED_RENDER_ROOT_RVA": RENDER_ROOT,
        "VITA_FULLSPEED_MANAGER_ENTRY_OBSERVE_RVA": 0x004B0027,
        "VITA_FULLSPEED_GAME_UPDATE_CALL_RVA": 0x004B0311,
        "VITA_FULLSPEED_GAME_UPDATE_RETURN_RVA": 0x004B0316,
        "VITA_FULLSPEED_GAME_UPDATE_ROOT_RVA": GAME_UPDATE_ROOT,
        "VITA_FULLSPEED_GAME_UPDATE_END_RVA": GAME_UPDATE_END,
        "VITA_FULLSPEED_GAME_UPDATE_NORMAL_RETURN_RVA": 0x002CE74F,
        "VITA_FULLSPEED_GAME_UPDATE_EARLY_RETURN_RVAS":
            GAME_UPDATE_EARLY_RETURNS,
        "VITA_FULLSPEED_GAME_UPDATE_RETURN_RVAS": GAME_UPDATE_RETURNS,
        "VITA_FULLSPEED_GAME_UPDATE_SWITCH_ENTRIES":
            GAME_UPDATE_SWITCH_ENTRIES,
        "VITA_FULLSPEED_GAME_UPDATE_SWITCH_EDGES": {
            0x002CDEDC: GAME_UPDATE_SWITCH_TARGETS,
        },
        "VITA_FULLSPEED_RENDER_PARITY_OBSERVE_RVA": 0x004B0614,
        "VITA_FULLSPEED_GAME_PUBLISH_ROOT_RVA": GAME_PUBLISH_ROOT,
        "VITA_FULLSPEED_MANAGER_REBASE_OBSERVE_RVA": 0x004B4031,
        "VITA_FULLSPEED_GAME_PUBLISH_OBSERVE_RVA": 0x004B406D,
        "VITA_FULLSPEED_MANAGER_COUNTER_OFFSET": 0x0004A264,
        "VITA_FULLSPEED_INTERPOLATION_OFFSET": 0x00029E73,
        "VITA_FULLSPEED_GAME_FRAME_OFFSET": 0x001A30DC,
    }
    for name, expected in expected_constants.items():
        if getattr(G, name) != expected:
            raise AssertionError(f"generator J835 constant changed: {name}")

    G.EXE = str(path)
    pin = Image(str(path), DEFAULT_BASE)
    expected_calls = {
        0x0048BDF0: 0x00598D60,
        0x0048BDF5: MANAGER_ROOT,
        0x0048BDFA: RENDER_ROOT,
        0x004B0079: GAME_PUBLISH_ROOT,
        0x004B0311: GAME_UPDATE_ROOT,
        0x004B404A: 0x005EACF8,
        0x004B4061: 0x002C5AA0,
        0x004B4072: 0x002C8C90,
    }
    for site, target in expected_calls.items():
        if call_target(pin, site) != target:
            raise AssertionError(
                f"J835 direct-call target changed at 0x{site:08x}"
            )

    manager_switch = discover_switch_info(
        pin, MANAGER_ROOT, MANAGER_END, MANAGER_SWITCH_ENTRIES,
        0x004B0291, MANAGER_SWITCH_TARGETS,
    )
    render_switch = discover_switch_info(
        pin, RENDER_ROOT, RENDER_END, RENDER_SWITCH_ENTRIES,
        0x004B07CF, RENDER_SWITCH_TARGETS,
    )
    game_update_switch = discover_switch_info(
        pin, GAME_UPDATE_ROOT, GAME_UPDATE_END,
        GAME_UPDATE_SWITCH_ENTRIES,
        0x002CDEDC, GAME_UPDATE_SWITCH_TARGETS,
    )

    default_results = {
        APP_ROOT: translate(pin, pin, APP_ROOT, {}),
        MANAGER_ROOT: translate(pin, pin, MANAGER_ROOT, manager_switch),
        GAME_UPDATE_ROOT: translate(
            pin, pin, GAME_UPDATE_ROOT, game_update_switch
        ),
        RENDER_ROOT: translate(pin, pin, RENDER_ROOT, render_switch),
        GAME_PUBLISH_ROOT: translate(pin, pin, GAME_PUBLISH_ROOT, {}),
    }
    default_text = {
        APP_ROOT: verify_app(default_results[APP_ROOT]),
        MANAGER_ROOT: verify_manager(default_results[MANAGER_ROOT]),
        GAME_UPDATE_ROOT: verify_game_update(
            default_results[GAME_UPDATE_ROOT]
        ),
        RENDER_ROOT: verify_render(default_results[RENDER_ROOT]),
        GAME_PUBLISH_ROOT: verify_game_publish(
            default_results[GAME_PUBLISH_ROOT]
        ),
    }

    vita_image = Image(str(path), VITA_IMAGE_BASE)
    vita_results = {
        APP_ROOT: translate(vita_image, pin, APP_ROOT, {}),
        MANAGER_ROOT: translate(
            vita_image, pin, MANAGER_ROOT, manager_switch
        ),
        GAME_UPDATE_ROOT: translate(
            vita_image, pin, GAME_UPDATE_ROOT, game_update_switch
        ),
        RENDER_ROOT: translate(vita_image, pin, RENDER_ROOT, render_switch),
        GAME_PUBLISH_ROOT: translate(
            vita_image, pin, GAME_PUBLISH_ROOT, {}
        ),
    }
    vita_text = {
        APP_ROOT: verify_app(vita_results[APP_ROOT]),
        MANAGER_ROOT: verify_manager(vita_results[MANAGER_ROOT]),
        GAME_UPDATE_ROOT: verify_game_update(vita_results[GAME_UPDATE_ROOT]),
        RENDER_ROOT: verify_render(vita_results[RENDER_ROOT]),
        GAME_PUBLISH_ROOT: verify_game_publish(
            vita_results[GAME_PUBLISH_ROOT]
        ),
    }
    execute_app_render_seam(vita_results[APP_ROOT])
    for root, markers in {
        APP_ROOT: (
            "kage_pc_backend_fullspeed_plan_render(",
            "kage_pc_backend_fullspeed_bypass_limiter()",
        ),
        MANAGER_ROOT: (
            "kage_pc_backend_fullspeed_note_manager_entry(",
            "kage_pc_backend_fullspeed_note_game_update_begin(",
            "kage_pc_backend_fullspeed_note_game_update_end(",
            "sub_002cdcf0(c);",
        ),
        GAME_UPDATE_ROOT: (
            "kage_pc_backend_fullspeed_note_game_update_early_return(",
            "/* 002ce598  mov esi, dword ptr [edi + 0x1a30dc] */",
            "/* 002ce74f  ret  */",
        ),
        RENDER_ROOT: ("kage_pc_backend_fullspeed_note_render_body(",),
        GAME_PUBLISH_ROOT: (
            "kage_pc_backend_fullspeed_note_manager_counter_rebase(",
            "kage_pc_backend_fullspeed_note_game_pointer_publish(",
            "sub_002c5aa0(c);",
            "sub_002c8c90(c);",
        ),
    }.items():
        for marker in markers:
            if marker not in default_text[root] or marker not in vita_text[root]:
                raise AssertionError(
                    f"cadence seam differs by image base: {marker}"
                )

    mutation_cases = (
        (APP_ROOT, {}, G.KAGE_PC_RENDER_RETURN_RVA + 2,
         ("loop heartbeat Update/Render/return machine window changed",
          "Lua tick bypass machine window changed")),
        (APP_ROOT, {}, G.KAGE_PC_LIMITER_ENTRY_RVA,
         "loop heartbeat software limiter entry machine window changed"),
        (APP_ROOT, {}, G.KAGE_PC_LIMITER_EXIT_RVA,
         "loop heartbeat terminal backedge machine window changed"),
        (MANAGER_ROOT, manager_switch,
         G.VITA_FULLSPEED_MANAGER_ENTRY_MACHINE_RVA,
         "fullspeed cadence Manager entry/parity machine window changed"),
        (MANAGER_ROOT, manager_switch,
         G.VITA_FULLSPEED_MANAGER_FAST_INCREMENT_RVA,
         "fullspeed cadence Manager fast increment machine window changed"),
        (MANAGER_ROOT, manager_switch,
         G.VITA_FULLSPEED_GAME_CALL_MACHINE_RVA,
         "fullspeed cadence Game::Update call machine window changed"),
        (MANAGER_ROOT, manager_switch,
         G.VITA_FULLSPEED_MANAGER_FINAL_INCREMENT_RVA,
         "fullspeed cadence Manager final increment machine window changed"),
        (MANAGER_ROOT, manager_switch,
         G.VITA_FULLSPEED_GAME_FRAME_MACHINE_RVA,
         "fullspeed cadence Game frame increment machine window changed"),
        (MANAGER_ROOT, manager_switch,
         G.VITA_FULLSPEED_CONTINUE_CALL_MACHINE_RVA,
         "fullspeed cadence Continue helper call machine window changed"),
        (GAME_PUBLISH_ROOT, {},
         G.VITA_FULLSPEED_GAME_PUBLISH_MACHINE_RVA,
         "fullspeed cadence Game publication chain machine window changed"),
        (RENDER_ROOT, render_switch,
         G.VITA_FULLSPEED_RENDER_PARITY_MACHINE_RVA,
         ("fullspeed cadence Render parity/interpolation machine window changed",
          "KAGE present body/CFG identity changed at root 004b0600")),
        (GAME_UPDATE_ROOT, game_update_switch, GAME_UPDATE_ROOT,
         "fullspeed cadence Game::Update body/order/return set changed"),
        (GAME_UPDATE_ROOT, game_update_switch, 0x002CDEDC,
         "fullspeed cadence Game::Update body/order/return set changed"),
        (GAME_UPDATE_ROOT, game_update_switch,
         G.VITA_FULLSPEED_GAME_FRAME_MACHINE_RVA,
         ("fullspeed cadence Game frame increment machine window changed",
          "fullspeed cadence Game::Update body/order/return set changed")),
    ) + tuple(
        (GAME_UPDATE_ROOT, game_update_switch, site,
         "fullspeed cadence Game::Update body/order/return set changed")
        for site in GAME_UPDATE_RETURNS
    )
    for root, switch_info, rva, needle in mutation_cases:
        expect_pin_failure(pin, pin, switch_info, root, rva, needle)

    print(
        "Vita cadence30 generator pin: PASS; exact J835 calls/offsets; "
        "Manager/Continue/Game lifecycle/Render read-only observations; exact "
        "Game::Update full-switch body, 9 RETs and 8 early tokens; original "
        "Game construction and Update once; full-phase gate; "
        "whole-limiter bypass; bases=2; "
        f"seam mutations={len(mutation_cases)} closed"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
