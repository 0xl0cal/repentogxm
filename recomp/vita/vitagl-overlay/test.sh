#!/usr/bin/env bash
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
vita_dir=$(CDPATH= cd -- "$script_dir/.." && pwd)
runtime_dir=$(CDPATH= cd -- "$vita_dir/../runtime" && pwd)
source_commit=f4b23b61c84e8ba59de542832f8e507b3660f994
draw_first_fix_commit=a136dd9ff73534a53bb45dcbeceb7c5ffafdb038

if [ "$#" -ne 1 ]; then
    echo "usage: $0 OUTPUT_ROOT" >&2
    exit 2
fi
if [ -z "${VITASDK:-}" ]; then
    echo "VITASDK must name the softfp VitaSDK" >&2
    exit 2
fi

case "$1" in
    /*) output_root=$1 ;;
    *) output_root=$(pwd)/$1 ;;
esac
library="$output_root/prefix/lib/libvitaGL.a"
header="$output_root/prefix/include/vitaGL.h"
proof="$output_root/proof"
mkdir -p "$proof"

cc="$VITASDK/bin/arm-vita-eabi-gcc"
ar="$VITASDK/bin/arm-vita-eabi-ar"
nm="$VITASDK/bin/arm-vita-eabi-nm"
readelf="$VITASDK/bin/arm-vita-eabi-readelf"
objdump="$VITASDK/bin/arm-vita-eabi-objdump"
strings="$VITASDK/bin/arm-vita-eabi-strings"
ld="$VITASDK/bin/arm-vita-eabi-ld"
common_flags='-std=gnu11 -O2 -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=softfp -mthumb -ffunction-sections -fdata-sections -Wall -Wextra -Werror'
host_cc=${CC:-cc}
python_cmd=${PYTHON:-python3}

known_color_cfg_verifier="$proof/verify-known-color-cfg.py"
cat > "$known_color_cfg_verifier" <<'PY'
import pathlib
import re
import sys


INSTRUCTION = re.compile(
    r"^\s*([0-9a-f]+):\s+"
    r"((?:[0-9a-f]{4,8})(?:\s+[0-9a-f]{4,8})*)\s+"
    r"([.a-z][.a-z0-9]*)(?:\s+(.*?))?\s*$",
    re.IGNORECASE,
)
FUNCTION = re.compile(r"(?m)^\s*[0-9a-f]+ <([^>]+)>:\s*$")
CONDITIONAL_BRANCH = re.compile(
    r"^b(?:eq|ne|cs|cc|hs|lo|mi|pl|vs|vc|hi|ls|ge|lt|gt|le)"
    r"(?:\.[nw])?$",
    re.IGNORECASE,
)
UNCONDITIONAL_BRANCHES = {"b", "b.n", "b.w"}
UNCONDITIONAL_CALLS = {"bl", "bl.w", "blx", "blx.w"}
CALL = re.compile(
    r"^blx?(?:(?:eq|ne|cs|cc|hs|lo|mi|pl|vs|vc|hi|ls|ge|lt|gt|le))?"
    r"(?:\.[nw])?$",
    re.IGNORECASE,
)
SAFE_B_DATA_INSTRUCTIONS = {
    "bfc", "bfc.w", "bfi", "bfi.w", "bic", "bic.w", "bics", "bics.w",
}
TRAP_INSTRUCTIONS = {"bkpt", "hvc", "smc", "svc", "udf", "udf.w"}


class VerificationError(RuntimeError):
    pass


def function_text(text: str) -> str:
    headers = list(FUNCTION.finditer(text))
    matches = [index for index, match in enumerate(headers)
               if match.group(1) == "vglSwapBuffers"]
    if len(matches) != 1:
        raise VerificationError(
            f"expected one vglSwapBuffers body, found {len(matches)}"
        )
    index = matches[0]
    end = headers[index + 1].start() if index + 1 < len(headers) else len(text)
    return text[headers[index].end():end]


def parse_instructions(text: str):
    instructions = {}
    for line in function_text(text).splitlines():
        match = INSTRUCTION.match(line)
        if match is None:
            continue
        address = int(match.group(1), 16)
        if address in instructions:
            raise VerificationError(f"duplicate instruction address 0x{address:x}")
        instructions[address] = (
            match.group(3).lower(), (match.group(4) or "").strip()
        )
    if not instructions:
        raise VerificationError("no vglSwapBuffers instructions parsed")
    ordered = sorted(instructions)
    following = {
        address: ordered[index + 1] if index + 1 < len(ordered) else None
        for index, address in enumerate(ordered)
    }
    return instructions, following


def target_name(operands: str):
    match = re.search(r"<([^>]+)>", operands)
    if match is None:
        return None
    return match.group(1).split("+", 1)[0]


def canonical_call(name):
    if name is None:
        return None
    known = (
        "sceGxmFinish",
        "sceGxmTransferFill",
        "sceGxmTransferFinish",
        "sceGxmDisplayQueueAddEntry",
    )
    for symbol in known:
        if name == symbol or re.fullmatch(
                r"_+" + re.escape(symbol) + r"_(?:from_thumb|veneer)",
                name):
            return symbol
    return name


def is_call(mnemonic: str) -> bool:
    return CALL.fullmatch(mnemonic) is not None


def is_return(mnemonic: str, operands: str) -> bool:
    compact = operands.replace(" ", "").lower()
    return (
        (mnemonic in {"bx", "bx.n", "bx.w"} and compact == "lr")
        or (mnemonic.startswith("pop") and "pc" in compact)
        or (mnemonic.startswith("ldm") and "pc" in compact)
        or (mnemonic.startswith("mov") and compact.startswith("pc,"))
    )


def branch_target(operands: str):
    match = re.search(r"(?:^|,\s*)([0-9a-f]+)\s*<[^>]+>\s*$",
                      operands, re.IGNORECASE)
    return int(match.group(1), 16) if match else None


def walk_clean(instructions, following, start, target, allow_jump, label):
    address = start
    visited = set()
    jump_count = 0
    while address != target:
        if address is None or address not in instructions:
            raise VerificationError(f"{label}: path left vglSwapBuffers")
        if address in visited:
            raise VerificationError(f"{label}: path looped before target")
        visited.add(address)
        mnemonic, operands = instructions[address]
        if is_call(mnemonic):
            name = canonical_call(target_name(operands))
            raise VerificationError(
                f"{label}: call {name or operands} bypasses target at 0x{address:x}"
            )
        if is_return(mnemonic, operands):
            raise VerificationError(f"{label}: returned before target")
        if CONDITIONAL_BRANCH.fullmatch(mnemonic) or mnemonic in {"cbz", "cbnz"}:
            raise VerificationError(f"{label}: ambiguous conditional path")
        if mnemonic in UNCONDITIONAL_BRANCHES:
            if not allow_jump:
                raise VerificationError(f"{label}: unexpected control transfer")
            destination = branch_target(operands)
            if destination is None or destination not in instructions:
                raise VerificationError(f"{label}: branch target is outside function")
            address = destination
            jump_count += 1
            continue
        first_operand = operands.split(",", 1)[0].strip().lower()
        if first_operand == "pc":
            raise VerificationError(f"{label}: instruction writes pc")
        if mnemonic.startswith("it"):
            raise VerificationError(f"{label}: conditional IT block")
        if mnemonic in TRAP_INSTRUCTIONS:
            raise VerificationError(f"{label}: trap before target")
        if mnemonic in {"bx", "bx.n", "bx.w", "bxj", "tbb", "tbh",
                        "eret", "ret", "rfe", "rfeia", "rfedb"}:
            raise VerificationError(f"{label}: indirect control transfer")
        if mnemonic.startswith("b") and mnemonic not in SAFE_B_DATA_INSTRUCTIONS:
            raise VerificationError(
                f"{label}: unclassified branch-family instruction {mnemonic}"
            )
        address = following[address]
    return jump_count


def verify(text: str, label: str) -> None:
    instructions, following = parse_instructions(text)
    calls = []
    for address, (mnemonic, operands) in instructions.items():
        if is_call(mnemonic):
            calls.append((address, canonical_call(target_name(operands))))

    def unique(symbol):
        sites = [address for address, name in calls if name == symbol]
        if len(sites) != 1:
            raise VerificationError(
                f"{label}: expected one {symbol} call, found {len(sites)}"
            )
        if instructions[sites[0]][0] not in UNCONDITIONAL_CALLS:
            raise VerificationError(
                f"{label}: conditional critical call to {symbol}"
            )
        return sites[0]

    fill = unique("sceGxmTransferFill")
    transfer_finish = unique("sceGxmTransferFinish")
    queue = unique("sceGxmDisplayQueueAddEntry")
    alternate = sorted({
        name for _, name in calls
        if name is not None and "sceGxmTransfer" in name
        and name not in {"sceGxmTransferFill", "sceGxmTransferFinish"}
    })
    if alternate:
        raise VerificationError(
            f"{label}: alternate transfer calls present: {alternate}"
        )

    context_candidates = []
    for address, name in calls:
        if (name != "sceGxmFinish" or
                instructions[address][0] not in UNCONDITIONAL_CALLS):
            continue
        try:
            walk_clean(
                instructions, following, following[address], fill, False,
                f"{label}: Finish-to-Fill",
            )
        except VerificationError:
            continue
        context_candidates.append(address)
    if len(context_candidates) != 1:
        raise VerificationError(
            f"{label}: expected one clean Finish predecessor, "
            f"found {len(context_candidates)}"
        )

    walk_clean(
        instructions, following, following[fill], transfer_finish, False,
        f"{label}: Fill-to-TransferFinish",
    )
    jumps = walk_clean(
        instructions, following, following[transfer_finish], queue, True,
        f"{label}: TransferFinish-to-Queue",
    )
    direction = "cold-backward" if queue < transfer_finish else "forward"
    print(
        f"known-color {label} CFG order: PASS "
        f"(Finish->Fill->TransferFinish->{direction}-Queue, jumps={jumps})"
    )


def self_test() -> None:
    valid = """
00000000 <vglSwapBuffers>:
   0: e00e       b.n 20 <vglSwapBuffers+0x20>
   4: f000 f800  bl 40 <sceGxmDisplayQueueAddEntry>
   8: 4770       bx lr
   c: 4770       bx lr
  20: f000 f800  bl 44 <sceGxmFinish>
  24: f000 f800  bl 48 <sceGxmTransferFill>
  28: f000 f800  bl 4c <sceGxmTransferFinish>
  2c: e7ea       b.n 4 <vglSwapBuffers+0x4>
"""
    verify(valid, "selftest-valid")

    hostile = {
        "wrong-adjacent-block": valid.replace(
            "b.n 4 <vglSwapBuffers+0x4>",
            "b.n c <vglSwapBuffers+0xc>",
        ),
        "branch-past-queue": valid.replace(
            "b.n 4 <vglSwapBuffers+0x4>",
            "b.n 8 <vglSwapBuffers+0x8>",
        ),
        "fill-queue-bypass": valid.replace(
            "bl 4c <sceGxmTransferFinish>",
            "b.n 4 <vglSwapBuffers+0x4>",
        ),
        "extra-fill": valid.replace(
            "bl 40 <sceGxmDisplayQueueAddEntry>",
            "bl 40 <sceGxmTransferFill>",
        ),
        "extra-finish": valid.replace(
            "bl 40 <sceGxmDisplayQueueAddEntry>",
            "bl 40 <sceGxmTransferFinish>",
        ),
        "pc-write-bypass": """
00000000 <vglSwapBuffers>:
   0: f000 f800  bl 40 <sceGxmFinish>
   4: f000 f800  bl 44 <sceGxmTransferFill>
   8: f000 f800  bl 48 <sceGxmTransferFinish>
   c: f8d0 f000  ldr.w pc, [r0]
  10: f000 f800  bl 4c <sceGxmDisplayQueueAddEntry>
  14: 4770       bx lr
""",
        "it-predication": """
00000000 <vglSwapBuffers>:
   0: f000 f800  bl 40 <sceGxmFinish>
   4: f000 f800  bl 44 <sceGxmTransferFill>
   8: f000 f800  bl 48 <sceGxmTransferFinish>
   c: bf08       it eq
   e: 4600       moveq r0, r0
  10: f000 f800  bl 4c <sceGxmDisplayQueueAddEntry>
  14: 4770       bx lr
""",
        "conditional-queue-call": """
00000000 <vglSwapBuffers>:
   0: f000 f800  bl 40 <sceGxmFinish>
   4: f000 f800  bl 44 <sceGxmTransferFill>
   8: f000 f800  bl 48 <sceGxmTransferFinish>
   c: f000 f800  bleq 4c <sceGxmDisplayQueueAddEntry>
  10: 4770       bx lr
""",
    }
    for name, fixture in hostile.items():
        try:
            verify(fixture, "selftest-" + name)
        except VerificationError:
            continue
        raise SystemExit("hostile known-color CFG fixture passed: " + name)
    print("known-color CFG cold-block hostile self-test: PASS")


if len(sys.argv) == 2 and sys.argv[1] == "--self-test":
    self_test()
elif len(sys.argv) == 3:
    try:
        verify(pathlib.Path(sys.argv[1]).read_text(encoding="utf-8"), sys.argv[2])
    except VerificationError as exc:
        raise SystemExit(str(exc)) from exc
else:
    raise SystemExit(f"usage: {sys.argv[0]} --self-test | DISASSEMBLY LABEL")
PY
"$python_cmd" "$known_color_cfg_verifier" --self-test

if [ ! -s "$library" ] || [ ! -s "$header" ]; then
    echo "missing pinned vitaGL overlay archive/header under $output_root" >&2
    exit 3
fi
"$host_cc" -std=c11 -O2 -Wall -Wextra -Werror \
    "$script_dir/rt_recovery_oracle.c" \
    -o "$proof/rt-recovery-oracle"
"$proof/rt-recovery-oracle"
"$host_cc" -std=c11 -O2 -Wall -Wextra -Werror \
    "$script_dir/display_queue_oracle.c" \
    -o "$proof/display-queue-oracle"
"$proof/display-queue-oracle"
known_color_host_warning_flags=
if "$host_cc" --version 2>/dev/null | grep -qi clang; then
    known_color_host_warning_flags=-Wno-deprecated-non-prototype
elif "$host_cc" --version 2>/dev/null |
        grep -Eqi 'gcc|Free Software Foundation'; then
    # GCC 11 diagnoses an unrelated legacy guest.h fallback while this
    # host-only oracle deliberately compiles the whole backend translation.
    known_color_host_warning_flags=-Wno-maybe-uninitialized
fi
"$host_cc" -std=c11 -O2 -Wall -Wextra -Werror \
    $known_color_host_warning_flags -Wno-unused-function \
    -DISAAC_GL_VITA_BACKEND_ORACLE=1 \
    -DISAAC_GL_VITA_FIRST_FRAME_ORACLE=1 \
    -DISAAC_VITA_DIRECT_DEFAULT=1 \
    -DISAAC_VITA_KNOWN_COLOR_PROBE=1 \
    -include "$vita_dir/gl_vita_first_frame_oracle_config.h" \
    -I"$runtime_dir" -I"$vita_dir" \
    "$runtime_dir/gl_bridge.c" \
    "$runtime_dir/gl_vita_backend.c" \
    "$runtime_dir/guest_stack_legacy_oracle_stub.c" \
    "$runtime_dir/gl_vita_known_color_oracle.c" \
    -o "$proof/known-color-runtime-oracle"
"$proof/known-color-runtime-oracle"
if ! grep -Fq '0x80290002u' "$script_dir/display_queue_oracle.c" ||
   grep -Fq '0x80290004u' "$script_dir/display_queue_oracle.c"; then
    echo "display oracle lost exact SCE_DISPLAY_ERROR_INVALID_ADDR" >&2
    exit 3
fi
for failure_case in 1 2 3 4 5; do
    "$host_cc" -std=c11 -O2 -Wall -Wextra -Werror \
        -DISAAC_KAGE_VITA_BACKEND_ORACLE=1 \
        -DISAAC_VITA_STALL_PROBE=1 \
        -DISAAC_KAGE_DISPLAY_STATUS_FAILURE_CASE="$failure_case" \
        -I"$runtime_dir" \
        "$runtime_dir/kage_vita_backend.c" \
        "$runtime_dir/kage_vita_backend_teardown_oracle.c" \
        -o "$proof/display-status-fail-$failure_case"
    "$proof/display-status-fail-$failure_case"
done
if ISAAC_DISPLAY_QUEUE_PROBE=invalid \
        "$script_dir/build.sh" "$proof/invalid-probe-build" \
        >"$proof/invalid-probe-build.log" 2>&1; then
    echo "vitaGL overlay accepted an invalid display-queue probe mode" >&2
    exit 3
fi
if ISAAC_KNOWN_COLOR_PROBE=invalid \
        "$script_dir/build.sh" "$proof/invalid-known-color-build" \
        >"$proof/invalid-known-color-build.log" 2>&1; then
    echo "vitaGL overlay accepted an invalid known-color probe mode" >&2
    exit 3
fi
if ISAAC_DISPLAY_QUEUE_PROBE=0 ISAAC_KNOWN_COLOR_PROBE=1 \
        "$script_dir/build.sh" "$proof/unpaired-known-color-build" \
        >"$proof/unpaired-known-color-build.log" 2>&1; then
    echo "known-color overlay built without display lineage" >&2
    exit 3
fi
expected=$(sed -n '1{s/[[:space:]].*//;p;}' "$output_root/libvitaGL.sha256")
actual=$(sha256sum "$library" | awk '{print $1}')
if [ -z "$expected" ] || [ "$actual" != "$expected" ]; then
    echo "vitaGL overlay checksum does not match libvitaGL.a" >&2
    exit 3
