#!/usr/bin/env python3
"""Frozen-PE/codegen/build and host oracle for FILE/Continue attribution."""

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


HERE = Path(__file__).resolve().parent
RUNTIME = HERE / "runtime"
PE_SIZE = 8_650_240
PE_SHA256 = "31846486979cfa07c8c968221553052c3ff603518681ca11912d445f96ca9404"
VITA_BASE = 0x98000000
EXPECTED_HOOKS = {
    0x002CBA60: (0x002CBD49, 0x002CBD4E, 0x002CBDB6, 0x002CBDBB,
                 0x002CC01E, 0x002CC023, 0x002CC0AA, 0x002CC0AF,
                 0x002CC0B2, 0x002CC0B7, 0x002CC0BE, 0x002CC0C3),
    0x00302950: (0x0030300C, 0x00303011, 0x003032A0, 0x003032A5),
    0x003E5400: (0x003E547C, 0x003E57E9),
    0x004B3FD0: (0x004B4026, 0x004B420C, 0x004B4211, 0x004B43AA),
    0x0052F7C0: (0x0052F7C0, 0x0052F963, 0x0052FA90),
    0x004B4500: (0x004B4741, 0x004B4746, 0x004B4763, 0x004B4768),
    0x004B47A0: (0x004B4927, 0x004B492C, 0x004B4CF8, 0x004B4CFD),
    0x0055E330: (0x0055E330, 0x0055E4E3, 0x0055E4E6,
                 0x0055E4F7, 0x0055E4FA, 0x0055E545),
}
EVENT_NAMES = (
    "FILE_BEGIN", "PERSISTENT_BEGIN", "PERSISTENT_END",
    "GAMESTATE_LOAD_BEGIN", "GAMESTATE_LOAD_END",
    "GAMESTATE_READ_BEGIN", "GAMESTATE_READ_END", "FILE_END",
    "CANDIDATE_BEGIN", "RESTORE_BEGIN", "RESTORE_END",
    "CANDIDATE_END", "PLAYER_CREATE_BEGIN", "PLAYER_CREATE_END",
    "ITEMPOOL_INIT_BEGIN", "ITEMPOOL_INIT_END", "SFX_LOAD_BEGIN",
    "SFX_LOAD_END", "ITEMPOOL_RESTORE_BEGIN", "ITEMPOOL_RESTORE_END",
    "PLAYERS_PRE_BEGIN", "PLAYERS_PRE_END", "LEVEL_RESTORE_BEGIN",
    "LEVEL_RESTORE_END", "PLAYERS_POST_BEGIN", "PLAYERS_POST_END",
    "ROOM_LOAD_BEGIN", "ROOM_LOAD_END", "GUEST_LOG_BEGIN",
    "GUEST_LOG_WRITE_BEGIN", "GUEST_LOG_WRITE_END",
    "GUEST_LOG_FLUSH_BEGIN", "GUEST_LOG_FLUSH_END", "GUEST_LOG_END",
)
FENCE = (
    "#if defined(__vita__) && defined(ISAAC_VITA_CONTINUE_PROFILE)"
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
    header = (RUNTIME / "kage_vita_continue_profile.h").read_text(
        encoding="utf-8"
    )
    for value, name in enumerate(EVENT_NAMES, 1):
        generated = getattr(
            generator, "VITA_CONTINUE_PROFILE_EVENT_" + name, None
        )
        match = re.search(
            r"\bISAAC_VITA_CONTINUE_%s\s*=\s*(\d+)\b" % name,
            header,
        )
        hosted = int(match.group(1)) if match else None
        if generated != value or hosted != value:
            raise AssertionError(
                "Continue event ABI mismatch for %s: gen=%r host=%r"
                % (name, generated, hosted)
            )


def render_fresh(generator, default_base, Image, pe_path: Path):
    expected = frozenset(
        (root, site) for root, sites in EXPECTED_HOOKS.items()
        for site in sites
    )
    assert len(expected) == 39
    assert generator.VITA_CONTINUE_PROFILE_EXPECTED_HOOKS == expected
    pin = Image(str(pe_path), default_base)
    rendered = {}
    for emit_base in (default_base, VITA_BASE):
        emit = Image(str(pe_path), emit_base)
        for root, hooks in EXPECTED_HOOKS.items():
            result = generator._translate_function(
                emit, {"rva": root}, None, {}, pin_img=pin
            )
            assert result["stub"] is None
            assert result["vita_continue_profile_hooks"] == hooks
            text = result["text"]
            assert text.count(FENCE) == len(hooks) + 1
            assert text.count("isaac_vita_continue_profile_note(") == \
                len(hooks) + 1
            for site in hooks:
                comment = "    /* %08x  " % site
                hook = "    isaac_vita_continue_profile_note("
                comment_at = text.index(comment)
                hook_at = text.rfind(hook, 0, comment_at)
                previous_instruction = text.rfind(
                    "\n    /* 00", 0, comment_at
                )
                if hook_at < previous_instruction:
                    raise AssertionError(
                        "Continue hook is not before its instruction %08x" %
                        site
                    )
            if emit_base == VITA_BASE:
                rendered[root] = text
    return pin, rendered


def verify_scalar_calls(rendered: dict[int, str]) -> None:
    calls = []
    pattern = re.compile(
        r"isaac_vita_continue_profile_note\((\d+)U,\s*([^;]+)\);"
    )
    for text in rendered.values():
        calls.extend(pattern.findall(text))
    assert len(calls) == 39
    values = {value.strip() for unused_event, value in calls}
    assert values == {"0U", "c->edx", "_guest_vita_room_stage",
                      "c->eax & 0xffU"}
    for unused_event, value in calls:
        assert "uintptr_t" not in value and "CPU" not in value


def verify_mutation_closed(generator, pin) -> None:
    # The PE identity pins the complete input; these local mutations prove
    # direct and indirect operand checks are independently active too.
    for site, byte_offset, expected_message in (
        (0x0052F7DE, 1, "file-select direct target changed"),
        (0x0055E4E3, 2, "indirect target changed"),
    ):
        mutated = copy.copy(pin)
        memory = bytearray(pin.mem)
        memory[site + byte_offset] ^= 4
        mutated.mem = bytes(memory)
        root = 0x0052F7C0 if site == 0x0052F7DE else 0x0055E330
        translated = generator.B.translate(
            pin, root, "continue_mutation", None
        )
        unused_em, unused_insns, order, body, unsupported, \
            unused_indirect, unused_members = translated
        assert not unsupported
        try:
            generator.vita_continue_profile_for_body(
                mutated, root, order, body
            )
        except RuntimeError as exc:
            if expected_message not in str(exc):
                raise
        else:
            raise AssertionError(
                "mutated Continue edge passed at %08x" % site
            )


def verify_membership_closed(generator, pin) -> None:
    foreign_site = EXPECTED_HOOKS[0x0052F7C0][0]
    try:
        generator.vita_continue_profile_for_body(
            pin, 0x00DEAD00, (foreign_site,), ()
        )
    except RuntimeError as exc:
        assert "leaked into non-owner" in str(exc)
    else:
        raise AssertionError("Continue hook passed in a foreign owner")

    root = 0x0052F7C0
    mixed = (root,) + EXPECTED_HOOKS[root] + \
        (EXPECTED_HOOKS[0x00302950][0],)
    try:
        generator.vita_continue_profile_for_body(pin, root, mixed, ())
    except RuntimeError as exc:
        assert "hook membership changed" in str(exc)
    else:
        raise AssertionError("Continue owner accepted a foreign hook")

    original_translate = generator.B.translate
    poisoned_fn = {"rva": foreign_site - 4, "end": foreign_site + 4}

    def unsupported_owner(*unused_args, **unused_kwargs):
        order = (root,) + EXPECTED_HOOKS[root]
        return None, {}, order, (), [object()], None, None

    def failed_translation(*unused_args, **unused_kwargs):
        raise ValueError("Continue extent poison")

    try:
        generator.B.translate = unsupported_owner
        try:
            generator._translate_function(
                pin, {"rva": root}, None, {}, pin_img=pin
            )
        except RuntimeError as exc:
            assert "owner became unsupported" in str(exc)
        else:
            raise AssertionError("unsupported Continue owner became a stub")

        generator.B.translate = failed_translation
        try:
            generator._translate_function(
                pin, poisoned_fn, None, {}, pin_img=pin
            )
        except RuntimeError as exc:
            assert "hook extent failed decode" in str(exc)
        else:
            raise AssertionError("Continue hook decode failure became a stub")
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
            prefix="isaac-continue-profile-") as temporary:
        root = Path(temporary)
        runtime_object = root / "continue-profile-runtime.obj"
        oracle_object = root / "continue-profile-oracle.obj"
        executable = root / "continue-profile-oracle.exe"
        common = [
            compiler, "/nologo", "/c", "/W4", "/WX", "/O2", "/std:c11",
            "/DISAAC_VITA_CONTINUE_PROFILE=1",
            "/DISAAC_VITA_CONTINUE_OVERLAY=1",
            "/DISAAC_VITA_CONTINUE_PROFILE_ORACLE=1",
            "/I", str(RUNTIME),
        ]
        run_command(common + [
            str(RUNTIME / "kage_vita_continue_profile.c"),
            "/Fo:" + str(runtime_object),
        ], environment)
        run_command(common + [
            str(RUNTIME / "kage_vita_continue_profile_oracle.c"),
            "/Fo:" + str(oracle_object),
        ], environment)
        run_command([
            compiler, "/nologo", str(runtime_object), str(oracle_object),
            "/Fe:" + str(executable), "/link", "/INCREMENTAL:NO",
        ], environment)
        output = run_command([str(executable)], environment)
        assert "continue-profile native oracle: PASS" in output

        # OFF is a warning-clean, dependency-free translation unit.
        run_command([
            compiler, "/nologo", "/c", "/W4", "/WX", "/O2", "/std:c11",
            "/I", str(RUNTIME),
            str(RUNTIME / "kage_vita_continue_profile.c"),
            "/Fo:" + str(root / "continue-profile-disabled.obj"),
        ], environment)


