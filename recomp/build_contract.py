"""Content-addressed contracts between the recompilation build stages.

This module deliberately separates *logical* names, which are portable and
may be hashed, from physical paths, which are local build-machine details.
File mtimes are never part of a recipe or an output identity.

Typical use::

    rid, r = recipe(
        "generate",
        {"functions.json": "/work/functions.json",
         "emitter.py": "/work/recomp/emit.py"},
        {"target_bytes": 1024 * 1024},
        tool_ids(),
    )
    outs = snapshot(GEN, ["guest_0000.c", "guest_funcs.h"])
    write_manifest_atomic(STAMP, make_manifest("generate", rid, r, outs))

    # A mapping is explicit about both halves of the logical/physical split.
    require_manifest(
        STAMP, "generate", rid,
        {entry["path"]: os.path.join(GEN, *entry["path"].split("/"))
         for entry in outs},
    )

``recipe`` accepts a physical path or a 64-character SHA-256 hex digest for
each input value.  Use ``digest(hex_value)`` to remove the only ambiguity (a
real file whose name itself consists of 64 hex characters).
"""

from __future__ import print_function

import hashlib
import json
import ntpath
import os
import platform
import re
import stat
import sys
import tempfile
import threading
from contextlib import contextmanager


SCHEMA = 1
_SHA256_RE = re.compile(r"^[0-9a-fA-F]{64}$")

# ``exclusive_lock`` is also used by wrappers which hold one pipeline lock
# across several legacy stages.  Those stages acquire the same byte-range
# lock internally, so remember ownership by process *and thread*: a nested
# acquisition in the owning thread is safe, while another thread in this
# process must still fail fast just like another process.
_PROCESS_LOCK_GUARD = threading.Lock()
_PROCESS_LOCK_OWNERS = {}


class BuildContractError(RuntimeError):
    """Base class for an unusable build contract."""


class ContractIOError(BuildContractError):
    """A contract or one of its files could not be read or written."""


class ManifestFormatError(BuildContractError):
    """A manifest is not well-formed schema version 1 data."""


class ContractMismatchError(BuildContractError):
    """A well-formed manifest does not describe the requested build."""


class RecipeMismatchError(ContractMismatchError):
    """The manifest was produced from a different recipe."""


class OutputMismatchError(ContractMismatchError):
    """The output set is missing, stale, corrupt, or has extra members."""


class Digest(object):
    """An explicit precomputed SHA-256 input for :func:`recipe`."""

    __slots__ = ("value",)

    def __init__(self, value):
        self.value = _normalise_digest(value, "digest")

    def __repr__(self):
        return "Digest(%r)" % self.value


def digest(value):
    """Return an explicit precomputed input digest."""

    return Digest(value)


def _normalise_digest(value, where):
    if not isinstance(value, str) or not _SHA256_RE.match(value):
        raise ManifestFormatError(
            "%s must be exactly 64 hexadecimal SHA-256 characters" % where
        )
    return value.lower()


def _hash_open_file(fh):
    h = hashlib.sha256()
    size = 0
    while True:
        block = fh.read(1024 * 1024)
        if not block:
            break
        h.update(block)
        size += len(block)
    return size, h.hexdigest()


def _file_identity(path):
    physical = os.fspath(path)
    try:
        with open(physical, "rb") as fh:
            mode = os.fstat(fh.fileno()).st_mode
            if not stat.S_ISREG(mode):
                raise ContractIOError("not a regular file: %s" % physical)
            return _hash_open_file(fh)
    except BuildContractError:
        raise
    except (OSError, TypeError, ValueError) as exc:
        raise ContractIOError("cannot hash %s: %s" % (physical, exc)) from exc


def sha256_file(path):
    """Return the lowercase SHA-256 hex digest of *path*."""

    return _file_identity(path)[1]