fi
recipe=$(sed -n '1p' "$output_root/recipe.txt")
source_dir="$output_root/work/source-$recipe/vitaGL-$source_commit"
build_contract="$output_root/build-contract.txt"
probe_mode=$(sed -n 's/^display_queue_probe=//p' "$build_contract")
known_color_mode=$(sed -n 's/^known_color_probe=//p' "$build_contract")
case "$probe_mode" in
    0|1) ;;
    *)
        echo "vitaGL build contract has invalid display-queue probe mode" >&2
        exit 3
        ;;
esac
case "$known_color_mode" in
    0|1) ;;
    *)
        echo "vitaGL build contract has invalid known-color probe mode" >&2
        exit 3
        ;;
esac
if [ "$(grep -c '^display_queue_probe=' "$build_contract")" -ne 1 ] ||
   ! grep -Fq "ISAAC_DISPLAY_QUEUE_PROBE=$probe_mode" "$build_contract"; then
    echo "vitaGL recipe does not bind the display-queue probe mode" >&2
    exit 3
fi
if [ "$(grep -c '^known_color_probe=' "$build_contract")" -ne 1 ] ||
   ! grep -Fq "ISAAC_KNOWN_COLOR_PROBE=$known_color_mode" \
        "$build_contract" ||
   { [ "$known_color_mode" -eq 1 ] && [ "$probe_mode" -ne 1 ]; }; then
    echo "vitaGL recipe does not bind a valid known-color/lineage pair" >&2
    exit 3
