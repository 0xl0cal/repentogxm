#!/usr/bin/env python3
"""Frozen PE + exact 32-bit Lua oracle for EID startup before frame one."""

from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
from typing import NoReturn


HERE = Path(__file__).resolve().parent
VITA = HERE / "vita"
HARNESS = VITA / "eid_oracle" / "eid_startup_harness.lua"
RUNNER = VITA / "eid_oracle" / "eid_oracle_main.c"

PE_SIZE = 8_650_240
PE_SHA256 = "31846486979cfa07c8c968221553052c3ff603518681ca11912d445f96ca9404"
REGISTER_CLASSES_RVA = 0x004033F0
REGISTER_CLASSES_INSNS = 7_645
REGISTER_CLASSES_CFG_SHA256 = (
    "bedf2cdac2057f1832142e92266c8a58ccfcdd72fbfc23bb762d130092b790c6"
)
REGISTER_STRING_ROWS = 1_461
REGISTER_STRING_NAMES = 1_278
REGISTER_STRING_ROWS_SHA256 = (
    "3ffebc150e581c4a380290765a727989673eca1e8c6ac7de998d9c458cec18e5"
)
PUSHCLOSURE_DIRECT_SITES_SHA256 = (
    "1552b344a8ec2f342081d99c7502da7b3658f1744e51f4bbc3719536f4ecdc6a"
)
PUSHCLOSURE_LOAD_SITES_SHA256 = (
    "4ea73b14aa6d2291296f829a83382728fc3e7378e36a7b76631ea09faf0ce80e"
)

CORE_FILES = {
    "enums.lua": (149_353,
                  "dce1892f51e4e645d66ac8ffb31d217e8c36a32457c1005dd128978141103dc5"),
    "main.lua": (45_891,
                 "ae0efa082e0ea8844a6e7a5ab55e4a96a8dc87a8fa046483e590c0677c40cfdf"),
    "json.lua": (17_323,
                 "30f33a7250dd161a7e49b76cd9ee56fc0a00f06c7cc5ecbf4b906be65d454810"),
}
EID_FILES = 218
EID_BYTES = 27_355_052
EID_TREE_SHA256 = "c35df40ddb3d91ac1e02e323ed9cf406b3e3a547654a83dc4105eac4c052f63d"
EID_LUA_FILES = 135
EID_LUA_BYTES = 8_305_198
EID_LUA_TREE_SHA256 = (
    "65f3355481213588b8bbcbe56107e301b33694b23ff4a2c004e4f04221a6065e"
)
TRANSCRIPT_LINES = 429
TRANSCRIPT_SHA256 = "fbe00bcdd77237a6e96efafdef323a53861b6b0e702c2f2586827c5036b992be"

CORE_STRINGS = {
    0x0074F598: b"resources/scripts/enums.lua\0",
    0x0074F5D0: b".\\resources\\scripts\\?.lua\0",
    0x0074F708: b"resources/scripts/main.lua\0",
    0x0074F724: b"_RunCallback\0",
}

REQUIRED_NATIVE_NAMES = {
    "Isaac", "Vector", "Color", "KColor", "BitSet128", "Font",
    "ItemPool", "SFXManager", "HUD", "TemporaryEffects", "Room",
    "MusicManager", "Game", "Game_0", "Level", "Sprite", "EntityTear",
    "EntityBomb", "EntityKnife", "EntityLaser", "EntityProjectile",
    "EntityFamiliar", "EntityNPC", "EntityPickup", "EntityPlayer",
    "Options", "Input", "ItemConfig", "RoomDescriptor", "RegisterMod",
    "SetBuiltInCallbackState", "HasModData", "LoadModData", "SaveModData",
    "RemoveModData",
}


def fail(message: str) -> NoReturn:
    raise SystemExit(f"FAIL: {message}")


def require(condition: bool, message: str) -> None:
    if not condition:
        fail(message)


def digest(path: Path) -> str:
    value = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            value.update(block)
    return value.hexdigest()


