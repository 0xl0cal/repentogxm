#!/usr/bin/env python3
"""Frozen-PE/codegen/build and host oracle for Exit -> menu attribution."""

from __future__ import annotations

import argparse
import copy
import hashlib
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
from gpr_locals import legacy_text  # noqa: E402


HERE = Path(__file__).resolve().parent
RUNTIME = HERE / "runtime"
PE_SIZE = 8_650_240
PE_SHA256 = "31846486979cfa07c8c968221553052c3ff603518681ca11912d445f96ca9404"
VITA_BASE = 0x98000000
EXPECTED_HOOKS = {
    0x004B0010: (0x004B0079, 0x004B0080, 0x004B0085,
                 0x004B0173, 0x004B017A, 0x004B017F),
    0x004B43D0: (0x004B445E, 0x004B4463, 0x004B449F, 0x004B44A4,
                 0x004B44B2, 0x004B44B7, 0x004B44BD, 0x004B44C2),
    0x004B4500: (0x004B4741, 0x004B4746, 0x004B4763, 0x004B4768),
    0x004B47A0: (0x004B4927, 0x004B492C, 0x004B4CF8, 0x004B4CFD),
    0x0025B340: (0x0025B340, 0x0025B398, 0x0025B4A8),
}
EVENT_NAMES = (
    "CANDIDATE_BEGIN", "SET_SAVE_BEGIN", "SET_SAVE_END", "CTOR_BEGIN",
    "CTOR_END", "INIT_BEGIN", "INIT_END", "POST_BEGIN", "POST_END",
    "PERSISTENT_BEGIN", "PERSISTENT_END", "GAMESTATE_BEGIN",
    "GAMESTATE_END", "READ_BEGIN", "READ_END", "CHECKSUM_BEGIN",
    "CHECKSUM_END", "CANDIDATE_END", "GAME_SNAPSHOT",
)
FENCE = "#if defined(__vita__) && defined(ISAAC_VITA_EXIT_MENU_PROFILE)"
MANAGER_SWITCH_ENTRIES = (
    0x004B0298, 0x004B047F, 0x004B0486, 0x004B0493, 0x004B049E,
)
MANAGER_SWITCH_TARGETS = (
    0x004B047F, 0x004B0298, 0x004B0486, 0x004B049E, 0x004B0493,
)


def sha256(path: Path) -> str:
    result = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            result.update(chunk)
    return result.hexdigest()


def load_generator(pe_path: Path):
    os.environ["REPENTOGXM_PE"] = str(pe_path)
    sys.path.insert(0, str(HERE))
    import build_all  # noqa: E402
    import gen_all  # noqa: E402
    from image import DEFAULT_BASE, Image  # noqa: E402

    return build_all, gen_all, DEFAULT_BASE, Image


def verify_event_parity(generator) -> None:
    header = (RUNTIME / "kage_vita_exit_menu_profile.h").read_text(
        encoding="utf-8"
    )
    for value, name in enumerate(EVENT_NAMES, 1):
        generated = getattr(
            generator, "VITA_EXIT_MENU_PROFILE_EVENT_" + name, None
        )
        match = re.search(
            r"\bISAAC_VITA_EXIT_MENU_%s\s*=\s*(\d+)\b" % name,
            header,
        )
        hosted = int(match.group(1)) if match else None
        if generated != value or hosted != value:
            raise AssertionError(
                "Exit-menu event ABI mismatch for %s: gen=%r host=%r"
                % (name, generated, hosted)
            )


def manager_switch_info(generator, image) -> dict[int, dict[str, object]]:
    entries, edges, rejected, tables = generator.discover_jump_tables(
        image, 0x004B0010, owner_end=0x004B0600
    )
    expected = {0x004B0291: MANAGER_SWITCH_TARGETS}
    assert entries == MANAGER_SWITCH_ENTRIES
    assert edges == expected and tables == expected and not rejected
    return {
        0x004B0010: {
            "entries": entries,
            "edges": edges,
            "rejected": rejected,
            "tables": tables,
        }
    }


