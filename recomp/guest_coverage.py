"""Versioned semantic coverage files for the real-entry probe.

The child only performs idempotent byte stores.  The parent owns the backing
file and mapping for the complete lifetime of the child, so a guest fault,
Win32 exception or forced timeout cannot discard the observations made before
the stop.  Coverage identities come from ``guest_coverage.json`` generated
alongside the translated program; data from different schemas is never ORed.
"""

from dataclasses import dataclass
from contextlib import contextmanager
import copy
import hashlib
import json
import mmap
import msvcrt
import os
import shutil
import struct
import threading
import time


ABI_VERSION = 1
HEADER_SIZE = 4096
MAGIC = b"REPENTOGXM-COV1\0"
if len(MAGIC) != 16:  # import-time assertion keeps the C/Python ABI honest
    raise AssertionError("coverage magic must be exactly 16 bytes")

MAGIC_OFFSET = 0
VERSION_OFFSET = 16
HEADER_SIZE_OFFSET = 20
TOTAL_SIZE_OFFSET = 24
FLAGS_OFFSET = 28
FUNCTION_COUNT_OFFSET = 32
IMPORT_COUNT_OFFSET = 36
CASE_COUNT_OFFSET = 40
RESERVED_OFFSET = 44
SCHEMA_ID_OFFSET = 48
BUILD_ID_OFFSET = 112
IDENTITY_BYTES = 64

_DEFAULT_WORKDIR = os.path.abspath(os.environ.get(
    "REPENTOGXM_WORKDIR",
    os.path.join(os.path.dirname(__file__), "..", "build", "recompiler"),
))
GEN = os.path.join(_DEFAULT_WORKDIR, "gen")
DEFAULT_CONTRACT_PATH = os.path.join(GEN, "guest_coverage.json")
DEFAULT_COVERAGE_ROOT = os.path.join(_DEFAULT_WORKDIR, "coverage")
ENABLE_ENV = "REPENTOGXM_PC_COVERAGE"
FILE_ENV = "REPENTOGXM_PC_COVERAGE_FILE"

# ``msvcrt.locking`` is the cross-process half of the cumulative archive
# contract.  It must not also be asked to arbitrate threads in one launcher:
# Windows byte-range locks are process-scoped enough that same-process callers
# can otherwise enter the critical section together.  The registry guard is
# held only while finding the stable RLock; the per-schema RLock is acquired
# before the byte-range lock below.
_SCHEMA_THREAD_LOCKS_GUARD = threading.Lock()
_SCHEMA_THREAD_LOCKS = {}

DOCUMENT_FIELDS = frozenset(("schema_id", "build_id", "generation_id"))
SEMANTIC_FIELDS = frozenset((
    "abi_version", "header_size", "schema_hash_rule",
    "function_count", "import_count", "case_count",
    "case_executable_count", "case_blocked_count", "case_edge_count",
    "case_hook_count", "function_generated_entry_hook_count",
    "function_manual_hook_count", "function_cross_root_hook_count",
    "function_hook_count", "function_rvas_sha256", "imports_sha256",
    "cases_sha256", "case_executable_sha256", "case_edges_sha256",
    "case_hooks_sha256", "blocked_cases_sha256",
    "cross_root_pairs_sha256", "cross_root_owners_sha256",
    "cross_root_targets_sha256", "functions", "imports", "cases",
    "case_edges", "blocked_case_ids", "blocked_cases", "cross_root_hooks",
))

SCHEMA_HASH_RULE = (
    "sha256-canonical-json-excluding-schema_id-build_id-generation_id-v1"
)


class CoverageFormatError(RuntimeError):
    """A generated or captured coverage contract is not exact."""


def _hex64(value, field):
    if (not isinstance(value, str) or len(value) != IDENTITY_BYTES or
            value.lower() != value or
            any(ch not in "0123456789abcdef" for ch in value)):
        raise CoverageFormatError("invalid %s (expected 64 lowercase hex)" % field)
    return value


def _uint32(value, field, allow_zero=False):
    if (not isinstance(value, int) or isinstance(value, bool) or value < 0 or
            value > 0xffffffff or (not allow_zero and value == 0)):
        raise CoverageFormatError("invalid %s" % field)
    return value


def _dense_ids(records, count, field, exact_fields):
    if not isinstance(records, list) or len(records) != count:
        raise CoverageFormatError(
            "%s record count changed: expected %d" % (field, count)
        )
    ids = []
    for record in records:
        if not isinstance(record, dict) or set(record) != set(exact_fields):
            raise CoverageFormatError("%s record is not an object" % field)
        value = record.get("id")
        if not isinstance(value, int) or isinstance(value, bool):
            raise CoverageFormatError("%s record has an invalid id" % field)
        ids.append(value)
    if ids != list(range(count)):
        raise CoverageFormatError("%s ids are not dense and ordered" % field)
    return records


def _canonical_json(value):
    return json.dumps(
        value, sort_keys=True, separators=(",", ":"), ensure_ascii=False,
        allow_nan=False,
    ).encode("utf-8")


def _sha256(value):
    return hashlib.sha256(value).hexdigest()


def _plain_string(value, field):
    if not isinstance(value, str) or not value or "\0" in value:
        raise CoverageFormatError("invalid %s" % field)
    try:
        value.encode("utf-8")
    except UnicodeError as exc:
        raise CoverageFormatError("invalid UTF-8 %s" % field) from exc
    return value