fi
if [ "$known_color_mode" -eq 1 ]; then
    known_color_contract=p2-green-clear-transfer-fill-64-64-512-256-v1
else
    known_color_contract=off
fi
if ! grep -Fxq "known_color_contract=$known_color_contract" \
        "$build_contract"; then
    echo "vitaGL build contract misstates the known-color mode" >&2
    exit 3
fi
for contract_line in \
    'uniform_reserve_guard=fail-closed-v1' \
    'scene_begin_guard=fail-closed-v1' \
    'render_target_recovery=out-of-render-targets-v1' \
    'render_target_early_reserve=1024x1024-scenes8-none-v1' \
    'render_target_scene_capacity=display8-fbo8-v1' \
    'render_target_telemetry=bounded-381-v1' \
    'display_surface_allocation=dedicated-cdram-transaction-v2' \
    'display_surface_init_gate=bounded-100-fail-closed-v1' \
    'display_surface_lifecycle_log=action-fail-closed-v1' \
    'display_surface_resize_release=queue-finish-null-nextframe-wait-retire-v2'; do
    if ! grep -Fxq "$contract_line" "$build_contract"; then
        echo "vitaGL build contract lost: $contract_line" >&2
        exit 3
    fi
done
if [ "$probe_mode" -eq 1 ]; then
    expected_sentinel=16x16-at-16x16-v1
else
    expected_sentinel=off
fi
if ! grep -Fxq "display_queue_cpu_sentinel=$expected_sentinel" \
        "$build_contract"; then
    echo "vitaGL build contract misstates the CPU sentinel mode" >&2
    exit 3
fi
for queue_evidence in \
    'ISAAC_VITAGL_DISPLAY_QUEUE_PROBE_DEFINED' \
    'ISAAC_VITAGL_DISPLAY_QUEUE_SAMPLE_COUNT 8u' \
    'begin_fragment_sync' \
    'mismatch_mask' \
    'back_memblock_uid' \
    'back_get_base_result' \
    'back_map_result' \
    'back_dedicated' \
    'uint32_t sync;' \
    'sizeof(IsaacVitaGlDisplayQueueProbe) == 96u'; do
    if ! grep -Fq "$queue_evidence" "$header"; then
        echo "vitaGL display-queue ABI header proof missing: $queue_evidence" >&2
        exit 3
    fi
done
for known_color_abi in \
    'ISAAC_VITAGL_KNOWN_COLOR_PROBE_DEFINED' \
    'ISAAC_VITAGL_KNOWN_COLOR_PRESENT_INDEX 2u' \
    'ISAAC_VITAGL_KNOWN_COLOR_FILL          0xffff00ffu' \
    'ISAAC_VITAGL_KNOWN_COLOR_FORMAT        0x00060000u' \
    'ISAAC_VITAGL_KNOWN_COLOR_GATE_ALL' \
    'uint32_t gate_mask;' \
    'sizeof(IsaacVitaGlKnownColorProbe) == 88u'; do
    if ! grep -Fq "$known_color_abi" "$header"; then
        echo "vitaGL known-color ABI proof missing: $known_color_abi" >&2
        exit 3
    fi
done
for display_status_evidence in \
    'ISAAC_VITAGL_DISPLAY_SURFACE_STATUS_DEFINED' \
    'ISAAC_VITAGL_DISPLAY_SURFACE_COUNT 3u' \
    'IsaacVitaGlDisplaySurfaceStatus' \
    'color_init_result' \
    'sync_create_result' \
    'sizeof(IsaacVitaGlDisplaySurfaceStatus) == 100u'; do
    if ! grep -Fq "$display_status_evidence" "$header"; then
        echo "vitaGL display-surface status header proof missing: $display_status_evidence" >&2
        exit 3
    fi
done
for queue_evidence in \
    'isaac_vitagl_next_display_queue_sequence' \
    'IsaacVitaGlPendingDisplayLineage' \
    'isaac_vitagl_note_display_begin' \
    'isaac_vitagl_note_display_end' \
    'sceGxmColorSurfaceGetData' \
    'queue_result = sceGxmDisplayQueueAddEntry' \
    'display_result = sceDisplaySetFrameBuf' \
    'isaac_vitagl_probe_display_queue_add' \
    'isaac_vitagl_prepare_display_queue_callback' \
    '{ 480, 271 }'; do
    if ! grep -Fq "$queue_evidence" "$source_dir/source/gxm.c"; then
        echo "vitaGL display-queue source proof missing: $queue_evidence" >&2
        exit 3
    fi
done
for known_color_source in \
    'isaac_vita_vitagl_known_color_present_index' \
    'isaac_vita_vitagl_known_color_probe' \
    'isaac_vitagl_known_color_gate' \
    'lineage->begin_count == 1' \
    'lineage->end_count == 1' \
    'lineage->begin_result == 0' \
    'lineage->end_result == 0' \
    'lineage->begin_color_data ==' \
    'lineage->mismatch_mask == 0' \
    'SCE_GXM_TRANSFER_FORMAT_U8U8U8U8_ABGR' \
    'Isaac known-color transfer format drifted' \
    'DISPLAY_STRIDE * 4, NULL, 0, NULL' \
    'sceGxmTransferFinish();'; do
    if ! grep -Fq "$known_color_source" "$source_dir/source/gxm.c"; then
        echo "vitaGL known-color source proof missing: $known_color_source" >&2
        exit 3
    fi
done
if grep -Fq 'lineage->begin_data' "$source_dir/source/gxm.c"; then
    echo "vitaGL known-color gate references a nonexistent lineage field" >&2
    exit 3
