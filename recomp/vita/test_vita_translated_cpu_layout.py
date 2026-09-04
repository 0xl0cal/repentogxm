#!/usr/bin/env python3
"""Host tests for the ISAAC_VITA_TRANSLATED_CPU layout tooling.

Covers the hot-set reader/renderer, the BFS selection with a byte budget and
DFS placement, and the raw allocator gate's acceptance of the layout header as
the second forced include on generated units only.
"""
from __future__ import annotations

import json
import os
from pathlib import Path
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import vita_raw_allocator_gate as gate  # noqa: E402
import vita_translated_cpu_hot_set as hot_set  # noqa: E402
import vita_translated_cpu_layout as layout  # noqa: E402


def expect(condition: bool, label: str) -> None:
    if not condition:
        raise AssertionError(label)


def must_fail(label, callback, needle: str) -> None:
    try:
        callback()
    except gate.GateError as exc:
        if needle not in str(exc):
            raise AssertionError(f"{label}: wrong failure: {exc}") from exc
    else:
        raise AssertionError(f"{label}: hostile fixture passed")


def layout_tests(root: Path) -> None:
    root.mkdir()
    funcs = root / "guest_funcs.h"
    funcs.write_text(
        "/* GENERATED */\n#include \"guest.h\"\n\n"
        "void sub_00000010(CPU *__restrict c);\n"
        "void sub_00000020(CPU *__restrict c);\n"
        "void sub_00000030(CPU *__restrict c);\n",
        encoding="ascii")
    hot = root / "hot.txt"
    hot.write_text("# comment\nsub_00000030\n\nsub_00000010\nsub_deadbeef\n",
                   encoding="ascii")
    text = layout.render(layout.read_hot_set(str(hot)),
                         layout.read_declared(str(funcs)), cold_rest=False)
    lines = text.splitlines()
    expect('#include "guest.h"' in lines, "layout header includes guest.h")
    expect('void sub_00000030(CPU *__restrict c) __attribute__((hot, '
           'section(".text.sorted.000000")));' in lines, "first hot slot")
    expect('void sub_00000010(CPU *__restrict c) __attribute__((hot, '
           'section(".text.sorted.000001")));' in lines, "second hot slot")
    expect("sub_deadbeef" not in text, "undeclared hot entry is skipped")
    expect("cold" not in text, "no cold attributes without --cold-rest")
    expect("#define ISAAC_VITA_TRANSLATED_CPU_LAYOUT_HOT_COUNT 2U" in lines,
           "hot count reflects placed functions")

    text = layout.render(layout.read_hot_set(str(hot)),
                         layout.read_declared(str(funcs)), cold_rest=True)
    expect('void sub_00000020(CPU *__restrict c) __attribute__((cold));'
           in text.splitlines(), "cold rest marks the non-hot function")
    expect(text.count("__attribute__((cold))") == 1, "only non-hot are cold")

    hot.write_text("sub_00000010\nsub_00000010\n", encoding="ascii")
    try:
        layout.read_hot_set(str(hot))
    except ValueError as exc:
        expect("duplicate" in str(exc), "duplicate diagnostic")
    else:
        raise AssertionError("duplicate hot entry accepted")
    hot.write_text("main\n", encoding="ascii")
    try:
        layout.read_hot_set(str(hot))
    except ValueError as exc:
        expect("not a translated function name" in str(exc), "name diagnostic")
    else:
        raise AssertionError("non-translated name accepted")


def hot_set_tests(root: Path) -> None:
    generated = root / "generated"
    generated.mkdir(parents=True)
    (generated / "guest_0000.c").write_text(
        "void sub_00000010(CPU *__restrict c)\n{\n"
        "    sub_00000020(c); sub_00000030(c);\n}\n"
        "void sub_00000020(CPU *__restrict c)\n{\n    sub_00000040(c);\n}\n"
        "void sub_00000030(CPU *__restrict c)\n{\n    sub_00000030(c);\n}\n"
        "void sub_00000040(CPU *__restrict c)\n{\n}\n"
        "void sub_00000050(CPU *__restrict c)\n{\n    sub_00000010(c);\n}\n",
        encoding="ascii")
    graph = hot_set.load_graph(str(generated))
    expect(graph["sub_00000010"] == ["sub_00000020", "sub_00000030"], "edges")
    expect(graph["sub_00000030"] == [], "self edge dropped")
    sizes = {"sub_00000010": 100, "sub_00000020": 100, "sub_00000030": 100,
             "sub_00000040": 1000, "sub_00000050": 100}
    selected, depth, total = hot_set.select(graph, sizes, ["sub_00000010"], 350)
    expect(selected == ["sub_00000010", "sub_00000020", "sub_00000030"],
           f"budget stops at depth-2 giant: {selected}")
    expect(total == 300 and depth["sub_00000040"] == 2, "depth bookkeeping")
    expect("sub_00000050" not in depth, "unreachable root caller excluded")
    placed = hot_set.order(graph, ["sub_00000010"], selected)
    expect(placed == ["sub_00000010", "sub_00000020", "sub_00000030"],
           f"DFS preorder places callee after caller: {placed}")