@dataclass(frozen=True)
class CoverageContract:
    schema_id: str
    build_id: str
    function_count: int
    import_count: int
    case_count: int
    case_executable_count: int
    case_blocked_count: int
    case_edge_count: int
    case_hook_count: int
    blocked_case_ids: tuple

    @property
    def total_size(self):
        return (HEADER_SIZE + self.function_count + self.import_count +
                self.case_count)

    @property
    def offsets(self):
        function = HEADER_SIZE
        imported = function + self.function_count
        case = imported + self.import_count
        return {"functions": function, "imports": imported, "cases": case}

    @property
    def counts(self):
        return {
            "functions": self.function_count,
            "imports": self.import_count,
            "cases": self.case_count,
        }


@dataclass(frozen=True)
class CoverageSnapshot:
    """One immutable, fully validated view of the three semantic domains."""

    schema_id: str
    build_id: str
    writer_ready: bool
    functions: bytes
    imports: bytes
    cases: bytes

    @property
    def domains(self):
        return {
            "functions": self.functions,
            "imports": self.imports,
            "cases": self.cases,
        }

    @property
    def counts(self):
        return {name: sum(values) for name, values in self.domains.items()}

    def marked_ids(self, name):
        try:
            values = self.domains[name]
        except KeyError as exc:
            raise ValueError("unknown coverage domain: %s" % name) from exc
        return tuple(index for index, value in enumerate(values) if value)

    def new_ids(self, baseline, name):
        if not isinstance(baseline, CoverageSnapshot):
            raise TypeError("coverage baseline is not a snapshot")
        if self.schema_id != baseline.schema_id:
            raise CoverageFormatError("coverage snapshot schemas differ")
        try:
            current = self.domains[name]
            previous = baseline.domains[name]
        except KeyError as exc:
            raise ValueError("unknown coverage domain: %s" % name) from exc
        if len(current) != len(previous):
            raise CoverageFormatError("coverage snapshot domain sizes differ")
        return tuple(
            index for index, (old, new) in enumerate(zip(previous, current))
            if new and not old
        )

    def require_monotonic_after(self, previous):
        """Reject a 1->0 transition between successive snapshots of one run."""
        if not isinstance(previous, CoverageSnapshot):
            raise TypeError("coverage baseline is not a snapshot")
        if self.schema_id != previous.schema_id:
            raise CoverageFormatError("coverage snapshot schemas differ")
        if self.build_id != previous.build_id:
            raise CoverageFormatError("coverage snapshot builds differ")
        if previous.writer_ready and not self.writer_ready:
            raise CoverageFormatError(
                "coverage snapshot writer-ready flag regressed"
            )
        for name, current in self.domains.items():
            older = previous.domains[name]
            if len(current) != len(older):
                raise CoverageFormatError(
                    "coverage snapshot domain sizes differ"
                )
            if any(old and not new for old, new in zip(older, current)):
                raise CoverageFormatError(
                    "coverage snapshot regressed from 1 to 0"
                )
        return self