def _logical_name(name):
    try:
        value = os.fspath(name)
    except TypeError as exc:
        raise ManifestFormatError("logical path is not path-like: %r" % (name,)) from exc
    if isinstance(value, bytes):
        try:
            value = value.decode("utf-8")
        except UnicodeDecodeError as exc:
            raise ManifestFormatError("logical path is not UTF-8: %r" % (name,)) from exc
    if not isinstance(value, str) or not value or "\x00" in value:
        raise ManifestFormatError("invalid empty or NUL-containing logical path: %r" % value)

    # Check before slash canonicalisation so drive-relative and UNC spellings
    # cannot be smuggled into a portable recipe.
    drive, _tail = ntpath.splitdrive(value)
    if drive or value.startswith(("/", "\\")):
        raise ManifestFormatError("logical path must be relative, got %r" % value)

    parts = []
    for part in value.replace("\\", "/").split("/"):
        if part in ("", "."):
            continue
        if part == "..":
            raise ManifestFormatError("logical path escapes its root: %r" % value)
        parts.append(part)
    if not parts:
        raise ManifestFormatError("logical path has no filename: %r" % value)
    return "/".join(parts)


def _canonical_bytes(value):
    try:
        text = json.dumps(
            value,
            sort_keys=True,
            separators=(",", ":"),
            ensure_ascii=False,
            allow_nan=False,
        )
    except (TypeError, ValueError) as exc:
        raise ManifestFormatError("value is not canonical JSON data: %s" % exc) from exc
    return text.encode("utf-8")


def _json_copy(value, where="value"):
    """Validate JSON data while rejecting machine-local recipe details."""

    if value is None or isinstance(value, (bool, int, str)):
        if isinstance(value, str):
            drive, _tail = ntpath.splitdrive(value)
            if drive or value.startswith(("/", "\\")):
                raise ManifestFormatError(
                    "%s contains an absolute path; use a logical name instead: %r"
                    % (where, value)
                )
        return value
    if isinstance(value, float):
        # json.dumps(..., allow_nan=False) rejects NaN and infinities.
        _canonical_bytes(value)
        return value
    if isinstance(value, (list, tuple)):
        return [_json_copy(item, "%s[%d]" % (where, i))
                for i, item in enumerate(value)]
    if isinstance(value, dict):
        result = {}
        for key, item in value.items():
            if not isinstance(key, str):
                raise ManifestFormatError("%s has a non-string key: %r" % (where, key))
            if key.lower() in ("mtime", "mtime_ns", "modified_time", "file_mtime"):
                raise ManifestFormatError("%s.%s is an mtime and cannot enter a recipe"
                                          % (where, key))
            result[key] = _json_copy(item, "%s.%s" % (where, key))
        return result
    raise ManifestFormatError("%s is not JSON data: %r" % (where, value))


def _stage_name(stage):
    if not isinstance(stage, str) or not stage.strip():
        raise ManifestFormatError("stage must be a non-empty string")
    if stage != stage.strip():
        raise ManifestFormatError("stage must not have leading or trailing whitespace")
    return stage


def _input_digest(source, logical):
    if isinstance(source, Digest):
        return source.value
    # A PathLike is always a physical path.  A string names an existing file
    # first; only a non-existing 64-hex string is interpreted as a digest.
    if isinstance(source, os.PathLike):
        return sha256_file(source)
    if isinstance(source, str):
        if os.path.isfile(source):
            return sha256_file(source)
        if _SHA256_RE.match(source):
            return source.lower()
        raise ContractIOError(
            "recipe input %r is neither a readable file nor a SHA-256 digest: %s"
            % (logical, source)
        )
    raise ManifestFormatError(
        "recipe input %r must be a path, SHA-256 string, or Digest" % logical
    )


