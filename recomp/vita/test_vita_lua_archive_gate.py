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

    # ISAAC_VITA_LUA_CORE_ARM: CMake swaps -mthumb for -marm in place
    # (recomp/vita/CMakeLists.txt, ISAAC_LUA53_ISA); nothing else may move.
    must_fail(
        "Thumb archive certified as ARM",
        lambda: gate.lua_compile_closure(
            commands, lua_source, records, require_objects=False,
            core_arm=True,
        ),
        "option sequence changed",
    )
    arm = json.loads(json.dumps(entries))
    for entry in arm:
        arguments = entry["arguments"]
        arguments[arguments.index("-mthumb")] = "-marm"
    commands.write_text(json.dumps(arm), encoding="utf-8")
    assert len(gate.lua_compile_closure(
        commands, lua_source, records, require_objects=False, core_arm=True
    )) == gate.LUA_C_SOURCE_COUNT
    must_fail(
        "ARM archive certified as Thumb",
        lambda: gate.lua_compile_closure(
            commands, lua_source, records, require_objects=False
        ),
        "option sequence changed",
    )
    mixed = json.loads(json.dumps(arm))
    mixed[5]["arguments"][mixed[5]["arguments"].index("-marm")] = "-mthumb"
    commands.write_text(json.dumps(mixed), encoding="utf-8")
    must_fail(
        "one Thumb member inside an ARM archive",
        lambda: gate.lua_compile_closure(
            commands, lua_source, records, require_objects=False,
            core_arm=True,
        ),
        "option sequence changed",
    )
    both = json.loads(json.dumps(arm))
    both[0]["arguments"].insert(
        both[0]["arguments"].index("-marm") + 1, "-mthumb"
    )
    commands.write_text(json.dumps(both), encoding="utf-8")
    must_fail(
        "both ISA options on one ARM member",
        lambda: gate.lua_compile_closure(
            commands, lua_source, records, require_objects=False,
            core_arm=True,
        ),
        "option sequence changed",
    )
    commands.write_text(json.dumps(entries), encoding="utf-8")


def derived_compile_tests(root: Path) -> None:
    """ISAAC_VITA_LUA_GC_FINALIZER_PACE: one source compiled from the build tree."""

    (root / "derived").mkdir()
    lua_source, commands, records = compile_fixture(root / "derived")
    build = commands.parent
    source_dir = (lua_source / "src").resolve()
    replaced_name = records[5][0]
    pristine = source_dir / replaced_name
    derived = build / gate.LUA_GC_FINALIZER_PACE_DIR / replaced_name
    derived.parent.mkdir()
    derived.write_bytes(b"derived " + replaced_name.encode("ascii"))
    entries = json.loads(commands.read_text(encoding="utf-8"))
    pristine_entries = json.loads(json.dumps(entries))
    output = (f"CMakeFiles/{gate.LUA_TARGET}.dir/"
              f"{gate.LUA_GC_FINALIZER_PACE_DIR}/{replaced_name}.obj")
    arguments = entries[5]["arguments"]
    arguments[arguments.index("-o") + 1] = output
    arguments[-1] = str(derived)
    entries[5]["file"] = str(derived)
    entries[5]["output"] = output
    commands.write_text(json.dumps(entries), encoding="utf-8")
    compiled = gate.lua_compile_closure(
        commands, lua_source, records, require_objects=False,
        derived_sources={pristine: derived},
    )
    assert len(compiled) == gate.LUA_C_SOURCE_COUNT
    record = compiled[pristine.resolve()]
    assert record["derived_source"] == derived.resolve()
    assert record["source"] == derived.resolve()
    assert record["derived_sha256"] == gate._sha256(derived)
    must_fail(
        "derived compile without the knob",
        lambda: gate.lua_compile_closure(
            commands, lua_source, records, require_objects=False
        ),
        "Lua compile-source closure changed",
    )
    commands.write_text(json.dumps(pristine_entries), encoding="utf-8")
    must_fail(
        "knob ON but the pristine source compiled",
        lambda: gate.lua_compile_closure(
            commands, lua_source, records, require_objects=False,
            derived_sources={pristine: derived},
        ),
        "Lua compile-source closure changed",
    )
    must_fail(
        "derived source renamed",
        lambda: gate.lua_compile_closure(
            commands, lua_source, records, require_objects=False,
            derived_sources={pristine: derived.with_name("other.c")},
        ),
        "renames",
    )
    must_fail(
        "derived source outside the closure",
        lambda: gate.lua_compile_closure(
            commands, lua_source, records, require_objects=False,
            derived_sources={source_dir / "lgc_extra.c": derived},
        ),
        "outside the closure",
    )

    # ISAAC_VITA_LUA_CORE_ARM together with the pace (both lua-native and
    # lua-gc ON, the integration's measurement recipe): every member,
    # the derived lgc.c included, carries -marm in place of -mthumb.
    arm = json.loads(json.dumps(entries))
    for entry in arm:
        arguments = entry["arguments"]
        arguments[arguments.index("-mthumb")] = "-marm"
    commands.write_text(json.dumps(arm), encoding="utf-8")
    compiled = gate.lua_compile_closure(
        commands, lua_source, records, require_objects=False, core_arm=True,
        derived_sources={pristine: derived},
    )
    assert len(compiled) == gate.LUA_C_SOURCE_COUNT
    assert compiled[pristine.resolve()]["derived_sha256"] == gate._sha256(derived)
    must_fail(
        "ARM pace archive certified as Thumb",
        lambda: gate.lua_compile_closure(
            commands, lua_source, records, require_objects=False,
            derived_sources={pristine: derived},
        ),
        "option sequence changed",
    )
    thumb_derived = json.loads(json.dumps(arm))
    arguments = thumb_derived[5]["arguments"]
    arguments[arguments.index("-marm")] = "-mthumb"
    commands.write_text(json.dumps(thumb_derived), encoding="utf-8")
    must_fail(
        "Thumb derived lgc.c inside an ARM archive",
        lambda: gate.lua_compile_closure(
            commands, lua_source, records, require_objects=False,
            core_arm=True, derived_sources={pristine: derived},
        ),
        "option sequence changed",
    )
    commands.write_text(json.dumps(entries), encoding="utf-8")