def render_fresh(generator, default_base, Image, pe_path: Path):
    expected = frozenset(
        (root, site) for root, sites in EXPECTED_HOOKS.items()
        for site in sites
    )
    assert len(expected) == 25
    assert generator.VITA_EXIT_MENU_PROFILE_EXPECTED_HOOKS == expected
    pin = Image(str(pe_path), default_base)
    manager_switch = manager_switch_info(generator, pin)
    rendered = {}
    for emit_base in (default_base, VITA_BASE):
        emit = Image(str(pe_path), emit_base)
        for root, hooks in EXPECTED_HOOKS.items():
            result = generator._translate_function(
                emit, {"rva": root}, None,
                manager_switch if root == 0x004B0010 else {}, pin_img=pin
            )
            assert result["stub"] is None
            assert result["vita_exit_menu_profile_hooks"] == hooks
            text = legacy_text(result["text"])
            assert text.count(FENCE) == len(hooks) + 1
            assert text.count("isaac_vita_exit_menu_profile_note(") == \
                len(hooks) + 1
            for site in hooks:
                comment = "    /* %08x  " % site
                hook = "    isaac_vita_exit_menu_profile_note("
                comment_at = text.index(comment)
                hook_at = text.rfind(hook, 0, comment_at)
                previous_instruction = text.rfind(
                    "\n    /* 00", 0, comment_at
                )
                if hook_at < previous_instruction:
                    raise AssertionError(
                        "Exit-menu hook is not before instruction %08x" % site
                    )
            if emit_base == VITA_BASE:
                rendered[root] = text
    return pin, rendered


def verify_scalar_calls(rendered: dict[int, str]) -> None:
    calls = []
    pattern = re.compile(
        r"isaac_vita_exit_menu_profile_note\((\d+)U,\s*([^;]+)\);"
    )
    for text in rendered.values():
        calls.extend(pattern.findall(text))
    assert len(calls) == 25
    assert {value.strip() for unused_event, value in calls} == {
        "0U", "c->ebx", "ld32((uint32_t)(c->esp + 8U))",
        "ld8((uint32_t)(c->edi + 0x001ea818U))",
        "ld32((uint32_t)(GUEST_IMAGE_BASE + 0x007fd65cU))",
    }
    slot_events = {10, 12}
    for event, value in calls:
        value = value.strip()
        if int(event) in slot_events:
            assert value == "c->ebx"
        if int(event) == 16:
            assert value == "ld32((uint32_t)(c->esp + 8U))"
        if int(event) == 1:
            assert value == "ld8((uint32_t)(c->edi + 0x001ea818U))"
        if int(event) == 19:
            assert value == \
                "ld32((uint32_t)(GUEST_IMAGE_BASE + 0x007fd65cU))"
        assert "uintptr_t" not in value and "CPU" not in value


def translated_body(generator, pin, root: int):
    if root == 0x004B0010:
        info = manager_switch_info(generator, pin)[root]
        translated = generator.B.translate(
            pin, root, "exit_mutation", None,
            info["entries"], info["edges"]
        )
    else:
        translated = generator.B.translate(pin, root, "exit_mutation", None)
    unused_em, unused_insns, order, body, unsupported, \
        unused_indirect, unused_members = translated
    assert not unsupported
    return order, body


def expect_mutation(generator, pin, root: int, site: int,
                    byte_offset: int, message: str) -> None:
    mutated = copy.copy(pin)
    memory = bytearray(pin.mem)
    memory[site + byte_offset] ^= 1
    mutated.mem = bytes(memory)
    order, body = translated_body(generator, pin, root)
    try:
        generator.vita_exit_menu_profile_for_body(
            mutated, root, order, body
        )
    except RuntimeError as exc:
        if message not in str(exc):
            raise
    else:
        raise AssertionError("mutated Exit seam passed at %08x" % site)