def load_contract(path=DEFAULT_CONTRACT_PATH):
    try:
        with open(path, "r", encoding="utf-8") as stream:
            data = json.load(stream)
    except (OSError, ValueError) as exc:
        raise CoverageFormatError(
            "cannot read generated coverage contract %s: %s" % (path, exc)
        )
    if not isinstance(data, dict):
        raise CoverageFormatError("coverage contract root is not an object")
    if set(data) != SEMANTIC_FIELDS | DOCUMENT_FIELDS:
        missing = sorted((SEMANTIC_FIELDS | DOCUMENT_FIELDS) - set(data))
        extra = sorted(set(data) - (SEMANTIC_FIELDS | DOCUMENT_FIELDS))
        raise CoverageFormatError(
            "coverage contract fields changed (missing=%r extra=%r)" %
            (missing, extra)
        )
    if data.get("abi_version") != ABI_VERSION:
        raise CoverageFormatError("coverage ABI version changed")
    if data.get("header_size") != HEADER_SIZE:
        raise CoverageFormatError("coverage header size changed")
    if data.get("schema_hash_rule") != SCHEMA_HASH_RULE:
        raise CoverageFormatError("coverage schema hash rule changed")

    build_id = _hex64(data.get("build_id"), "build_id")
    if data.get("generation_id") != "gen:" + build_id:
        raise CoverageFormatError("coverage generation/build identity changed")
    semantic = {key: value for key, value in data.items()
                if key not in DOCUMENT_FIELDS}
    schema_id = _hex64(data.get("schema_id"), "schema_id")
    if _sha256(_canonical_json(semantic)) != schema_id:
        raise CoverageFormatError("coverage schema hash does not match records")

    function_count = _uint32(data.get("function_count"), "function_count")
    import_count = _uint32(data.get("import_count"), "import_count")
    case_count = _uint32(data.get("case_count"), "case_count")
    executable = _uint32(
        data.get("case_executable_count"), "case_executable_count",
        allow_zero=True,
    )
    blocked = _uint32(
        data.get("case_blocked_count"), "case_blocked_count", allow_zero=True
    )
    edge_count = _uint32(data.get("case_edge_count"), "case_edge_count")
    hook_count = _uint32(data.get("case_hook_count"), "case_hook_count")
    if executable + blocked != case_count:
        raise CoverageFormatError(
            "executable and blocked case counts do not cover stable case ids"
        )
    if hook_count < executable or edge_count < hook_count:
        raise CoverageFormatError("case edge/hook denominators are inconsistent")

    generated_hooks = _uint32(
        data.get("function_generated_entry_hook_count"),
        "function_generated_entry_hook_count", allow_zero=True,
    )
    manual_hooks = _uint32(
        data.get("function_manual_hook_count"),
        "function_manual_hook_count", allow_zero=True,
    )
    cross_hooks = _uint32(
        data.get("function_cross_root_hook_count"),
        "function_cross_root_hook_count", allow_zero=True,
    )
    function_hooks = _uint32(
        data.get("function_hook_count"), "function_hook_count"
    )
    if generated_hooks + manual_hooks != function_count or \
            function_hooks != function_count + cross_hooks:
        raise CoverageFormatError("function hook denominators are inconsistent")

    functions = _dense_ids(
        data.get("functions"), function_count, "functions",
        ("id", "rva", "symbol", "emitted_symbol", "manual", "stub"),
    )
    function_rvas = []
    function_rva_set = set()
    function_symbols = set()
    for record in functions:
        rva = _uint32(record["rva"], "function rva")
        symbol = _plain_string(record["symbol"], "function symbol")
        _plain_string(record["emitted_symbol"], "emitted function symbol")
        if not isinstance(record["manual"], bool) or \
                not isinstance(record["stub"], bool) or \
                (record["manual"] and record["stub"]):
            raise CoverageFormatError("invalid function kind flags")
        if rva in function_rva_set or symbol in function_symbols:
            raise CoverageFormatError("duplicate function rva/symbol")
        function_rvas.append(rva)
        function_rva_set.add(rva)
        function_symbols.add(symbol)
    if function_rvas != sorted(function_rvas):
        raise CoverageFormatError("function records are not sorted by rva")
    if sum(record["manual"] for record in functions) != manual_hooks:
        raise CoverageFormatError("manual function records/count disagree")

    imports = _dense_ids(
        data.get("imports"), import_count, "imports",
        ("id", "slot_rva", "name"),
    )
    import_slots = []
    for record in imports:
        slot = _uint32(record["slot_rva"], "import slot rva")
        _plain_string(record["name"], "import name")
        if slot in import_slots:
            raise CoverageFormatError("duplicate import slot rva")
        import_slots.append(slot)
    if import_slots != sorted(import_slots):
        raise CoverageFormatError("import records are not sorted by slot rva")

    cases = _dense_ids(
        data.get("cases"), case_count, "cases",
        ("id", "root_rva", "entry_rva", "executable",
         "edge_occurrences", "hook_occurrences"),
    )
    case_pairs = []
    case_pair_set = set()
    executable_ids = []
    for record in cases:
        root = _uint32(record["root_rva"], "case root rva")
        entry = _uint32(record["entry_rva"], "case entry rva")
        edges = _uint32(record["edge_occurrences"], "case edge occurrences")
        hooks = _uint32(
            record["hook_occurrences"], "case hook occurrences",
            allow_zero=True,
        )
        if not isinstance(record["executable"], bool):
            raise CoverageFormatError("case executable flag is not boolean")
        if record["executable"]:
            if hooks != edges:
                raise CoverageFormatError("executable case edge/hook mismatch")
            executable_ids.append(record["id"])
        elif hooks != 0:
            raise CoverageFormatError("blocked case unexpectedly has a hook")
        pair = (root, entry)
        if pair in case_pair_set:
            raise CoverageFormatError("duplicate semantic case")
        case_pairs.append(pair)
        case_pair_set.add(pair)
    if case_pairs != sorted(case_pairs):
        raise CoverageFormatError("case records are not sorted by root/entry")
    if len(executable_ids) != executable or \
            sum(record["edge_occurrences"] for record in cases) != edge_count or \
            sum(record["hook_occurrences"] for record in cases) != hook_count:
        raise CoverageFormatError("case records/counts disagree")

    blocked_ids = data.get("blocked_case_ids")
    if (not isinstance(blocked_ids, list) or
            any(not isinstance(value, int) or isinstance(value, bool)
                for value in blocked_ids) or
            blocked_ids != sorted(set(blocked_ids)) or
            blocked_ids != [record["id"] for record in cases
                            if not record["executable"]]):
        raise CoverageFormatError("blocked case id inventory changed")
    if len(blocked_ids) != blocked:
        raise CoverageFormatError("blocked case id count changed")
    blocked_cases = data.get("blocked_cases")
    if not isinstance(blocked_cases, list) or len(blocked_cases) != blocked:
        raise CoverageFormatError("blocked case record count changed")
    for index, record in enumerate(blocked_cases):
        if not isinstance(record, dict) or set(record) != {
                "id", "root_rva", "entry_rva"}:
            raise CoverageFormatError("invalid blocked case record")
        source = cases[blocked_ids[index]]
        if record != {key: source[key]
                      for key in ("id", "root_rva", "entry_rva")}:
            raise CoverageFormatError("blocked case record does not match case")

    case_edges = data.get("case_edges")
    if not isinstance(case_edges, list) or len(case_edges) != edge_count:
        raise CoverageFormatError("case edge record count changed")
    edge_rows = []
    edge_hook_rows = []
    edge_case_counts = [0] * case_count
    for record in case_edges:
        if not isinstance(record, dict) or set(record) != {
                "root_rva", "site_rva", "entry_rva", "case_id",
                "executable"}:
            raise CoverageFormatError("invalid case edge record")
        root = _uint32(record["root_rva"], "case edge root")
        site = _uint32(record["site_rva"], "case edge site")
        entry = _uint32(record["entry_rva"], "case edge entry")
        case_id = record["case_id"]
        if (not isinstance(case_id, int) or isinstance(case_id, bool) or
                not 0 <= case_id < case_count or
                not isinstance(record["executable"], bool)):
            raise CoverageFormatError("invalid case edge identity")
        source = cases[case_id]
        if ((root, entry) != (source["root_rva"], source["entry_rva"]) or
                record["executable"] != source["executable"]):
            raise CoverageFormatError("case edge does not match semantic case")
        edge_case_counts[case_id] += 1
        row = (root, site, entry)
        edge_rows.append(row)
        if record["executable"]:
            edge_hook_rows.append(row)
    if edge_rows != sorted(edge_rows) or len(set(edge_rows)) != len(edge_rows):
        raise CoverageFormatError("case edges are not sorted and unique")
    if edge_case_counts != [record["edge_occurrences"] for record in cases]:
        raise CoverageFormatError("case edge multiplicity changed")

    cross = data.get("cross_root_hooks")
    if not isinstance(cross, list) or len(cross) != cross_hooks:
        raise CoverageFormatError("cross-root hook count changed")
    cross_rows = []
    for record in cross:
        if not isinstance(record, dict) or set(record) != {
                "owner_rva", "target_rva", "function_id"}:
            raise CoverageFormatError("invalid cross-root hook record")
        owner = _uint32(record["owner_rva"], "cross-root owner")
        target = _uint32(record["target_rva"], "cross-root target")
        function_id = record["function_id"]
        if (not isinstance(function_id, int) or isinstance(function_id, bool) or
                not 0 <= function_id < function_count or
                functions[function_id]["rva"] != target):
            raise CoverageFormatError("cross-root function id/target mismatch")
        cross_rows.append((owner, target, function_id))
    if cross_rows != sorted(cross_rows) or len(set(cross_rows)) != len(cross_rows):
        raise CoverageFormatError("cross-root hooks are not sorted and unique")

    for key in (
            "function_rvas_sha256", "imports_sha256", "cases_sha256",
            "case_executable_sha256", "case_edges_sha256",
            "case_hooks_sha256", "blocked_cases_sha256",
            "cross_root_pairs_sha256", "cross_root_owners_sha256",
            "cross_root_targets_sha256"):
        _hex64(data.get(key), key)

    function_bytes = b"".join(struct.pack("<I", rva)
                              for rva in function_rvas)
    import_bytes = b"".join(
        struct.pack("<I", record["slot_rva"]) +
        record["name"].encode("utf-8") + b"\0" for record in imports
    )
    case_bytes = b"".join(struct.pack("<II", *pair) for pair in case_pairs)
    executable_bytes = b"".join(
        struct.pack("<II", record["root_rva"], record["entry_rva"])
        for record in cases if record["executable"]
    )
    edge_bytes = b"".join(struct.pack("<III", *row) for row in edge_rows)
    hook_bytes = b"".join(
        struct.pack("<III", *row) for row in edge_hook_rows
    )
    cross_pairs = [{"owner_rva": owner, "target_rva": target}
                   for owner, target, _ in cross_rows]
    expected_hashes = {
        "function_rvas_sha256": _sha256(function_bytes),
        "imports_sha256": _sha256(import_bytes),
        "cases_sha256": _sha256(case_bytes),
        "case_executable_sha256": _sha256(executable_bytes),
        "case_edges_sha256": _sha256(edge_bytes),
        "case_hooks_sha256": _sha256(hook_bytes),
        "blocked_cases_sha256": _sha256(_canonical_json(blocked_cases)),
        "cross_root_pairs_sha256": _sha256(_canonical_json(cross_pairs)),
        "cross_root_owners_sha256": _sha256(_canonical_json(
            sorted(set(owner for owner, _, _ in cross_rows)))),
        "cross_root_targets_sha256": _sha256(_canonical_json(
            sorted(set(target for _, target, _ in cross_rows)))),
    }
    if any(data[key] != value for key, value in expected_hashes.items()):
        raise CoverageFormatError("coverage record digest changed")

    contract = CoverageContract(
        schema_id=schema_id,
        build_id=build_id,
        function_count=function_count,
        import_count=import_count,
        case_count=case_count,
        case_executable_count=executable,
        case_blocked_count=blocked,
        case_edge_count=edge_count,
        case_hook_count=hook_count,
        blocked_case_ids=tuple(blocked_ids),
    )
    if contract.total_size > 0xffffffff:
        raise CoverageFormatError("coverage file does not fit the 32-bit ABI")
    return contract