def derived_source_tests(root: Path) -> None:
    build = root / "pace-build"
    lua_source = root / "pace-lua"
    (lua_source / "src").mkdir(parents=True)
    pristine = lua_source / "src" / gate.LUA_GC_FINALIZER_PACE_SOURCE
    pristine.write_bytes(b"pristine lgc\n")
    derived = build / gate.LUA_GC_FINALIZER_PACE_DIR / gate.LUA_GC_FINALIZER_PACE_SOURCE
    calls: list[tuple[bytes, int, int, int]] = []

    def fake_derive(data: bytes, max_per_call: int, cost: int,
                    sweep_cost: int) -> bytes:
        calls.append((data, max_per_call, cost, sweep_cost))
        return data + (f"max={max_per_call} cost={cost} "
                       f"sweep={sweep_cost}\n").encode("ascii")

    on = {
        gate.LUA_GC_FINALIZER_PACE_CACHE_KEY: "ON",
        gate.LUA_GC_FINALIZER_MAX_CACHE_KEY: "8",
        gate.LUA_GC_FINALIZER_COST_CACHE_KEY: "512",
        gate.LUA_GC_SWEEP_COST_CACHE_KEY: "32",
    }
    off = {**on, gate.LUA_GC_FINALIZER_PACE_CACHE_KEY: "OFF"}
    assert gate.lua_derived_sources(
        off, build_dir=build, lua_source=lua_source, derive=fake_derive) == {}
    assert not calls
    must_fail(
        "knob ON without the derived file",
        lambda: gate.lua_derived_sources(
            on, build_dir=build, lua_source=lua_source, derive=fake_derive),
        "not a regular non-symlink file",
    )
    derived.parent.mkdir(parents=True)
    derived.write_bytes(b"pristine lgc\nmax=8 cost=512 sweep=32\n")
    assert gate.lua_derived_sources(
        on, build_dir=build, lua_source=lua_source, derive=fake_derive
    ) == {pristine.resolve(): derived.resolve()}
    assert calls[-1] == (b"pristine lgc\n", 8, 512, 32)
    must_fail(
        "derived bytes from other constants",
        lambda: gate.lua_derived_sources(
            {**on, gate.LUA_GC_FINALIZER_COST_CACHE_KEY: "1024"},
            build_dir=build, lua_source=lua_source, derive=fake_derive),
        "differs from the derivation",
    )
    must_fail(
        "derived bytes from another sweep cost",
        lambda: gate.lua_derived_sources(
            {**on, gate.LUA_GC_SWEEP_COST_CACHE_KEY: "5"},
            build_dir=build, lua_source=lua_source, derive=fake_derive),
        "differs from the derivation",
    )
    must_fail(
        "knob ON without SWEEP_COST in the cache",
        lambda: gate.lua_derived_sources(
            {key: value for key, value in on.items()
             if key != gate.LUA_GC_SWEEP_COST_CACHE_KEY},
            build_dir=build, lua_source=lua_source, derive=fake_derive),
        "missing ISAAC_VITA_LUA_GC_SWEEP_COST",
    )
    derived.write_bytes(b"pristine lgc\nmax=8 cost=512 sweep=32 tampered\n")
    must_fail(
        "tampered derived source",
        lambda: gate.lua_derived_sources(
            on, build_dir=build, lua_source=lua_source, derive=fake_derive),
        "differs from the derivation",
    )
    must_fail(
        "knob ON without MAX in the cache",
        lambda: gate.lua_derived_sources(
            {gate.LUA_GC_FINALIZER_PACE_CACHE_KEY: "ON",
             gate.LUA_GC_FINALIZER_COST_CACHE_KEY: "512",
             gate.LUA_GC_SWEEP_COST_CACHE_KEY: "32"},
            build_dir=build, lua_source=lua_source, derive=fake_derive),
        "missing ISAAC_VITA_LUA_GC_FINALIZER_MAX",
    )
    must_fail(
        "non-numeric COST",
        lambda: gate.lua_derived_sources(
            {**on, gate.LUA_GC_FINALIZER_COST_CACHE_KEY: "fast"},
            build_dir=build, lua_source=lua_source, derive=fake_derive),
        "not a whole number",
    )
    must_fail(
        "cache without the knob",
        lambda: gate.lua_derived_sources(
            {}, build_dir=build, lua_source=lua_source, derive=fake_derive),
        "missing ISAAC_VITA_LUA_GC_FINALIZER_PACE",
    )
    # The real derivation refuses a non-pristine input (it pins the manifest
    # sha of lgc.c), and the gate reports that as a gate error.
    derived.write_bytes(b"anything\n")
    must_fail(
        "real derivation of a non-pristine lgc.c",
        lambda: gate.lua_derived_sources(
            on, build_dir=build, lua_source=lua_source),
        "not pristine Lua 5.3.3 lgc.c",
    )