def tree_digest(root: Path, suffix: str | None = None) -> tuple[int, int, str]:
    files = [
        path for path in root.rglob("*")
        if path.is_file() and (suffix is None or path.suffix == suffix)
    ]
    require(not any(path.is_symlink() for path in files),
            "EID oracle input contains a symlink")
    files.sort(key=lambda path: path.relative_to(root).as_posix().encode())
    value = hashlib.sha256()
    total = 0
    for path in files:
        relative = path.relative_to(root).as_posix().encode("utf-8")
        data = path.read_bytes()
        total += len(data)
        value.update(struct.pack("<I", len(relative)))
        value.update(relative)
        value.update(struct.pack("<Q", len(data)))
        value.update(hashlib.sha256(data).digest())
    return len(files), total, value.hexdigest()


def validate_inputs(pe: Path, eid: Path, core: Path) -> None:
    require(pe.is_file(), f"frozen PE is missing: {pe}")
    require(pe.stat().st_size == PE_SIZE, "frozen PE size changed")
    require(digest(pe) == PE_SHA256, "frozen PE hash changed")
    for name, (size, expected) in CORE_FILES.items():
        path = core / name
        require(path.is_file(), f"core script is missing: {name}")
        require(path.stat().st_size == size, f"core script size changed: {name}")
        require(digest(path) == expected, f"core script hash changed: {name}")
    require(tree_digest(eid) == (EID_FILES, EID_BYTES, EID_TREE_SHA256),
            "EID 5.23 bc0551a full tree changed")
    require(tree_digest(eid, ".lua") ==
            (EID_LUA_FILES, EID_LUA_BYTES, EID_LUA_TREE_SHA256),
            "EID 5.23 bc0551a Lua closure changed")


