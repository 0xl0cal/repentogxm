#!/usr/bin/env python3
"""Fresh frozen-PE/codegen proof for the eight stage-memory seams."""

from __future__ import annotations

import argparse
import copy
import hashlib
import os
import re
import sys
from pathlib import Path
from gpr_locals import legacy_text  # noqa: E402


PE_SIZE = 8_650_240
PE_SHA256 = "31846486979cfa07c8c968221553052c3ff603518681ca11912d445f96ca9404"
VITA_IMAGE_BASE = 0x98000000
EXPECTED_HOOKS = {
    0x00311E60: (0x0031215B, 0x00312622, 0x0031275F, 0x00312E8C),
    0x003E5400: (0x003E547C, 0x003E5640, 0x003E57E9),
    0x003E5370: (0x003E53B7,),
}
EXPECTED_EVENTS = {
    "LEVEL_BEGIN": 1,
    "ROOM_REUSE": 2,
    "ROOM_LOAD_BEGIN": 3,
    "ROOM_AFTER_UNLOAD": 4,
    "ROOM_LOAD_END": 5,
    "LEVEL_END": 6,
}
EXPECTED_NOTE_CALLS = (
    "isaac_vita_stage_memory_note(1U, _guest_vita_level_stage, "
    "_guest_vita_level_type, UINT32_MAX);",
    "isaac_vita_stage_memory_note(2U, c->edx, c->ecx, UINT32_MAX);",
    "isaac_vita_stage_memory_note(2U, 0U, c->ecx, UINT32_MAX);",
    "isaac_vita_stage_memory_note(6U, _guest_vita_level_stage, "
    "_guest_vita_level_type, UINT32_MAX);",
    "isaac_vita_stage_memory_note(3U, _guest_vita_room_stage, "
    "_guest_vita_room_mode, UINT32_MAX);",
    "isaac_vita_stage_memory_note(4U, _guest_vita_room_stage, "
    "_guest_vita_room_mode, UINT32_MAX);",
    "isaac_vita_stage_memory_note(5U, _guest_vita_room_stage, "
    "_guest_vita_room_mode, c->eax & 0xffU);",
    "isaac_vita_stage_memory_note(2U, c->edx, c->eax, UINT32_MAX);",
)
REUSE_EDGES = {
    (0x00311E60, 0x00312622): (0x00312633, "c->edx", "c->ecx"),
    (0x00311E60, 0x0031275F): (0x0031276D, "0U", "c->ecx"),
    (0x003E5370, 0x003E53B7): (0x003E53C8, "c->edx", "c->eax"),
}
GUEST_LOAD_RE = re.compile(
    r"\b(ld8|ld16|ld32|ld64|ldf|ldd|ldx)\s*\(")
GUEST_STORE_RE = re.compile(
    r"\b(st8|st16|st32|st64|st80d|stf|std_|stx)\s*\(")
CPU_WRITE_RE = re.compile(
    r"(?:(?:\+\+|--)\s*c->\w+|"
    r"\bc->\w+\s*(?:\+\+|--|(?:<<|>>|[+\-*/%&|^])=|=(?!=)))")
INJECTED_HELPERS = frozenset((
    "guest_stack_contains", "ld32", "isaac_vita_stage_memory_note", "cc_ne",
))


def digest(path: Path) -> str:
    result = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            result.update(chunk)
    return result.hexdigest()


def verify_input(path: Path) -> None:
    if path.stat().st_size != PE_SIZE or digest(path) != PE_SHA256:
        raise AssertionError("stage-memory test received a different PE")


def load_generator(path: Path):
    os.environ["REPENTOGXM_PE"] = str(path)
    here = Path(__file__).resolve().parent
    sys.path.insert(0, str(here))
    import gen_all as generator  # noqa: E402
    from image import Image  # noqa: E402

    return generator, Image