def recipe(stage, inputs, params=None, tools=None):
    """Return ``(recipe_id, recipe_dict)`` for a build stage.

    ``inputs`` maps portable logical names to physical files or precomputed
    digests.  Only the logical name and content digest enter the recipe.
    ``params`` and ``tools`` must be portable JSON data: absolute paths and
    mtime fields are rejected rather than quietly making a machine-local ID.
    """

    stage = _stage_name(stage)
    if not isinstance(inputs, dict):
        raise ManifestFormatError("inputs must map logical names to paths or digests")

    hashed_inputs = {}
    for supplied_name, source in inputs.items():
        logical = _logical_name(supplied_name)
        if logical in hashed_inputs:
            raise ManifestFormatError("duplicate canonical input name: %s" % logical)
        hashed_inputs[logical] = _input_digest(source, logical)

    data = {
        "stage": stage,
        "inputs": dict(sorted(hashed_inputs.items())),
        "params": _json_copy({} if params is None else params, "params"),
        "tools": _json_copy({} if tools is None else tools, "tools"),
    }
    recipe_id = hashlib.sha256(_canonical_bytes(data)).hexdigest()
    return recipe_id, data


def _path_beneath(root, logical):
    root_real = os.path.realpath(os.path.abspath(os.fspath(root)))
    physical = os.path.realpath(os.path.join(root_real, *logical.split("/")))
    try:
        common = os.path.commonpath([root_real, physical])
    except ValueError as exc:
        raise ContractIOError("output %s is on a different volume from %s"
                              % (logical, root_real)) from exc
    if os.path.normcase(common) != os.path.normcase(root_real):
        raise ContractIOError("output path escapes root: %s" % logical)
    return physical


def snapshot(root, names):
    """Snapshot *names* below *root* as sorted path/size/SHA-256 entries."""

    entries = []
    seen = set()
    try:
        iterator = iter(names)
    except TypeError as exc:
        raise ManifestFormatError("snapshot names must be iterable") from exc
    for supplied_name in iterator:
        logical = _logical_name(supplied_name)
        if logical in seen:
            raise ManifestFormatError("duplicate canonical output name: %s" % logical)
        seen.add(logical)
        physical = _path_beneath(root, logical)
        size, value = _file_identity(physical)
        entries.append({"path": logical, "size": size, "sha256": value})
    entries.sort(key=lambda entry: entry["path"])
    return entries


def _normalise_entries(entries, require_sorted=False):
    if not isinstance(entries, list):
        raise ManifestFormatError("outputs must be a list")
    clean = []
    seen = set()
    for index, entry in enumerate(entries):
        where = "outputs[%d]" % index
        if not isinstance(entry, dict):
            raise ManifestFormatError("%s must be an object" % where)
        if set(entry) != {"path", "size", "sha256"}:
            raise ManifestFormatError(
                "%s must contain exactly path, size, and sha256" % where
            )
        logical = _logical_name(entry["path"])
        if logical != entry["path"]:
            raise ManifestFormatError("%s.path is not canonical: %r"
                                      % (where, entry["path"]))
        if logical in seen:
            raise ManifestFormatError("duplicate output path: %s" % logical)
        seen.add(logical)
        size = entry["size"]
        if isinstance(size, bool) or not isinstance(size, int) or size < 0:
            raise ManifestFormatError("%s.size must be a non-negative integer" % where)
        value = _normalise_digest(entry["sha256"], "%s.sha256" % where)
        clean.append({"path": logical, "size": size, "sha256": value})
    ordered = sorted(clean, key=lambda entry: entry["path"])
    if require_sorted and clean != ordered:
        raise ManifestFormatError("outputs must be sorted by canonical path")
    return ordered


def set_id(entries):
    """Return the order-independent identity of an exact output set."""

    ordered = _normalise_entries(entries)
    return hashlib.sha256(_canonical_bytes(ordered)).hexdigest()