def multi_derived_compile_tests(root: Path) -> None:
    """Two knobs at once (finalizer pace + integer2str): two members of the
    one closure compile from two build-tree directories, each keeping its
    archive slot, and each record carries its own derived identity."""

    (root / "multi").mkdir()
    lua_source, commands, records = compile_fixture(root / "multi")
    build = commands.parent
    source_dir = (lua_source / "src").resolve()
    entries = json.loads(commands.read_text(encoding="utf-8"))
    pairs: dict[Path, Path] = {}
    for index, directory in ((5, gate.LUA_GC_FINALIZER_PACE_DIR),
                             (17, gate.LUA_INTEGER2STR_DIR)):
        name = records[index][0]
        derived = build / directory / name
        derived.parent.mkdir()
        derived.write_bytes(f"derived {directory} {name}".encode("ascii"))
        output = f"CMakeFiles/{gate.LUA_TARGET}.dir/{directory}/{name}.obj"
        arguments = entries[index]["arguments"]
        arguments[arguments.index("-o") + 1] = output
        arguments[-1] = str(derived)
        entries[index]["file"] = str(derived)
        entries[index]["output"] = output
        pairs[source_dir / name] = derived
    commands.write_text(json.dumps(entries), encoding="utf-8")
    compiled = gate.lua_compile_closure(
        commands, lua_source, records, require_objects=False,
        derived_sources=pairs,
    )
    assert len(compiled) == gate.LUA_C_SOURCE_COUNT
    for pristine, derived in pairs.items():
        record = compiled[pristine.resolve()]
        assert record["derived_source"] == derived.resolve()
        assert record["derived_sha256"] == gate._sha256(derived)
    assert sum("derived_sha256" in record for record in compiled.values()) == 2
    first, second = list(pairs.items())
    must_fail(
        "second derived compile without its knob",
        lambda: gate.lua_compile_closure(
            commands, lua_source, records, require_objects=False,
            derived_sources=dict([first]),
        ),
        "Lua compile-source closure changed",
    )
    must_fail(
        "second knob ON but only one derived compile",
        lambda: gate.lua_compile_closure(
            commands, lua_source, records, require_objects=False,
            derived_sources={**pairs, source_dir / records[9][0]:
                             build / gate.LUA_INTEGER2STR_DIR / records[9][0]},
        ),
        "Lua compile-source closure changed",
    )


