#!/usr/bin/env bash
set -euo pipefail

if [ -z "${VITASDK:-}" ]; then
    echo "VITASDK must name the softfp VitaSDK" >&2
    exit 2
fi
if [ -z "${REPENTOGXM_PE:-}" ]; then
    echo "REPENTOGXM_PE must name the frozen 8,650,240-byte PE" >&2
    exit 2
fi

root=${ISAAC_STAGE_MEMORY_TEST_ROOT:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}
if [ -n "${ISAAC_STAGE_MEMORY_TEST_OUT:-}" ]; then
    work=$ISAAC_STAGE_MEMORY_TEST_OUT
    mkdir -p "$work"
else
    work=$(mktemp -d "${TMPDIR:-/tmp}/isaac-stage-memory.XXXXXX")
fi
recomp_python=${ISAAC_RECOMP_PYTHON:-python3}
host_gcc=${HOST_GCC:-gcc}
host_clang=${HOST_CLANG:-clang}

for command in "$recomp_python" "$host_gcc" "$host_clang" \
        "$VITASDK/bin/arm-vita-eabi-gcc" \
        "$VITASDK/bin/arm-vita-eabi-nm" \
        "$VITASDK/bin/arm-vita-eabi-objdump" \
        "$VITASDK/bin/arm-vita-eabi-readelf"; do
    if ! command -v "$command" >/dev/null 2>&1; then
        echo "required stage-memory gate tool is absent: $command" >&2
        exit 2
    fi
done
if ! "$recomp_python" -c 'import capstone, pefile' >/dev/null 2>&1; then
    echo "ISAAC_RECOMP_PYTHON must provide capstone and pefile" >&2
    exit 2
fi
if [ "$(realpath "$(command -v "$host_gcc")")" = \
     "$(realpath "$(command -v "$host_clang")")" ]; then
    echo "HOST_GCC and HOST_CLANG must be distinct compilers" >&2
    exit 2
fi

# Do not infer the backend from an executable name: wrappers and renamed
# binaries are common on build hosts.  The compiler's own predefined macros
# must prove that both genuinely distinct backends are exercised.
for backend in gcc clang; do
    case "$backend" in
        gcc) compiler=$host_gcc ;;
        clang) compiler=$host_clang ;;
    esac
    "$compiler" -dM -E -x c /dev/null \
        > "$work/host-$backend.predefined"
    if ! grep -Eq '^#define[[:space:]]+__GNUC__([[:space:]]|$)' \
            "$work/host-$backend.predefined"; then
        echo "$backend gate compiler does not identify a GNU C frontend" >&2
        exit 2
    fi
    if [ "$backend" = gcc ]; then
        if grep -Eq '^#define[[:space:]]+__clang__([[:space:]]|$)' \
                "$work/host-$backend.predefined"; then
            echo "HOST_GCC is Clang, not a genuine GCC backend" >&2
            exit 2
        fi
    elif ! grep -Eq '^#define[[:space:]]+__clang__([[:space:]]|$)' \
            "$work/host-$backend.predefined"; then
        echo "HOST_CLANG is not a genuine Clang backend" >&2
        exit 2
    fi
done

# The selector always translates these three bodies from the frozen PE in
# this invocation.  The emitted TU is therefore fresh, never a cached input.
"$recomp_python" "$root/test_stage_memory_codegen.py" \
    --pe "$REPENTOGXM_PE" --emit-c "$work/stage-memory-fresh.c"

# Pin the bounded guest-lock section's shape.  mallinfo and the synchronous
# logger live outside that lock and are intentionally not called O(1) here.
# The existing cold diagnostic implementation remains available to old users.
"$recomp_python" - "$root" <<'PY'
import ast
import pathlib
import re
import sys

root = pathlib.Path(sys.argv[1])

def c_structure(source, strip_preprocessor=True):
    """Blank comments and string/character literals without moving offsets."""
    result = list(source)
    cursor = 0
    length = len(source)
    while cursor < length:
        if source.startswith("//", cursor):
            end = cursor + 2
            while end < length and source[end] not in "\r\n":
                end += 1
            for index in range(cursor, end):
                result[index] = " "
            cursor = end
            continue
        if source.startswith("/*", cursor):
            end = source.find("*/", cursor + 2)
            if end < 0:
                raise SystemExit("unterminated C block comment")
            end += 2
            for index in range(cursor, end):
                if result[index] not in "\r\n":
                    result[index] = " "
            cursor = end
            continue
        if source[cursor] in ('"', "'"):
            quote = source[cursor]
            end = cursor + 1
            escaped = False
            while end < length:
                character = source[end]
                if escaped:
                    escaped = False
                elif character == "\\":
                    escaped = True
                elif character == quote:
                    end += 1
                    break
                end += 1
            else:
                raise SystemExit("unterminated C string/character literal")
            for index in range(cursor, end):
                if result[index] not in "\r\n":
                    result[index] = " "
            cursor = end
            continue
        cursor += 1
    structure = "".join(result)
    if strip_preprocessor:
        lines = []
        continuation = False
        for line in structure.splitlines(keepends=True):
            directive = continuation or line.lstrip().startswith("#")
            if directive:
                lines.append("".join(
                    character if character in "\r\n" else " "
                    for character in line))
            else:
                lines.append(line)
            continuation = directive and \
                line.rstrip("\r\n").rstrip().endswith("\\")
        structure = "".join(lines)
    return structure

def function_body(source, name):
    structure = c_structure(source)
    match = re.search(r"\b%s\s*\([^;]*?\)\s*\{" % re.escape(name),
                      structure, flags=re.S)
    if not match:
        raise SystemExit("missing function: %s" % name)
    depth = 1
    cursor = match.end()
    while cursor < len(structure) and depth:
        if structure[cursor] == "{":
            depth += 1
        elif structure[cursor] == "}":
            depth -= 1
        cursor += 1
    if depth:
        raise SystemExit("unterminated function: %s" % name)
    return source[match.end():cursor - 1]

def indirect_call_names(body):
    structure = c_structure(body)
    return tuple(re.findall(
        r"\(\s*\*\s*([A-Za-z_]\w*)\s*\)\s*\(", structure))

def helper_calls(body):
    structure = c_structure(body)
    calls = set(re.findall(r"\b([A-Za-z_]\w*)\s*\(", structure))
    calls.difference_update((
        "if", "else", "for", "while", "switch", "sizeof", "return",
        "defined", "_Alignof", "_Static_assert"))
    return calls

def helper_call_sequence(body):
    structure = c_structure(body)
    ignored = {
        "if", "else", "for", "while", "switch", "sizeof", "return",
        "defined", "_Alignof", "_Static_assert",
    }
    return tuple(call for call in
                 re.findall(r"\b([A-Za-z_]\w*)\s*\(", structure)
                 if call not in ignored)

def bounded_chain_problem(body, expected_calls, expected_loops=0):
    structure = c_structure(body)
    indirect = indirect_call_names(body)
    if indirect:
        return "indirect calls are forbidden: %r" % (indirect,)
    loops = re.findall(r"\b(?:for|while)\s*\(", structure)
    if len(loops) != expected_loops:
        return "loop census %d != %d" % (len(loops), expected_loops)
    calls = helper_call_sequence(body)
    if calls != tuple(expected_calls):
        return "call sequence %r != %r" % (calls, tuple(expected_calls))
    return None

def require_bounded_chain(body, name, expected_calls=(), expected_loops=0):
    problem = bounded_chain_problem(body, expected_calls, expected_loops)
    if problem:
        raise SystemExit("%s changed: %s" % (name, problem))

def require_scalar_helper(body, name, allowed_calls=()):
    structure = c_structure(body)
    indirect = indirect_call_names(body)
    if indirect:
        raise SystemExit("%s gained indirect calls: %r" %
                         (name, indirect))
    loops = re.findall(r"\b(?:for|while)\s*\(", structure)
    if loops:
        raise SystemExit("%s gained a loop" % name)
    calls = helper_calls(body)
    if calls != set(allowed_calls):
        raise SystemExit("%s helper-call set changed: %s" %
                         (name, ",".join(sorted(calls))))

def call_contents(source, name, occurrence=0):
    marker = name + "("
    start = -1
    for _ in range(occurrence + 1):
        start = source.find(marker, start + 1)
        if start < 0:
            raise SystemExit("missing call %d: %s" % (occurrence, name))
    open_paren = start + len(name)
    depth = 0
    quote = None
    escaped = False
    for cursor in range(open_paren, len(source)):
        character = source[cursor]
        if quote is not None:
            if escaped:
                escaped = False
            elif character == "\\":
                escaped = True
            elif character == quote:
                quote = None
            continue
        if character in ('"', "'"):
            quote = character
        elif character == "(":
            depth += 1
        elif character == ")":
            depth -= 1
            if depth == 0:
                return source[open_paren + 1:cursor]
    raise SystemExit("unterminated call: %s" % name)

def split_c_arguments(contents):
    arguments = []
    start = 0
    depth = 0
    quote = None
    escaped = False
    for cursor, character in enumerate(contents):
        if quote is not None:
            if escaped:
                escaped = False
            elif character == "\\":
                escaped = True
            elif character == quote:
                quote = None
            continue
        if character in ('"', "'"):
            quote = character
        elif character == "(":
            depth += 1
        elif character == ")":
            depth -= 1
        elif character == "," and depth == 0:
            arguments.append(contents[start:cursor].strip())
            start = cursor + 1
    arguments.append(contents[start:].strip())
    return arguments

def normalized_call_arguments(source, name):
    structure = c_structure(source)
    count = len(re.findall(r"\b%s\s*\(" % re.escape(name), structure))
    return tuple(re.sub(r"\s+", " ", call_contents(
        structure, name, occurrence=index)).strip()
        for index in range(count))