def make_manifest(stage, recipe_id, recipe_dict, outputs):
    """Construct (but do not write) a schema-1 manifest."""

    stage = _stage_name(stage)
    recipe_id = _normalise_digest(recipe_id, "recipe_id")
    if not isinstance(recipe_dict, dict):
        raise ManifestFormatError("recipe must be an object")
    # Round-trip through canonical JSON so callers cannot mutate the manifest
    # through a shared list/dict after it has been constructed.
    try:
        recipe_copy = json.loads(_canonical_bytes(recipe_dict).decode("utf-8"))
    except (json.JSONDecodeError, UnicodeError) as exc:  # pragma: no cover
        raise ManifestFormatError("recipe is not valid JSON: %s" % exc) from exc
    actual_recipe_id = hashlib.sha256(_canonical_bytes(recipe_copy)).hexdigest()
    if actual_recipe_id != recipe_id:
        raise RecipeMismatchError(
            "recipe_id %s does not match embedded recipe %s"
            % (recipe_id, actual_recipe_id)
        )
    if recipe_copy.get("stage") != stage:
        raise RecipeMismatchError(
            "embedded recipe stage %r does not match manifest stage %r"
            % (recipe_copy.get("stage"), stage)
        )
    ordered = _normalise_entries(outputs)
    return {
        "schema": SCHEMA,
        "stage": stage,
        "recipe_id": recipe_id,
        "recipe": recipe_copy,
        "outputs": ordered,
        "output_set_id": set_id(ordered),
    }


def write_manifest_atomic(path, manifest):
    """Atomically replace *path* with canonical UTF-8 JSON *manifest*."""

    physical = os.path.abspath(os.fspath(path))
    directory = os.path.dirname(physical) or os.curdir
    try:
        os.makedirs(directory, exist_ok=True)
    except OSError as exc:
        raise ContractIOError("cannot create manifest directory %s: %s"
                              % (directory, exc)) from exc

    payload = _canonical_bytes(manifest) + b"\n"
    fd = None
    temporary = None
    try:
        fd, temporary = tempfile.mkstemp(
            prefix=".%s." % os.path.basename(physical), suffix=".tmp", dir=directory
        )
        with os.fdopen(fd, "wb") as fh:
            fd = None
            fh.write(payload)
            fh.flush()
            os.fsync(fh.fileno())
        os.replace(temporary, physical)
        temporary = None
    except (OSError, TypeError, ValueError) as exc:
        raise ContractIOError("cannot atomically write manifest %s: %s"
                              % (physical, exc)) from exc
    finally:
        if fd is not None:
            try:
                os.close(fd)
            except OSError:
                pass
        if temporary is not None:
            try:
                os.unlink(temporary)
            except OSError:
                pass


