#!/usr/bin/env bash
set -eu

if [ -z "${VITASDK:-}" ]; then
    echo "VITASDK must name the softfp VitaSDK" >&2
    exit 2
fi

root=${ISAAC_PHASE_TEST_ROOT:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}
work=${ISAAC_PHASE_TEST_OUT:-$(mktemp -d "${TMPDIR:-/tmp}/isaac-phase.XXXXXX")}
mkdir -p "$work"
host_cc=${CC:-cc}
cc="$VITASDK/bin/arm-vita-eabi-gcc"
readelf="$VITASDK/bin/arm-vita-eabi-readelf"
nm="$VITASDK/bin/arm-vita-eabi-nm"
objdump="$VITASDK/bin/arm-vita-eabi-objdump"
sources="
$root/runtime/kage_vita_preloop_phase.c
$root/runtime/kage_vita_preloop_phase_fake_guest.c
$root/runtime/kage_vita_preloop_phase_oracle.c
"
wraps="
sub_0059fa30
sub_00598c80
sub_0050b240
sub_00563880
sub_004ab7a0
sub_005aeb00
sub_005c2fd0
sub_005ce7d0
"
wrap_flags=
for symbol in $wraps; do
    wrap_flags="$wrap_flags -Wl,--wrap=$symbol"
done
flags="-std=gnu11 -O2 -Wall -Wextra -Werror -I$root/runtime"
arm_flags="$flags -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=softfp -mthumb -ffunction-sections -fdata-sections"

# Executing the host image proves that every cross-object caller actually
# entered the wrapper, called its real body, nested correctly, and left it.
# The ARM image independently proves the Vita linker implements the same
# aliases and emits direct BL edges to all eight wrappers.
"$host_cc" $flags $sources $wrap_flags \
    -o "$work/kage-vita-preloop-phase-host-oracle"
"$work/kage-vita-preloop-phase-host-oracle"

"$cc" $arm_flags $sources $wrap_flags \
    -Wl,--gc-sections \
    -Wl,-Map="$work/kage-vita-preloop-phase-softfp-oracle.map" \
    -o "$work/kage-vita-preloop-phase-softfp-oracle.elf"
"$cc" $arm_flags -c "$root/runtime/kage_vita_preloop_phase.c" \
    -o "$work/kage-vita-preloop-phase.production.o"

"$readelf" -h "$work/kage-vita-preloop-phase-softfp-oracle.elf" \
    > "$work/header"
"$readelf" -A "$work/kage-vita-preloop-phase-softfp-oracle.elf" \
    > "$work/attributes"
"$nm" -u "$work/kage-vita-preloop-phase-softfp-oracle.elf" \
    > "$work/oracle.undefined"
"$nm" -u "$work/kage-vita-preloop-phase.production.o" \
    > "$work/production.undefined"
"$nm" "$work/kage-vita-preloop-phase-softfp-oracle.elf" \
    > "$work/oracle.symbols"
"$nm" "$work/kage-vita-preloop-phase.production.o" \
    > "$work/production.symbols"
"$objdump" -dr "$work/kage-vita-preloop-phase-softfp-oracle.elf" \
    > "$work/oracle.disassembly"
"$objdump" -dr "$work/kage-vita-preloop-phase.production.o" \
    > "$work/production.disassembly"
cat "$work/header"
cat "$work/attributes"
cat "$work/production.undefined"

if ! grep -q "soft-float ABI" "$work/header"; then
    echo "phase oracle is not marked soft-float ABI" >&2
    exit 3
fi
if grep -q "Tag_ABI_VFP_args" "$work/attributes"; then
    echo "phase oracle unexpectedly advertises hardfp arguments" >&2
    exit 4
fi
if [ -s "$work/oracle.undefined" ]; then
    cat "$work/oracle.undefined" >&2
    echo "phase oracle has unresolved symbols" >&2
    exit 5
fi
if "$nm" "$work/kage-vita-preloop-phase-softfp-oracle.elf" \
        "$work/kage-vita-preloop-phase.production.o" | \
        grep -Eq '__atomic|libatomic'; then
    echo "32-bit phase snapshot unexpectedly depends on libatomic" >&2
    exit 6
fi
if grep -Eiq '[[:space:]](ldrex|strex|dmb)([[:space:].]|$)' \
        "$work/production.disassembly"; then
    echo "single-writer phase hot path retained exclusives or barriers" >&2
    exit 6
fi

