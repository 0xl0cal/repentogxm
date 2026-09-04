#!/usr/bin/env python3
"""Focused hostile fixtures for the exact Lua static-archive closure."""

from __future__ import annotations

import collections
import json
from pathlib import Path
import tempfile

import vita_raw_allocator_gate as gate


def must_fail(label: str, callback, needle: str) -> None:
    try:
        callback()
    except gate.GateError as exc:
        if needle not in str(exc):
            raise AssertionError(f"{label}: wrong failure: {exc}") from exc
    else:
        raise AssertionError(f"{label}: hostile fixture passed")


def source_tests(root: Path) -> None:
    manifest = Path(__file__).resolve().with_name("lua53_vanilla.cmake")
    source = root / "missing-lua-5.3.3"
    (source / "src").mkdir(parents=True)
    must_fail(
        "missing pinned source",
        lambda: gate.lua_source_records(source, manifest),
        "source is missing or symlinked: lapi.c",
    )
    (source / "src" / "lapi.c").write_bytes(b"hostile source\n")
    must_fail(
        "wrong pinned source hash",
        lambda: gate.lua_source_records(source, manifest),
        "source hash mismatch for lapi.c",
    )


def cache_tests(root: Path) -> None:
    source_root = root / "source"
    build = root / "build"
    lua_source = root / "lua-5.3.3"
    manifest = source_root / "recomp" / "vita" / "lua53_vanilla.cmake"
    archive = build / gate.LUA_ARCHIVE_NAME
    ar = root / "tool" / "arm-vita-eabi-ar"
    cache = {
        gate.LUA_CACHE_KEY: "ON",
        gate.LUA_SOURCE_DIR_CACHE_KEY: str(lua_source),
        "CMAKE_AR": str(ar),
    }
    assert gate.lua_gate_paths(
        cache,
        source_root=source_root,
        build_dir=build,
        lua_source=lua_source,
        lua_manifest=manifest,
        lua_archive=archive,
        ar=ar,
    ) == tuple(path.resolve() for path in (lua_source, manifest, archive, ar))
    must_fail(
        "ON cache missing archive argument",
        lambda: gate.lua_gate_paths(
            cache,
            source_root=source_root,
            build_dir=build,
            lua_source=lua_source,
            lua_manifest=manifest,
            lua_archive=None,
            ar=ar,
        ),
        "requires --lua-source",
    )
    off = {
        gate.LUA_CACHE_KEY: "OFF",
        gate.LUA_SOURCE_DIR_CACHE_KEY: "",
    }
    assert gate.lua_gate_paths(
        off,
        source_root=source_root,
        build_dir=build,
        lua_source=None,
        lua_manifest=None,
        lua_archive=None,
        ar=None,
    ) is None
    must_fail(
        "OFF cache with Lua arguments",
        lambda: gate.lua_gate_paths(
            off,
            source_root=source_root,
            build_dir=build,
            lua_source=lua_source,
            lua_manifest=manifest,
            lua_archive=archive,
            ar=ar,
        ),
        "supplied while ISAAC_VITA_LUA=OFF",
    )
    must_fail(
        "OFF cache retained source",
        lambda: gate.lua_gate_paths(
            {**off, gate.LUA_SOURCE_DIR_CACHE_KEY: str(lua_source)},
            source_root=source_root,
            build_dir=build,
            lua_source=None,
            lua_manifest=None,
            lua_archive=None,
            ar=None,
        ),
        "retained ISAAC_LUA53_SOURCE_DIR",
    )
    must_fail(
        "wrong build-owned archive",
        lambda: gate.lua_gate_paths(
            cache,
            source_root=source_root,
            build_dir=build,
            lua_source=lua_source,
            lua_manifest=manifest,
            lua_archive=root / "other" / gate.LUA_ARCHIVE_NAME,
            ar=ar,
        ),
        "exact build-owned",
    )


def compile_fixture(root: Path) -> tuple[Path, Path, tuple[tuple[str, str], ...]]:
    build = root / "compile-build"
    build.mkdir()
    lua_source = root / "compile-lua"
    source_dir = lua_source / "src"
    source_dir.mkdir(parents=True)
    records = tuple(
        (f"fixture_{index:02d}.c", f"{index:064x}")
        for index in range(gate.LUA_C_SOURCE_COUNT)
    )
    entries = []
    for name, unused_sha256 in records:
        source = source_dir / name
        source.write_bytes(name.encode("ascii"))
        output = f"CMakeFiles/{gate.LUA_TARGET}.dir/{name}.obj"
        entries.append({
            "directory": str(build),
            "arguments": [
                "cc", f"-I{source_dir}",
                *gate.LUA_REQUIRED_COMPILE_OPTIONS,
                "-o", output, "-c", str(source),
            ],
            "file": str(source),
            "output": output,
        })
    commands = build / "compile_commands.json"
    commands.write_text(json.dumps(entries), encoding="utf-8")
    return lua_source, commands, records