@contextmanager
def exclusive_lock(path):
    """Hold a non-blocking, process-wide file lock for a pipeline interval.

    All mutating stages use the same lock path.  A crash releases the OS lock;
    the small file may remain and is intentionally harmless.  Failing instead
    of waiting prevents two agents from silently compiling a mixed generation.
    The owning thread may nest the same canonical path so a whole-pipeline
    wrapper can cover legacy stages which still lock themselves.
    """

    physical = os.path.abspath(os.fspath(path))
    identity = os.path.normcase(os.path.realpath(physical))
    owner = (os.getpid(), threading.get_ident())
    nested = False
    with _PROCESS_LOCK_GUARD:
        state = _PROCESS_LOCK_OWNERS.get(identity)
        # A fork inherits Python memory but is a different lock owner.  Never
        # let the copied registry turn into an unlocked child-side re-entry.
        if state is not None and state[0] != owner[0]:
            del _PROCESS_LOCK_OWNERS[identity]
            state = None
        if state is not None:
            if tuple(state[:2]) != owner:
                raise ContractIOError(
                    "another recompilation stage holds %s" % physical
                )
            state[2] += 1
            nested = True
        else:
            _PROCESS_LOCK_OWNERS[identity] = [owner[0], owner[1], 1]

    if nested:
        try:
            yield
        finally:
            with _PROCESS_LOCK_GUARD:
                state = _PROCESS_LOCK_OWNERS.get(identity)
                if state is not None and tuple(state[:2]) == owner:
                    state[2] -= 1
        return

    directory = os.path.dirname(physical) or os.curdir
    fh = None
    locked = False
    try:
        try:
            os.makedirs(directory, exist_ok=True)
            fh = open(physical, "a+b")
        except OSError as exc:
            raise ContractIOError("cannot open pipeline lock %s: %s"
                                  % (physical, exc)) from exc
        fh.seek(0, os.SEEK_END)
        if fh.tell() == 0:
            fh.write(b"\0")
            fh.flush()
        fh.seek(0)
        try:
            if os.name == "nt":
                import msvcrt
                msvcrt.locking(fh.fileno(), msvcrt.LK_NBLCK, 1)
            else:  # pragma: no cover - the production host is Windows.
                import fcntl
                fcntl.flock(fh.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
            locked = True
        except (OSError, IOError) as exc:
            raise ContractIOError(
                "another recompilation stage holds %s" % physical
            ) from exc
        yield
    finally:
        if locked and fh is not None:
            try:
                fh.seek(0)
                if os.name == "nt":
                    import msvcrt
                    msvcrt.locking(fh.fileno(), msvcrt.LK_UNLCK, 1)
                else:  # pragma: no cover
                    import fcntl
                    fcntl.flock(fh.fileno(), fcntl.LOCK_UN)
            except OSError:
                pass
        try:
            if fh is not None:
                fh.close()
        finally:
            with _PROCESS_LOCK_GUARD:
                state = _PROCESS_LOCK_OWNERS.get(identity)
                if state is not None and tuple(state[:2]) == owner:
                    del _PROCESS_LOCK_OWNERS[identity]


def load_manifest(path):
    """Load a manifest JSON object, with contract-specific diagnostics."""

    physical = os.fspath(path)
    try:
        with open(physical, "r", encoding="utf-8") as fh:
            data = json.load(fh)
    except FileNotFoundError as exc:
        raise ContractIOError("manifest does not exist: %s" % physical) from exc
    except json.JSONDecodeError as exc:
        raise ManifestFormatError(
            "manifest %s is invalid JSON at line %d column %d: %s"
            % (physical, exc.lineno, exc.colno, exc.msg)
        ) from exc
    except (OSError, UnicodeError) as exc:
        raise ContractIOError("cannot read manifest %s: %s" % (physical, exc)) from exc
    if not isinstance(data, dict):
        raise ManifestFormatError("manifest root must be an object: %s" % physical)
    return data


def _snapshot_output_mapping(output_paths):
    if not isinstance(output_paths, dict):
        raise ManifestFormatError(
            "output_paths must map logical output names to physical files"
        )
    entries = []
    seen = set()
    for supplied_name, physical in output_paths.items():
        logical = _logical_name(supplied_name)
        if logical in seen:
            raise ManifestFormatError("duplicate canonical output name: %s" % logical)
        seen.add(logical)
        size, value = _file_identity(physical)
        entries.append({"path": logical, "size": size, "sha256": value})
    entries.sort(key=lambda entry: entry["path"])
    return entries


def require_manifest(path, stage, expected_recipe_id, output_paths):
    """Load and fully validate one build-stage manifest.

    ``output_paths`` is a mapping from the expected logical output name to its
    current physical file.  Its keys must be the manifest's *exact* output set;
    missing or additional keys fail before content hashes are compared.
    Returns the validated manifest, otherwise raises a specific
    :class:`BuildContractError` subclass.
    """

    expected_stage = _stage_name(stage)
    expected_recipe_id = _normalise_digest(expected_recipe_id,
                                            "expected_recipe_id")
    data = load_manifest(path)
    required_keys = {
        "schema", "stage", "recipe_id", "recipe", "outputs", "output_set_id"
    }
    if set(data) != required_keys:
        missing = sorted(required_keys - set(data))
        extra = sorted(set(data) - required_keys)
        raise ManifestFormatError(
            "manifest fields differ from schema 1 (missing=%r, extra=%r)"
            % (missing, extra)
        )
    if isinstance(data["schema"], bool) or data["schema"] != SCHEMA:
        raise ManifestFormatError(
            "unsupported manifest schema %r (expected %d)"
            % (data["schema"], SCHEMA)
        )
    if data["stage"] != expected_stage:
        raise ContractMismatchError(
            "manifest stage is %r, expected %r" % (data["stage"], expected_stage)
        )

    stored_recipe_id = _normalise_digest(data["recipe_id"], "recipe_id")
    if stored_recipe_id != expected_recipe_id:
        raise RecipeMismatchError(
            "manifest recipe is %s, expected %s"
            % (stored_recipe_id, expected_recipe_id)
        )
    if not isinstance(data["recipe"], dict):
        raise ManifestFormatError("manifest recipe must be an object")
    embedded_id = hashlib.sha256(_canonical_bytes(data["recipe"])).hexdigest()
    if embedded_id != stored_recipe_id:
        raise RecipeMismatchError(
            "embedded recipe hashes to %s, manifest records %s"
            % (embedded_id, stored_recipe_id)
        )
    if data["recipe"].get("stage") != expected_stage:
        raise RecipeMismatchError(
            "embedded recipe stage is %r, expected %r"
            % (data["recipe"].get("stage"), expected_stage)
        )

    stored_outputs = _normalise_entries(data["outputs"], require_sorted=True)
    stored_set_id = _normalise_digest(data["output_set_id"], "output_set_id")
    computed_set_id = set_id(stored_outputs)
    if stored_set_id != computed_set_id:
        raise OutputMismatchError(
            "manifest output_set_id is %s, entries hash to %s"
            % (stored_set_id, computed_set_id)
        )

    actual_outputs = _snapshot_output_mapping(output_paths)
    stored_names = [entry["path"] for entry in stored_outputs]
    actual_names = [entry["path"] for entry in actual_outputs]
    if actual_names != stored_names:
        missing = sorted(set(stored_names) - set(actual_names))
        extra = sorted(set(actual_names) - set(stored_names))
        raise OutputMismatchError(
            "output set differs from manifest (missing=%r, extra=%r)"
            % (missing, extra)
        )
    if actual_outputs != stored_outputs:
        stored_by_name = {entry["path"]: entry for entry in stored_outputs}
        changed = []
        for actual in actual_outputs:
            previous = stored_by_name[actual["path"]]
            differences = []
            if actual["size"] != previous["size"]:
                differences.append("size %d != %d"
                                   % (actual["size"], previous["size"]))
            if actual["sha256"] != previous["sha256"]:
                differences.append("sha256 %s != %s"
                                   % (actual["sha256"], previous["sha256"]))
            if differences:
                changed.append("%s (%s)" % (actual["path"], ", ".join(differences)))
        raise OutputMismatchError("output content differs: %s" % "; ".join(changed))

    return data


def _package_version(distribution, module_name):
    try:
        try:
            from importlib import metadata
        except ImportError:  # Python 3.7 with the importlib_metadata backport.
            import importlib_metadata as metadata  # type: ignore
        return metadata.version(distribution)
    except Exception:  # Package metadata is optional in embedded Python builds.
        try:
            module = __import__(module_name)
            value = getattr(module, "__version__", None)
            if value is not None:
                return str(value)
        except Exception:
            pass
    return "missing"


def tool_ids():
    """Return portable IDs for the Python/disassembler/PE toolchain."""

    return {
        "python": "%s-%d.%d" % (
            platform.python_implementation().lower(),
            sys.version_info[0],
            sys.version_info[1],
        ),
        "capstone": _package_version("capstone", "capstone"),
        "pefile": _package_version("pefile", "pefile"),
    }


__all__ = [
    "SCHEMA",
    "BuildContractError",
    "ContractIOError",
    "ManifestFormatError",
    "ContractMismatchError",
    "RecipeMismatchError",
    "OutputMismatchError",
    "Digest",
    "digest",
    "sha256_file",
    "recipe",
    "snapshot",
    "set_id",
    "make_manifest",
    "write_manifest_atomic",
    "exclusive_lock",
    "load_manifest",
    "require_manifest",
    "tool_ids",
]