def verify_build_switch() -> None:
    cmake = (HERE / "vita/CMakeLists.txt").read_text(encoding="utf-8")
    wrapper = (HERE.parent / "tools/build_vita.py").read_text(
        encoding="utf-8"
    )
    for option in ("PROFILE", "OVERLAY"):
        match = re.search(
            r"option\(ISAAC_VITA_CONTINUE_%s\s+.*?\s+(ON|OFF)\)" % option,
            cmake, flags=re.S,
        )
        assert match is not None and match.group(1) == "OFF"
    root_block = re.search(
        r"set\(ISAAC_VITA_CONTINUE_PROFILE_ROOTS\s+(.*?)\)",
        cmake, flags=re.S,
    )
    assert root_block is not None
    assert sorted(re.findall(r"sub_[0-9a-f]{8}", root_block.group(1))) == [
        "sub_002cba60", "sub_00302950", "sub_003e5400",
        "sub_004b3fd0", "sub_0052f7c0", "sub_0055e330",
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
    assert "ISAAC_GENERATED_CANONICAL_C" in cmake
    assert "--continue-profile" in wrapper
    assert "--continue-overlay" in wrapper
    assert '"ISAAC_VITA_CONTINUE_PROFILE": continue_profile' in wrapper
    assert '"ISAAC_VITA_CONTINUE_OVERLAY": continue_overlay' in wrapper


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pe", type=Path, required=True)
    args = parser.parse_args()
    pe_path = args.pe.resolve()
    if pe_path.stat().st_size != PE_SIZE or sha256(pe_path) != PE_SHA256:
        raise AssertionError("Continue-profile oracle received a different PE")
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
    print("Vita Continue frozen/codegen/build oracle: PASS; "
          "roots=8 hooks=39 mutations=2 membership=closed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