def encode_header(contract, build_id=None):
    build_id = contract.build_id if build_id is None else _hex64(
        build_id, "build_id"
    )
    header = bytearray(HEADER_SIZE)
    header[MAGIC_OFFSET:MAGIC_OFFSET + len(MAGIC)] = MAGIC
    struct.pack_into(
        "<8I", header, VERSION_OFFSET,
        ABI_VERSION, HEADER_SIZE, contract.total_size, 0,
        contract.function_count, contract.import_count, contract.case_count, 0,
    )
    header[SCHEMA_ID_OFFSET:SCHEMA_ID_OFFSET + IDENTITY_BYTES] = \
        contract.schema_id.encode("ascii")
    header[BUILD_ID_OFFSET:BUILD_ID_OFFSET + IDENTITY_BYTES] = \
        build_id.encode("ascii")
    return bytes(header)


def parse_header(buffer):
    if len(buffer) < HEADER_SIZE:
        raise CoverageFormatError("coverage file is shorter than its header")
    if bytes(buffer[MAGIC_OFFSET:MAGIC_OFFSET + len(MAGIC)]) != MAGIC:
        raise CoverageFormatError("coverage magic changed")
    values = struct.unpack_from("<8I", buffer, VERSION_OFFSET)
    (version, header_size, total_size, flags, function_count, import_count,
     case_count, reserved) = values
    if version != ABI_VERSION or header_size != HEADER_SIZE:
        raise CoverageFormatError("coverage header ABI changed")
    if flags not in (0, 1) or reserved != 0:
        raise CoverageFormatError("coverage header flags/reserved fields changed")
    if total_size != HEADER_SIZE + function_count + import_count + case_count:
        raise CoverageFormatError("coverage total size is inconsistent")
    if len(buffer) != total_size:
        raise CoverageFormatError("coverage file length is inconsistent")
    try:
        schema_id = bytes(buffer[
            SCHEMA_ID_OFFSET:SCHEMA_ID_OFFSET + IDENTITY_BYTES
        ]).decode("ascii")
        build_id = bytes(buffer[
            BUILD_ID_OFFSET:BUILD_ID_OFFSET + IDENTITY_BYTES
        ]).decode("ascii")
    except UnicodeDecodeError as exc:
        raise CoverageFormatError("coverage identity is not ASCII") from exc
    _hex64(schema_id, "schema_id")
    _hex64(build_id, "build_id")
    if any(buffer[BUILD_ID_OFFSET + IDENTITY_BYTES:HEADER_SIZE]):
        raise CoverageFormatError("coverage header padding is nonzero")
    return {
        "version": version,
        "header_size": header_size,
        "total_size": total_size,
        "writer_ready": bool(flags),
        "function_count": function_count,
        "import_count": import_count,
        "case_count": case_count,
        "schema_id": schema_id,
        "build_id": build_id,
    }