def static_registration_oracle(pe: Path) -> tuple[set[str], bytes]:
    os.environ["REPENTOGXM_PE"] = str(pe)
    sys.path.insert(0, str(HERE))
    import gen_all as G  # noqa: E402
    import trans as T  # noqa: E402
    from image import DEFAULT_BASE, Image  # noqa: E402

    image = Image(str(pe), DEFAULT_BASE)
    for rva, expected in CORE_STRINGS.items():
        require(image.code_at(rva, len(expected)) == expected,
                f"frozen Lua startup string changed at {rva:#x}")

    instructions, order, indirect = T.decode(image, REGISTER_CLASSES_RVA)
    cfg = hashlib.sha256()
    for rva in order:
        instruction = instructions[rva]
        cfg.update(struct.pack("<I", rva))
        cfg.update(bytes((instruction.size,)))
        cfg.update(image.code_at(rva, instruction.size))
    require(len(order) == REGISTER_CLASSES_INSNS and not indirect,
            "RegisterClasses decoded shape changed")
    require(cfg.hexdigest() == REGISTER_CLASSES_CFG_SHA256,
            "RegisterClasses decoded CFG changed")

    rows: list[tuple[int, int, str]] = []
    for rva in order:
        instruction = instructions[rva]
        if (instruction.mnemonic != "push" or len(instruction.operands) != 1 or
                instruction.operands[0].type != T.cx.X86_OP_IMM or
                instruction.imm_offset <= 0 or instruction.imm_size != 4 or
                rva + instruction.imm_offset not in image.reloc_rvas):
            continue
        address = instruction.operands[0].imm & 0xFFFFFFFF
        if not image.base <= address < image.base + image.size:
            continue
        string_rva = image.rva_of(address)
        data = bytearray()
        for offset in range(512):
            byte = image.code_at(string_rva + offset, 1)[0]
            if byte == 0:
                break
            data.append(byte)
        if data and all(32 <= byte < 127 for byte in data):
            rows.append((rva, string_rva, data.decode("ascii")))
    row_hash = hashlib.sha256()
    for site, string_rva, name in rows:
        row_hash.update(struct.pack("<II", site, string_rva))
        row_hash.update(name.encode("ascii") + b"\0")
    names = {name for _, _, name in rows}
    require(len(rows) == REGISTER_STRING_ROWS and
            len(names) == REGISTER_STRING_NAMES and
            row_hash.hexdigest() == REGISTER_STRING_ROWS_SHA256,
            "RegisterClasses relocated string inventory changed")

    invalid_register = (0, T.cx.X86_REG_INVALID)
    iat_rva = G.LUA_PUSHCLOSURE_IAT_RVA
    iat_value = image.va(iat_rva)

    def is_iat_operand(instruction, operand) -> bool:
        if operand.type != T.cx.X86_OP_MEM or operand.size != 4:
            return False
        memory = operand.mem
        relocation = instruction.address + instruction.disp_offset
        return (
            memory.segment in invalid_register and
            memory.base in invalid_register and
            memory.index in invalid_register and
            (memory.disp & 0xFFFFFFFF) == iat_value and
            instruction.disp_offset > 0 and instruction.disp_size == 4 and
            relocation in image.reloc_rvas and
            image.code_at(relocation, 4) == struct.pack("<I", iat_value)
        )

    direct: set[int] = set()
    loads: set[int] = set()
    for relocation in sorted(image.reloc_rvas):
        if (relocation < image.text_rva or relocation + 4 > image.text_end or
                image.code_at(relocation, 4) != struct.pack("<I", iat_value)):
            continue
        for distance in range(1, 9):
            start = relocation - distance
            if start < image.text_rva:
                continue
            decoded = list(T.CS.disasm(image.code_at(start, 16), start, count=1))
            if len(decoded) != 1:
                continue
            instruction = decoded[0]
            operands = instruction.operands
            if (instruction.address + instruction.size > image.text_end or
                    instruction.address + instruction.disp_offset != relocation):
                continue
            if (instruction.mnemonic == "call" and len(operands) == 1 and
                    is_iat_operand(instruction, operands[0])):
                direct.add(start)
            elif (instruction.mnemonic == "mov" and len(operands) == 2 and
                  operands[0].type == T.cx.X86_OP_REG and
                  operands[0].size == 4 and
                  is_iat_operand(instruction, operands[1])):
                loads.add(start)

    def rva_set_hash(values: set[int]) -> str:
        value = hashlib.sha256()
        for rva in sorted(values):
            value.update(struct.pack("<I", rva))
        return value.hexdigest()

    require(len(direct) == 1_047 and
            rva_set_hash(direct) == PUSHCLOSURE_DIRECT_SITES_SHA256,
            "raw direct lua_pushcclosure site census changed")
    require(len(loads) == 27 and
            rva_set_hash(loads) == PUSHCLOSURE_LOAD_SITES_SHA256,
            "raw register-loaded lua_pushcclosure site census changed")
    frozen = (
        G.LUA_CALLBACK_STATIC_ROWS, G.LUA_CALLBACK_TARGETS,
        G.LUA_CALLBACK_CANONICAL_TARGETS,
        G.LUA_CALLBACK_SUPPLEMENTAL_TARGETS,
        G.LUA_CALLBACK_CALL_ROWS_SHA256,
        G.LUA_CALLBACK_TARGETS_SHA256,
    )
    require(frozen == (
        1_105, 930, 762, 168,
        "9524c1240b564175ed66ec8e1711cb120be936ed74c8067bf4c8c95708f4f55f",
        "611a2bd5acdb70c0b26bd1389dea072e57a40af31e39c9dad2f957a5e5ba40ff",
    ), "frozen generated Lua callback closure changed")
    require(REQUIRED_NATIVE_NAMES <= names,
            "RegisterClasses is missing a required startup namespace/API")
    return names, pe.read_bytes()


def verify_transcript(path: Path, pe_bytes: bytes) -> None:
    data = path.read_bytes()
    rows = data.decode("utf-8").splitlines()
    require(len(rows) == TRANSCRIPT_LINES, "EID startup transcript line count changed")
    require(hashlib.sha256(data).hexdigest() == TRANSCRIPT_SHA256,
            "EID startup transcript changed")
    require("result\tok" in rows, "EID top-level did not complete")
    require("require_path\tjson\t<core>/json.lua" in rows,
            "EID json did not resolve through the frozen core path")
    require({"script\t<core>/enums.lua", "script\t<core>/main.lua",
             "script\t<eid>/main.lua"} <= set(rows),
            "core/EID startup script order proof is incomplete")
    require(rows[-1] == "callback_count\t35",
            "EID callback registration count changed")
    require(sum(row.startswith("callback\t") for row in rows) == 35,
            "EID callback capture is incomplete")
    require(sum(row.startswith("callback_registry\t") for row in rows) == 17,
            "EID core callback registry census changed")
    require(not any(row.startswith("callback_dispatch\t") for row in rows),
            "pre-frame oracle invoked an EID callback")
    require(sum(row.startswith("require\t") for row in rows) == 113 and
            sum(row.startswith("require_path\t") for row in rows) == 102,
            "EID startup require closure changed")

    used_names = set(REQUIRED_NATIVE_NAMES)
    for row in rows:
        fields = row.split("\t")
        if fields[0] not in {"call", "field", "native_method"}:
            continue
        leaf = fields[1].rsplit(".", 1)[-1].replace("()", "")
        if leaf and leaf != "<native>":
            used_names.add(leaf)
    missing = sorted(
        name for name in used_names if name.encode("ascii") + b"\0" not in pe_bytes
    )
    require(not missing,
            "startup API spellings absent from frozen PE: " + ", ".join(missing))