fi
known_swap="$proof/known-color-vglSwap.source"
sed -n '/void vglSwapBuffers(/,/needs_scene_reset = GL_TRUE;/p' \
    "$source_dir/source/gxm.c" > "$known_swap"
known_scene_end=$(grep -n 'scene_end();' "$known_swap" | head -1 | cut -d: -f1)
known_finish=$(grep -n 'sceGxmFinish(gxm_context);' "$known_swap" | head -1 | cut -d: -f1)
known_fill=$(grep -n 'sceGxmTransferFill(' "$known_swap" | cut -d: -f1)
known_transfer_finish=$(grep -n 'sceGxmTransferFinish();' \
    "$known_swap" | cut -d: -f1)
known_queue=$(grep -n 'queue_result = sceGxmDisplayQueueAddEntry' \
    "$known_swap" | cut -d: -f1)
known_shared=$(grep -n 'sceSharedFbEnd(shared_fb);' \
    "$known_swap" | cut -d: -f1)
known_event=$(grep -n 'isaac_vita_vitagl_known_color_probe' \
    "$known_swap" | tail -1 | cut -d: -f1)
if [ -z "$known_scene_end" ] || [ -z "$known_finish" ] ||
   [ -z "$known_fill" ] || [ -z "$known_transfer_finish" ] ||
   [ -z "$known_queue" ] || [ -z "$known_shared" ] ||
   [ -z "$known_event" ] ||
   [ "$known_scene_end" -ge "$known_finish" ] ||
   [ "$known_finish" -ge "$known_fill" ] ||
   [ "$known_fill" -ge "$known_transfer_finish" ] ||
   [ "$known_transfer_finish" -ge "$known_queue" ] ||
   [ "$known_queue" -ge "$known_event" ] ||
   [ "$known_shared" -ge "$known_event" ] ||
   [ "$(grep -c 'sceGxmTransferFill(' "$known_swap")" -ne 1 ] ||
   [ "$(grep -c 'sceGxmTransferFinish();' "$known_swap")" -ne 1 ] ||
   [ "$(grep -c 'isaac_vita_vitagl_known_color_probe' \
        "$known_swap")" -ne 1 ]; then
    echo "known-color source lost End -> Finish -> Fill -> Finish -> Queue -> event order" >&2
    exit 3
fi
known_system_path="$proof/known-color-system-path.source"
sed -n '/if (system_app_mode) {/,/} else {/p' \
    "$known_swap" > "$known_system_path"
if ! grep -Fq 'sceSharedFbEnd(shared_fb);' "$known_system_path" ||
   grep -Eq 'sceGxm(Finish|Transfer|DisplayQueue)|known_color_gate' \
        "$known_system_path" ||
   ! grep -A1 -F 'known_color_event.queue_result =' "$known_swap" |
        head -2 | grep -Fq 'ISAAC_VITAGL_KNOWN_COLOR_NOT_CALLED' ||
   ! grep -Fq 'ISAAC_VITAGL_KNOWN_COLOR_GATE_TAG' "$known_swap"; then
    echo "known-color system p2 lost TAG-only NOT_CALLED skip semantics" >&2
    exit 3
fi
known_clear="$proof/known-color-clear.source"
sed -n '/static void gl_vita_known_color_clear(/,/^}/p' \
    "$runtime_dir/gl_vita_backend.c" > "$known_clear"
if ! grep -Fq 'glIsEnabled((GLenum)GL_VITA_FF_SCISSOR_TEST)' \
        "$known_clear" ||
   ! grep -Fq 'scissor_enabled || saved_draw != 0' "$known_clear" ||
   ! grep -Fq 'glClearColor(0.0f, 1.0f, 0.0f, 1.0f)' "$known_clear" ||
   grep -Eq 'gl(BindFramebuffer|Disable|Enable|Scissor)\(' "$known_clear"; then
    echo "runtime p2 clear lost no-bind/no-scissor fail-closed contract" >&2
    exit 3
fi
known_query="$proof/known-color-present-query.source"
sed -n '/uint32_t isaac_vita_vitagl_known_color_present_index(/,/^}/p' \
    "$runtime_dir/gl_vita_backend.c" > "$known_query"
if ! grep -Fq 's_first_frame_queue_present_tag' "$known_query" ||
   grep -Eq 'sequence|queue_map|queue_add|queue_callback' "$known_query"; then
    echo "known-color selection is not exclusively the runtime present tag" >&2
    exit 3
fi
for display_surface_evidence in \
    'SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW' \
    'isaac_color_surface_alloc_results' \
    'isaac_color_surface_get_base_results' \
    'isaac_color_surface_map_results' \
    'isaac_color_surface_dedicated' \
    'isaac_color_surface_init_results' \
    'isaac_color_surface_sync_results' \
    'isaac_vita_vitagl_display_surface_status' \
    'isaac_vita_vitagl_display_surface_lifecycle_failure' \
    'isaac_vitagl_release_display_color_surface' \
    'GLboolean may_free = GL_TRUE;' \
    'init-rollback:unmap-scanout' \
    'init-rollback:free-scanout' \
    'action=fail-closed' \
    'status_copy_count > ISAAC_VITAGL_DISPLAY_SURFACE_COUNT' \
    'int32_t queue_finish_result = sceGxmDisplayQueueFinish();' \
    'NULL, SCE_DISPLAY_SETBUF_NEXTFRAME' \
    'int32_t wait_result = sceDisplayWaitSetFrameBuf();' \
    'int32_t destroy_result =' \
    'int32_t unmap_result = sceGxmUnmapMemory(address);' \
    'int32_t free_result = sceKernelFreeMemBlock(uid);' \
    'for (marker_y = 16; marker_y < 32; marker_y++)'; do
    if ! grep -Fq "$display_surface_evidence" \
            "$source_dir/source/gxm.c"; then
        echo "vitaGL dedicated display source proof missing: $display_surface_evidence" >&2
        exit 3
    fi
done
rollback_block="$proof/display-init-rollback.source"
sed -n '/if (!dedicated_ok)/,/^[[:space:]]*} else {/p' \
    "$source_dir/source/gxm.c" > "$rollback_block"
rollback_unmap_line=$(grep -n 'unmap_result = sceGxmUnmapMemory' \
    "$rollback_block" | cut -d: -f1)
rollback_unmap_fail_line=$(grep -n 'if (unmap_result)' \
    "$rollback_block" | cut -d: -f1)
rollback_no_free_line=$(grep -n 'may_free = GL_FALSE' \
    "$rollback_block" | cut -d: -f1)
rollback_free_gate_line=$(grep -n 'if (may_free)' \
    "$rollback_block" | cut -d: -f1)
rollback_free_line=$(grep -n 'free_result = sceKernelFreeMemBlock' \
    "$rollback_block" | cut -d: -f1)
rollback_retire_line=$(grep -n 'gxm_color_surface_memblocks\[i\] = -1' \
    "$rollback_block" | cut -d: -f1)
if [ -z "$rollback_unmap_line" ] || [ -z "$rollback_unmap_fail_line" ] ||
   [ -z "$rollback_no_free_line" ] || [ -z "$rollback_free_gate_line" ] ||
   [ -z "$rollback_free_line" ] || [ -z "$rollback_retire_line" ] ||
   [ "$rollback_unmap_line" -ge "$rollback_unmap_fail_line" ] ||
   [ "$rollback_unmap_fail_line" -ge "$rollback_no_free_line" ] ||
   [ "$rollback_no_free_line" -ge "$rollback_free_gate_line" ] ||
   [ "$rollback_free_gate_line" -ge "$rollback_free_line" ] ||
   [ "$rollback_free_line" -ge "$rollback_retire_line" ] ||
   [ "$(grep -c 'sceKernelFreeMemBlock' "$rollback_block")" -ne 1 ]; then
    echo "display init rollback lost unmap-failure -> no-free -> retire order" >&2
    exit 3
fi
resize_block="$proof/display-resize-release.source"
sed -n '/Perform resolution change/,/Starting garbage collector/p' \
    "$source_dir/source/gxm.c" > "$resize_block"
queue_finish_line=$(grep -n 'queue_finish_result = sceGxmDisplayQueueFinish' \
    "$resize_block" | cut -d: -f1)