def compile_tests(root: Path) -> None:
    lua_source, commands, records = compile_fixture(root)
    compiled = gate.lua_compile_closure(
        commands, lua_source, records, require_objects=False
    )
    assert len(compiled) == gate.LUA_C_SOURCE_COUNT
    entries = json.loads(commands.read_text(encoding="utf-8"))

    missing = entries[:-1]
    commands.write_text(json.dumps(missing), encoding="utf-8")
    must_fail(
        "missing compile record",
        lambda: gate.lua_compile_closure(
            commands, lua_source, records, require_objects=False
        ),
        "Lua compile-source closure changed",
    )
    commands.write_text(json.dumps(entries), encoding="utf-8")

    wrong_options = json.loads(json.dumps(entries))
    wrong_options[0]["arguments"][
        wrong_options[0]["arguments"].index("-O2")
    ] = "-O0"
    commands.write_text(json.dumps(wrong_options), encoding="utf-8")
    must_fail(
        "wrong compile option",
        lambda: gate.lua_compile_closure(
            commands, lua_source, records, require_objects=False
        ),
        "option sequence changed",
    )
    commands.write_text(json.dumps(entries), encoding="utf-8")

    wrong_input = json.loads(json.dumps(entries))
    wrong_input[0]["arguments"][-1] = str(lua_source / "src" / "other.c")
    commands.write_text(json.dumps(wrong_input), encoding="utf-8")
    must_fail(
        "compile command source binding",
        lambda: gate.lua_compile_closure(
            commands, lua_source, records, require_objects=False
        ),
        "command/file/output binding changed",
    )
    commands.write_text(json.dumps(entries), encoding="utf-8")

    leaked = json.loads(json.dumps(entries))
    leaked[0]["arguments"].insert(1, "-DISAAC_VITA_RAW_ALLOCATOR_EXEMPT=1")
    commands.write_text(json.dumps(leaked), encoding="utf-8")
    must_fail(
        "allocator exemption leaked into Lua",
        lambda: gate.lua_compile_closure(
            commands, lua_source, records, require_objects=False
        ),
        "preprocessor policy changed",
    )
    commands.write_text(json.dumps(entries), encoding="utf-8")


def member_tests() -> None:
    expected = [
        f"fixture_{index:02d}.c.obj"
        for index in range(gate.LUA_C_SOURCE_COUNT)
    ]
    gate.verify_lua_archive_members(expected, expected)
    must_fail(
        "missing archive member",
        lambda: gate.verify_lua_archive_members(expected[:-1], expected),
        "member closure changed",
    )
    reordered = list(expected)
    reordered[0], reordered[1] = reordered[1], reordered[0]
    must_fail(
        "reordered archive member",
        lambda: gate.verify_lua_archive_members(reordered, expected),
        "member closure changed",
    )
    must_fail(
        "wrong archive member payload",
        lambda: gate.verify_lua_archive_member_payload(
            expected[0], b"archive", b"compile object"
        ),
        "differs from its compile output",
    )


def link_tests(root: Path) -> None:
    build = root / "link-build"
    build.mkdir()
    archive = (build / gate.LUA_ARCHIVE_NAME).resolve()
    obj = f"CMakeFiles/{gate.TARGET}.dir/main.c.obj"
    ninja = build / "build.ninja"

    def write(implicit: str, order_only: str, libraries: str,
              direct: str = "") -> None:
        ninja.write_text(
            f"build {gate.TARGET}: C_LINK {obj}{direct} | {implicit} || "
            f"dependency {order_only}\n"
            f"  LINK_LIBRARIES = {libraries}\n",
            encoding="utf-8",
        )

    write(gate.LUA_ARCHIVE_NAME, gate.LUA_ARCHIVE_NAME,
          f"{gate.LUA_ARCHIVE_NAME} -lm")
    gate.verify_lua_link_closure(ninja, archive)
    assert gate.ninja_link_objects(ninja, build) == [(build / obj).resolve()]
    must_fail(
        "Lua archive leaked into OFF link",
        lambda: gate.verify_lua_link_closure(ninja, None),
        "leaked into an ISAAC_VITA_LUA=OFF link",
    )

    write(f"wrong/{gate.LUA_ARCHIVE_NAME}", gate.LUA_ARCHIVE_NAME,
          gate.LUA_ARCHIVE_NAME)
    must_fail(
        "wrong Lua archive path",
        lambda: gate.verify_lua_link_closure(ninja, archive),
        "exact sole Lua archive",
    )
    write(gate.LUA_ARCHIVE_NAME, gate.LUA_ARCHIVE_NAME,
          gate.LUA_ARCHIVE_NAME, direct=" arbitrary.a")
    must_fail(
        "arbitrary direct archive",
        lambda: gate.ninja_link_objects(ninja, build),
        "non-object direct target link input",
    )


def relocation_tests() -> None:
    gate.verify_lua_allocator_relocations(
        "lauxlib.c", gate.LUA_EXPECTED_RAW_RELOCATIONS.copy()
    )
    gate.verify_lua_allocator_relocations("lapi.c", collections.Counter())
    must_fail(
        "wrong lauxlib relocation type",
        lambda: gate.verify_lua_allocator_relocations(
            "lauxlib.c",
            gate._counter(((1, "l_alloc", "free", "R_ARM_ABS32"),)),
        ),
        "relocation policy changed",
    )
    must_fail(
        "raw allocator relocation in ordinary Lua source",
        lambda: gate.verify_lua_allocator_relocations(
            "lapi.c", gate.LUA_EXPECTED_RAW_RELOCATIONS.copy()
        ),
        "relocation policy changed",
    )


def main() -> int:
    with tempfile.TemporaryDirectory(
        prefix="isaac-vita-lua-archive-gate-"
    ) as value:
        root = Path(value).resolve()
        source_tests(root)
        cache_tests(root)
        compile_tests(root)
        member_tests()
        link_tests(root)
        relocation_tests()
    print(
        "Vita Lua exact cache/source/33-compile/archive-member/link/"
        "allocator-relocation hostile gate: PASS"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