def verify_event_parity(generator) -> None:
    header = (Path(__file__).resolve().parent /
              "runtime/host_vita_heap.h").read_text(encoding="utf-8")
    for name, expected in EXPECTED_EVENTS.items():
        generated = getattr(
            generator, "VITA_STAGE_MEMORY_EVENT_" + name, None)
        match = re.search(
            r"\bISAAC_VITA_STAGE_MEMORY_%s\s*=\s*(\d+)\b" % name,
            header,
        )
        hosted = int(match.group(1)) if match else None
        if generated != expected or hosted != expected:
            raise AssertionError(
                "stage-memory event ABI mismatch for %s: gen=%r host=%r"
                % (name, generated, hosted)
            )
    prototype = re.search(
        r"\bvoid\s+isaac_vita_stage_memory_note\s*\((.*?)\)\s*;",
        header, flags=re.S,
    )
    normalized = re.sub(r"\s+", " ", prototype.group(1)).strip() \
        if prototype else None
    expected_prototype = (
        "uint32_t event, uint32_t stage, uint32_t mode_or_type, "
        "uint32_t result"
    )
    if normalized != expected_prototype or "*" in normalized:
        raise AssertionError(
            "host stage-memory API is not exactly four uint32_t scalars: %r"
            % normalized
        )


def render_fresh(generator, Image, path: Path):
    expected_pairs = frozenset(
        (root, site) for root, sites in EXPECTED_HOOKS.items()
        for site in sites
    )
    if generator.VITA_STAGE_MEMORY_EXPECTED_HOOKS != expected_pairs:
        raise AssertionError("generator eight-hook specification drifted")
    if len(expected_pairs) != 8:
        raise AssertionError("test hook census is not eight")

    pin = Image(str(path), generator.DEFAULT_BASE)
    emit = Image(str(path), VITA_IMAGE_BASE)
    rendered = {}
    for root, expected in EXPECTED_HOOKS.items():
        result = generator._translate_function(
            emit, {"rva": root}, None, {}, pin_img=pin
        )
        if result.get("stub") is not None:
            raise AssertionError("stage-memory owner %08x became a stub" % root)
        if result.get("vita_stage_memory_hooks") != expected:
            raise AssertionError("fresh hook census changed at %08x" % root)
        rendered[root] = legacy_text(result["text"])
    return pin, emit, rendered


def extract_note_calls(text: str) -> list[str]:
    calls = []
    needle = "isaac_vita_stage_memory_note("
    offset = 0
    while True:
        start = text.find(needle, offset)
        if start < 0:
            break
        line_start = text.rfind("\n", 0, start) + 1
        end = text.find(");", start)
        if end < 0:
            raise AssertionError("unterminated stage-memory API occurrence")
        statement = text[line_start:end + 2]
        if "extern void" not in statement:
            calls.append(statement)
        offset = end + 2
    return calls


def verify_note_call_shapes(calls: list[str]) -> None:
    normalized = [re.sub(r"\s+", " ", call).strip() for call in calls]
    if sorted(normalized) != sorted(EXPECTED_NOTE_CALLS):
        raise AssertionError(
            "generated stage-memory call argument shapes changed: %r"
            % normalized
        )
    for call in normalized:
        scrubbed = call
        for allowed in ("c->eax", "c->edx", "c->ecx"):
            scrubbed = scrubbed.replace(allowed, "SCALAR_REGISTER")
        if re.search(r"\bc\b", scrubbed) or "uintptr_t" in scrubbed or \
                "CPU" in scrubbed:
            raise AssertionError("CPU/pointer escaped scalar call: %s" % call)


def verify_call_shape_self_test() -> None:
    for poison in (
            "isaac_vita_stage_memory_note(1U, (uint32_t)(uintptr_t)c, "
            "_guest_vita_level_type, UINT32_MAX);",
            "isaac_vita_stage_memory_note(1U, helper(c), "
            "_guest_vita_level_type, UINT32_MAX);"):
        calls = list(EXPECTED_NOTE_CALLS)
        calls[0] = poison
        try:
            verify_note_call_shapes(calls)
        except AssertionError:
            pass
        else:
            raise AssertionError("call-shape oracle accepted poison: %s" % poison)


def assert_vita_guards(text: str) -> None:
    depth = 0
    for line in text.splitlines():
        stripped = line.strip()
        if stripped == "#if defined(__vita__)":
            depth += 1
        elif stripped == "#endif" and depth:
            depth -= 1
        if "isaac_vita_stage_memory_note(" in line and \
                "extern void" not in line and not depth:
            raise AssertionError("stage-memory call escaped its Vita guard")
    if depth:
        raise AssertionError("unclosed Vita codegen guard")