gxm_finish_line=$(grep -n 'sceGxmFinish(gxm_context)' \
    "$resize_block" | cut -d: -f1)
detach_line=$(grep -n 'detach_result = sceDisplaySetFrameBuf' \
    "$resize_block" | cut -d: -f1)
wait_detach_line=$(grep -n 'wait_result = sceDisplayWaitSetFrameBuf' \
    "$resize_block" | cut -d: -f1)
destroy_line=$(grep -n 'sceGxmDestroyRenderTarget' \
    "$resize_block" | cut -d: -f1)
release_line=$(grep -n 'isaac_vitagl_release_display_color_surface' \
    "$resize_block" | cut -d: -f1)
if [ -z "$queue_finish_line" ] || [ -z "$gxm_finish_line" ] ||
   [ -z "$detach_line" ] || [ -z "$wait_detach_line" ] ||
   [ -z "$destroy_line" ] || [ -z "$release_line" ] ||
   [ "$queue_finish_line" -ge "$gxm_finish_line" ] ||
   [ "$gxm_finish_line" -ge "$detach_line" ] ||
   [ "$detach_line" -ge "$wait_detach_line" ] ||
   [ "$wait_detach_line" -ge "$destroy_line" ] ||
   [ "$destroy_line" -ge "$release_line" ]; then
    echo "display resize lost queue -> finish -> detach -> wait -> free order" >&2
    exit 3
fi
sample_line=$(grep -n 'uint32_t sample = pixels' \
    "$source_dir/source/gxm.c" | tail -1 | cut -d: -f1)
marker_line=$(grep -n 'for (marker_y = 16' \
    "$source_dir/source/gxm.c" | tail -1 | cut -d: -f1)
set_source_line=$(grep -n 'display_result = sceDisplaySetFrameBuf' \
    "$source_dir/source/gxm.c" | tail -1 | cut -d: -f1)
if [ -z "$sample_line" ] || [ -z "$marker_line" ] ||
   [ -z "$set_source_line" ] || [ "$sample_line" -ge "$marker_line" ] ||
   [ "$marker_line" -ge "$set_source_line" ]; then
    echo "display callback lost sample -> sentinel -> SetFrameBuf order" >&2
    exit 3
fi
if ! grep -Fq 'struct display_queue_callback_data queue_cb_data = { 0 };' \
        "$source_dir/source/splashscreen.c"; then
    echo "vitaGL splash callback data lost its zero sentinel" >&2
    exit 3
fi
for render_target_evidence in \
    'SCE_GXM_ERROR_OUT_OF_RENDER_TARGETS' \
    'isaac_vitagl_reserve_render_target_once' \
    'ISAAC_RESERVED_RT_WIDTH 1024' \
    'ISAAC_RESERVED_RT_SCENES 8' \
    'init:reserved-rt:late-fallback' \
    'scene_reset:framebuffer:reserved' \
    'isaac_reserved_render_target = NULL;' \
    'msaa_mode == SCE_GXM_MULTISAMPLE_NONE' \
    'SceGxmRenderTarget *target = NULL;' \
    'if (!pending_total)' \
    'isaac_vitagl_has_duplicate_pending_render_target()' \
    'event->finish_called = 1;' \
    'if (event->destroy_result)' \
    'event->retry_attempted = 1;' \
    '_Static_assert(FRAME_PURGE_FREQ == 4' \
    'isaac_vitagl_rearm_scene_after_failure'; do
    if ! grep -Fq "$render_target_evidence" "$source_dir/source/gxm.c"; then
        echo "vitaGL render-target recovery source proof missing: $render_target_evidence" >&2
        exit 3
    fi
done
if [ "$(grep -Fc 'setup_render_target(&target, w, h, MAX_SCENES_PER_FRAME);' \
        "$source_dir/source/gxm.c")" -ne 2 ]; then
    echo "vitaGL FBO render targets lost the eight-scene create/retry contract" >&2
    exit 3
fi
for release_evidence in \
    'fb->target = NULL;' \
    'fb->depthbuffer_ptr = NULL;'; do
    if ! grep -Fq "$release_evidence" \
            "$source_dir/source/framebuffers.c"; then
        echo "vitaGL idempotent framebuffer release proof missing: $release_evidence" >&2
        exit 3
    fi
done
for draw_fix_evidence in \
    'ptrs[0] = (void *)target_vbo->ptr + first * streams[0].stride;' \
    'ptrs[0] = (void *)target_vbo->ptr + lowest * streams[0].stride;' \
    'gpu_alloc_mapped_temp((highest - lowest) * streams[0].stride)'; do
    if ! grep -Fq "$draw_fix_evidence" \
            "$source_dir/source/custom_shaders.c"; then
        echo "vitaGL pin lost packed-VBO first/range fix $draw_first_fix_commit" >&2
        exit 3
    fi
done
for scene_evidence in \
    '"scene_reset:display"' \
    '"scene_reset:framebuffer"'; do
    if ! grep -Fq "$scene_evidence" "$source_dir/source/gxm.c"; then
        echo "vitaGL scene-begin failure source proof missing: $scene_evidence" >&2
        exit 3
    fi
done
for reserve_evidence in \
    '"update_scissor_test:mask"' \
    '"update_scissor_test:restore"'; do
    if ! grep -Fq "$reserve_evidence" "$source_dir/source/tests.c"; then
        echo "vitaGL reserve-failure source proof missing: $reserve_evidence" >&2
        exit 3
    fi
done
for reserve_evidence in '"glClear:vertex"' '"glClear:fragment"'; do
    if ! grep -Fq "$reserve_evidence" "$source_dir/source/misc.c"; then
        echo "vitaGL clear reserve-failure source proof missing: $reserve_evidence" >&2
        exit 3
    fi
done
non_splash_reserves=$(grep -R -h \
    'sceGxmReserve.*DefaultUniformBuffer' "$source_dir/source" \
    --include='*.c' --exclude='splashscreen.c' | wc -l)
if [ "$non_splash_reserves" -ne 4 ]; then
    echo "vitaGL compiled non-splash reserve census drifted: $non_splash_reserves" >&2
    exit 3
fi

"$ar" p "$library" gxm.o > "$proof/gxm.o"
"$ar" p "$library" splashscreen.o > "$proof/splashscreen.o"
"$readelf" -h "$proof/gxm.o" > "$proof/gxm.header"
"$readelf" -A "$proof/gxm.o" > "$proof/gxm.attributes"
"$nm" "$library" > "$proof/libvitaGL.symbols"
"$nm" -u "$proof/gxm.o" > "$proof/gxm.undefined"
"$objdump" -dr --disassemble=init_gxm "$proof/gxm.o" > \
    "$proof/init_gxm.disassembly"
"$objdump" -dr --disassemble=vglSwapBuffers "$proof/gxm.o" > \
    "$proof/vglSwapBuffers.disassembly"
callback_start=$("$nm" -n "$proof/gxm.o" | awk \
    '$3 == "display_queue_callback" { print "0x" $1; exit }')