for symbol in $wraps; do
    if ! grep -Eq "[[:space:]]T[[:space:]]+__wrap_$symbol$" \
            "$work/oracle.symbols"; then
        echo "ARM image is missing wrapper definition: $symbol" >&2
        exit 7
    fi
    if ! grep -Eq "[[:space:]]T[[:space:]]+$symbol$" \
            "$work/oracle.symbols"; then
        echo "ARM image is missing real definition: $symbol" >&2
        exit 8
    fi
    if ! grep -Eq "[[:space:]]U[[:space:]]+__real_$symbol$" \
            "$work/production.undefined"; then
        echo "phase owner is missing real relocation: $symbol" >&2
        exit 9
    fi
    if ! grep -Eq "[[:space:]]b(l|\\.w)?[[:space:]].*<__wrap_$symbol>" \
            "$work/oracle.disassembly"; then
        echo "ARM caller did not branch through wrapper: $symbol" >&2
        exit 10
    fi
    if ! grep -Eq "bl.*<$symbol>" \
            "$work/oracle.disassembly"; then
        echo "ARM wrapper did not branch to real body: $symbol" >&2
        exit 11
    fi
done
if grep -Eq "[[:space:]]T[[:space:]]+__wrap_sub_0059b7a0$" \
        "$work/oracle.symbols"; then
    echo "same-TU GLFW error body unexpectedly has a linker wrapper" >&2
    exit 12
fi
if ! grep -Eq "[[:space:]]b(l|\.w)?[[:space:]].*<sub_0059b7a0>" \
        "$work/oracle.disassembly"; then
    echo "same-TU GLFW regression no longer contains the raw inner edge" >&2
    exit 13
fi

# Static production wiring: every option and source edge must stay inside the
# existing diagnostic gate.  OFF therefore contributes no wrapper source,
# wrapper relocation, or --wrap linker option to the executable.
python3 - "$root/vita/CMakeLists.txt" $wraps <<'PY'
import pathlib
import sys

path = pathlib.Path(sys.argv[1])
symbols = sys.argv[2:]
text = path.read_text(encoding="utf-8")
start = text.index("    if(ISAAC_VITA_STALL_PROBE)\n")
end = text.index("\n    endif()", start)
block = text[start:end]
source = '"${ISAAC_RUNTIME}/kage_vita_preloop_phase.c"'
if text.count(source) != 1 or source not in block:
    raise SystemExit("phase source escaped or is absent from STALL_PROBE gate")
for symbol in symbols:
    token = "-Wl,--wrap=" + symbol
    if text.count(token) != 1 or token not in block:
        raise SystemExit("wrapper option escaped or is absent: " + symbol)
stale = "-Wl,--wrap=sub_0059b7a0"
if stale in text:
    raise SystemExit("same-TU GLFW error body must not be a linker-wrap target")
PY

generated_proof=SKIP
if [ -n "${ISAAC_PHASE_GENERATED_ROOT:-}" ]; then
    python3 - "$ISAAC_PHASE_GENERATED_ROOT" <<'PY'
import pathlib
import re
import sys

root = pathlib.Path(sys.argv[1])
# Exact pre-loop direct edges.  Each wrapped definition must live in another
# generated object or GNU ld --wrap is allowed to leave the assembler-resolved
# call alone.  The first edge deliberately wraps the sole parent of the
# same-object GLFW error body.
edges = (
    ("sub_0059fa30", "guest_0167.c", "569899", "guest_0173.c"),
    ("sub_00598c80", "guest_0140.c", "48bce8", "guest_0172.c"),
    ("sub_0050b240", "guest_0140.c", "48bd1a", "guest_0154.c"),
    ("sub_00563880", "guest_0154.c", "50b90f", "guest_0166.c"),
    ("sub_004ab7a0", "guest_0154.c", "50b96c", "guest_0143.c"),
    ("sub_005aeb00", "guest_0173.c", "59c3f8", "guest_0175.c"),
    ("sub_005c2fd0", "guest_0175.c", "5b072f", "guest_0178.c"),
    ("sub_005ce7d0", "guest_0178.c", "5c39ad", "guest_0180.c"),
)
files = list(root.glob("guest_*.c"))
if not files:
    raise SystemExit("generated phase proof found no guest_*.c files")