def marker_block(text: str, marker: str) -> str:
    start = text.find(marker)
    if start < 0:
        raise AssertionError("missing generated marker: %s" % marker)
    end = text.find("#endif", start)
    if end < 0:
        raise AssertionError("missing marker guard terminator")
    return text[start:end]


def verify_local_assignments(text: str, name: str,
                             expected: tuple[str, ...]) -> None:
    assignments = tuple(
        re.sub(r"\s+", " ", rhs).strip()
        for rhs in re.findall(
            r"\b%s\s*=(?!=)\s*([^;]+);" % re.escape(name), text)
    )
    if assignments != expected:
        raise AssertionError("%s assignment contract changed: %r" %
                             (name, assignments))


def verify_injected_contract(injected: str) -> None:
    loads = tuple(GUEST_LOAD_RE.findall(injected))
    if loads != ("ld32", "ld32"):
        raise AssertionError("injected guest-load census changed: %r" %
                             (loads,))
    stores = tuple(GUEST_STORE_RE.findall(injected))
    if stores:
        raise AssertionError("injected guest store escaped: %r" % (stores,))

    helpers = set(re.findall(r"\b([A-Za-z_]\w*)\s*\(", injected))
    helpers.difference_update(("if", "defined", "uint32_t"))
    if helpers != INJECTED_HELPERS:
        raise AssertionError("injected helper-call set changed: %r" %
                             (sorted(helpers),))
    if CPU_WRITE_RE.search(injected):
        raise AssertionError("injected hook writes CPU state")

    scrubbed = injected.replace("guest_stack_contains(c,",
                                "guest_stack_contains(")
    scrubbed = scrubbed.replace("cc_ne(c)", "cc_ne()")
    scrubbed = re.sub(r"\bc->(?:fault|esp|eax|edx|ecx)\b", "cpu_scalar",
                      scrubbed)
    if "uintptr_t" in scrubbed or re.search(r"\bc\b", scrubbed):
        raise AssertionError("injected hook leaked a CPU pointer")


def verify_injected_contract_self_test() -> None:
    clean = (
        "if (!c->fault && guest_stack_contains(c, c->esp, 8U)) { "
        "a = ld32(c->esp); b = ld32((uint32_t)(c->esp + 4U)); } "
        "if (cc_ne(c)) isaac_vita_stage_memory_note(2U, c->eax, "
        "c->edx, c->ecx);"
    )
    verify_injected_contract(clean)
    poisons = (
        " ld8(c->esp);",
        " st64(c->esp, 0U);",
        " c->eax += 1U;",
        " ++c->edx;",
        " helper(c);",
        " value = (uint32_t)(uintptr_t)c;",
    )
    for poison in poisons:
        try:
            verify_injected_contract(clean + poison)
        except AssertionError:
            continue
        raise AssertionError("injected-contract poison was accepted: %s" %
                             poison.strip())