def gate_tests(root: Path) -> None:
    root.mkdir()
    poison = (root / "poison.h").resolve()
    poison.write_text("/* poison */\n", encoding="ascii")
    build = root / "build"
    build.mkdir()
    header = build / gate.TRANSLATED_CPU_LAYOUT_HEADER_RELATIVE
    header.parent.mkdir(parents=True)
    header.write_text("/* layout */\n", encoding="ascii")
    header = header.resolve()
    sources = {}
    entries = []
    for name, extra in (("guest_0007.c", f' -include "{header.as_posix()}"'),
                        ("guest.c", "")):
        source = (root / "source" / "recomp" /
                  ("runtime" if name == "guest.c" else "generated") / name)
        source.parent.mkdir(parents=True, exist_ok=True)
        source.write_text("int x;\n", encoding="ascii")
        source = source.resolve()
        output = f"CMakeFiles/isaac_first_arm_fault.dir/{name}.obj"
        obj = build / output
        obj.parent.mkdir(parents=True, exist_ok=True)
        obj.write_bytes(b"fixture")
        exempt = " -DISAAC_VITA_RAW_ALLOCATOR_EXEMPT=1" if name == "guest.c" else ""
        entries.append({
            "directory": str(build),
            "command": (f"cc -DISAAC_VITA_RAW_ALLOCATOR_GATE=1{exempt} -include "
                        f'"{poison.as_posix()}"{extra} -o {output} -c '
                        f'"{source.as_posix()}"'),
            "file": str(source), "output": output,
        })
        sources[name] = source
    commands = build / "compile_commands.json"
    commands.write_text(json.dumps(entries), encoding="utf-8")
    expected = set(sources.values())

    records = gate.compile_closure(commands, expected, poison,
                                   layout_header=header)
    expect(set(records) == expected, "layout header accepted on generated unit")
    must_fail("layout include without the knob",
              lambda: gate.compile_closure(commands, expected, poison),
              "forced-include closure")

    swapped = json.loads(commands.read_text(encoding="utf-8"))
    swapped[0]["command"] = swapped[0]["command"].replace(
        f' -include "{header.as_posix()}"', "")
    swapped[1]["command"] = swapped[1]["command"].replace(
        " -o ", f' -include "{header.as_posix()}" -o ')
    commands.write_text(json.dumps(swapped), encoding="utf-8")
    must_fail("layout include on a runtime source",
              lambda: gate.compile_closure(commands, expected, poison,
                                           layout_header=header),
              "forced-include closure")

    cache = {"ISAAC_VITA_TRANSLATED_CPU": "ON",
             "ISAAC_VITA_TRANSLATED_CPU_HOT_LAYOUT": "ON"}
    expect(gate.translated_cpu_layout_header(cache, build) == header,
           "cache ON/ON resolves the header")
    expect(gate.translated_cpu_layout_header({}, build) is None,
           "absent keys mean OFF")
    expect(gate.translated_cpu_layout_header(
        {"ISAAC_VITA_TRANSLATED_CPU": "ON",
         "ISAAC_VITA_TRANSLATED_CPU_HOT_LAYOUT": "OFF"}, build) is None,
           "hot layout OFF means no header")
    header.unlink()
    must_fail("missing header with knob ON",
              lambda: gate.translated_cpu_layout_header(cache, build),
              "layout header is missing")


def main() -> int:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        layout_tests(root / "layout")
        hot_set_tests(root / "hot")
        gate_tests(root / "gate")
    print("translated-cpu layout tests passed: render/hot-set/gate-include")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