def derived_source_map_tests(root: Path) -> None:
    """lua_derived_source_map: the union of every ON derivation's pairs."""

    build = root / "map-build"
    lua_source = root / "map-lua"
    (lua_source / "src").mkdir(parents=True)
    lobject = lua_source / "src" / gate.LUA_INTEGER2STR_SOURCE
    lobject.write_bytes(b"pristine lobject\n")
    derived = build / gate.LUA_INTEGER2STR_DIR / gate.LUA_INTEGER2STR_SOURCE
    calls: list[bytes] = []

    def fake_derive(data: bytes) -> bytes:
        calls.append(data)
        return data + b"integer2str\n"

    on = {gate.LUA_INTEGER2STR_CACHE_KEY: "ON"}
    off = {gate.LUA_INTEGER2STR_CACHE_KEY: "OFF"}
    assert gate.lua_integer2str_sources(
        off, build_dir=build, lua_source=lua_source, derive=fake_derive) == {}
    assert not calls
    must_fail(
        "integer2str ON without the derived file",
        lambda: gate.lua_integer2str_sources(
            on, build_dir=build, lua_source=lua_source, derive=fake_derive),
        "not a regular non-symlink file",
    )
    must_fail(
        "integer2str knob missing from the cache",
        lambda: gate.lua_integer2str_sources(
            {}, build_dir=build, lua_source=lua_source, derive=fake_derive),
        "missing ISAAC_VITA_LUA_INTEGER2STR",
    )
    derived.parent.mkdir(parents=True)
    derived.write_bytes(b"pristine lobject\ninteger2str\n")
    assert gate.lua_integer2str_sources(
        on, build_dir=build, lua_source=lua_source, derive=fake_derive
    ) == {lobject.resolve(): derived.resolve()}
    assert calls[-1] == b"pristine lobject\n"
    derived.write_bytes(b"pristine lobject\ninteger2str tampered\n")
    must_fail(
        "tampered derived lobject.c",
        lambda: gate.lua_integer2str_sources(
            on, build_dir=build, lua_source=lua_source, derive=fake_derive),
        "derived lobject.c differs from the derivation",
    )
    # The real derivation pins the manifest sha of lobject.c.
    must_fail(
        "real derivation of a non-pristine lobject.c",
        lambda: gate.lua_integer2str_sources(
            on, build_dir=build, lua_source=lua_source),
        "not pristine Lua 5.3.3 lobject.c",
    )
    derived.write_bytes(b"pristine lobject\ninteger2str\n")

    # The aggregator: both knobs, either, neither.
    lgc = lua_source / "src" / "lgc.c"
    paced = build / gate.LUA_GC_FINALIZER_PACE_DIR / "lgc.c"

    def pace_rows(cache, *, build_dir, lua_source):
        assert build_dir == build
        if cache.get("PACE") != "ON":
            return {}
        return {lgc: paced}

    def i2s_rows(cache, *, build_dir, lua_source):
        return gate.lua_integer2str_sources(
            cache, build_dir=build_dir, lua_source=lua_source,
            derive=fake_derive)

    rows = (("PACE", pace_rows), (gate.LUA_INTEGER2STR_CACHE_KEY, i2s_rows))
    both = gate.lua_derived_source_map(
        {**on, "PACE": "ON"}, build_dir=build, lua_source=lua_source,
        derivations=rows)
    assert both == {lgc.resolve(): paced.resolve(),
                    lobject.resolve(): derived.resolve()}
    assert gate.lua_derived_source_map(
        {**off, "PACE": "ON"}, build_dir=build, lua_source=lua_source,
        derivations=rows) == {lgc.resolve(): paced.resolve()}
    assert gate.lua_derived_source_map(
        {**on, "PACE": "OFF"}, build_dir=build, lua_source=lua_source,
        derivations=rows) == {lobject.resolve(): derived.resolve()}
    assert gate.lua_derived_source_map(
        {**off, "PACE": "OFF"}, build_dir=build, lua_source=lua_source,
        derivations=rows) == {}

    def lobject_too(cache, *, build_dir, lua_source):
        return {lobject: build / "other" / "lobject.c"}

    must_fail(
        "two derivations replace one pristine file",
        lambda: gate.lua_derived_source_map(
            on, build_dir=build, lua_source=lua_source,
            derivations=rows[1:] + (("OTHER", lobject_too),)),
        "both replace lobject.c",
    )

    def same_derived(cache, *, build_dir, lua_source):
        return {lua_source / "src" / "lapi.c": derived}

    must_fail(
        "two derivations claim one derived file",
        lambda: gate.lua_derived_source_map(
            on, build_dir=build, lua_source=lua_source,
            derivations=rows[1:] + (("OTHER", same_derived),)),
        "both claim",
    )
    # The production registry names both knobs, and an all-OFF cache selects
    # nothing (the pristine 33-source closure).
    assert [name for name, unused in gate.LUA_DERIVATIONS] == [
        gate.LUA_GC_FINALIZER_PACE_CACHE_KEY, gate.LUA_INTEGER2STR_CACHE_KEY]
    assert gate.lua_derived_source_map(
        {gate.LUA_GC_FINALIZER_PACE_CACHE_KEY: "OFF", **off},
        build_dir=build, lua_source=lua_source) == {}
    must_fail(
        "registry with the integer2str knob missing from the cache",
        lambda: gate.lua_derived_source_map(
            {gate.LUA_GC_FINALIZER_PACE_CACHE_KEY: "OFF"},
            build_dir=build, lua_source=lua_source),
        "missing ISAAC_VITA_LUA_INTEGER2STR",
    )


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