def verify_generated_semantics(rendered: dict[int, str]) -> None:
    level = rendered[0x00311E60]
    load = rendered[0x003E5400]
    all_injected = []

    begin = marker_block(
        level, "Exact Level::Init post-cleanup/pre-stage-log checkpoint")
    if begin.count("guest_stack_contains(c, c->esp, 8U)") != 1 or \
            tuple(GUEST_LOAD_RE.findall(begin)) != ("ld32", "ld32") or \
            begin.find("guest_stack_contains") > begin.find("ld32(") or \
            "if (!c->fault && guest_stack_contains" not in begin:
        raise AssertionError("Level begin read/capture contract changed")
    if "_guest_vita_level_stage = ld32(c->esp);" not in begin or \
            "_guest_vita_level_type = ld32((uint32_t)(c->esp + 4U));" \
            not in begin:
        raise AssertionError("Level begin scalar sources changed")
    all_injected.append(begin)

    fixed_blocks = (
        (level, "Exact Level::Init exit checkpoint", 6,
         "_guest_vita_level_captured != 0U && !c->fault"),
        (load, "Exact RoomConfig load entry", 3,
         "_guest_vita_room_stage = c->edx;"),
        (load, "Exact checkpoint after the sole old RoomSet unload call", 4,
         "_guest_vita_room_captured != 0U && !c->fault"),
        (load, "Exact RoomConfig load result", 5,
         "c->eax & 0xffU"),
    )
    for text, marker, event, required in fixed_blocks:
        block = marker_block(text, marker)
        if required not in block or \
                "isaac_vita_stage_memory_note(%dU" % event not in block:
            raise AssertionError("fixed hook changed: %s" % marker)
        all_injected.append(block)

    for (root, site), (target, stage, mode) in REUSE_EDGES.items():
        text = rendered[root]
        marker = "    /* %08x  jne " % site
        start = text.find(marker)
        end = text.find("\n    /* ", start + len(marker))
        block = text[start:end if end >= 0 else len(text)]
        expected_call = (
            "isaac_vita_stage_memory_note(2U, %s, %s, UINT32_MAX);"
            % (stage, mode)
        )
        expected_goto = "goto L_%08x;" % target
        if start < 0 or block.count("cc_ne(c)") != 1 or \
                block.count(expected_call) != 1 or \
                block.count(expected_goto) != 1 or \
                block.find(expected_call) > block.find(expected_goto):
            raise AssertionError("reuse edge changed at %08x" % site)
        if "if (cc_ne(c))" in block:
            raise AssertionError("reuse edge evaluates cc_ne twice")
        label = "L_%08x:" % target
        label_start = text.find(label)
        next_instruction = text.find("\n    /* ", label_start)
        target_prefix = text[label_start:next_instruction]
        if label_start < 0 or "isaac_vita_stage_memory_note" in target_prefix:
            raise AssertionError("reuse probe moved to target %08x" % target)
        all_injected.append(block)

    injected = "\n".join(all_injected)
    verify_injected_contract(injected)

    verify_local_assignments(level, "_guest_vita_level_stage", (
        "UINT32_MAX", "ld32(c->esp)"))
    verify_local_assignments(level, "_guest_vita_level_type", (
        "UINT32_MAX", "ld32((uint32_t)(c->esp + 4U))"))
    verify_local_assignments(level, "_guest_vita_level_captured", (
        "0U", "1U"))
    verify_local_assignments(load, "_guest_vita_room_stage", (
        "UINT32_MAX", "c->edx"))
    verify_local_assignments(load, "_guest_vita_room_mode", (
        "UINT32_MAX", "c->eax"))
    verify_local_assignments(load, "_guest_vita_room_captured", (
        "0U", "1U"))

    calls = []
    for text in rendered.values():
        assert_vita_guards(text)
        if text.count(
                "extern void isaac_vita_stage_memory_note(\n"
                "        uint32_t, uint32_t, uint32_t, uint32_t);") != 1:
            raise AssertionError("generated producer API is not scalar-only")
        calls.extend(extract_note_calls(text))
    if len(calls) != 8:
        raise AssertionError("generated API call census is %d, expected 8"
                             % len(calls))
    verify_note_call_shapes(calls)

    if load.count("goto L_003e57e9;") != 1 or \
            load.count("L_003e57e9:") != 1:
        raise AssertionError("early missing-config result path changed")
    early = load[load.find("/* 003e548d  xor al, al */"):
                 load.find("L_003e5494:")]
    if "c->eax = (c->eax & 0xFFFFFF00U)" not in early or \
            early.count("goto L_003e57e9;") != 1:
        raise AssertionError("early missing-config AL=0 lowering changed")
    success_store = (
        "/* 003e57a6  mov byte ptr [ebp - 0x5d], 1 */\n"
        "    st8((uint32_t)(c->ebp + (uint32_t)(int32_t)(-93)), "
        "(uint8_t)(0x1U));"
    )
    failure_store = (
        "/* 003e57ac  mov byte ptr [ebp - 0x5d], 0 */\n"
        "    st8((uint32_t)(c->ebp + (uint32_t)(int32_t)(-93)), "
        "(uint8_t)(0x0U));"
    )
    result_load = (
        "/* 003e57e6  mov al, byte ptr [ebp - 0x5d] */\n"
        "    c->eax = (c->eax & 0xFFFFFF00U) | "
        "((uint32_t)(ld8((uint32_t)(c->ebp + "
        "(uint32_t)(int32_t)(-93)))) & 0xFFU);"
    )
    if load.count(success_store) != 1 or load.count(failure_store) != 1 or \
            load.count(result_load) != 1:
        raise AssertionError("normal 1/0 result materialisation changed")