def verify_mutation_closed(generator, pin) -> None:
    # Each mutation targets a distinct frozen seam fact and occurs after
    # whole-PE identity has been cached by the clean rendering pass.
    expect_mutation(generator, pin, 0x004B43D0, 0x004B44B2, 1,
                    "call/resume/target changed")
    expect_mutation(generator, pin, 0x004B0010, 0x004B017A, 1,
                    "call/resume/target changed")
    expect_mutation(generator, pin, 0x004B0010, 0x004B0079, 1,
                    "gameplay qualifier machine chain changed")
    expect_mutation(generator, pin, 0x004B0010, 0x004B0173, 2,
                    "second caller work materialization changed")
    expect_mutation(generator, pin, 0x004B0010, 0x004B0140, 2,
                    "second caller path changed")
    expect_mutation(generator, pin, 0x004B0010, 0x004B030A, 1,
                    "gameplay qualifier machine chain changed")
    expect_mutation(generator, pin, 0x004B43D0, 0x004B440C, 2,
                    "work predicate changed")
    expect_mutation(generator, pin, 0x004B4500, 0x004B452A, 2,
                    "slot provenance changed")
    expect_mutation(generator, pin, 0x0025B340, 0x00524655, 1,
                    "checksum caller census changed")
    expect_mutation(generator, pin, 0x0025B340, 0x0025B398, 1,
                    "checksum RET 8 ABI changed")
    expect_mutation(generator, pin, 0x004B0010, 0x004B0010, 0,
                    "body/order/extent changed")

    order, body = translated_body(generator, pin, 0x004B43D0)
    try:
        generator.vita_exit_menu_profile_for_body(
            pin, 0x004B43D0, order[:-1], body
        )
    except RuntimeError as exc:
        assert "body/order/extent changed" in str(exc)
    else:
        raise AssertionError("truncated Exit owner order passed")


def verify_membership_closed(generator, pin) -> None:
    foreign_site = EXPECTED_HOOKS[0x004B43D0][0]
    try:
        generator.vita_exit_menu_profile_for_body(
            pin, 0x00DEAD00, (foreign_site,), ()
        )
    except RuntimeError as exc:
        assert "leaked into non-owner" in str(exc)
    else:
        raise AssertionError("Exit hook passed in a foreign owner")

    root = 0x004B43D0
    mixed = (root,) + EXPECTED_HOOKS[root] + \
        (EXPECTED_HOOKS[0x004B47A0][0],)
    try:
        generator.vita_exit_menu_profile_for_body(pin, root, mixed, ())
    except RuntimeError as exc:
        assert "hook membership changed" in str(exc)
    else:
        raise AssertionError("Exit owner accepted a foreign hook")

    original_translate = generator.B.translate
    poisoned_fn = {"rva": foreign_site - 4, "end": foreign_site + 4}

    def unsupported_owner(*unused_args, **unused_kwargs):
        order = (root,) + EXPECTED_HOOKS[root]
        return None, {}, order, (), [object()], None, None

    def failed_translation(*unused_args, **unused_kwargs):
        raise ValueError("Exit extent poison")

    try:
        generator.B.translate = unsupported_owner
        try:
            generator._translate_function(
                pin, {"rva": root}, None, {}, pin_img=pin
            )
        except RuntimeError as exc:
            assert "Exit-menu owner became unsupported" in str(exc)
        else:
            raise AssertionError("unsupported Exit owner became a stub")

        generator.B.translate = failed_translation
        try:
            generator._translate_function(
                pin, poisoned_fn, None, {}, pin_img=pin
            )
        except RuntimeError as exc:
            assert "Exit-menu hook extent failed decode" in str(exc)
        else:
            raise AssertionError("Exit hook decode failure became a stub")
    finally:
        generator.B.translate = original_translate


def run_command(command: list[str], environment: dict[str, str]) -> str:
    completed = subprocess.run(
        command, cwd=HERE, env=environment,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
    )
    output = completed.stdout.decode("cp866", errors="replace")
    print(output, end="")
    if completed.returncode:
        raise subprocess.CalledProcessError(completed.returncode, command)
    return output


def verify_native_oracle(build_all) -> None:
    environment, compiler = build_all.msvc_env()
    with tempfile.TemporaryDirectory(
            prefix="isaac-exit-menu-profile-") as temporary:
        root = Path(temporary)
        runtime_object = root / "exit-menu-profile-runtime.obj"
        oracle_object = root / "exit-menu-profile-oracle.obj"
        executable = root / "exit-menu-profile-oracle.exe"
        common = [
            compiler, "/nologo", "/c", "/W4", "/WX", "/O2", "/std:c11",
            "/DISAAC_VITA_EXIT_MENU_PROFILE=1",
            "/DISAAC_VITA_EXIT_MENU_PROFILE_ORACLE=1",
            "/I", str(RUNTIME),
        ]
        run_command(common + [
            str(RUNTIME / "kage_vita_exit_menu_profile.c"),
            "/Fo:" + str(runtime_object),
        ], environment)
        run_command(common + [
            str(RUNTIME / "kage_vita_exit_menu_profile_oracle.c"),
            "/Fo:" + str(oracle_object),
        ], environment)
        run_command([
            compiler, "/nologo", str(runtime_object), str(oracle_object),
            "/Fe:" + str(executable), "/link", "/INCREMENTAL:NO",
        ], environment)
        output = run_command([str(executable)], environment)
        assert "Exit-menu native oracle: PASS" in output

        # OFF remains a warning-clean, dependency-free translation unit.
        run_command([
            compiler, "/nologo", "/c", "/W4", "/WX", "/O2", "/std:c11",
            "/I", str(RUNTIME),
            str(RUNTIME / "kage_vita_exit_menu_profile.c"),
            "/Fo:" + str(root / "exit-menu-profile-disabled.obj"),
        ], environment)