def build_and_run(lua_source: Path | None, eid: Path, core: Path,
                  keep_build: Path | None, pe_bytes: bytes) -> None:
    import test_vita_lua_runtime as lua_runtime  # noqa: E402

    manifest = lua_runtime.records()
    source = lua_runtime.discover_source(lua_source)
    lua_sources = lua_runtime.validate_source(source, manifest)
    vcvars = lua_runtime.find_vcvars()

    temporary = None
    if keep_build is None:
        temporary = tempfile.TemporaryDirectory(prefix="isaac-eid-startup-")
        build = Path(temporary.name)
    else:
        build = keep_build.resolve()
        build.mkdir(parents=True, exist_ok=True)

    quote = lua_runtime.quote
    lua_compile = " ".join(
        ["cl", "/nologo", "/std:c11", "/O2", "/W3", "/MT",
         "/D_CRT_SECURE_NO_WARNINGS", f"/I{quote(source / 'src')}", "/c"] +
        [quote(path) for path in lua_sources]
    )
    runner_compile = " ".join([
        "cl", "/nologo", "/std:c11", "/O2", "/W4", "/WX", "/MT",
        f"/I{quote(source / 'src')}", "/c", quote(RUNNER),
    ])
    objects = [f"{path.stem}.obj" for path in lua_sources] + ["eid_oracle_main.obj"]
    executable = build / "eid-startup-oracle.exe"
    link = " ".join(["link", "/nologo", f"/out:{quote(executable)}", *objects])
    lua_runtime.run_cmd(vcvars, [lua_compile, runner_compile, link], build)

    outputs = []
    transcripts = []
    for index in range(2):
        transcript = build / f"eid-startup-{index}.tsv"
        completed = subprocess.run(
            [str(executable), str(HARNESS), str(eid), str(core), str(transcript)],
            cwd=build, text=True, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, check=False,
        )
        require(completed.returncode == 0,
                f"32-bit EID startup failed ({completed.returncode}):\n"
                f"{completed.stdout}{completed.stderr}")
        require(completed.stderr == "", "32-bit EID startup wrote stderr")
        outputs.append(completed.stdout.replace("\r\n", "\n"))
        transcripts.append(transcript.read_bytes())
        verify_transcript(transcript, pe_bytes)
    require(outputs == ["External Item Descriptions v5.23_bc0551a loaded.\n"] * 2,
            "EID top-level stdout changed")
    require(transcripts[0] == transcripts[1],
            "EID startup transcript is nondeterministic")
    if temporary is not None:
        temporary.cleanup()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pe", required=True, type=Path)
    parser.add_argument("--eid", required=True, type=Path)
    parser.add_argument("--core-scripts", required=True, type=Path)
    parser.add_argument("--lua-source", type=Path)
    parser.add_argument("--keep-build", type=Path)
    arguments = parser.parse_args()

    pe = arguments.pe.resolve()
    eid = arguments.eid.resolve()
    core = arguments.core_scripts.resolve()
    validate_inputs(pe, eid, core)
    _names, pe_bytes = static_registration_oracle(pe)
    build_and_run(arguments.lua_source, eid, core, arguments.keep_build, pe_bytes)
    print("PASS: EID top-level + 102 resolved requires + 35 core callback registrations")
    print("NEXT: native engine _RunCallback -> first EID callback on Vita")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