def verify_mutation_closed(generator, pin, emit) -> None:
    mutated = copy.copy(pin)
    memory = bytearray(pin.mem)
    memory[0x00312155] ^= 1
    mutated.mem = bytes(memory)
    try:
        generator._translate_function(
            emit, {"rva": 0x00311E60}, None, {}, pin_img=mutated
        )
    except RuntimeError as exc:
        if "stage-memory owner" not in str(exc):
            raise
    else:
        raise AssertionError("mutated stage-memory body was accepted")


def verify_membership_closed(generator, pin) -> None:
    foreign_site = EXPECTED_HOOKS[0x003E5400][0]
    try:
        generator.vita_stage_memory_for_body(
            pin, 0x00DEAD00, None, (foreign_site,), None
        )
    except RuntimeError as exc:
        if "leaked into non-owner" not in str(exc):
            raise
    else:
        raise AssertionError("stage-memory site was accepted in a non-owner")

    level_root = 0x00311E60
    mixed_order = (level_root,) + EXPECTED_HOOKS[level_root] + (foreign_site,)
    try:
        generator.vita_stage_memory_for_body(
            pin, level_root, None, mixed_order, None
        )
    except RuntimeError as exc:
        if "hook membership changed" not in str(exc):
            raise
    else:
        raise AssertionError("stage-memory owner accepted a foreign hook site")

    original_translate = generator.B.translate
    poisoned_root = foreign_site - 4
    poisoned_fn = {"rva": poisoned_root, "end": foreign_site + 4}

    def unsupported_translation(*_args, **_kwargs):
        return None, {}, (foreign_site,), (), [object()], None, None

    def failed_translation(*_args, **_kwargs):
        raise ValueError("membership poison")

    try:
        generator.B.translate = unsupported_translation
        try:
            generator._translate_function(pin, poisoned_fn, None, {},
                                          pin_img=pin)
        except RuntimeError as exc:
            if "leaked into non-owner" not in str(exc):
                raise
        else:
            raise AssertionError(
                "unsupported stage-memory membership returned a stub")

        generator.B.translate = failed_translation
        try:
            generator._translate_function(pin, poisoned_fn, None, {},
                                          pin_img=pin)
        except RuntimeError as exc:
            if "hook extent failed decode" not in str(exc):
                raise
        else:
            raise AssertionError("hook-containing decode failure became a stub")
    finally:
        generator.B.translate = original_translate


def emit_compilation_unit(path: Path, rendered: dict[int, str]) -> None:
    body = "\n".join(rendered[root] for root in EXPECTED_HOOKS)
    symbols = sorted(set(re.findall(r"\bsub_[0-9a-f]{8}(?=\(c\))", body)))
    declarations = "\n".join(
        "void %s(CPU *__restrict c);" % symbol
        for symbol in symbols
    )
    source = (
        "/* Fresh stage-memory gate TU; generated from the frozen PE. */\n"
        "#include <math.h>\n#include \"guest.h\"\n\n" +
        declarations + "\n\n" + body
    )
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(source, encoding="utf-8", newline="\n")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--pe", type=Path, required=True)
    parser.add_argument("--emit-c", type=Path)
    args = parser.parse_args()
    path = args.pe.resolve()
    verify_input(path)
    generator, Image = load_generator(path)
    verify_event_parity(generator)
    verify_call_shape_self_test()
    verify_injected_contract_self_test()
    pin, emit, rendered = render_fresh(generator, Image, path)
    verify_generated_semantics(rendered)
    verify_mutation_closed(generator, pin, emit)
    verify_membership_closed(generator, pin)
    if args.emit_c:
        emit_compilation_unit(args.emit_c.resolve(), rendered)
    print("stage-memory codegen: PASS; fresh=3 roots; hooks=8; "
          "mutation=closed; membership=closed+2-poison; call-poison=2-closed; "
          "injected-poison=6-closed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