def exact_call_arguments_problem(body, name, expected):
    actual = normalized_call_arguments(body, name)
    if actual != tuple(expected):
        return "%s payload %r != %r" % (name, actual, tuple(expected))
    return None

def require_exact_call_arguments(body, owner, name, expected):
    problem = exact_call_arguments_problem(body, name, expected)
    if problem:
        raise SystemExit("%s changed: %s" % (owner, problem))

def control_parenthesized_contents(body, keyword):
    structure = c_structure(body)
    matches = list(re.finditer(r"\b%s\s*\(" % re.escape(keyword),
                               structure))
    if len(matches) != 1:
        raise ValueError("%s census is %d" % (keyword, len(matches)))
    open_paren = structure.find("(", matches[0].start())
    depth = 1
    cursor = open_paren + 1
    while cursor < len(structure) and depth:
        if structure[cursor] == "(":
            depth += 1
        elif structure[cursor] == ")":
            depth -= 1
        cursor += 1
    if depth:
        raise ValueError("unterminated %s condition" % keyword)
    return structure[open_paren + 1:cursor - 1], cursor

def braced_control_end(body, keyword):
    structure = c_structure(body)
    _, cursor = control_parenthesized_contents(body, keyword)
    while cursor < len(structure) and structure[cursor].isspace():
        cursor += 1
    if cursor >= len(structure) or structure[cursor] != "{":
        raise ValueError("%s body is not braced" % keyword)
    depth = 1
    cursor += 1
    while cursor < len(structure) and depth:
        if structure[cursor] == "{":
            depth += 1
        elif structure[cursor] == "}":
            depth -= 1
        cursor += 1
    if depth:
        raise ValueError("unterminated %s body" % keyword)
    return cursor

def lock_contract_problem(body):
    expected_calls = (
        "atomic_flag_test_and_set_explicit",
        "vita_heap_test_counter_add",
        "vita_heap_test_counter_add",
    )
    problem = bounded_chain_problem(body, expected_calls, expected_loops=1)
    if problem:
        return problem
    problem = exact_call_arguments_problem(
        body, "atomic_flag_test_and_set_explicit",
        ("&s_ledger_lock, memory_order_acquire",))
    if problem:
        return problem
    problem = exact_call_arguments_problem(body, "vita_heap_test_counter_add", (
        "&s_heap_test_probe_stats.lock_acquisitions, 1U",
        "&s_heap_test_probe_stats.lock_spins, spins",
    ))
    if problem:
        return problem
    try:
        condition, _ = control_parenthesized_contents(body, "while")
        loop_end = braced_control_end(body, "while")
    except ValueError as error:
        return str(error)
    if re.sub(r"\s+", "", condition) != (
            "atomic_flag_test_and_set_explicit("
            "&s_ledger_lock,memory_order_acquire)"):
        return "lock spin condition changed"
    structure = c_structure(body)
    held = list(re.finditer(r"\bs_heap_test_lock_held\s*=\s*1\s*;",
                            structure))
    counters = list(re.finditer(r"\bvita_heap_test_counter_add\s*\(",
                                structure))
    if len(held) != 1 or len(counters) != 2:
        return "lock test-held/counter census changed"
    if not (loop_end < counters[0].start() < counters[1].start() <
            held[0].start()):
        return "lock counter/test-held publication order changed"
    return None

def unlock_contract_problem(body):
    problem = bounded_chain_problem(
        body, ("atomic_flag_clear_explicit",), expected_loops=0)
    if problem:
        return problem
    problem = exact_call_arguments_problem(
        body, "atomic_flag_clear_explicit",
        ("&s_ledger_lock, memory_order_release",))
    if problem:
        return problem
    structure = c_structure(body)
    held = list(re.finditer(r"\bs_heap_test_lock_held\s*=\s*0\s*;",
                            structure))
    clear = structure.find("atomic_flag_clear_explicit(")
    if len(held) != 1 or clear < 0 or held[0].start() >= clear:
        return "unlock test-held clear/release order changed"
    return None

def macro_definitions(source, name):
    structure = c_structure(source, strip_preprocessor=False)
    return tuple(value.strip() for value in re.findall(
        r"(?m)^\s*#define\s+%s\s+([^\r\n]+)$" % re.escape(name),
        structure))

def require_macro_definitions(source, name, expected):
    actual = macro_definitions(source, name)
    if actual != tuple(expected):
        raise SystemExit("macro %s changed: %r" % (name, actual))

heap = (root / "runtime/host_vita_heap.c").read_text(encoding="utf-8")
heap_header = (root / "runtime/host_vita_heap.h").read_text(
    encoding="utf-8")
cmake = (root / "vita/CMakeLists.txt").read_text(encoding="utf-8")
slab = (root / "runtime/host_vita_room_entry_slab.c").read_text(
    encoding="utf-8")
slab_header = (root / "runtime/host_vita_room_entry_slab.h").read_text(
    encoding="utf-8")
overflow = (root / "runtime/host_vita_heap_overflow_mspace.c").read_text(
    encoding="utf-8")

expected_diagnostic_option = (
    "option(ISAAC_VITA_STAGE_MEMORY_DIAGNOSTICS\n"
    "       \"Synchronously snapshot and log heap state at level/room "
    "boundaries\" OFF)")
if cmake.count(expected_diagnostic_option) != 1:
    raise SystemExit("stage-memory diagnostic option is not default-OFF")
if cmake.count("ISAAC_VITA_STAGE_MEMORY_DIAGNOSTICS=1") != 1:
    raise SystemExit("stage-memory diagnostic compile definition drifted")
diagnostic_cmake_start = cmake.index(
    "if(ISAAC_VITA_STAGE_MEMORY_DIAGNOSTICS)")
diagnostic_cmake_end = cmake.index("endif()", diagnostic_cmake_start)
diagnostic_cmake = cmake[diagnostic_cmake_start:diagnostic_cmake_end]
for marker in (
        '"${ISAAC_RUNTIME}/host_vita_heap.c"',
        "APPEND PROPERTY COMPILE_DEFINITIONS",
        "ISAAC_VITA_STAGE_MEMORY_DIAGNOSTICS=1"):
    if diagnostic_cmake.count(marker) != 1:
        raise SystemExit(
            "stage-memory diagnostic owner scope changed: " + marker)

heap_lock = function_body(heap, "vita_heap_lock")
heap_unlock = function_body(heap, "vita_heap_unlock")
problem = lock_contract_problem(heap_lock)
if problem:
    raise SystemExit("heap lock contract changed: %s" % problem)
problem = unlock_contract_problem(heap_unlock)
if problem:
    raise SystemExit("heap unlock contract changed: %s" % problem)
lock_log_poison = heap_lock.replace(
    "s_heap_test_lock_held = 1;",
    'isaac_vita_log("audit-before-held");\n'
    "    s_heap_test_lock_held = 1;", 1)
if lock_log_poison == heap_lock or \
        lock_contract_problem(lock_log_poison) is None:
    raise SystemExit("lock pre-held logger poison was not rejected")
lock_scan_poison = heap_lock.replace(
    "s_heap_test_lock_held = 1;",
    "for (size_t audit_slot = 0U; audit_slot < s_ledger_capacity; "
    "++audit_slot) (void)s_ledger[audit_slot];\n"
    "    s_heap_test_lock_held = 1;", 1)
if lock_scan_poison == heap_lock or \
        lock_contract_problem(lock_scan_poison) is None:
    raise SystemExit("lock ledger-scan poison was not rejected")
unlock_log_poison = heap_unlock.replace(
    "atomic_flag_clear_explicit(&s_ledger_lock, memory_order_release);",
    'isaac_vita_log("audit-after-held-clear");\n'
    "    atomic_flag_clear_explicit(&s_ledger_lock, "
    "memory_order_release);", 1)
if unlock_log_poison == heap_unlock or \
        unlock_contract_problem(unlock_log_poison) is None:
    raise SystemExit("unlock pre-release logger poison was not rejected")

size_u32 = function_body(heap, "vita_stage_memory_size_u32")
require_scalar_helper(size_u32, "vita_stage_memory_size_u32")
for marker in (
        "#if SIZE_MAX > UINT32_MAX",
        "if (value > UINT32_MAX)",
        "*valid = 0U;",
        "return UINT32_MAX;",
        "#else",
        "(void)valid;",
        "#endif",
        "return (uint32_t)value;"):
    if size_u32.count(marker) != 1:
        raise SystemExit("size_u32 compile-time split changed: %s" % marker)
stage = function_body(heap, "vita_stage_memory_snapshot_locked")
require_scalar_helper(stage, "vita_stage_memory_snapshot_locked", (
    "memset",
    "vita_stage_memory_size_u32",
    "isaac_vita_heap_overflow_mspace_snapshot_get",
    "isaac_vita_room_entry_slab_fast_snapshot_locked",
))
stage_structure = c_structure(stage)
if stage_structure.count(
        "isaac_vita_room_entry_slab_fast_snapshot_locked") != 1 or \
        "isaac_vita_room_entry_slab_snapshot_locked" in stage_structure:
    raise SystemExit("stage snapshot lost its sole fast slab observer")
require_exact_call_arguments(
    stage, "stage snapshot", "memset",
    ("snapshot, 0, sizeof *snapshot",))
require_exact_call_arguments(stage, "stage snapshot", "memcpy", ())
near_point = function_body(heap, "vita_stage_memory_heap_near_point")
require_scalar_helper(near_point, "vita_stage_memory_heap_near_point", (
    "vita_stage_memory_size_u32",
))
if c_structure(near_point).count("vita_stage_memory_size_u32") != 4:
    raise SystemExit("mallinfo near-point field census changed")