def snapshot_capture(buffer, contract, require_build=True,
                     require_ready=None):
    """Copy and validate a live or archived mapping without reopening it.

    The child only changes bytes from zero to one.  Copying the three domains
    can therefore observe a slightly earlier/later combination of independent
    hits, but never invents a hit; a later poll converges.  The immutable copy
    also prevents a caller from accidentally mutating the parent-owned mmap.
    """
    if not isinstance(contract, CoverageContract):
        raise TypeError("coverage contract is not a CoverageContract")
    header = parse_header(buffer)
    if (header["schema_id"] != contract.schema_id or
            header["function_count"] != contract.function_count or
            header["import_count"] != contract.import_count or
            header["case_count"] != contract.case_count):
        raise CoverageFormatError("coverage schema/count identity changed")
    if require_build and header["build_id"] != contract.build_id:
        raise CoverageFormatError("coverage build identity changed")
    if require_ready is not None and \
            header["writer_ready"] != require_ready:
        raise CoverageFormatError(
            "coverage child writer-ready handshake is missing" if
            require_ready else
            "cumulative coverage unexpectedly has a child writer flag"
        )

    domains = {}
    for name, count in contract.counts.items():
        offset = contract.offsets[name]
        values = bytes(buffer[offset:offset + count])
        if len(values) != count or any(value not in (0, 1)
                                       for value in values):
            raise CoverageFormatError(
                "%s coverage contains a non-byte-marker value" % name
            )
        domains[name] = values
    if any(domains["cases"][case_id]
           for case_id in contract.blocked_case_ids):
        raise CoverageFormatError(
            "coverage wrote a blocked loud-stub case id"
        )
    return CoverageSnapshot(
        schema_id=header["schema_id"],
        build_id=header["build_id"],
        writer_ready=header["writer_ready"],
        functions=domains["functions"],
        imports=domains["imports"],
        cases=domains["cases"],
    )


def _sha256_file(path):
    digest = hashlib.sha256()
    with open(path, "rb") as stream:
        while True:
            block = stream.read(1024 * 1024)
            if not block:
                break
            digest.update(block)
    return digest.hexdigest()


def classify_stop(result):
    reason = result.get("stop_reason")
    output = result.get("output", b"")
    # Durable child evidence outranks the parent's later reason for stopping.
    # A host exception can print its record just before the deadline owner
    # observes timeout, and a stop-pattern can itself be a guest fault line.
    if b"host exception              :" in output:
        return "host-exception"
    if b"guarded run status          : 1 " in output or \
            b"first startup fault         :" in output:
        return "guest-fault"
    if b"guarded run status          : 2 " in output or \
            b"guest process exit          :" in output:
        return "guest-exit"
    if reason == "timeout":
        return "timeout"
    if reason == "matched":
        return "stop-pattern"
    if reason == "controller":
        return "controller"
    if reason == "controller-error":
        return "controller-error"
    returncode = result.get("returncode")
    if returncode == 0:
        return "normal"
    if isinstance(returncode, int) and \
            (returncode & 0xffffffff) >= 0xc0000000:
        return "host-exception"
    return "child-error"