def build_vita_receipt_tests() -> None:
    """tools/build_vita.py re-verifies the gate receipt of a wrapper build.

    With ISAAC_VITA_LUA_GC_FINALIZER_PACE=ON lua_archive_closure adds
    derived_sha256 to the lgc.c member row (and only to it); the wrapper must
    accept exactly that shape with the knob ON and nothing else."""

    import importlib.util

    path = Path(__file__).resolve().parents[2] / "tools" / "build_vita.py"
    spec = importlib.util.spec_from_file_location(
        "build_vita_lua_archive_gate_test", path
    )
    assert spec is not None and spec.loader is not None
    bv = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(bv)
    pinned = bv._load_lua53_pinned_records()
    c_records = [item for item in pinned if item[0].endswith(".c")]
    archive = {"size": 4096, "sha256": "a" * 64}

    def record(derived_on: tuple[str, ...] = (), core_arm: bool = False,
               derived_value: str = "c" * 64) -> dict[str, object]:
        dead = list(
            bv.LUA53_DEAD_RAW_RELOCATIONS_ARM if core_arm
            else bv.LUA53_DEAD_RAW_RELOCATIONS
        )
        members = []
        for name, source_sha256 in c_records:
            source = f"{bv.LUA53_LOGICAL_SOURCE_PREFIX}/{name}"
            row: dict[str, object] = {
                "source": source,
                "source_sha256": source_sha256,
                "object": (f"{bv.RAW_ALLOCATOR_GATE_LOGICAL_OBJECT_PREFIX}"
                           f"{source}.obj"),
                "member": name + ".obj",
                "size": 64,
                "sha256": "b" * 64,
                "raw_relocations": dead if name == "lauxlib.c" else [],
            }
            if name in derived_on:
                row["derived_sha256"] = derived_value
            members.append(row)
        return {
            "path": bv.LUA53_ARCHIVE_NAME,
            **archive,
            "manifest_sha256": bv.LUA53_CMAKE_MANIFEST_SHA256,
            "source_set_sha256": bv._lua53_source_set_sha256(pinned),
            "source_count": len(c_records),
            "member_count": len(members),
            "members": members,
            "allocator_policy": {
                "required_final_symbol": bv.LUA53_REQUIRED_FINAL_SYMBOL,
                "discarded_final_symbols": sorted(
                    bv.LUA53_DISCARDED_ALLOCATOR_SYMBOLS
                ),
                "dead_raw_relocations": dead,
            },
        }

    def verify(rec: dict[str, object], **knobs: bool) -> int:
        return bv._verify_raw_allocator_lua_archive(
            rec, archive_input=archive, **knobs
        )

    def must_reject(label: str, callback, needle: str) -> None:
        try:
            callback()
        except bv.BuildError as exc:
            if needle not in str(exc):
                raise AssertionError(f"{label}: wrong failure: {exc}") from exc
        else:
            raise AssertionError(f"{label}: hostile receipt passed")

    count = gate.LUA_C_SOURCE_COUNT
    assert verify(record()) == count
    assert verify(record(("lgc.c",)), finalizer_pace=True) == count
    assert verify(record(("lgc.c",), core_arm=True), core_arm=True,
                  finalizer_pace=True) == count
    must_reject("derived row without the knob",
                lambda: verify(record(("lgc.c",))), "member row is malformed")
    must_reject("knob without the derived row",
                lambda: verify(record(), finalizer_pace=True),
                "member row is malformed")
    must_reject("derived key on a second member",
                lambda: verify(record(("lapi.c", "lgc.c")), finalizer_pace=True),
                "member row is malformed")
    must_reject("derived key on the wrong member",
                lambda: verify(record(("lapi.c",)), finalizer_pace=True),
                "member row is malformed")
    must_reject("derived identity not a sha256",
                lambda: verify(record(("lgc.c",), derived_value="lgc"),
                               finalizer_pace=True),
                "derived lgc.c identity is malformed")
    # ISAAC_VITA_LUA_INTEGER2STR: the same contract on lobject.c, alone and
    # together with the finalizer pace (two derived rows, one per knob).
    assert verify(record(("lobject.c",)), integer2str=True) == count
    assert verify(record(("lgc.c", "lobject.c")), finalizer_pace=True,
                  integer2str=True) == count
    assert verify(record(("lgc.c", "lobject.c"), core_arm=True), core_arm=True,
                  finalizer_pace=True, integer2str=True) == count
    must_reject("lobject.c derived row without the knob",
                lambda: verify(record(("lobject.c",))),
                "member row is malformed")
    must_reject("integer2str knob without the derived row",
                lambda: verify(record(), integer2str=True),
                "member row is malformed")
    must_reject("both knobs, only the lgc.c row",
                lambda: verify(record(("lgc.c",)), finalizer_pace=True,
                               integer2str=True),
                "member row is malformed")
    must_reject("integer2str knob, derived row on lgc.c",
                lambda: verify(record(("lgc.c",)), integer2str=True),
                "member row is malformed")
    must_reject("derived lobject.c identity not a sha256",
                lambda: verify(record(("lobject.c",), derived_value="lobj"),
                               integer2str=True),
                "derived lobject.c identity is malformed")


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

    # ISAAC_VITA_LUA_CORE_ARM: the same two l_alloc edges in ARM encoding.
    arm = gate._counter((
        (1, "l_alloc", "free", "R_ARM_CALL"),
        (1, "l_alloc", "realloc", "R_ARM_JUMP24"),
    ))
    assert gate.lua_expected_raw_relocations(True) == arm
    assert gate.lua_expected_raw_relocations(False) == (
        gate.LUA_EXPECTED_RAW_RELOCATIONS
    )
    gate.verify_lua_allocator_relocations("lauxlib.c", arm.copy(), True)
    gate.verify_lua_allocator_relocations("lapi.c", collections.Counter(), True)
    must_fail(
        "ARM l_alloc edges under the Thumb policy",
        lambda: gate.verify_lua_allocator_relocations("lauxlib.c", arm.copy()),
        "relocation policy changed",
    )
    must_fail(
        "Thumb l_alloc edges under the ARM policy",
        lambda: gate.verify_lua_allocator_relocations(
            "lauxlib.c", gate.LUA_EXPECTED_RAW_RELOCATIONS.copy(), True
        ),
        "relocation policy changed",
    )
    must_fail(
        "ARM raw allocator relocation in ordinary Lua source",
        lambda: gate.verify_lua_allocator_relocations("lapi.c", arm.copy(), True),
        "relocation policy changed",
    )
    must_fail(
        "extra ARM raw allocator edge in lauxlib",
        lambda: gate.verify_lua_allocator_relocations(
            "lauxlib.c",
            arm + gate._counter(((1, "l_alloc", "malloc", "R_ARM_CALL"),)),
            True,
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
        derived_compile_tests(root)
        derived_source_tests(root)
        multi_derived_compile_tests(root)
        derived_source_map_tests(root)
        member_tests()
        build_vita_receipt_tests()
        link_tests(root)
        relocation_tests()
    print(
        "Vita Lua exact cache/source/33-compile/derived-lgc+lobject/archive-member/"
        "link/allocator-relocation hostile gate: PASS"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