next_sequence = function_body(heap, "vita_stage_memory_next_sequence_locked")
context = function_body(heap, "vita_stage_memory_context_locked")
clear_room = function_body(heap, "vita_stage_memory_clear_room_locked")
clear_level = function_body(heap, "vita_stage_memory_clear_level_locked")
require_scalar_helper(next_sequence, "vita_stage_memory_next_sequence_locked")
require_scalar_helper(context, "vita_stage_memory_context_locked")
require_scalar_helper(clear_room, "vita_stage_memory_clear_room_locked")
require_scalar_helper(clear_level, "vita_stage_memory_clear_level_locked",
                      ("vita_stage_memory_clear_room_locked",))

note = function_body(heap, "isaac_vita_stage_memory_note")
note_structure = c_structure(note)
diagnostic_guard = "#ifdef VITA_STAGE_MEMORY_DIAGNOSTICS_ENABLED"
if note.count(diagnostic_guard) != 3:
    raise SystemExit("stage note diagnostic guard census changed")
lock_marker = "vita_heap_lock();"
unlock_marker = "vita_heap_unlock();"
if note_structure.count(lock_marker) != 1 or \
        note_structure.count(unlock_marker) != 1:
    raise SystemExit("stage note lock/unlock census changed")
lock_start = note_structure.index(lock_marker)
unlock_end = note_structure.index(unlock_marker) + len(unlock_marker)
critical = note[lock_start:unlock_end]
require_scalar_helper(critical, "stage note critical section", (
    "vita_heap_lock",
    "vita_stage_memory_next_sequence_locked",
    "vita_stage_memory_context_locked",
    "vita_floor_lifetime_stage_event_locked",
    "vita_stage_memory_snapshot_locked",
    "vita_floor_lifetime_record_locked",
    "vita_stage_memory_clear_room_locked",
    "vita_stage_memory_clear_level_locked",
    "vita_heap_unlock",
))
floor_event_start = note_structure.index(
    "vita_floor_lifetime_stage_event_locked(")
snapshot_guard_start = note.index(
    diagnostic_guard, floor_event_start)
snapshot_start = note_structure.index(
    "vita_stage_memory_snapshot_locked(", floor_event_start)
snapshot_guard_end = note.index("#endif", snapshot_start)
if not (floor_event_start < snapshot_guard_start < snapshot_start <
        snapshot_guard_end < unlock_end):
    raise SystemExit(
        "release floor transition/diagnostic snapshot boundary changed")
mallinfo_start = note_structure.index("VITA_STAGE_MEMORY_MALLINFO()")
near_point_start = note_structure.index("vita_stage_memory_heap_near_point(")
logger_start = note_structure.index("isaac_vita_log(")
if not (lock_start < unlock_end < mallinfo_start < near_point_start <
        logger_start):
    raise SystemExit("mallinfo/guest-lock/logger ordering changed")
post_guard_start = note.index(diagnostic_guard, unlock_end)
post_guard_end = note.rindex("#endif")
if not (unlock_end < post_guard_start < mallinfo_start < logger_start <
        post_guard_end):
    raise SystemExit("release post-boundary diagnostic guard changed")
require_exact_call_arguments(
    note, "stage record", "memset", ("&record, 0, sizeof record",))
require_exact_call_arguments(note, "stage record", "memcpy", ())

log_arguments = split_c_arguments(call_contents(note, "isaac_vita_log"))
if len(log_arguments) != 30:
    raise SystemExit("stage log argument census changed: %d" %
                     len(log_arguments))
format_tokens = re.findall(r'"(?:\\.|[^"\\])*"', log_arguments[0])
format_string = "".join(ast.literal_eval(token) for token in format_tokens)
expected_format = (
    "stagemem: q=%x lq/rq=%x/%x e=%s ctx_valid=%x active=%x "
    "ls=%x lt=%x rs=%x mode=%x ok=%x mi=%x/%x/%x/%x ledger=%x "
    "ov=%x/%x/%x/%x slab=%x/%x/%x/%x/%x/%x "
    "valid/term/sat=%x/%x/%x"
)
if format_string != expected_format:
    raise SystemExit("stage log format literal changed")
conversions = re.findall(
    r"%(?:[-+ #0]*\d*(?:\.\d+)?(?:hh|h|ll|l|j|z|t|L)?[A-Za-z%])",
    format_string)
if conversions.count("%x") != 28 or conversions.count("%s") != 1 or \
        any(conversion not in ("%x", "%s") for conversion in conversions):
    raise SystemExit("stage log conversion whitelist changed: %r" %
                     conversions)
expected_payload = (
    "(unsigned)record.sequence",
    "(unsigned)record.level_sequence",
    "(unsigned)record.room_sequence",
    "vita_stage_memory_event_name(record.event)",
    "(unsigned)record.context_valid",
    "(unsigned)record.active",
    "(unsigned)record.level_stage",
    "(unsigned)record.level_type",
    "(unsigned)record.room_stage",
    "(unsigned)record.room_mode",
    "(unsigned)record.result",
    "(unsigned)record.memory.arena",
    "(unsigned)record.memory.uordblks",
    "(unsigned)record.memory.fordblks",
    "(unsigned)record.memory.ordblks",
    "(unsigned)record.memory.ledger_count",
    "(unsigned)record.memory.overflow_live",
    "(unsigned)record.memory.overflow_requested",
    "(unsigned)record.memory.raw_internal_live",
    "(unsigned)record.memory.raw_internal_requested",
    "(unsigned)record.memory.slab_pages",
    "(unsigned)record.memory.slab_raw_pages",
    "(unsigned)record.memory.slab_external_pages",
    "(unsigned)record.memory.slab_live",
    "(unsigned)record.memory.slab_allocations",
    "(unsigned)record.memory.slab_frees",
    "(unsigned)record.memory.valid",
    "(unsigned)record.memory.terminal",
    "(unsigned)record.memory.counter_saturated",
)
payload = tuple(re.sub(r"\s+", " ", argument).strip()
                for argument in log_arguments[1:])
if payload != expected_payload:
    raise SystemExit("stage log payload/cast order changed")

floor_arguments = split_c_arguments(
    call_contents(note, "isaac_vita_log", occurrence=1))
if len(floor_arguments) != 31:
    raise SystemExit("floor log argument census changed: %d" %
                     len(floor_arguments))
floor_format_tokens = re.findall(
    r'"(?:\\.|[^"\\])*"', floor_arguments[0])
floor_format = "".join(ast.literal_eval(token)
                       for token in floor_format_tokens)
expected_floor_format = (
    "floorlife: q=%x e=%s epoch=%x boot/roll=%x/%x phase=%s "
    "heap=%x/%x/%x/%x,%x/%x/%x/%x "
    "hphase=%x/%x/%x,%x/%x/%x slab=%x/%x/%x/%x "
    "sphase=%x/%x/%x valid/term/sat=%x/%x/%x"
)
if floor_format != expected_floor_format:
    raise SystemExit("floor log format literal changed")
floor_conversions = re.findall(
    r"%(?:[-+ #0]*\d*(?:\.\d+)?(?:hh|h|ll|l|j|z|t|L)?[A-Za-z%])",
    floor_format)
if floor_conversions.count("%x") != 28 or \
        floor_conversions.count("%s") != 2 or \
        any(conversion not in ("%x", "%s")
            for conversion in floor_conversions):
    raise SystemExit("floor log conversion whitelist changed: %r" %
                     floor_conversions)
expected_floor_payload = (
    "(unsigned)record.sequence",
    "vita_stage_memory_event_name(record.event)",
    "(unsigned)floor_record.heap.epoch",
    "(unsigned)floor_record.heap.bootstrap_count",
    "(unsigned)floor_record.heap.rollover_count",
    "vita_floor_lifetime_phase_name(floor_record.heap.phase)",
    "(unsigned)floor_record.heap.total_count",
    "(unsigned)floor_record.heap.unscoped_count",
    "(unsigned)floor_record.heap.prior_count",
    "(unsigned)floor_record.heap.current_count",
    "(unsigned)floor_record.heap.total_requested_bytes",
    "(unsigned)floor_record.heap.unscoped_requested_bytes",
    "(unsigned)floor_record.heap.prior_requested_bytes",
    "(unsigned)floor_record.heap.current_requested_bytes",
    "(unsigned)floor_record.heap.phase_count[0]",
    "(unsigned)floor_record.heap.phase_count[1]",
    "(unsigned)floor_record.heap.phase_count[2]",
    "(unsigned)floor_record.heap.phase_requested_bytes[0]",
    "(unsigned)floor_record.heap.phase_requested_bytes[1]",
    "(unsigned)floor_record.heap.phase_requested_bytes[2]",
    "(unsigned)floor_record.slab_total",
    "(unsigned)floor_record.slab_unscoped",
    "(unsigned)floor_record.slab_prior",
    "(unsigned)floor_record.slab_current",
    "(unsigned)floor_record.slab_phase[0]",
    "(unsigned)floor_record.slab_phase[1]",
    "(unsigned)floor_record.slab_phase[2]",
    "(unsigned)floor_record.valid",
    "(unsigned)floor_record.terminal",
    "(unsigned)floor_record.counter_saturated",
)
floor_payload = tuple(re.sub(r"\s+", " ", argument).strip()
                      for argument in floor_arguments[1:])
if floor_payload != expected_floor_payload:
    raise SystemExit("floor log payload/cast order changed")
max_floor_values = tuple(
    "room-after-unload" if conversion == "%s" and index == 1 else
    "level-init" if conversion == "%s" else 0xffffffff
    for index, conversion in enumerate(floor_conversions))
if len(floor_format % max_floor_values) != 356:
    raise SystemExit("floor log worst-case length changed: %d" %
                     len(floor_format % max_floor_values))

