#!/usr/bin/env python3
"""Freeze the exact J835 logger/fflush boundary used by Vita batching."""

from __future__ import annotations

import argparse
import hashlib
from pathlib import Path

import pefile

from gpr_locals import legacy_text


PE_SIZE = 8_650_240
PE_SHA256 = "31846486979cfa07c8c968221553052c3ff603518681ca11912d445f96ca9404"
LOGGER_RVA = 0x0055E330
LOGGER_SIZE = 0x21C
LOGGER_SHA256 = "d961a84adaabf4eadc61087b1bb8aea2d2f9787130991628e93f5334630a3d50"
GATE_RVA = 0x0055E4E6
GATE_SIZE = 0x14
GATE_SHA256 = "54eb8cf1fc20c070523420fc2a7b55fa861c321b7310ccc8d5dca5e55969d86a"
ADAPTER_RVA = 0x005965C0
ADAPTER = bytes.fromhex("ff710cff150066a00059c3")
ADAPTER_SHA256 = "1a71c6efca6447846b7f13da90a2555d0fe9121d9b969dfb5adc8a9128c19b6b"
FFLUSH_CALL = bytes.fromhex("ff150066a000")
FFLUSH_SITES = (0x003FCAEA, 0x005965C3)


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--pe", type=Path, required=True)
    parser.add_argument(
        "--root", type=Path,
        default=Path(__file__).resolve().parents[1],
    )
    parser.add_argument("--generated-dir", type=Path)
    args = parser.parse_args()
    payload = args.pe.read_bytes()
    require(len(payload) == PE_SIZE, "frozen PE size changed")
    require(sha256(payload) == PE_SHA256, "frozen PE identity changed")
    pe = pefile.PE(data=payload, fast_load=True)
    require(pe.OPTIONAL_HEADER.ImageBase == 0x00400000,
            "frozen PE image base changed")

    logger = pe.get_data(LOGGER_RVA, LOGGER_SIZE)
    gate = pe.get_data(GATE_RVA, GATE_SIZE)
    adapter = pe.get_data(ADAPTER_RVA, len(ADAPTER))
    require(len(logger) == LOGGER_SIZE and sha256(logger) == LOGGER_SHA256,
            "logger body changed")
    require(len(gate) == GATE_SIZE and sha256(gate) == GATE_SHA256,
            "logger flush gate changed")
    require(adapter == ADAPTER and sha256(adapter) == ADAPTER_SHA256,
            "fflush adapter changed")
    require(payload.count(FFLUSH_CALL) == len(FFLUSH_SITES),
            "fflush import-call census changed")
    for rva in FFLUSH_SITES:
        require(pe.get_data(rva, len(FFLUSH_CALL)) == FFLUSH_CALL,
                f"fflush callsite changed at {rva:08x}")

    root = args.root.resolve()
    crt = (root / "recomp/runtime/host_vita_crt.c").read_text("utf-8")
    header = (root / "recomp/runtime/host_vita_crt.h").read_text("utf-8")
    cmake = (root / "recomp/vita/CMakeLists.txt").read_text("utf-8")
    wrapper = (root / "tools/build_vita.py").read_text("utf-8")
    gate_source = (root / "recomp/vita/vita_raw_allocator_gate.py").read_text(
        "utf-8"
    )
    require(header.count("#define ISAAC_VITA_CRT_LOG_BATCH_SIZE      8U") == 1,
            "bounded batch size is not frozen at eight")
    for marker in (
        "guest_stack_contains(c, c->esp, 12U)",
        "ld32(c->esp + 8U)",
        "guest_stack_contains(c, severity_address, 4U)",
        "ld32(severity_address) & 3U",
        "s_log_batch.sticky_fail_open = 1U",
        "VITA_CRT_LOG_BATCH_DEFER",
    ):
        require(marker in crt, f"production logger gate lost marker: {marker}")
    require(cmake.count("option(ISAAC_VITA_GAME_LOG_BATCH") == 1,
            "CMake game-log option is not unique")
    require("host_vita_crt.c\"\n      APPEND PROPERTY COMPILE_DEFINITIONS ISAAC_VITA_GAME_LOG_BATCH=1" in cmake,
            "CMake lost source-scoped CRT definition")
    require("-DISAAC_VITA_GAME_LOG_BATCH={on_off(game_log_batch)}" in wrapper,
            "canonical build wrapper does not pin the feature")
    require('GAME_LOG_BATCH_CACHE_KEY = "ISAAC_VITA_GAME_LOG_BATCH"' in gate_source,
            "allocator closure does not fail-close the feature")

    if args.generated_dir is not None:
        units = list(args.generated_dir.glob("guest_[0-9][0-9][0-9][0-9].c"))
        texts = [(path, legacy_text(path.read_text("utf-8")))
                 for path in units]

        def exact_body(signature: str) -> str:
            matches = [(path, text, text.find(signature))
                       for path, text in texts if signature in text]
            require(len(matches) == 1,
                    f"generated ownership changed for {signature}")
            _, text, begin = matches[0]
            end = text.find("\nvoid sub_", begin + len(signature))
            return text[begin:end if end >= 0 else len(text)]

        logger_body = exact_body("void sub_0055e330(CPU *__restrict c)")
        adapter_body = exact_body("void sub_005965c0(CPU *__restrict c)")
        require(logger_body.count("gpush_generated(c, 0x55e4faU)") == 1,
                "generated logger return word is not exact raw RVA")
        require(adapter_body.count("gpush_generated(c, 0x5965c9U)") == 1,
                "generated fflush return word is not exact raw RVA")
        require("c->ebp = (uint32_t)(c->esp);" in logger_body,
                "generated logger lost the EBP frame used by the boundary")

    print(
        "Vita exact game-log INFO batch frozen contract: PASS "
        "(logger/gate/adapter, two fflush sites, N=8, source-scoped closure"
        + (", generated raw-RVA stack" if args.generated_dir else "") + ")"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