def _artifact_record(value):
    fields = {
        "path", "size", "sha256", "entry_recipe_id",
        "entry_output_set_id", "generation_recipe_id",
        "object_output_set_id",
    }
    if not isinstance(value, dict) or set(value) != fields:
        raise CoverageFormatError("coverage run has no exact artifact identity")
    path = value["path"]
    size = value["size"]
    if (not isinstance(path, str) or not os.path.isabs(path) or not path or
            not isinstance(size, int) or isinstance(size, bool) or size <= 0):
        raise CoverageFormatError("coverage artifact path/size is invalid")
    clean = {"path": os.path.abspath(path), "size": size}
    for field in fields - {"path", "size"}:
        clean[field] = _hex64(value[field], "artifact.%s" % field)
    return clean


@contextmanager
def _schema_lock(schema_dir):
    """Serialize cumulative validation, OR and summary publication."""
    schema_dir = os.path.abspath(schema_dir)
    key = os.path.normcase(os.path.realpath(schema_dir))
    with _SCHEMA_THREAD_LOCKS_GUARD:
        thread_lock = _SCHEMA_THREAD_LOCKS.get(key)
        if thread_lock is None:
            thread_lock = threading.RLock()
            _SCHEMA_THREAD_LOCKS[key] = thread_lock
    # Lock order is invariant: local RLock, then the cross-process byte lock.
    # Keeping the RLock outermost is what makes four same-parent CoverageRun
    # finalizers safe without weakening independent-launcher serialization.
    with thread_lock:
        os.makedirs(schema_dir, exist_ok=True)
        path = os.path.join(schema_dir, ".cumulative.lock")
        with open(path, "a+b") as stream:
            stream.seek(0, os.SEEK_END)
            if stream.tell() == 0:
                stream.write(b"\0")
                stream.flush()
            stream.seek(0)
            msvcrt.locking(stream.fileno(), msvcrt.LK_LOCK, 1)
            try:
                yield
            finally:
                stream.seek(0)
                msvcrt.locking(stream.fileno(), msvcrt.LK_UNLCK, 1)


def read_cumulative_snapshot(contract, root=DEFAULT_COVERAGE_ROOT):
    """Return one lock-consistent aggregate snapshot without creating it."""
    if not isinstance(contract, CoverageContract):
        raise TypeError("coverage contract is not a CoverageContract")
    schema_dir = os.path.join(os.path.abspath(root), contract.schema_id)
    path = os.path.join(schema_dir, "cumulative.cov")
    with _schema_lock(schema_dir):
        if not os.path.exists(path):
            zeros = {
                name: bytes(count) for name, count in contract.counts.items()
            }
            return CoverageSnapshot(
                schema_id=contract.schema_id,
                build_id=contract.build_id,
                writer_ready=False,
                functions=zeros["functions"],
                imports=zeros["imports"],
                cases=zeros["cases"],
            )
        stream = open(path, "rb")
        try:
            if os.fstat(stream.fileno()).st_size != contract.total_size:
                raise CoverageFormatError(
                    "cumulative coverage file size changed"
                )
            mapping = mmap.mmap(
                stream.fileno(), contract.total_size,
                access=mmap.ACCESS_READ,
            )
            try:
                return snapshot_capture(
                    mapping, contract, require_build=False,
                    require_ready=False,
                )
            finally:
                mapping.close()
        finally:
            stream.close()