fast = function_body(slab, "isaac_vita_room_entry_slab_fast_snapshot_locked")
expected_fast_body = (
    "if (!snapshot_out || !room_slab_validate_fast()) return 0; "
    "snapshot_out->pages = s_room_slab.page_count; "
    "snapshot_out->raw_pages = s_room_slab.raw_page_count; "
    "snapshot_out->external_pages = s_room_slab.external_page_count; "
    "snapshot_out->live_slots = s_room_slab.live_slots; "
    "snapshot_out->allocations = s_room_slab.allocations; "
    "snapshot_out->frees = s_room_slab.frees; "
    "snapshot_out->terminal = s_room_slab.terminal; "
    "snapshot_out->counter_saturated = s_room_slab.counter_saturated; "
    "return 1;"
)
if re.sub(r"\s+", " ", fast).strip() != expected_fast_body:
    raise SystemExit("fast slab snapshot body changed")
require_scalar_helper(fast, "fast slab snapshot",
                      ("room_slab_validate_fast",))
if fast.count("room_slab_validate_fast()") != 1:
    raise SystemExit("fast slab snapshot lost its scalar validator")
expected_fast_mapping = (
    ("pages", "page_count"),
    ("raw_pages", "raw_page_count"),
    ("external_pages", "external_page_count"),
    ("live_slots", "live_slots"),
    ("allocations", "allocations"),
    ("frees", "frees"),
    ("terminal", "terminal"),
    ("counter_saturated", "counter_saturated"),
)
fast_mapping = tuple(re.findall(
    r"\bsnapshot_out->([A-Za-z_]\w*)\s*=\s*"
    r"s_room_slab\.([A-Za-z_]\w*)\s*;", fast))
all_fast_assignments = tuple(re.findall(
    r"\bsnapshot_out->([A-Za-z_]\w*)\s*=", fast))
if fast_mapping != expected_fast_mapping or \
        all_fast_assignments != tuple(field for field, _ in expected_fast_mapping):
    raise SystemExit("fast slab scalar mapping changed: %r" %
                     (fast_mapping,))
for forbidden in ("validate_cold", "snapshot_fill", "snapshot_get",
                  "room_slab_bitmap", "room_slab_bit_", "for (", "while ("):
    if forbidden in fast:
        raise SystemExit("fast slab snapshot gained cold work: %s" % forbidden)
validator = function_body(slab, "room_slab_validate_fast")
validator_structure = c_structure(validator)
if validator_structure.count("word_index < ROOM_SLAB_NONFULL_WORDS") != 1:
    raise SystemExit("fast slab validator lost its bounded mask census")
loops = re.findall(r"\b(?:for|while)\s*\((.*?)\)",
                   validator_structure, flags=re.S)
if len(loops) != 1 or "word_index < ROOM_SLAB_NONFULL_WORDS" not in loops[0]:
    raise SystemExit("fast slab validator gained a non-mask loop")
if indirect_call_names(validator):
    raise SystemExit("fast slab validator gained an indirect helper call")
calls = helper_calls(validator)
if calls:
    raise SystemExit("fast slab validator gained a helper call: %s" %
                     ",".join(sorted(calls)))
for forbidden in ("validate_cold", "snapshot_fill", "snapshot_get",
                  "room_slab_bitmap", "room_slab_bit_",
                  "isaac_vita_room_entry_external_"):
    if forbidden in validator:
        raise SystemExit("fast slab validator gained cold work: %s" % forbidden)

# Close the entire hot floor-lifetime call graph.  A loop-free top-level
# producer is not bounded if a newly introduced helper can scan a ledger or
# bitmap, so every reachable project helper has an exact ordered call census.
overflow_snapshot = function_body(
    overflow, "isaac_vita_heap_overflow_mspace_snapshot_get")
overflow_can_free = function_body(overflow, "overflow_state_can_free")
require_bounded_chain(overflow_snapshot, "overflow snapshot",
                      ("overflow_state_can_free",
                       "overflow_state_can_free"))
require_bounded_chain(overflow_can_free, "overflow state predicate")

floor_event = function_body(heap, "vita_floor_lifetime_stage_event_locked")
floor_record = function_body(heap, "vita_floor_lifetime_record_locked")
heap_floor_snapshot = function_body(
    heap, "vita_heap_floor_lifetime_snapshot_locked")
heap_floor_validate = function_body(
    heap, "vita_heap_floor_lifetime_accounting_valid_locked")
heap_floor_invalidate = function_body(
    heap, "vita_heap_floor_lifetime_invalidate_locked")
heap_floor_phase = function_body(
    heap, "vita_heap_floor_lifetime_phase_set_locked")
heap_floor_bootstrap = function_body(
    heap, "vita_heap_floor_lifetime_bootstrap_locked")
heap_floor_rollover = function_body(
    heap, "vita_heap_floor_lifetime_rollover_locked")
heap_floor_saturate = function_body(
    heap, "vita_heap_floor_lifetime_saturate_locked")
heap_combined_layout = function_body(heap, "vita_heap_ledger_combined_layout")
require_bounded_chain(floor_event, "floor stage event", (
    "vita_heap_floor_lifetime_invalidate_locked",
    "vita_heap_floor_lifetime_bootstrap_locked",
    "vita_heap_floor_lifetime_rollover_locked",
    "vita_heap_floor_lifetime_bootstrap_locked",
    "vita_heap_floor_lifetime_phase_set_locked",
    "vita_heap_floor_lifetime_phase_set_locked",
    "vita_heap_floor_lifetime_phase_set_locked",
    "vita_heap_floor_lifetime_invalidate_locked",
    "vita_heap_floor_lifetime_invalidate_locked",
))
require_bounded_chain(floor_record, "floor record", (
    "memset", "vita_heap_floor_lifetime_snapshot_locked", "memset",
    "isaac_vita_room_entry_slab_floor_lifetime_snapshot_locked",
))
heap_floor_snapshot_calls = (
    "vita_heap_floor_lifetime_accounting_valid_locked", "memset",
    "memcpy", "memcpy",
)
require_bounded_chain(
    heap_floor_snapshot, "heap floor snapshot", heap_floor_snapshot_calls)
require_bounded_chain(heap_floor_validate, "heap floor accounting validator")
require_bounded_chain(heap_floor_invalidate, "heap floor invalidation", (
    "isaac_vita_room_entry_slab_floor_lifetime_phase_set_locked",
))
require_bounded_chain(heap_floor_phase, "heap floor phase-set", (
    "vita_heap_floor_lifetime_invalidate_locked",
    "isaac_vita_room_entry_slab_floor_lifetime_phase_set_locked",
    "vita_heap_floor_lifetime_invalidate_locked",
))
require_bounded_chain(heap_floor_bootstrap, "heap floor bootstrap", (
    "vita_heap_floor_lifetime_invalidate_locked",
    "isaac_vita_room_entry_slab_floor_lifetime_bootstrap_locked",
    "vita_heap_floor_lifetime_invalidate_locked",
))
require_bounded_chain(heap_floor_rollover, "heap floor rollover", (
    "vita_heap_ledger_combined_layout",
    "vita_heap_floor_lifetime_saturate_locked",
    "vita_heap_floor_lifetime_invalidate_locked",
    "isaac_vita_room_entry_slab_floor_lifetime_rollover_locked",
    "vita_heap_floor_lifetime_invalidate_locked",
    "memset", "memset", "memset",
    "isaac_vita_room_entry_slab_floor_lifetime_phase_set_locked",
    "vita_heap_floor_lifetime_invalidate_locked",
))
require_bounded_chain(heap_floor_saturate, "heap floor saturate")
require_bounded_chain(heap_combined_layout, "heap combined layout", ("memset",))
floor_record_memsets = (
    "record, 0, sizeof *record",
    "&slab, 0, sizeof slab",
)
require_exact_call_arguments(
    floor_record, "floor record", "memset", floor_record_memsets)
require_exact_call_arguments(floor_record, "floor record", "memcpy", ())
heap_floor_snapshot_memsets = ("snapshot, 0, sizeof *snapshot",)
heap_floor_snapshot_memcpys = (
    "snapshot->phase_count, s_floor_lifetime.phase_count, "
        "sizeof snapshot->phase_count",
    "snapshot->phase_requested_bytes, "
        "s_floor_lifetime.phase_requested_bytes, "
        "sizeof snapshot->phase_requested_bytes",
)
require_exact_call_arguments(
    heap_floor_snapshot, "heap floor snapshot", "memset",
    heap_floor_snapshot_memsets)
require_exact_call_arguments(
    heap_floor_snapshot, "heap floor snapshot", "memcpy",
    heap_floor_snapshot_memcpys)
require_exact_call_arguments(
    heap_combined_layout, "heap combined layout", "memset",
    ("layout, 0, sizeof *layout",))
require_exact_call_arguments(
    heap_combined_layout, "heap combined layout", "memcpy", ())

slab_floor_snapshot = function_body(
    slab, "isaac_vita_room_entry_slab_floor_lifetime_snapshot_locked")
slab_floor_validate = function_body(slab, "room_slab_floor_validate_fast")
slab_floor_phase_valid = function_body(slab, "room_slab_floor_phase_valid")
slab_floor_invalidate = function_body(slab, "room_slab_floor_invalidate")
slab_floor_bootstrap = function_body(
    slab, "isaac_vita_room_entry_slab_floor_lifetime_bootstrap_locked")
slab_floor_rollover = function_body(
    slab, "isaac_vita_room_entry_slab_floor_lifetime_rollover_locked")
slab_floor_phase = function_body(
    slab, "isaac_vita_room_entry_slab_floor_lifetime_phase_set_locked")
require_bounded_chain(slab_floor_snapshot, "slab floor snapshot",
                      ("room_slab_floor_validate_fast",))