texts = {path.name: path.read_text(encoding="utf-8") for path in files}
for symbol, caller, return_rva, owner in edges:
    if caller == owner or caller not in texts or owner not in texts:
        raise SystemExit(f"bad/missing generated TU mapping for {symbol}")
    call = re.compile(rf"0x{return_rva}U.*\b{symbol}\(c\);")
    if not any(call.search(line) for line in texts[caller].splitlines()):
        raise SystemExit(f"exact generated callsite missing: {caller} {symbol}")
    marker = f"/* {symbol}  RVA"
    definitions = [name for name, text in texts.items() if marker in text]
    if definitions != [owner]:
        raise SystemExit(
            f"generated owner mismatch for {symbol}: {definitions}, expected {owner}")

# This is the production regression which made wrapping sub_0059b7a0 invalid:
# both it and its caller are assembler-resolved in guest_0173.c.  Prove the raw
# edge still exists, while the new parent seam has exactly one cross-TU caller.
inner_symbol = "sub_0059b7a0"
inner_owner = "guest_0173.c"
inner_call = re.compile(r"0x59fa57U.*\bsub_0059b7a0\(c\);")
inner_calls = [
    name for name, text in texts.items()
    for line in text.splitlines() if inner_call.search(line)
]
inner_definitions = [
    name for name, text in texts.items()
    if f"/* {inner_symbol}  RVA" in text
]
if inner_calls != [inner_owner] or inner_definitions != [inner_owner]:
    raise SystemExit(
        f"same-TU GLFW inner edge changed: calls={inner_calls} "
        f"definitions={inner_definitions}")
parent_call = re.compile(r"0x569899U.*\bsub_0059fa30\(c\);")
parent_calls = [
    name for name, text in texts.items()
    for line in text.splitlines() if parent_call.search(line)
]
if parent_calls != ["guest_0167.c"]:
    raise SystemExit(f"GLFW parent seam is no longer unique/cross-TU: {parent_calls}")
print("Production generated cross-object phase-edge oracle: PASS")
PY
    generated_proof=PASS
fi

final_link_proof=SKIP
if [ -n "${ISAAC_PHASE_FINAL_ELF:-}" ]; then
    if [ ! -f "$ISAAC_PHASE_FINAL_ELF" ]; then
        echo "production final ELF is missing: $ISAAC_PHASE_FINAL_ELF" >&2
        exit 14
    fi
    "$objdump" -d "$ISAAC_PHASE_FINAL_ELF" > "$work/final.disassembly"
    python3 - "$work/final.disassembly" <<'PY'
import pathlib
import re
import sys

text = pathlib.Path(sys.argv[1]).read_text(encoding="utf-8")
edges = (
    ("sub_00569830", "sub_0059fa30"),
    ("sub_0048bc50", "sub_00598c80"),
    ("sub_0048bc50", "sub_0050b240"),
    ("sub_0050b240", "sub_00563880"),
    ("sub_0050b240", "sub_004ab7a0"),
    ("sub_0059c270", "sub_005aeb00"),
    ("sub_005b06f0", "sub_005c2fd0"),
    ("sub_005c38a0", "sub_005ce7d0"),
)
functions = {}
matches = list(re.finditer(r"(?m)^[0-9a-f]+ <([^>]+)>:\n", text))
for index, match in enumerate(matches):
    end = matches[index + 1].start() if index + 1 < len(matches) else len(text)
    functions[match.group(1)] = text[match.end():end]
for caller, target in edges:
    body = functions.get(caller)
    if body is None:
        raise SystemExit(f"production caller symbol missing: {caller}")
    wrapper_edges = (
        f"<__wrap_{target}>",
        f"<____wrap_{target}_veneer>",
    )
    if not any(edge in body for edge in wrapper_edges):
        raise SystemExit(f"production edge bypassed wrapper: {caller} -> {target}")
print("Production final-ELF generated phase-edge interception: PASS")
PY
    final_link_proof=PASS
fi

sha256sum \
    "$root/runtime/kage_vita_preloop_phase.h" \
    "$root/runtime/kage_vita_preloop_phase.c" \
    "$root/runtime/kage_vita_preloop_phase_fake_guest.c" \
    "$root/runtime/kage_vita_preloop_phase_oracle.c" \
    "$root/vita/test_kage_vita_preloop_phase.sh" \
    "$work/kage-vita-preloop-phase-host-oracle" \
    "$work/kage-vita-preloop-phase-softfp-oracle.elf" \
    "$work/kage-vita-preloop-phase.production.o"
echo "Vita pre-loop 8-slot linker-wrap phase probe + lock-free snapshot + host behavior + ARM softfp link + OFF static gate: PASS (ARM ELF was not executed; generated=$generated_proof final_link=$final_link_proof)"