def verify_build_switch() -> None:
    cmake = (HERE / "vita/CMakeLists.txt").read_text(encoding="utf-8")
    wrapper = (HERE.parent / "tools/build_vita.py").read_text(
        encoding="utf-8"
    )
    raw_gate = (HERE / "vita/vita_raw_allocator_gate.py").read_text(
        encoding="utf-8"
    )
    match = re.search(
        r"option\(ISAAC_VITA_EXIT_MENU_PROFILE\s+.*?\s+(ON|OFF)\)",
        cmake, flags=re.S,
    )
    assert match is not None and match.group(1) == "OFF"
    root_block = re.search(
        r"set\(ISAAC_VITA_EXIT_MENU_PROFILE_ROOTS\s+(.*?)\)",
        cmake, flags=re.S,
    )
    assert root_block is not None
    assert sorted(re.findall(r"sub_[0-9a-f]{8}", root_block.group(1))) == [
        "sub_0025b340", "sub_004b0010", "sub_004b43d0",
    ]
    assert "${ISAAC_VITA_SAVE_LOAD_PROFILE_ROOTS}" in root_block.group(1)
    shared_block = re.search(
        r"set\(ISAAC_VITA_SAVE_LOAD_PROFILE_ROOTS\s+(.*?)\)",
        cmake, flags=re.S,
    )
    assert shared_block is not None
    assert sorted(re.findall(
        r"sub_[0-9a-f]{8}", shared_block.group(1)
    )) == ["sub_004b4500", "sub_004b47a0"]
    assert cmake.count("kage_vita_exit_menu_profile.c") == 2
    roots_at = cmake.index("set(ISAAC_VITA_EXIT_MENU_PROFILE_ROOTS")
    exit_block = cmake[
        cmake.rfind("if(ISAAC_VITA_EXIT_MENU_PROFILE)", 0, roots_at):
        cmake.index("if(ISAAC_VITA_KAGE)", roots_at)
    ]
    assert "ISAAC_VITA_DIRECT_DEFAULT_CANONICAL_SOURCE" in exit_block
    assert "ISAAC_VITA_DIRECT_DEFAULT_OUTPUT" in exit_block
    assert "--exit-menu-profile" in wrapper
    assert '"ISAAC_VITA_EXIT_MENU_PROFILE": exit_menu_profile' in wrapper
    assert '"ISAAC_VITA_EXIT_MENU_PROFILE"' in raw_gate
    assert '"host_vita_crt.c"' in raw_gate

    crt = (RUNTIME / "host_vita_crt.c").read_text(encoding="utf-8")
    begin = crt.index("isaac_vita_exit_menu_profile_fread_begin(bytes);")
    errno_clear = crt.index("errno = 0;", begin)
    native = crt.index("result = vita_crt_native_fread(", errno_clear)
    errno_capture = crt.index("read_errno = errno;", native)
    end = crt.index("isaac_vita_exit_menu_profile_fread_end(", errno_capture)
    assert begin < errno_clear < native < errno_capture < end


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pe", type=Path, required=True)
    args = parser.parse_args()
    pe_path = args.pe.resolve()
    if pe_path.stat().st_size != PE_SIZE or sha256(pe_path) != PE_SHA256:
        raise AssertionError("Exit-menu oracle received a different PE")
    build_all, generator, default_base, Image = load_generator(pe_path)
    verify_event_parity(generator)
    pin, rendered = render_fresh(
        generator, default_base, Image, pe_path
    )
    verify_scalar_calls(rendered)
    verify_mutation_closed(generator, pin)
    verify_membership_closed(generator, pin)
    verify_native_oracle(build_all)
    verify_build_switch()
    print("Vita Exit-menu frozen/codegen/build oracle: PASS; "
          "roots=5 hooks=25 mutations=12 xrefs=closed checksum_calls=83")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