require_bounded_chain(slab_floor_validate, "slab floor fast validator",
                      ("room_slab_floor_phase_valid",))
require_bounded_chain(slab_floor_phase_valid, "slab floor phase predicate")
require_bounded_chain(slab_floor_invalidate, "slab floor invalidation")
require_bounded_chain(slab_floor_bootstrap, "slab floor bootstrap", (
    "room_slab_floor_validate_fast", "room_slab_floor_phase_valid",
    "room_slab_floor_invalidate", "room_slab_floor_validate_fast",
    "room_slab_floor_invalidate",
))
require_bounded_chain(slab_floor_rollover, "slab floor rollover", (
    "room_slab_floor_validate_fast", "room_slab_floor_invalidate",
    "room_slab_floor_invalidate", "memset", "memset",
    "room_slab_floor_validate_fast", "room_slab_floor_invalidate",
))
require_bounded_chain(slab_floor_phase, "slab floor phase-set", (
    "room_slab_floor_validate_fast", "room_slab_floor_phase_valid",
    "room_slab_floor_invalidate",
))

# Pin the two only O(metadata) rollover clears.  The heap bitmap is exactly
# 524288 / 8 = 65536 bytes.  Hybrid192 owns 192 * 504 = 96768 CURRENT bytes;
# its immutable packed birth-phase rows must survive rollover.
require_macro_definitions(
    heap_header, "ISAAC_VITA_GUEST_HEAP_FIXED_LEDGER_CAPACITY", ("524288U",))
require_macro_definitions(
    heap_header, "ISAAC_VITA_GUEST_HEAP_FLOOR_CURRENT_BYTES",
    ("0x00010000U",))
if 524288 // 8 != 65536 or int("00010000", 16) != 65536:
    raise SystemExit("heap CURRENT byte arithmetic in the auditor is broken")
heap_combined_layout_structure = c_structure(heap_combined_layout)
if heap_combined_layout_structure.count(
        "current_bytes = capacity / 8U;") != 1 or \
        heap_combined_layout_structure.count(
            "layout->current_bytes = current_bytes;") != 1:
    raise SystemExit("heap combined layout lost its one-bit CURRENT derivation")
heap_rollover_memsets = (
    "s_ledger_floor_current, 0, layout.current_bytes",
    "s_floor_lifetime.phase_count, 0, sizeof s_floor_lifetime.phase_count",
    "s_floor_lifetime.phase_requested_bytes, 0, "
        "sizeof s_floor_lifetime.phase_requested_bytes",
)
require_exact_call_arguments(
    heap_floor_rollover, "heap floor rollover", "memset",
    heap_rollover_memsets)
if "s_ledger_floor_phase" in c_structure(heap_floor_rollover):
    raise SystemExit("heap rollover clears immutable birth-phase metadata")

require_macro_definitions(
    slab_header, "ISAAC_VITA_ROOM_ENTRY_SLAB_BITMAP_BYTES", ("504U",))
require_macro_definitions(
    slab_header, "ISAAC_VITA_ROOM_ENTRY_SLAB_RAW_MAX_PAGES", ("96U",))
require_macro_definitions(
    slab_header, "ISAAC_VITA_ROOM_ENTRY_SLAB_TOTAL_MAX_PAGES", ("192U",))
require_macro_definitions(slab, "ROOM_SLAB_PAGE_CAP", (
    "ISAAC_VITA_ROOM_ENTRY_SLAB_TOTAL_MAX_PAGES",
    "ISAAC_VITA_ROOM_ENTRY_SLAB_RAW_MAX_PAGES",
))
require_macro_definitions(
    slab, "ROOM_SLAB_NONFULL_WORDS",
    ("((ROOM_SLAB_PAGE_CAP + 31U) / 32U)",))
if (96 + 31) // 32 != 3 or (192 + 31) // 32 != 6:
    raise SystemExit("raw/Hybrid nonfull-word arithmetic in auditor is broken")
if 192 * 504 != 96768:
    raise SystemExit("Hybrid CURRENT byte arithmetic in auditor is broken")
normalized_slab = re.sub(r"\s+", " ", c_structure(slab))
for declaration in (
        "unsigned char floor_current[ROOM_SLAB_PAGE_CAP] "
            "[ISAAC_VITA_ROOM_ENTRY_SLAB_BITMAP_BYTES];",
        "uint32_t floor_phase_slots[3];"):
    if normalized_slab.count(declaration) != 1:
        raise SystemExit("slab floor metadata declaration changed: %s" %
                         declaration)
slab_rollover_memsets = (
    "s_room_slab.floor_phase_slots, 0, "
        "sizeof s_room_slab.floor_phase_slots",
    "s_room_slab.floor_current, 0, sizeof s_room_slab.floor_current",
)
require_exact_call_arguments(
    slab_floor_rollover, "slab floor rollover", "memset",
    slab_rollover_memsets)
if "floor_birth_phase" in c_structure(slab_floor_rollover):
    raise SystemExit("slab rollover clears immutable birth-phase metadata")
normalized_validator = re.sub(r"\s+", " ", validator_structure)
expected_mask_loop = (
    "for (word_index = 0U; word_index < ROOM_SLAB_NONFULL_WORDS; "
    "++word_index) has_nonfull |= "
    "s_room_slab.nonfull_mask[word_index] != 0U;"
)
if normalized_validator.count(expected_mask_loop) != 1:
    raise SystemExit("fast slab validator's exact 3/6-word mask loop changed")

# Practical poison probes exercise the predicates themselves.  These must
# fail for comments/strings that look like calls, an indirect call, a hidden
# helper or loop, changed order, an oversized copy/record clear, and a new
# birth-phase sidecar clear respectively.
snapshot_validator_call = "vita_heap_floor_lifetime_accounting_valid_locked()"
if c_structure(heap_floor_snapshot).count(snapshot_validator_call) != 1:
    raise SystemExit("heap floor snapshot validator call census changed")
comment_call_poison = heap_floor_snapshot.replace(
    snapshot_validator_call, "/* validator call removed */ 1U", 1)
string_call_poison = heap_floor_snapshot.replace(
    snapshot_validator_call,
    '"vita_heap_floor_lifetime_accounting_valid_locked()"', 1)
indirect_call_poison = heap_floor_snapshot.replace(
    snapshot_validator_call,
    "(*vita_heap_floor_lifetime_accounting_valid_locked)()", 1)
for label, poison in (
        ("comment", comment_call_poison),
        ("string", string_call_poison),
        ("indirect", indirect_call_poison)):
    if poison == heap_floor_snapshot or bounded_chain_problem(
            poison, heap_floor_snapshot_calls) is None:
        raise SystemExit("snapshot %s-call poison was not rejected" % label)
if bounded_chain_problem(floor_event + "\naudit_poison();",
        helper_call_sequence(floor_event)) is None:
    raise SystemExit("call-census poison was not rejected")
if bounded_chain_problem(heap_floor_validate + "\nwhile (0) {}", ()) is None:
    raise SystemExit("loop poison was not rejected")
order_poison = floor_event.replace(
    "vita_heap_floor_lifetime_bootstrap_locked", "AUDIT_ORDER_SLOT", 1)
order_poison = order_poison.replace(
    "vita_heap_floor_lifetime_rollover_locked",
    "vita_heap_floor_lifetime_bootstrap_locked", 1)
order_poison = order_poison.replace(
    "AUDIT_ORDER_SLOT", "vita_heap_floor_lifetime_rollover_locked", 1)
if bounded_chain_problem(
        order_poison, helper_call_sequence(floor_event)) is None:
    raise SystemExit("call-order poison was not rejected")
sidecar_poison = heap_floor_rollover + \
    "\nmemset(s_ledger_floor_phase, 0, layout.phase_bytes);"
if normalized_call_arguments(sidecar_poison, "memset") == \
        heap_rollover_memsets:
    raise SystemExit("sidecar memset poison was not rejected")
copy_poison = heap_floor_snapshot.replace(
    "sizeof snapshot->phase_count", "s_ledger_capacity", 1)
if copy_poison == heap_floor_snapshot or exact_call_arguments_problem(
        copy_poison, "memcpy", heap_floor_snapshot_memcpys) is None:
    raise SystemExit("capacity-sized snapshot memcpy poison was not rejected")
record_memset_poison = floor_record.replace(
    "sizeof *record", "s_ledger_capacity", 1)
if record_memset_poison == floor_record or exact_call_arguments_problem(
        record_memset_poison, "memset", floor_record_memsets) is None:
    raise SystemExit("capacity-sized floor record memset poison was not rejected")
stage_record_memset_poison = note.replace(
    "sizeof record", "ISAAC_VITA_GUEST_HEAP_FIXED_LEDGER_CAPACITY", 1)
if stage_record_memset_poison == note or exact_call_arguments_problem(
        stage_record_memset_poison, "memset",
        ("&record, 0, sizeof record",)) is None:
    raise SystemExit("capacity-sized stage record memset poison was not rejected")
print("stage-memory source/cold-path gate: PASS")
PY

mkdir -p "$work/unsigned-mallinfo"
cat > "$work/unsigned-mallinfo/malloc.h" <<'EOF'
#ifndef ISAAC_STAGE_MEMORY_UNSIGNED_MALLINFO_H
#define ISAAC_STAGE_MEMORY_UNSIGNED_MALLINFO_H
#include <stddef.h>
struct mallinfo {
    size_t arena;
    size_t uordblks;
    size_t fordblks;
    size_t ordblks;
};
#endif
EOF