class CoverageRun:
    """One parent-owned coverage mapping and its immutable run archive."""

    def __init__(self, contract, root=DEFAULT_COVERAGE_ROOT,
                 timestamp_ns=None, pid=None,
                 publish_latest_stdout=True):
        if not isinstance(publish_latest_stdout, bool):
            raise TypeError("publish_latest_stdout is not boolean")
        self.contract = contract
        # GC owns the immutable per-run stdout path.  run_entry owns the
        # convenience "latest" copy and consults this explicit policy: serial
        # launches keep the historical default, batch workers disable it so
        # four same-parent finalizers cannot race one shared destination.
        self.publish_latest_stdout = publish_latest_stdout
        self.root = os.path.abspath(root)
        self.schema_dir = os.path.join(self.root, contract.schema_id)
        self.runs_dir = os.path.join(self.schema_dir, "runs")
        os.makedirs(self.runs_dir, exist_ok=True)
        if timestamp_ns is None:
            timestamp_ns = time.time_ns()
        if pid is None:
            pid = os.getpid()
        stamp = time.strftime(
            "%Y%m%dT%H%M%S", time.gmtime(timestamp_ns // 1000000000)
        )
        stem = "%s.%09dZ-p%d" % (
            stamp, timestamp_ns % 1000000000, pid
        )
        # Multiple workers in one coordinator can legitimately share both a
        # PID and a nanosecond supplied by a deterministic test/wave.  Claim a
        # stem by exclusive creation and retry with an exact ordinal.  The
        # retained .cov is the namespace reservation, so no global counter (or
        # cross-process race around one) is needed.
        ordinal = 0
        while True:
            run_stem = stem if ordinal == 0 else "%s-n%04d" % (
                stem, ordinal
            )
            coverage_path = os.path.join(self.runs_dir, run_stem + ".cov")
            try:
                run_file = open(coverage_path, "x+b")
            except FileExistsError:
                ordinal += 1
                continue
            break
        self.run_stem = run_stem
        self.coverage_path = coverage_path
        self.stdout_path = os.path.join(
            self.runs_dir, run_stem + ".stdout.log"
        )
        self.summary_path = os.path.join(self.runs_dir, run_stem + ".json")
        self.cumulative_path = os.path.join(
            self.schema_dir, "cumulative.cov"
        )
        self.cumulative_summary_path = os.path.join(
            self.schema_dir, "cumulative.json"
        )
        self.started_ns = timestamp_ns
        self._closed = False
        self._file = run_file
        try:
            self._file.truncate(contract.total_size)
            self._file.seek(0)
            self._file.write(encode_header(contract))
            self._file.flush()
            self.mapping = mmap.mmap(
                self._file.fileno(), contract.total_size,
                access=mmap.ACCESS_WRITE,
            )
        except BaseException:
            self._file.close()
            raise

    def child_environment(self, environment):
        child = dict(environment)
        child[ENABLE_ENV] = "1"
        child[FILE_ENV] = self.coverage_path
        return child

    def _domain_views(self, mapping):
        offsets = self.contract.offsets
        return {
            name: memoryview(mapping)[offsets[name]:offsets[name] + count]
            for name, count in self.contract.counts.items()
        }

    def _validate_capture(self, mapping, require_build=True,
                          require_ready=None):
        return snapshot_capture(
            mapping, self.contract, require_build=require_build,
            require_ready=require_ready,
        )

    def snapshot(self, require_ready=None):
        """Return an immutable validated copy of this run's live bitmap."""
        if self._closed:
            raise RuntimeError("coverage run is already closed")
        return snapshot_capture(
            self.mapping, self.contract, require_build=True,
            require_ready=require_ready,
        )

    def cumulative_snapshot(self):
        return read_cumulative_snapshot(self.contract, self.root)

    def _open_cumulative(self):
        if not os.path.exists(self.cumulative_path):
            with open(self.cumulative_path, "x+b") as stream:
                stream.truncate(self.contract.total_size)
                stream.seek(0)
                stream.write(encode_header(self.contract))
                stream.flush()
        stream = open(self.cumulative_path, "r+b")
        if os.fstat(stream.fileno()).st_size != self.contract.total_size:
            stream.close()
            raise CoverageFormatError("cumulative coverage file size changed")
        try:
            mapping = mmap.mmap(
                stream.fileno(), self.contract.total_size,
                access=mmap.ACCESS_WRITE,
            )
        except BaseException:
            stream.close()
            raise
        try:
            # A new build may retain the same semantic schema.  Counts and
            # schema gate OR; the per-run file, unlike this aggregate, pins the
            # exact build that wrote it.
            self._validate_capture(
                mapping, require_build=False, require_ready=False
            )
        except BaseException:
            mapping.close()
            stream.close()
            raise
        return stream, mapping

    def finish(self, result, live_stdout_path, summary_callback=None,
               callback_revoke=None):
        if self._closed:
            raise RuntimeError("coverage run was already finalized")
        if callback_revoke is not None and not callable(callback_revoke):
            raise ValueError("coverage callback revoker is not callable")
        self.mapping.flush()
        self._validate_capture(
            self.mapping, require_build=True, require_ready=True
        )
        if not os.path.isfile(live_stdout_path):
            raise CoverageFormatError(
                "coverage run has no durable stdout: %s" % live_stdout_path
            )
        if os.path.abspath(live_stdout_path) != os.path.abspath(self.stdout_path):
            shutil.copyfile(live_stdout_path, self.stdout_path)
        artifact = _artifact_record(result.get("artifact"))

        run_views = self._domain_views(self.mapping)
        run_counts = {}
        try:
            for name in ("functions", "imports", "cases"):
                run_counts[name] = sum(1 for value in run_views[name] if value)
            if run_counts["cases"] > self.contract.case_executable_count:
                raise CoverageFormatError(
                    "run case coverage exceeds executable denominator"
                )
        finally:
            for view in run_views.values():
                view.release()

        with _schema_lock(self.schema_dir):
            cumulative_file, cumulative = self._open_cumulative()
            cumulative_views = self._domain_views(cumulative)
            new_counts = {}
            cumulative_counts = {}
            run_views = self._domain_views(self.mapping)
            try:
                for name in ("functions", "imports", "cases"):
                    run_view = run_views[name]
                    cumulative_view = cumulative_views[name]
                    new_count = 0
                    for index, value in enumerate(run_view):
                        if value and not cumulative_view[index]:
                            new_count += 1
                            cumulative_view[index] = 1
                    new_counts[name] = new_count
                    cumulative_counts[name] = sum(
                        1 for value in cumulative_view if value
                    )
                if cumulative_counts["cases"] > \
                        self.contract.case_executable_count:
                    raise CoverageFormatError(
                        "cumulative cases exceed executable denominator"
                    )
                cumulative.flush()
            finally:
                for view in cumulative_views.values():
                    view.release()
                for view in run_views.values():
                    view.release()
                cumulative.close()
                cumulative_file.close()

            finished_ns = time.time_ns()
            summary = {
                "abi_version": ABI_VERSION,
                "schema_id": self.contract.schema_id,
                "build_id": self.contract.build_id,
                "started_ns": self.started_ns,
                "finished_ns": finished_ns,
                "stop_classification": classify_stop(result),
                "returncode": result.get("returncode"),
                "stop_reason": result.get("stop_reason"),
                "matched_line": result.get("matched_line"),
                "controller_error": result.get("controller_error"),
                "artifact": artifact,
                "counts": self.contract.counts,
                "case_executable_count": self.contract.case_executable_count,
                "case_blocked_count": self.contract.case_blocked_count,
                "case_edge_count": self.contract.case_edge_count,
                "case_hook_count": self.contract.case_hook_count,
                "run": run_counts,
                "new": new_counts,
                "cumulative": cumulative_counts,
                "coverage_path": self.coverage_path,
                "coverage_sha256": _sha256_file(self.coverage_path),
                "stdout_path": self.stdout_path,
                "stdout_sha256": _sha256_file(self.stdout_path),
                "cumulative_path": self.cumulative_path,
                "cumulative_sha256": _sha256_file(self.cumulative_path),
            }
            temporary = self.summary_path + ".next"
            with open(temporary, "w", encoding="utf-8", newline="\n") \
                    as stream:
                json.dump(summary, stream, sort_keys=True, indent=2)
                stream.write("\n")
            os.replace(temporary, self.summary_path)
            cumulative_summary = dict(summary)
            cumulative_summary.pop("coverage_path")
            cumulative_summary.pop("coverage_sha256")
            cumulative_summary.pop("stdout_path")
            cumulative_summary.pop("stdout_sha256")
            cumulative_temporary = self.cumulative_summary_path + ".next"
            with open(cumulative_temporary, "w", encoding="utf-8",
                      newline="\n") as stream:
                json.dump(cumulative_summary, stream, sort_keys=True, indent=2)
                stream.write("\n")
            os.replace(cumulative_temporary, self.cumulative_summary_path)
            base_cumulative_summary = copy.deepcopy(cumulative_summary)

        # Arbitrary explorer work must never run under the cumulative lock: a
        # callback naturally reading cumulative_snapshot() would deadlock.
        # Publish the base summary first, invoke outside the lock, then annotate
        # only this immutable run if the callback fails.  The aggregate summary
        # is updated only if no newer run replaced it in the meantime.
        if summary_callback is not None:
            callback_failures = []

            def invoke_summary_callback():
                try:
                    summary_callback(copy.deepcopy(summary))
                except Exception as exc:
                    callback_failures.append(
                        "%s: %s" % (type(exc).__name__, exc)
                    )

            callback_thread = threading.Thread(
                target=invoke_summary_callback,
                name="coverage-summary-callback", daemon=True,
            )
            callback_thread.start()
            callback_thread.join(timeout=3.0)
            if callback_thread.is_alive():
                callback_failures.append(
                    "RuntimeError: coverage summary callback did not finish"
                )
            if callback_failures:
                failures = []
                for failure in (
                        [summary.get("controller_error")] +
                        callback_failures):
                    if failure and failure not in failures:
                        failures.append(failure)
                summary["controller_error"] = "; ".join(failures)
                if summary["stop_classification"] not in (
                        "host-exception", "guest-fault", "guest-exit"):
                    summary["stop_classification"] = "controller-error"
                temporary = self.summary_path + ".next"
                with open(temporary, "w", encoding="utf-8", newline="\n") \
                        as stream:
                    json.dump(summary, stream, sort_keys=True, indent=2)
                    stream.write("\n")
                os.replace(temporary, self.summary_path)
                with _schema_lock(self.schema_dir):
                    try:
                        with open(self.cumulative_summary_path, "r",
                                  encoding="utf-8") as stream:
                            current_cumulative = json.load(stream)
                    except (OSError, ValueError) as read_error:
                        raise CoverageFormatError(
                            "cannot re-read cumulative summary after "
                            "controller failure: %s" % read_error
                        )
                    # started_ns is not a run identity: distinct launcher PIDs
                    # can observe the same clock tick.  Update the aggregate
                    # annotation only when the entire summary we published is
                    # still current; otherwise a newer run owns that file.
                    if current_cumulative == base_cumulative_summary:
                        cumulative_summary = dict(summary)
                        cumulative_summary.pop("coverage_path")
                        cumulative_summary.pop("coverage_sha256")
                        cumulative_summary.pop("stdout_path")
                        cumulative_summary.pop("stdout_sha256")
                        cumulative_temporary = \
                            self.cumulative_summary_path + ".next"
                        with open(cumulative_temporary, "w", encoding="utf-8",
                                  newline="\n") as stream:
                            json.dump(cumulative_summary, stream,
                                      sort_keys=True, indent=2)
                            stream.write("\n")
                        os.replace(cumulative_temporary,
                                   self.cumulative_summary_path)
        if callback_revoke is not None:
            callback_revoke()
        self.close()
        return summary

    def close(self):
        if self._closed:
            return
        self._closed = True
        try:
            self.mapping.flush()
        finally:
            self.mapping.close()
            self._file.close()


def coverage_report_lines(summary):
    counts = summary["counts"]
    run = summary["run"]
    new = summary["new"]
    cumulative = summary["cumulative"]
    lines = [
        "coverage stop         : %s" % summary["stop_classification"],
        "coverage functions    : run=%d new=%d cumulative=%d / %d" % (
            run["functions"], new["functions"], cumulative["functions"],
            counts["functions"],
        ),
        "coverage imports      : run=%d new=%d cumulative=%d / %d" % (
            run["imports"], new["imports"], cumulative["imports"],
            counts["imports"],
        ),
        "coverage cases        : run=%d new=%d cumulative=%d / %d executable"
        " (+%d blocked-by-stub)" % (
            run["cases"], new["cases"], cumulative["cases"],
            summary["case_executable_count"],
            summary["case_blocked_count"],
        ),
        "coverage archive      : %s" % summary["coverage_path"],
        "coverage summary      : %s" % (
            os.path.splitext(summary["coverage_path"])[0] + ".json"
        ),
    ]
    if summary.get("controller_error") is not None:
        lines.append(
            "coverage controller   : FAILED %s" %
            summary["controller_error"]
        )
    return lines