callback_end=$("$nm" -n "$proof/gxm.o" | awk \
    '$3 == "display_queue_callback" { found = 1; next }
     found && $2 ~ /^[tT]$/ { print "0x" $1; exit }')
if [ -z "$callback_start" ] || [ -z "$callback_end" ]; then
    echo "could not bound display_queue_callback in gxm.o" >&2
    exit 4
fi
# --disassemble=symbol still emits relocations from earlier functions in this
# toolchain.  An explicit address range keeps the no-helper-call proof honest.
"$objdump" -dr --start-address="$callback_start" \
    --stop-address="$callback_end" "$proof/gxm.o" > \
    "$proof/display_queue_callback.disassembly"
awk '/<display_queue_callback>:/ { body = 1 } body { print }' \
    "$proof/display_queue_callback.disassembly" > \
    "$proof/display_queue_callback.body"
swap_start=$("$nm" -n "$proof/gxm.o" | awk \
    '$3 == "vglSwapBuffers" { print "0x" $1; exit }')
swap_end=$("$nm" -n "$proof/gxm.o" | awk \
    '$3 == "vglSwapBuffers" { found = 1; next }
     found && $2 ~ /^[tT]$/ { print "0x" $1; exit }')
if [ -z "$swap_start" ] || [ -z "$swap_end" ]; then
    echo "could not bound vglSwapBuffers in gxm.o" >&2
    exit 4
fi
"$objdump" -dr --start-address="$swap_start" --stop-address="$swap_end" \
    "$proof/gxm.o" > "$proof/vglSwapBuffers.body"
"$strings" "$proof/gxm.o" > "$proof/gxm.strings"

if ! grep -Fq 'Version5 EABI' "$proof/gxm.header" ||
   grep -q 'Tag_ABI_VFP_args' "$proof/gxm.attributes"; then
    echo "vitaGL overlay is not ARM EABI5 softfp" >&2
    exit 4
fi

if [ "$probe_mode" -eq 1 ]; then
    if ! grep -Eq '[[:space:]][Ww][[:space:]]+isaac_vitagl_display_queue_probe$' \
            "$proof/libvitaGL.symbols"; then
        echo "enabled vitaGL display-queue probe lost its weak endpoint" >&2
        exit 4
    fi
    if grep -Eq 'R_ARM_THM_CALL[[:space:]]+(sceClibMemset|memset)$' \
            "$proof/display_queue_callback.body"; then
        echo "display callback probe introduced a non-inlined memset" >&2
        exit 4
    fi
    if grep -Fq 'isaac_vitagl_prepare_display_queue_callback' \
            "$proof/display_queue_callback.body"; then
        echo "display callback failed to inline its bounded sampler" >&2
        exit 4
    fi
    hook_line=$(grep -n 'isaac_vitagl_display_queue_probe' \
        "$proof/display_queue_callback.body" | tail -1 | cut -d: -f1)
    display_line=$(grep -n 'sceDisplaySetFrameBuf' \
        "$proof/display_queue_callback.body" | tail -1 | cut -d: -f1)
    if [ -z "$hook_line" ] || [ -z "$display_line" ] ||
       [ "$hook_line" -le "$display_line" ]; then
        echo "display callback result capture is not after sceDisplaySetFrameBuf" >&2
        exit 4
    fi
    if [ "$(grep -Ec 'R_ARM_THM_CALL[[:space:]]+sceDisplaySetFrameBuf$' \
            "$proof/display_queue_callback.body")" -ne 1 ]; then
        echo "display callback no longer has exactly one SetFrameBuf call" >&2
        exit 4
    fi
    for forbidden_callback in vglMalloc vgl_free sceClibPrintf; do
        if grep -Fq "$forbidden_callback" \
                "$proof/display_queue_callback.body"; then
            echo "display callback probe contains forbidden call: $forbidden_callback" >&2
            exit 4
        fi
    done
else
    if grep -Eq '[[:space:]][TtWw][[:space:]]+isaac_vitagl_display_queue_probe$' \
            "$proof/libvitaGL.symbols" ||
       grep -Fq 'isaac_vitagl_display_queue_probe' \
            "$proof/display_queue_callback.body"; then
        echo "default-OFF vitaGL archive retained display-queue instrumentation" >&2
        exit 4
    fi
fi

if [ "$known_color_mode" -eq 1 ]; then
    for weak_known_hook in \
        isaac_vita_vitagl_known_color_present_index \
        isaac_vita_vitagl_known_color_probe; do
        if ! grep -Eq "[[:space:]][Ww][[:space:]]+$weak_known_hook$" \
                "$proof/libvitaGL.symbols"; then
            echo "enabled known-color archive lost weak hook: $weak_known_hook" >&2
            exit 4
        fi
    done
    if [ "$(grep -Ec 'R_ARM_THM_CALL[[:space:]]+sceGxmTransferFill$' \
            "$proof/vglSwapBuffers.body")" -ne 1 ] ||
       [ "$(grep -Ec 'R_ARM_THM_CALL[[:space:]]+sceGxmTransferFinish$' \
            "$proof/vglSwapBuffers.body")" -ne 1 ] ||
       [ "$(grep -Ec 'R_ARM_THM_CALL[[:space:]]+sceGxmDisplayQueueAddEntry$' \
            "$proof/vglSwapBuffers.body")" -ne 1 ]; then
        echo "enabled known-color vglSwap lost exact one Fill/TransferFinish/Queue" >&2
        exit 4
    fi
    "$python_cmd" "$known_color_cfg_verifier" \
        "$proof/vglSwapBuffers.body" gxm.o
else
    if grep -Eq 'sceGxmTransfer(Fill|Finish)' "$proof/gxm.undefined" ||
       grep -Eq '[[:space:]][TtWw][[:space:]]+isaac_vita_vitagl_known_color_' \
            "$proof/libvitaGL.symbols" ||
       grep -Eq 'sceGxmTransfer(Fill|Finish)' "$proof/vglSwapBuffers.body"; then
        echo "default-OFF vitaGL archive retained known-color transfer edges" >&2
        exit 4
    fi
fi

for symbol in \
    isaac_vitagl_single_threaded_gc_enabled \
    isaac_vitagl_shared_render_targets_mode \
    isaac_vitagl_softfp_abi_enabled \
    isaac_vitagl_splashscreen_disabled; do
    if ! grep -Eq "[[:space:]][Rr][[:space:]]+$symbol$" \
            "$proof/libvitaGL.symbols"; then
        echo "vitaGL overlay lost build-contract symbol: $symbol" >&2
        exit 5
    fi
done
if ! grep -Eq '[[:space:]][Ww][[:space:]]+isaac_vita_vitagl_reserve_failure$' \
        "$proof/libvitaGL.symbols" ||
   ! grep -Eq '[[:space:]][Tt][[:space:]]+isaac_vitagl_report_uniform_reserve_failure$' \
        "$proof/libvitaGL.symbols" ||
   ! grep -Eq '[[:space:]][Ww][[:space:]]+isaac_vita_vitagl_scene_failure$' \
        "$proof/libvitaGL.symbols" ||
   ! grep -Eq '[[:space:]][Tt][[:space:]]+isaac_vitagl_report_scene_failure$' \
        "$proof/libvitaGL.symbols" ||
   ! grep -Eq '[[:space:]][Ww][[:space:]]+isaac_vita_vitagl_render_target_event$' \
        "$proof/libvitaGL.symbols" ||
   ! grep -Eq '[[:space:]][Tt][[:space:]]+isaac_vitagl_report_render_target_event$' \
        "$proof/libvitaGL.symbols" ||
   ! grep -Eq '[[:space:]][Ww][[:space:]]+isaac_vita_vitagl_display_surface_status$' \
        "$proof/libvitaGL.symbols" ||
   ! grep -Eq '[[:space:]][Ww][[:space:]]+isaac_vita_vitagl_display_surface_lifecycle_failure$' \
        "$proof/libvitaGL.symbols"; then
    echo "vitaGL overlay lost fail-closed GXM failure boundaries" >&2
    exit 5
fi
for symbol in \
    __mark_rt_as_dirty \
    get_free_render_target \
    garbage_collector \
    glCreateShader \
    glCompileShader \
    glGenFramebuffers \
    glGenRenderbuffers \
    vglInit \
    vglSwapBuffers; do
    if ! grep -Eq "[[:space:]][Tt][[:space:]]+$symbol$" \
            "$proof/libvitaGL.symbols"; then
        echo "vitaGL overlay lost required implementation symbol: $symbol" >&2
        exit 5
    fi
done

# In SINGLE_THREADED_GC mode garbage_collector remains a synchronous function,
# called by the render path.  The decisive static distinction is that gxm.o has
# no GC thread/semaphore imports at all.  This catches a missing or misspelled
# Make flag independently of the marker symbols above.
for forbidden in \
    sceKernelCreateThread \
    sceKernelStartThread \
    sceKernelCreateSema \
    sceKernelWaitSema \
    sceKernelSignalSema \
    sceKernelExitDeleteThread; do
    if grep -Eq "[[:space:]]U[[:space:]]+$forbidden$" \
            "$proof/gxm.undefined"; then
        echo "vitaGL gxm.o still imports asynchronous GC primitive: $forbidden" >&2
        exit 6
    fi
done
if grep -Eq '[[:space:]][Tt][[:space:]]+invoke_splashscreen$' \
        "$proof/libvitaGL.symbols"; then
    echo "vitaGL overlay still contains the second-context splash entry point" >&2
    exit 6
fi
if grep -Fq 'Recycling an old rendertarget' "$proof/gxm.strings"; then
    echo "vitaGL overlay accidentally enabled SHARED_RENDERTARGETS=2" >&2
    exit 6
fi
if ! grep -Fq 'action=fail-closed' "$proof/gxm.strings"; then
    echo "vitaGL weak lifecycle hook lost its fail-closed action" >&2
    exit 6
fi

# Compile the production adapter and derive the exact GL surface it consumes.
# Every such symbol must exist in both the overlay and, when installed, the SDK
# archive that the current port previously linked.  This is stronger than a
# hand-maintained feature sample and catches a header/archive drift immediately.
"$cc" $common_flags \
    -DGUEST_IMAGE_BASE=0x98000000u \
    -I"$output_root/prefix/include" -I"$runtime_dir" -I"$vita_dir" \
    -c "$runtime_dir/gl_vita_backend.c" \
    -o "$proof/gl_vita_backend.production.o"
"$nm" -u "$proof/gl_vita_backend.production.o" > \
    "$proof/gl_vita_backend.undefined"
"$nm" "$proof/gl_vita_backend.production.o" > \
    "$proof/gl_vita_backend.symbols"
if [ "$known_color_mode" -eq 1 ]; then
    "$cc" $common_flags \
        -DGUEST_IMAGE_BASE=0x98000000u \
        -DISAAC_VITA_FIRST_FRAME_PROBE=1 \
        -DISAAC_VITA_DIRECT_DEFAULT=1 \
        -DISAAC_VITA_KNOWN_COLOR_PROBE=1 \
        '-DISAAC_VITA_FIRST_FRAME_BUILD_ID="overlay-known-color"' \
        -I"$output_root/prefix/include" -I"$runtime_dir" -I"$vita_dir" \
        -c "$runtime_dir/gl_vita_backend.c" \
        -o "$proof/gl_vita_backend.known-color.o"
    "$nm" "$proof/gl_vita_backend.known-color.o" > \
        "$proof/gl_vita_backend.known-color.symbols"
    "$objdump" -dr \
        -j .text.isaac_vita_vitagl_known_color_probe \
        "$proof/gl_vita_backend.known-color.o" > \
        "$proof/known-color-hook.disassembly"
    for strong_known_hook in \
        isaac_vita_vitagl_known_color_present_index \
        isaac_vita_vitagl_known_color_probe; do
        if ! grep -Eq "[[:space:]][Tt][[:space:]]+$strong_known_hook$" \
                "$proof/gl_vita_backend.known-color.symbols"; then
            echo "known-color runtime lost strong hook: $strong_known_hook" >&2
            exit 7
        fi
    done
    if grep -E 'R_ARM_(THM_)?CALL' "$proof/known-color-hook.disassembly" | \
            grep -Ev '[[:space:]](memcpy|isaac_vita_vitagl_known_color_present_index)$'; then
        echo "known-color bounded-copy hook gained a non-query/non-copy call" >&2
        exit 7
    fi
    for forbidden_known_hook in \
        isaac_vita_log sceClibPrintf malloc calloc realloc free \
        sceGxmFinish sceGxmTransferFill sceGxmTransferFinish; do
        if grep -Fq "$forbidden_known_hook" \
                "$proof/known-color-hook.disassembly"; then
            echo "known-color bounded-copy hook contains forbidden edge: $forbidden_known_hook" >&2
            exit 7
        fi
    done
    "$ld" -r "$proof/gl_vita_backend.known-color.o" "$proof/gxm.o" \
        -o "$proof/gxm-known-color-runtime.combined.o"
    "$nm" "$proof/gxm-known-color-runtime.combined.o" > \
        "$proof/gxm-known-color-runtime.combined.symbols"
    for strong_known_hook in \
        isaac_vita_vitagl_known_color_present_index \
        isaac_vita_vitagl_known_color_probe; do
        if ! grep -Eq "[[:space:]][Tt][[:space:]]+$strong_known_hook$" \
                "$proof/gxm-known-color-runtime.combined.symbols"; then
            echo "runtime strong hook did not override vitaGL weak endpoint: $strong_known_hook" >&2
            exit 7
        fi
    done
fi
for strong_hook in \
    isaac_vita_vitagl_reserve_failure \
    isaac_vita_vitagl_scene_failure \
    isaac_vita_vitagl_render_target_event \
    isaac_vita_vitagl_display_surface_status \
    isaac_vita_vitagl_display_surface_lifecycle_failure; do
    if ! grep -Eq "[[:space:]][Tt][[:space:]]+$strong_hook$" \
            "$proof/gl_vita_backend.symbols"; then
        echo "production adapter lost strong GXM failure hook: $strong_hook" >&2
        exit 7
    fi
done
if ! grep -Eq '[[:space:]][Tt][[:space:]]+gl_vita_backend_get_display_surface_status$' \
        "$proof/gl_vita_backend.symbols"; then
    echo "production adapter lost display-surface fail-closed getter" >&2
    exit 7
fi
"$ld" -r "$proof/gl_vita_backend.production.o" "$proof/gxm.o" \
    -o "$proof/gxm-project-hooks.combined.o"
"$nm" "$proof/gxm-project-hooks.combined.o" > \
    "$proof/gxm-project-hooks.combined.symbols"
for strong_hook in \
    isaac_vita_vitagl_reserve_failure \
    isaac_vita_vitagl_scene_failure \
    isaac_vita_vitagl_render_target_event \
    isaac_vita_vitagl_display_surface_status \
    isaac_vita_vitagl_display_surface_lifecycle_failure; do
    if ! grep -Eq "[[:space:]][Tt][[:space:]]+$strong_hook$" \
            "$proof/gxm-project-hooks.combined.symbols"; then
        echo "strong project hook did not override vitaGL weak endpoint: $strong_hook" >&2
        exit 7
    fi
done
for telemetry_evidence in \
    'KAGE VITA GXM RT %s: profile=gxm-io-v1 site=%.42s' \
    'vitaGL render-target acquire failed' \
    'KAGE VITA DISPLAY SURFACE: profile=dedicated-scanout-v1' \
    'b0=%08x/%08x/%08x/%u/%08x/%08x' \
    'KAGE VITA DISPLAY SURFACE LIFECYCLE FAILURE:' \
    'action=fail-closed'; do
    if ! "$strings" "$proof/gl_vita_backend.production.o" | \
            grep -Fq "$telemetry_evidence"; then
        echo "production adapter lost render-target telemetry: $telemetry_evidence" >&2
        exit 7
    fi
done
awk '$1 == "U" && $2 ~ /^gl[A-Z]/ { print $2 }' \
    "$proof/gl_vita_backend.undefined" | sort -u > \
    "$proof/required-vitaGL.symbols"
if [ ! -s "$proof/required-vitaGL.symbols" ]; then
    echo "production adapter yielded no required GL symbols" >&2
    exit 7
fi
while IFS= read -r symbol; do
    if ! grep -Eq "[[:space:]][TtWw][[:space:]]+$symbol$" \
            "$proof/libvitaGL.symbols"; then
        echo "vitaGL overlay lost production GL symbol: $symbol" >&2
        exit 7
    fi
done < "$proof/required-vitaGL.symbols"

sdk_library="$VITASDK/arm-vita-eabi/lib/libvitaGL.a"
if [ -s "$sdk_library" ]; then
    "$nm" "$sdk_library" > "$proof/sdk-libvitaGL.symbols"
    "$ar" p "$sdk_library" gxm.o > "$proof/sdk-gxm.o"
    "$readelf" -h "$proof/sdk-gxm.o" > "$proof/sdk-gxm.header"
    "$readelf" -A "$proof/sdk-gxm.o" > "$proof/sdk-gxm.attributes"
    if ! grep -Fq 'Version5 EABI' "$proof/sdk-gxm.header" ||
       grep -q 'Tag_ABI_VFP_args' "$proof/sdk-gxm.attributes"; then
        echo "installed vitaGL baseline is not the softfp ABI being preserved" >&2
        exit 7
    fi
    while IFS= read -r symbol; do
        if ! grep -Eq "[[:space:]][TtWw][[:space:]]+$symbol$" \
                "$proof/sdk-libvitaGL.symbols"; then
            echo "required GL symbol was not present in SDK baseline: $symbol" >&2
            exit 7
        fi
    done < "$proof/required-vitaGL.symbols"
fi

"$cc" $common_flags -I"$output_root/prefix/include" \
    -c "$script_dir/link_probe.c" -o "$proof/link_probe.o"
"$readelf" -A "$proof/link_probe.o" > "$proof/link_probe.attributes"
if grep -q 'Tag_ABI_VFP_args' "$proof/link_probe.attributes"; then
    echo "display-queue strong hook probe is not softfp" >&2
    exit 7
fi
"$cc" $common_flags -I"$output_root/prefix/include" \
    "$proof/link_probe.o" \
    -Wl,--gc-sections -Wl,-Map="$proof/overlay-link.map" \
    -o "$proof/overlay-link.elf" \
    "$library" \
    -lvitashark -lSceShaccCgExt -lmathneon -lstdc++ -ltaihen_stub \
    -lSceShaccCg_stub -lSceGxm_stub -lSceDisplay_stub -lSceCtrl_stub \
    -lSceAppMgr_stub -lSceCommonDialog_stub -lSceKernelDmacMgr_stub -lm
"$readelf" -h "$proof/overlay-link.elf" > "$proof/overlay-link.header"
"$readelf" -A "$proof/overlay-link.elf" > "$proof/overlay-link.attributes"
"$nm" -u "$proof/overlay-link.elf" > "$proof/overlay-link.undefined"
if [ -s "$proof/overlay-link.undefined" ] ||
   grep -q 'Tag_ABI_VFP_args' "$proof/overlay-link.attributes"; then
    echo "vitaGL overlay GLSL/FBO softfp probe retained an ABI or symbol failure" >&2
    cat "$proof/overlay-link.undefined" >&2
    exit 7
fi
"$nm" "$proof/overlay-link.elf" > "$proof/overlay-link.symbols"
if [ "$probe_mode" -eq 1 ] &&
   ! grep -Eq '[[:space:]][Tt][[:space:]]+isaac_vitagl_display_queue_probe$' \
        "$proof/overlay-link.symbols"; then
    echo "strong display-queue hook did not override vitaGL weak endpoint" >&2
    exit 7
fi
if [ "$probe_mode" -eq 1 ]; then
    "$objdump" -dr --disassemble=display_queue_callback \
        "$proof/overlay-link.elf" > \
        "$proof/overlay-link.display_queue_callback.disassembly"
    final_display_line=$(grep -n 'sceDisplaySetFrameBuf' \
        "$proof/overlay-link.display_queue_callback.disassembly" | \
        tail -1 | cut -d: -f1)
    final_hook_line=$(grep -n 'isaac_vitagl_display_queue_probe' \
        "$proof/overlay-link.display_queue_callback.disassembly" | \
        tail -1 | cut -d: -f1)
    if [ -z "$final_display_line" ] || [ -z "$final_hook_line" ] ||
       [ "$final_hook_line" -le "$final_display_line" ]; then
        echo "final ELF lost post-SetFrameBuf display-result capture" >&2
        exit 7
    fi
fi
if [ "$known_color_mode" -eq 1 ]; then
    final_swap_start=$("$nm" -n "$proof/overlay-link.elf" | awk \
        '$3 == "vglSwapBuffers" { print "0x" $1; exit }')
    final_swap_end=$("$nm" -n "$proof/overlay-link.elf" | awk \
        '$3 == "vglSwapBuffers" { found = 1; next }
         found && $2 ~ /^[tT]$/ { print "0x" $1; exit }')
    if [ -z "$final_swap_start" ] || [ -z "$final_swap_end" ]; then
        echo "could not bound final linked vglSwapBuffers" >&2
        exit 7
    fi
    "$objdump" -dr --start-address="$final_swap_start" \
        --stop-address="$final_swap_end" "$proof/overlay-link.elf" > \
        "$proof/overlay-link.vglSwapBuffers.disassembly"
    "$python_cmd" "$known_color_cfg_verifier" \
        "$proof/overlay-link.vglSwapBuffers.disassembly" overlay-link.elf
fi

# The option is default-OFF and its configure-time gate names every prerequisite.
for cmake_known_color_evidence in \
    'option(ISAAC_VITA_KNOWN_COLOR_PROBE' \
    'ISAAC_KNOWN_COLOR_PROBE=${ISAAC_VITA_KNOWN_COLOR_OVERLAY}'; do
    if ! grep -Fq "$cmake_known_color_evidence" "$vita_dir/CMakeLists.txt"; then
        echo "CMake known-color gate missing: $cmake_known_color_evidence" >&2
        exit 8
    fi
done
known_cmake_gate="$proof/known-color-cmake-gate.source"
sed -n '/if(ISAAC_VITA_KNOWN_COLOR_PROBE AND/,/^endif()/p' \
    "$vita_dir/CMakeLists.txt" > "$known_cmake_gate"
for known_color_requirement in \
    'NOT ISAAC_VITA_KAGE' \
    'NOT ISAAC_VITA_FIRST_FRAME_PROBE' \
    'NOT ISAAC_VITA_DIRECT_DEFAULT' \
    'NOT ISAAC_VITA_RAW_GXM_PROBE'; do
    if ! grep -Fq "$known_color_requirement" "$known_cmake_gate"; then
        echo "CMake known-color prerequisite missing: $known_color_requirement" >&2
        exit 8
    fi
done
if ! sed -n '/option(ISAAC_VITA_KNOWN_COLOR_PROBE/,+1p' \
        "$vita_dir/CMakeLists.txt" | grep -Fq 'scanout rectangle" OFF)'; then
    echo "CMake known-color option is not default-OFF" >&2
    exit 8
fi
known_invalid_cmake="$proof/package-known-invalid"
if cmake -S "$vita_dir" -B "$known_invalid_cmake" -G Ninja \
        -DISAAC_VITA_SCAFFOLD_ONLY=ON \
        -DISAAC_VITA_KAGE=ON \
        -DISAAC_VITA_KNOWN_COLOR_PROBE=ON \
        -DSOURCE_DATE_EPOCH=1700000000 \
        -DISAAC_VITA_AUDIO=OFF \
        -DISAAC_VITA_VITAGL_OVERLAY_DIR="$output_root" \
        >"$proof/package-known-invalid.log" 2>&1; then
    echo "CMake accepted known-color without its four required modes" >&2
    exit 8
fi
if ! grep -Fq 'ISAAC_VITA_KNOWN_COLOR_PROBE requires translated KAGE' \
        "$proof/package-known-invalid.log"; then
    echo "CMake known-color rejection did not come from its dependency gate" >&2
    exit 8
fi

# Configure only the scaffold and inspect commands; do not build or package it.
package_build="$proof/package-cmake"
cmake -S "$vita_dir" -B "$package_build" -G Ninja \
    -DISAAC_VITA_SCAFFOLD_ONLY=ON \
    -DSOURCE_DATE_EPOCH=1700000000 \
    -DISAAC_VITA_KAGE=ON \
    -DISAAC_VITA_AUDIO=OFF \
    -DISAAC_VITA_VITAGL_OVERLAY_DIR="$output_root"
ninja -C "$package_build" -t commands > "$proof/package.commands"
if ! grep -Fq "$library" "$proof/package.commands"; then
    echo "CMake link command does not select the project-local vitaGL archive" >&2
    exit 8
fi
if ! grep -Fq "$output_root/prefix/include" "$proof/package.commands"; then
    echo "CMake compile command does not select the pinned vitaGL header" >&2
    exit 8
fi
if grep -Eq '(^|[[:space:]])-lvitaGL([[:space:]]|$)' \
        "$proof/package.commands"; then
    echo "CMake command leaked back to VitaSDK's unpinned -lvitaGL" >&2
    exit 8
fi
if grep -Fq -- '--wrap=invoke_splashscreen' "$proof/package.commands"; then
    echo "obsolete splash wrapper survived despite the splash-free archive" >&2
    exit 8
fi

printf '%s\n' \
    'Isaac Vita vitaGL overlay: PASS (known-color p2 tag/gates/order and bounded copy proven; default-off transfer edges absent or enabled calls exact; a136dd9 packed-VBO fix; uniform/scene guards and transactional RT recovery retained; archive weak hooks resolve to project strong hooks; GL surface and ARM EABI5 softfp preserved vs SDK; GC thread imports absent; shared RT mode 1 only; splash/wrapper absent; GLSL/FBO link and CMake archive/header selection proven; ELF is not executed)'