cat > "$work/slab-fast-main.c" <<'EOF'
#include <stdint.h>
#include "host_vita_room_entry_slab.h"
int main(void)
{
    isaac_vita_room_entry_slab_fast_snapshot snapshot = {
        UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX,
        UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX
    };
    if (!isaac_vita_room_entry_slab_fast_snapshot_locked(&snapshot))
        return 1;
    return snapshot.pages || snapshot.raw_pages || snapshot.external_pages ||
        snapshot.live_slots || snapshot.allocations || snapshot.frees ||
        snapshot.terminal || snapshot.counter_saturated;
}
EOF

cat > "$work/floor-bounds-contract.c" <<'EOF'
#include "host_vita_heap.h"
#include "host_vita_room_entry_slab.h"
_Static_assert(ISAAC_VITA_GUEST_HEAP_FIXED_LEDGER_CAPACITY == 524288U,
               "fixed heap ledger capacity drifted");
_Static_assert(ISAAC_VITA_GUEST_HEAP_FLOOR_CURRENT_BYTES == 65536U,
               "fixed heap CURRENT bitmap is not 64 KiB");
_Static_assert(ISAAC_VITA_GUEST_HEAP_FIXED_LEDGER_CAPACITY / 8U ==
                   ISAAC_VITA_GUEST_HEAP_FLOOR_CURRENT_BYTES,
               "heap CURRENT bitmap is not one bit per slot");
_Static_assert(ISAAC_VITA_ROOM_ENTRY_SLAB_RAW_MAX_PAGES == 96U &&
                   (ISAAC_VITA_ROOM_ENTRY_SLAB_RAW_MAX_PAGES + 31U) / 32U ==
                       3U,
               "raw slab fast mask is not three words");
_Static_assert(ISAAC_VITA_ROOM_ENTRY_SLAB_TOTAL_MAX_PAGES == 192U &&
                   (ISAAC_VITA_ROOM_ENTRY_SLAB_TOTAL_MAX_PAGES + 31U) / 32U ==
                       6U,
               "Hybrid slab fast mask is not six words");
_Static_assert(ISAAC_VITA_ROOM_ENTRY_SLAB_TOTAL_MAX_PAGES *
                   ISAAC_VITA_ROOM_ENTRY_SLAB_BITMAP_BYTES == 96768U,
               "Hybrid slab CURRENT bitmap is not 96768 bytes");
int main(void) { return 0; }
EOF

cat > "$work/stage-layout-contract.c" <<'EOF'
#include "host_vita_heap.c"
_Static_assert(sizeof(isaac_vita_guest_heap_floor_lifetime_snapshot) == 84U,
               "heap floor snapshot size drifted");
_Static_assert(
    sizeof(isaac_vita_room_entry_slab_floor_lifetime_snapshot) == 52U,
    "slab floor snapshot size drifted");
_Static_assert(sizeof(vita_stage_memory_snapshot) == 72U,
               "stage snapshot size drifted");
_Static_assert(sizeof(vita_stage_memory_record) == 116U,
               "stage record size drifted");
_Static_assert(sizeof(vita_floor_lifetime_record) == 124U,
               "floor record size drifted");
EOF

# Build a fresh poisoned copy of the real implementation, not a toy shadow
# type.  Its added capacity-sized field must trip the same concrete layout
# contract that protects the production stage snapshot.
"$recomp_python" - \
    "$root/runtime/host_vita_heap.c" \
    "$work/host_vita_heap-capacity-field-poison.c" <<'PY'
import pathlib
import sys

source_path = pathlib.Path(sys.argv[1])
target_path = pathlib.Path(sys.argv[2])
source = source_path.read_text(encoding="utf-8")
needle = "typedef struct vita_stage_memory_snapshot {\n"
if source.count(needle) != 1:
    raise SystemExit("stage snapshot poison injection point changed")
poisoned = source.replace(
    needle,
    needle + "    unsigned char audit_capacity_field[524288U];\n",
    1)
poisoned += (
    "\n_Static_assert(sizeof(vita_stage_memory_snapshot) == 72U,\n"
    "               \"stage snapshot size drifted under 524288-byte "
    "field poison\");\n"
)
target_path.write_text(poisoned, encoding="utf-8")
PY

host_common=(
    -std=gnu11 -O1 -g -Wall -Wextra -Werror
    -fno-omit-frame-pointer -ffunction-sections -fdata-sections
    -fsanitize=address,undefined
)
oracle_defines=(
    -DISAAC_VITA_STAGE_MEMORY_ORACLE=1
    -DISAAC_VITA_HEAP_TESTING=1
)
feature_defines=(
    -DISAAC_VITA_HEAP_OVERFLOW_MSPACE=1
    -DISAAC_VITA_HEAP_LEDGER_MEMBLOCK=1
    -DISAAC_VITA_HEAP_LEDGER_BACKSHIFT=1
    -DISAAC_VITA_HEAP_RANGE_LEASE=1
    -DISAAC_VITA_ROOM_ENTRY_SLAB=1
    -DISAAC_VITA_ROOM_ENTRY_HYBRID=1
    -DGUEST_IMAGE_BASE=0x98000000U
)

for backend in gcc clang; do
    case "$backend" in
        gcc) compiler=$host_gcc ;;
        clang) compiler=$host_clang ;;
    esac
    tag=$backend
    compiler_warnings=()
    generated_compiler_warnings=()
    case "$backend" in
        gcc) compiler_warnings=(-Wno-maybe-uninitialized) ;;
        clang) generated_compiler_warnings=(-Wno-parentheses-equality) ;;
    esac

    "$compiler" -std=gnu11 -O2 -Wall -Wextra -Werror \
        -DISAAC_VITA_HEAP_OVERFLOW_MSPACE=1 \
        -I"$root/runtime" "$work/floor-bounds-contract.c" \
        -o "$work/floor-bounds-contract-$tag"
    "$work/floor-bounds-contract-$tag"

    layout_flags=(
        -std=gnu11 -O2 -Wall -Wextra -Werror
        -Wno-unused-function -Wno-unused-variable
        "${compiler_warnings[@]}" "${generated_compiler_warnings[@]}"
        -DISAAC_VITA_STAGE_MEMORY_ORACLE=1
        "${feature_defines[@]}" -I"$root/runtime"
    )
    "$compiler" "${layout_flags[@]}" \
        -c "$work/stage-layout-contract.c" \
        -o "$work/stage-layout-contract-$tag.o"
    if "$compiler" "${layout_flags[@]}" \
            -c "$work/host_vita_heap-capacity-field-poison.c" \
            -o "$work/stage-layout-poison-$tag.o" \
            > "$work/stage-layout-poison-$tag.log" 2>&1; then
        echo "$tag accepted the 524288-byte stage snapshot field poison" >&2
        exit 1
    fi
    if ! grep -q \
            'stage snapshot size drifted under 524288-byte field poison' \
            "$work/stage-layout-poison-$tag.log"; then
        echo "$tag rejected layout poison for an unexpected reason" >&2
        cat "$work/stage-layout-poison-$tag.log" >&2
        exit 1
    fi

    # Execute the real bounded helper once from its zero state for both
    # the raw 96-page build and production's Hybrid192 policy.
    for slab_mode in raw hybrid192; do
        slab_defines=()
        if [ "$slab_mode" = hybrid192 ]; then
            slab_defines=(-DISAAC_VITA_ROOM_ENTRY_HYBRID=1)
        fi
        "$compiler" "${host_common[@]}" "${compiler_warnings[@]}" \
            "${slab_defines[@]}" -I"$root/runtime" \
            "$root/runtime/host_vita_room_entry_slab.c" \
            "$work/slab-fast-main.c" -Wl,--gc-sections \
            -o "$work/slab-fast-$tag-$slab_mode"
        ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
        UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
            "$work/slab-fast-$tag-$slab_mode"
    done

    # This executable intentionally provides neither mallinfo nor the logger.
    # It therefore proves that the release path keeps floor-lifetime state
    # transitions while the diagnostic snapshots/I/O are absent.
    "$compiler" "${host_common[@]}" "${compiler_warnings[@]}" \
        -I"$root/runtime" \
        "$root/runtime/host_vita_stage_memory_release_oracle.c" \
        -Wl,--gc-sections -o "$work/stage-memory-release-$tag"
    ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
    UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
        "$work/stage-memory-release-$tag"
    nm -u "$work/stage-memory-release-$tag" \
        > "$work/stage-memory-release-$tag.undefined"
    if grep -Eq '[[:space:]]U[[:space:]]+(mallinfo|isaac_vita_log)$' \
            "$work/stage-memory-release-$tag.undefined"; then
        echo "$tag release stage path retained diagnostic imports" >&2
        exit 1
    fi
    if LC_ALL=C grep -aEq 'stagemem:|floorlife:' \
            "$work/stage-memory-release-$tag"; then
        echo "$tag release stage path retained diagnostic strings" >&2
        exit 1
    fi

    for mallinfo_kind in native unsigned; do
        mallinfo_include=()
        if [ "$mallinfo_kind" = unsigned ]; then
            mallinfo_include=(-I"$work/unsigned-mallinfo")
        fi
        for policy in off on; do
            policy_defines=()
            if [ "$policy" = on ]; then
                policy_defines=("${feature_defines[@]}")
            fi
            binary="$work/stage-memory-$tag-$mallinfo_kind-$policy"
            "$compiler" "${host_common[@]}" \
                "${compiler_warnings[@]}" \
                "${oracle_defines[@]}" "${policy_defines[@]}" \
                "${mallinfo_include[@]}" -I"$root/runtime" \
                "$root/runtime/host_vita_stage_memory_oracle.c" \
                -Wl,--gc-sections -o "$binary"
            ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
            UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
                "$binary"
        done
    done

    generated_warnings=(-Wno-unused-label -Wno-unused-variable)
    "$compiler" -std=gnu11 -O2 -Wall -Wextra -Werror \
        "${compiler_warnings[@]}" "${generated_warnings[@]}" \
        "${generated_compiler_warnings[@]}" \
        -D__vita__=1 -DGUEST_IMAGE_BASE=0x98000000U \
        -I"$root/runtime" -c "$work/stage-memory-fresh.c" \
        -o "$work/stage-memory-fresh-$tag.o"
    if [ "$(nm -u "$work/stage-memory-fresh-$tag.o" | \
             grep -Ec '[[:space:]]U[[:space:]]+isaac_vita_stage_memory_note$')" \
         -ne 1 ]; then
        echo "$tag fresh codegen lost its one scalar producer import" >&2
        exit 1
    fi
    "$compiler" -std=gnu11 -O2 -Wall -Wextra -Werror \
        "${compiler_warnings[@]}" "${generated_warnings[@]}" \
        "${generated_compiler_warnings[@]}" \
        -DGUEST_IMAGE_BASE=0x98000000U -I"$root/runtime" \
        -c "$work/stage-memory-fresh.c" \
        -o "$work/stage-memory-fresh-$tag-off.o"
    if nm -u "$work/stage-memory-fresh-$tag-off.o" | \
            grep -q 'isaac_vita_stage_memory_note'; then
        echo "$tag non-Vita fresh codegen retained the producer import" >&2
        exit 1
    fi
done

arm_cc="$VITASDK/bin/arm-vita-eabi-gcc"
arm_nm="$VITASDK/bin/arm-vita-eabi-nm"
arm_objdump="$VITASDK/bin/arm-vita-eabi-objdump"
arm_readelf="$VITASDK/bin/arm-vita-eabi-readelf"
arm_flags=(
    -std=gnu11 -O2 -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=softfp -mthumb
    -fno-strict-aliasing -ffunction-sections -fdata-sections
    -Wall -Wextra -Werror -Wno-maybe-uninitialized
)

if [ "$("$arm_cc" -dumpmachine)" != arm-vita-eabi ]; then
    echo "Vita compiler does not identify as arm-vita-eabi" >&2
    exit 2
fi
"$arm_cc" "${arm_flags[@]}" -dM -E -x c /dev/null \
    > "$work/arm-vita-eabi.predefined"
for macro in __vita__ __arm__ __VFP_FP__; do
    if ! grep -Eq "^#define[[:space:]]+$macro([[:space:]]|$)" \
            "$work/arm-vita-eabi.predefined"; then
        echo "arm-vita-eabi softfp compiler lacks predefined $macro" >&2
        exit 2
    fi
done
if grep -Eq '^#define[[:space:]]+__ARM_PCS_VFP([[:space:]]|$)' \
        "$work/arm-vita-eabi.predefined"; then
    echo "arm-vita-eabi compiler selected hardfp procedure-call ABI" >&2
    exit 2
fi

cat > "$work/arm-stage-main.c" <<'EOF'
#include <stdarg.h>
#include <stdint.h>
#include <string.h>
#include "host_vita_heap.h"
#ifdef ISAAC_STAGE_MEMORY_FEATURE_ON
#include "host_vita_heap_overflow_mspace.h"
#include "host_vita_room_entry_slab.h"
#endif

unsigned int _newlib_heap_size_user = 0x05100000U;
void isaac_vita_log(const char *format, ...) { (void)format; }

#ifdef ISAAC_STAGE_MEMORY_FEATURE_ON
int isaac_vita_heap_overflow_mspace_snapshot_get(
    isaac_vita_heap_overflow_mspace_snapshot *snapshot)
{
    memset(snapshot, 0, sizeof *snapshot);
    snapshot->state = ISAAC_VITA_HEAP_OVERFLOW_MSPACE_READY;
    snapshot->has_mspace = 1;
    return 1;
}
int isaac_vita_room_entry_slab_fast_snapshot_locked(
    isaac_vita_room_entry_slab_fast_snapshot *snapshot)
{
    memset(snapshot, 0, sizeof *snapshot);
    return 1;
}
static isaac_vita_room_entry_slab_floor_lifetime_snapshot s_floor = {
    .valid = 1U
};
int isaac_vita_room_entry_slab_floor_lifetime_bootstrap_locked(
    uint32_t phase)
{
    if (!s_floor.valid || s_floor.active || s_floor.epoch ||
        phase < ISAAC_VITA_ROOM_ENTRY_FLOOR_PHASE_LEVEL_INIT ||
        phase > ISAAC_VITA_ROOM_ENTRY_FLOOR_PHASE_PLAY)
        return 0;
    s_floor.active = 1U;
    s_floor.epoch = 1U;
    s_floor.phase = phase;
    return 1;
}
int isaac_vita_room_entry_slab_floor_lifetime_rollover_locked(void)
{
    if (!s_floor.valid || !s_floor.active || !s_floor.epoch ||
        s_floor.epoch == UINT32_MAX)
        return 0;
    ++s_floor.epoch;
    s_floor.prior_slots += s_floor.current_slots;
    s_floor.current_slots = 0U;
    s_floor.level_init_slots = 0U;
    s_floor.room_load_slots = 0U;
    s_floor.play_slots = 0U;
    return 1;
}
int isaac_vita_room_entry_slab_floor_lifetime_phase_set_locked(
    uint32_t phase)
{
    if (!s_floor.valid || !s_floor.active ||
        phase < ISAAC_VITA_ROOM_ENTRY_FLOOR_PHASE_LEVEL_INIT ||
        phase > ISAAC_VITA_ROOM_ENTRY_FLOOR_PHASE_PLAY)
        return 0;
    s_floor.phase = phase;
    return 1;
}
int isaac_vita_room_entry_slab_floor_lifetime_snapshot_locked(
    isaac_vita_room_entry_slab_floor_lifetime_snapshot *snapshot)
{
    if (!snapshot)
        return 0;
    *snapshot = s_floor;
    return s_floor.valid != 0U;
}
#endif

int main(void)
{
    isaac_vita_stage_memory_note(
        ISAAC_VITA_STAGE_MEMORY_LEVEL_BEGIN, 1U, 2U, UINT32_MAX);
    return 0;
}
EOF

"$arm_cc" "${arm_flags[@]}" -DGUEST_IMAGE_BASE=0x98000000U \
    -I"$root/runtime" \
    -c "$work/stage-memory-fresh.c" \
    -o "$work/stage-memory-fresh.arm.o"
"$arm_cc" "${arm_flags[@]}" -I"$root/runtime" \
    -c "$root/runtime/host_vita_room_entry_slab.c" \
    -o "$work/room-entry-slab.arm.o"
"$arm_cc" "${arm_flags[@]}" -DISAAC_VITA_ROOM_ENTRY_HYBRID=1 \
    -I"$root/runtime" -c "$root/runtime/host_vita_room_entry_slab.c" \
    -o "$work/room-entry-slab-hybrid192.arm.o"

for policy in off on; do
    policy_defines=(-DISAAC_VITA_STAGE_MEMORY_DIAGNOSTICS=1)
    main_defines=()
    if [ "$policy" = on ]; then
        policy_defines=(
            -DISAAC_VITA_STAGE_MEMORY_DIAGNOSTICS=1
            "${feature_defines[@]}"
        )
        main_defines=(-DISAAC_STAGE_MEMORY_FEATURE_ON=1)
    fi
    "$arm_cc" "${arm_flags[@]}" "${policy_defines[@]}" \
        -I"$root/runtime" -c "$root/runtime/host_vita_heap.c" \
        -o "$work/stage-memory-heap-$policy.arm.o"
    "$arm_cc" "${arm_flags[@]}" "${policy_defines[@]}" \
        "${main_defines[@]}" -I"$root/runtime" \
        -c "$work/arm-stage-main.c" \
        -o "$work/stage-memory-main-$policy.arm.o"
    "$arm_nm" -u "$work/stage-memory-heap-$policy.arm.o" \
        > "$work/stage-memory-heap-$policy.undefined"
    for symbol in mallinfo isaac_vita_log; do
        if [ "$(grep -Ec \
                "[[:space:]]U[[:space:]]+$symbol$" \
                "$work/stage-memory-heap-$policy.undefined")" -ne 1 ]; then
            echo "$policy ARM producer object lost its $symbol import" >&2
            exit 1
        fi
    done
    if [ "$policy" = on ]; then
        "$recomp_python" - \
            "$work/stage-memory-heap-$policy.undefined" <<'PY'
import pathlib
import sys

expected = {
    "_end",
    "_newlib_heap_size_user",
    "calloc",
    "free",
    "guest_fault",
    "guest_stack_owner_violation",
    "guest_stack_violation",
    "isaac_vita_heap_ledger_memblock_acquire",
    "isaac_vita_heap_ledger_memblock_layout",
    "isaac_vita_heap_ledger_memblock_release",
    "isaac_vita_heap_overflow_mspace_calloc",
    "isaac_vita_heap_overflow_mspace_contains",
    "isaac_vita_heap_overflow_mspace_free",
    "isaac_vita_heap_overflow_mspace_init",
    "isaac_vita_heap_overflow_mspace_malloc",
    "isaac_vita_heap_overflow_mspace_realloc",
    "isaac_vita_heap_overflow_mspace_snapshot_get",
    "isaac_vita_log",
    "isaac_vita_room_entry_external_init",
    "isaac_vita_room_entry_slab_claim_final_locked",
    "isaac_vita_room_entry_slab_event_init",
    "isaac_vita_room_entry_slab_fast_snapshot_locked",
    "isaac_vita_room_entry_slab_find_containing_locked",
    "isaac_vita_room_entry_slab_floor_lifetime_bootstrap_locked",
    "isaac_vita_room_entry_slab_floor_lifetime_phase_set_locked",
    "isaac_vita_room_entry_slab_floor_lifetime_rollover_locked",
    "isaac_vita_room_entry_slab_floor_lifetime_snapshot_locked",
    "isaac_vita_room_entry_slab_free_locked",
    "isaac_vita_room_entry_slab_log_event",
    "isaac_vita_room_entry_slab_malloc_locked",
    "isaac_vita_room_entry_slab_owns_exact_locked",
    "isaac_vita_room_entry_slab_raw_accounting_locked",
    "isaac_vita_room_entry_slab_realloc_begin_locked",
    "isaac_vita_room_entry_slab_realloc_cancel_locked",
    "isaac_vita_room_entry_slab_realloc_commit_locked",
    "mallinfo",
    "malloc",
    "memcpy",
    "memset",
    "realloc",
    "strcmp",
}
lines = pathlib.Path(sys.argv[1]).read_text(encoding="utf-8").splitlines()
malformed = [line for line in lines
             if len(line.split()) != 2 or line.split()[0] != "U"]
if malformed:
    raise SystemExit("Hybrid ARM undefined-import output changed shape: %r" %
                     malformed)
actual = {line.split()[1] for line in lines}
if actual != expected:
    raise SystemExit(
        "Hybrid ARM undefined-import allowlist changed; missing=%s extra=%s" %
        (",".join(sorted(expected - actual)),
         ",".join(sorted(actual - expected))))
PY
    fi
    if grep -Eq \
            '[[:space:]](__atomic[^[:space:]]*|__sync[^[:space:]]*|__aeabi_(ll|ul|l)[^[:space:]]*)$' \
            "$work/stage-memory-heap-$policy.undefined"; then
        echo "$policy ARM producer object gained an atomic or 64-bit helper" >&2
        exit 1
    fi
    "$arm_cc" "${arm_flags[@]}" \
        "$work/stage-memory-heap-$policy.arm.o" \
        "$work/stage-memory-main-$policy.arm.o" \
        -Wl,--gc-sections -o "$work/stage-memory-$policy.arm.elf"

    "$arm_readelf" -h "$work/stage-memory-$policy.arm.elf" \
        > "$work/stage-memory-$policy.header"
    "$arm_readelf" -A "$work/stage-memory-$policy.arm.elf" \
        > "$work/stage-memory-$policy.attributes"
    grep -q 'Machine:[[:space:]]*ARM$' \
        "$work/stage-memory-$policy.header"
    grep -q 'Version5 EABI, soft-float ABI' \
        "$work/stage-memory-$policy.header"
    if grep -q 'Tag_ABI_VFP_args' \
            "$work/stage-memory-$policy.attributes"; then
        echo "$policy stage-memory ELF advertises hardfp arguments" >&2
        exit 1
    fi
    "$arm_nm" -S "$work/stage-memory-$policy.arm.elf" \
        > "$work/stage-memory-$policy.nm"
    grep -Eq '[[:space:]]T[[:space:]]+isaac_vita_stage_memory_note$' \
        "$work/stage-memory-$policy.nm"
    grep -Eq '[[:space:]][TtWw][[:space:]]+mallinfo$' \
        "$work/stage-memory-$policy.nm"
    grep -Eq '[[:space:]][Tt][[:space:]]+isaac_vita_log$' \
        "$work/stage-memory-$policy.nm"
    if ! LC_ALL=C grep -aFq \
            'stagemem: q=%x lq/rq=%x/%x e=%s' \
            "$work/stage-memory-$policy.arm.elf"; then
        echo "$policy ARM ELF lost the stage-memory format literal" >&2
        exit 1
    fi
    if [ "$policy" = on ]; then
        if ! LC_ALL=C grep -aFq \
                'floorlife: q=%x e=%s epoch=%x' \
                "$work/stage-memory-$policy.arm.elf"; then
            echo "$policy ARM ELF lost the floor-lifetime format literal" >&2
            exit 1
        fi
    elif LC_ALL=C grep -aFq \
            'floorlife: q=%x e=%s epoch=%x' \
            "$work/stage-memory-$policy.arm.elf"; then
        echo "$policy ARM ELF unexpectedly retained floor-lifetime" >&2
        exit 1
    fi
    "$arm_objdump" -d "$work/stage-memory-$policy.arm.elf" \
        > "$work/stage-memory-$policy.disassembly"
    "$recomp_python" - \
        "$work/stage-memory-$policy.nm" \
        "$work/stage-memory-$policy.disassembly" "$policy" <<'PY'
import pathlib
import re
import sys

nm_path = pathlib.Path(sys.argv[1])
disassembly_path = pathlib.Path(sys.argv[2])
policy = sys.argv[3]
nm_lines = [line.split() for line in nm_path.read_text().splitlines()]
note_symbols = [row for row in nm_lines
                if len(row) == 4 and
                row[3] == "isaac_vita_stage_memory_note"]
if len(note_symbols) != 1 or int(note_symbols[0][1], 16) < 0x100:
    raise SystemExit("%s ARM note is missing or still a no-op: %r" %
                     (policy, note_symbols))

text = disassembly_path.read_text(encoding="utf-8")
match = re.search(
    r"(?ms)^[0-9a-f]+ <isaac_vita_stage_memory_note>:\n"
    r"(.*?)(?=^[0-9a-f]+ <[^>]+>:\n|\Z)", text)
if not match:
    raise SystemExit("%s ARM note disassembly is absent" % policy)
body = match.group(1)
instructions = re.findall(r"(?m)^\s*[0-9a-f]+:\s+[0-9a-f ]+\s+", body)
if len(instructions) < 40:
    raise SystemExit("%s ARM note is unexpectedly short: %d instructions" %
                     (policy, len(instructions)))
for callee in ("mallinfo", "isaac_vita_log"):
    if not re.search(r"\bblx?\b[^\n]*<%s>" % callee, body):
        raise SystemExit("%s ARM note lost its %s call" % (policy, callee))
PY
    if grep -Eq \
            '[[:space:]](__atomic[^[:space:]]*|__sync[^[:space:]]*|__aeabi_(ll|ul|l)[^[:space:]]*)$' \
            "$work/stage-memory-$policy.nm"; then
        echo "$policy stage-memory ELF contains atomic or 64-bit helpers" >&2
        exit 1
    fi
done

# Compile the production feature closure once more without the diagnostic
# define.  Other heap paths still use isaac_vita_log, so inspect the note's
# own disassembly and the final garbage-collected ELF rather than the whole
# input object's undefined-symbol set.
"$arm_cc" "${arm_flags[@]}" "${feature_defines[@]}" \
    -I"$root/runtime" -c "$root/runtime/host_vita_heap.c" \
    -o "$work/stage-memory-heap-release.arm.o"
"$arm_objdump" -dr "$work/stage-memory-heap-release.arm.o" \
    > "$work/stage-memory-heap-release.disassembly"
"$recomp_python" - "$work/stage-memory-heap-release.disassembly" <<'PY'
import pathlib
import re
import sys

text = pathlib.Path(sys.argv[1]).read_text(encoding="utf-8")
match = re.search(
    r"(?ms)^[0-9a-f]+ <isaac_vita_stage_memory_note>:\n"
    r"(.*?)(?=^Disassembly of section |^[0-9a-f]+ <[^>]+>:\n|\Z)",
    text)
if not match:
    raise SystemExit("release ARM stage note disassembly is absent")
body = match.group(1)
if len(re.findall(r"(?m)^\s*[0-9a-f]+:\s+[0-9a-f ]+\s+", body)) < 20:
    raise SystemExit("release ARM stage note unexpectedly collapsed")
for forbidden in ("mallinfo", "isaac_vita_log", "stagemem", "floorlife"):
    if forbidden in body:
        raise SystemExit(
            "release ARM stage note retained diagnostic reference: " +
            forbidden)
PY
"$arm_cc" "${arm_flags[@]}" "${feature_defines[@]}" \
    -DISAAC_STAGE_MEMORY_FEATURE_ON=1 -I"$root/runtime" \
    -c "$work/arm-stage-main.c" \
    -o "$work/stage-memory-main-release.arm.o"
"$arm_cc" "${arm_flags[@]}" \
    "$work/stage-memory-heap-release.arm.o" \
    "$work/stage-memory-main-release.arm.o" \
    -Wl,--gc-sections -o "$work/stage-memory-release.arm.elf"
if LC_ALL=C grep -aEq 'stagemem:|floorlife:' \
        "$work/stage-memory-release.arm.elf"; then
    echo "release ARM stage ELF retained diagnostic strings" >&2
    exit 1
fi
if "$arm_nm" "$work/stage-memory-release.arm.elf" | \
        grep -Eq '[[:space:]][TtWw][[:space:]]+mallinfo$'; then
    echo "release ARM stage ELF retained mallinfo" >&2
    exit 1
fi

"$arm_nm" -u "$work/stage-memory-fresh.arm.o" \
    > "$work/stage-memory-fresh.arm.undefined"
if [ "$(grep -Ec \
        '[[:space:]]U[[:space:]]+isaac_vita_stage_memory_note$' \
        "$work/stage-memory-fresh.arm.undefined")" -ne 1 ]; then
    echo "fresh ARM codegen lost its stage-memory producer import" >&2
    exit 1
fi
"$arm_readelf" -A "$work/stage-memory-fresh.arm.o" \
    > "$work/stage-memory-fresh.arm.attributes"
if grep -q 'Tag_ABI_VFP_args' "$work/stage-memory-fresh.arm.attributes"; then
    echo "fresh ARM codegen advertises hardfp arguments" >&2
    exit 1
fi

echo "Vita stage-memory telemetry gate: PASS (fresh roots=3, hooks=8, generated Vita ON/OFF; release path keeps lifetime state without snapshots/log I/O; diagnostic GCC+Clang ASan/UBSan; native+unsigned mallinfo; feature OFF/ON; ARM EABI5 softfp; ARM ELF not executed)"
