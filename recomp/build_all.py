"""Compile every generated translation unit and report the full-build baseline.

The whole program is now a regular regression step. It measures the three
quantities that matter when changing batching, the emitter, or optimisation:

  * wall-clock time for a full build,
  * peak RSS per compiler process (the NFS spike saw 337 MB on a 1.4 MB unit,
    and 16 of those in parallel is 5 GB),
  * the size of the resulting object code, which on a 512 MB console is a
    first-class constraint rather than a curiosity.

Mechanics that are deliberate, each from a rule that cost time before:

  * **Never pipe a build into a filter** -- the pipeline's exit status becomes
    the filter's, so a failed compile reports success. Output goes to a file
    and the return code is kept.
  * **Resolve `cl.exe` to a full path.** `subprocess` on Windows searches the
    *parent's* PATH, not the `env` handed to it, so passing the MSVC
    environment is not enough.
  * **Reuse is content-proved, never timestamp-based.** A generated C file may
    reuse its prior object only when its own hash and the compile-wide basis
    (headers, flags, compiler, versions, and command code) are unchanged.
  * MSVC speaks the host language; decode as cp866 before reading errors.
"""
import concurrent.futures as cf
import hashlib
import os
import subprocess
import sys
import time

import build_contract as BC
import gen_all as G

_DEFAULT_WORKDIR = os.path.abspath(os.environ.get(
    "REPENTOGXM_WORKDIR",
    os.path.join(os.path.dirname(__file__), "..", "build", "recompiler"),
))
GEN = os.path.join(_DEFAULT_WORKDIR, "gen")
RUNTIME = os.path.join(os.path.dirname(os.path.abspath(__file__)), "runtime")
OBJ = os.path.join(_DEFAULT_WORKDIR, "obj")
LOG = os.path.join(_DEFAULT_WORKDIR, "build_all.log")
MANIFEST = os.path.join(OBJ, "manifest.json")
INCREMENTAL_SCHEMA = 1

WARNING_FLAGS = ["/wd4101", "/wd4102", "/wd4244", "/wd4267", "/wd4700"]
TOOL_VERSION_KEYS = (
    "VCTOOLSVERSION", "WINDOWSSDKVERSION", "UCRTVERSION",
    "VISUALSTUDIOVERSION", "VSCMD_ARG_HOST_ARCH", "VSCMD_ARG_TGT_ARCH",
)
DIRECT_STANDARD_HEADERS = ("math.h", "setjmp.h", "stdint.h", "string.h")


def compile_flags(opt):
    return ["/nologo", "/c", opt, "/GS-", "/fp:strict",
            "/DGUEST_STACK_REQUIRED=1"] + WARNING_FLAGS


def recipe_flag(flag):
    """Portable flag spelling for a recipe (not a slash-rooted host path)."""
    return flag[1:] if flag.startswith(("/", "-")) else flag


def toolchain_components(cl, include_link=False):
    """Files that actually turn C into COFF (and optionally link it)."""
    tool_dir = os.path.dirname(cl)
    names = ["cl.exe", "c1.dll", "c2.dll"]
    if include_link:
        names.append("link.exe")
    result = {}
    for name in names:
        path = os.path.join(tool_dir, name)
        if not os.path.isfile(path):
            raise BC.ContractIOError("MSVC component is missing: %s" % path)
        result["tool/msvc-" + name] = path
    return result


def toolchain_versions(env):
    """Portable version/architecture labels for headers and default libs."""
    return {
        key.lower(): str(env.get(key, "missing")).rstrip("\\/")
        for key in TOOL_VERSION_KEYS
    }


def include_search_dirs(env):
    """Return the ordered, normalised MSVC INCLUDE search directories."""
    raw = env.get("INCLUDE")
    if not isinstance(raw, str) or not raw.strip():
        raise BC.ContractIOError("MSVC INCLUDE search path is missing")
    result = []
    for supplied in raw.split(os.pathsep):
        value = supplied.strip()
        if len(value) >= 2 and value[0] == value[-1] == '"':
            value = value[1:-1]
        if not value:
            continue
        result.append(os.path.normcase(os.path.normpath(os.path.abspath(value))))
    if not result:
        raise BC.ContractIOError("MSVC INCLUDE search path has no directories")
    return result


def include_basis_inputs(env):
    """Pin INCLUDE order and direct standard headers generated C consumes.

    Absolute search paths cannot enter a portable recipe, so their normalised
    ordered spelling is represented by a digest.  The first matching direct
    header in INCLUDE order is then content-hashed by the recipe.
    """
    directories = include_search_dirs(env)
    search_bytes = "\0".join(directories).encode("utf-8")
    result = {
        "tool/include-search-order": BC.digest(
            hashlib.sha256(search_bytes).hexdigest()
        )
    }
    for name in DIRECT_STANDARD_HEADERS:
        physical = next(
            (os.path.join(directory, name) for directory in directories
             if os.path.isfile(os.path.join(directory, name))),
            None,
        )
        if physical is None:
            raise BC.ContractIOError(
                "MSVC direct standard header is missing from INCLUDE: %s" % name
            )
        result["tool/direct-header/" + name] = physical
    return result


def compile_basis(generation_manifest, opt, cl, env):
    """Identity of every input shared by all generated C compilations.

    Generated C files are deliberately absent: their individual hashes live in
    :func:`incremental_provenance`.  Any generated non-C output is conservative
    compile-wide state (today both such outputs are headers), so a change there
    invalidates every object rather than trying to infer include dependencies.
    """
    inputs = {
        "runtime/guest.h": os.path.join(RUNTIME, "guest.h"),
        "source/recomp/build_all.py": __file__,
        "source/recomp/build_contract.py": BC.__file__,
    }
    for entry in generation_manifest["outputs"]:
        if not entry["path"].endswith(".c"):
            inputs["upstream/generated/" + entry["path"]] = BC.digest(
                entry["sha256"]
            )
    # Compiler/SDK versions, exact INCLUDE order, and direct standard-header
    # content jointly define the compile-wide environment.
    inputs.update(include_basis_inputs(env))
    inputs.update(toolchain_components(cl))
    return BC.recipe(
        "object-compile-basis",
        inputs,
        {
            "opt": recipe_flag(opt),
            "flags": [recipe_flag(flag) for flag in compile_flags(opt)],
            "target": "msvc-x86",
            "command_shape": [
                "cl", "flags", "-I runtime", "-I generated",
                "-Fo temporary-object", "generated-source",
            ],
        },
        dict({"compiler": "msvc-cl"}, **toolchain_versions(env)),
    )


def incremental_provenance(generation_manifest, opt, cl, env):
    """Return portable per-unit provenance embedded in a successful manifest."""
    basis_id, basis = compile_basis(generation_manifest, opt, cl, env)
    units = []
    seen_objects = set()
    for entry in generation_manifest["outputs"]:
        if not entry["path"].endswith(".c"):
            continue
        obj = os.path.splitext(os.path.basename(entry["path"]))[0] + ".obj"
        if obj in seen_objects:
            raise BC.OutputMismatchError("duplicate object name: %s" % obj)
        seen_objects.add(obj)
        units.append({
            "source": entry["path"],
            "source_sha256": entry["sha256"],
            "object": obj,
        })
    units.sort(key=lambda item: item["source"])
    return {
        "schema": INCREMENTAL_SCHEMA,
        "basis_id": basis_id,
        "basis": basis,
        "units": units,
    }


def object_recipe(generation_manifest, opt, cl, env=None):
    if env is None:
        env, discovered_cl = msvc_env()
        if os.path.normcase(os.path.abspath(discovered_cl)) != os.path.normcase(
                os.path.abspath(cl)):
            raise BC.ContractMismatchError(
                "requested compiler differs from the current MSVC environment"
            )
    inputs = {
        "upstream/generate.output_set": BC.digest(
            generation_manifest["output_set_id"]
        ),
        "runtime/guest.h": os.path.join(RUNTIME, "guest.h"),
        "source/recomp/build_all.py": __file__,
        "source/recomp/build_contract.py": BC.__file__,
    }
    inputs.update(toolchain_components(cl))
    incremental = incremental_provenance(
        generation_manifest, opt, cl, env
    )
    return BC.recipe(
        "objects",
        inputs,
        {
            "opt": recipe_flag(opt),
            "flags": [recipe_flag(flag) for flag in compile_flags(opt)],
            "target": "msvc-x86",
            "incremental": incremental,
        },
        dict({"compiler": "msvc-cl"}, **toolchain_versions(env)),
    )


def generated_sources(generation_manifest):
    return [os.path.join(GEN, entry["path"])
            for entry in generation_manifest["outputs"]
            if entry["path"].endswith(".c")]


def object_paths(generation_manifest):
    result = {}
    for src in generated_sources(generation_manifest):
        name = os.path.splitext(os.path.basename(src))[0] + ".obj"
        if name in result:
            raise BC.OutputMismatchError("duplicate object name: %s" % name)
        result[name] = os.path.join(OBJ, name)
    return result


def _validated_success_manifest(raw):
    """Validate a prior manifest without requiring its now-old generation."""
    required = {
        "schema", "stage", "recipe_id", "recipe", "outputs", "output_set_id"
    }
    if not isinstance(raw, dict) or set(raw) != required:
        raise BC.ManifestFormatError("previous object manifest fields are invalid")
    if type(raw["schema"]) is not int or raw["schema"] != BC.SCHEMA:
        raise BC.ManifestFormatError("previous object manifest schema is invalid")
    if raw["stage"] != "objects":
        raise BC.ContractMismatchError("previous manifest is not an object manifest")
    rebuilt = BC.make_manifest(
        "objects", raw["recipe_id"], raw["recipe"], raw["outputs"]
    )
    if rebuilt != raw:
        raise BC.ManifestFormatError(
            "previous object manifest is not canonical or has a bad output set"
        )
    return raw


def _validated_incremental(value):
    """Return ``(basis_id, units_by_source)`` for incremental metadata."""
    if not isinstance(value, dict) or set(value) != {
            "schema", "basis_id", "basis", "units"}:
        raise BC.ManifestFormatError("incremental provenance fields are invalid")
    if isinstance(value["schema"], bool) or value["schema"] != INCREMENTAL_SCHEMA:
        raise BC.ManifestFormatError("unsupported incremental provenance schema")
    basis = value["basis"]
    if not isinstance(basis, dict) or set(basis) != {
            "stage", "inputs", "params", "tools"}:
        raise BC.ManifestFormatError("incremental compile basis is invalid")
    if basis["stage"] != "object-compile-basis":
        raise BC.ManifestFormatError("incremental compile basis stage is invalid")
    if not isinstance(basis["inputs"], dict):
        raise BC.ManifestFormatError("incremental compile basis inputs are invalid")
    check_id, check_basis = BC.recipe(
        basis["stage"],
        {name: BC.digest(value)
         for name, value in basis["inputs"].items()},
        basis["params"], basis["tools"],
    )
    if check_basis != basis or check_id != value["basis_id"]:
        raise BC.RecipeMismatchError("incremental compile basis hash is invalid")

    units = value["units"]
    if not isinstance(units, list):
        raise BC.ManifestFormatError("incremental units must be a list")
    by_source = {}
    seen_objects = set()
    for unit in units:
        if not isinstance(unit, dict) or set(unit) != {
                "source", "source_sha256", "object"}:
            raise BC.ManifestFormatError("incremental unit fields are invalid")
        source = unit["source"]
        obj = unit["object"]
        if (not isinstance(source, str) or not source.endswith(".c") or
                not isinstance(obj, str) or not obj.endswith(".obj")):
            raise BC.ManifestFormatError("incremental unit paths are invalid")
        source_hash = BC.digest(unit["source_sha256"]).value
        if source in by_source or obj in seen_objects:
            raise BC.ManifestFormatError("duplicate incremental source or object")
        seen_objects.add(obj)
        by_source[source] = {
            "source": source, "source_sha256": source_hash, "object": obj,
        }
    if units != sorted(units, key=lambda item: item["source"]):
        raise BC.ManifestFormatError("incremental units are not sorted")
    return value["basis_id"], by_source


def select_reusable_objects(current_incremental, previous_manifest,
                            physical_objects):
    """Select content-proved old objects; malformed old state is a cache miss.

    Invalid *current* metadata is an implementation error and raises.  Invalid
    prior state merely returns an empty set, because rebuilding everything is
    always the safe fallback.
    """
    current_basis, current_units = _validated_incremental(current_incremental)
    try:
        previous = _validated_success_manifest(previous_manifest)
        old_value = previous["recipe"]["params"]["incremental"]
        old_basis, old_units = _validated_incremental(old_value)
    except (BC.BuildContractError, KeyError, TypeError) as exc:
        return set(), "previous manifest has no usable provenance: %s" % exc
    if old_basis != current_basis:
        return set(), "compile-wide basis changed"

    old_outputs = {entry["path"]: entry for entry in previous["outputs"]}
    reused = set()
    rejected = 0
    for source, current in current_units.items():
        old = old_units.get(source)
        if (old is None or old["source_sha256"] != current["source_sha256"] or
                old["object"] != current["object"]):
            rejected += 1
            continue
        obj = current["object"]
        expected = old_outputs.get(obj)
        physical = physical_objects.get(obj)
        if expected is None or physical is None:
            rejected += 1
            continue
        try:
            if (os.path.getsize(physical) != expected["size"] or
                    BC.sha256_file(physical) != expected["sha256"]):
                rejected += 1
                continue
        except (OSError, BC.BuildContractError):
            rejected += 1
            continue
        reused.add(obj)
    return reused, "%d content matches, %d changed/missing" % (
        len(reused), rejected
    )


def require_object_manifest(generation_manifest, cl, env=None):
    raw = BC.load_manifest(MANIFEST)
    try:
        opt_name = raw["recipe"]["params"]["opt"]
    except (KeyError, TypeError):
        raise BC.ManifestFormatError("object manifest has no compile opt")
    if not isinstance(opt_name, str) or not opt_name.startswith("O"):
        raise BC.ManifestFormatError("invalid object optimisation mode")
    opt = "/" + opt_name
    recipe_id, _ = object_recipe(generation_manifest, opt, cl, env)
    return BC.require_manifest(
        MANIFEST, "objects", recipe_id, object_paths(generation_manifest)
    )


def msvc_env():
    from setuptools import msvc
    raw = msvc.msvc14_get_vc_env("x86")

    # setuptools 65 lower-cases vcvarsall's output.  On this host vcvarsall
    # prints both a correct `PATH=` and an inherited stale `path=`; folding the
    # keys lets the latter overwrite the former.  VCToolsInstallDir survives
    # and identifies the toolchain unambiguously, so resolve cl.exe from it
    # first and only use PATH as a fallback.
    raw_ci = {k.upper(): v for k, v in raw.items()}
    out = {k.upper(): v for k, v in os.environ.items()}
    for k, v in raw_ci.items():
        if k != "PATH":
            out[k] = v

    candidates = []
    vc_tools = raw_ci.get("VCTOOLSINSTALLDIR")
    if vc_tools:
        candidates.extend([
            os.path.join(vc_tools, "bin", "HostX86", "x86", "cl.exe"),
            os.path.join(vc_tools, "bin", "HostX64", "x86", "cl.exe"),
        ])
    search_path = raw_ci.get("PATH", "") + os.pathsep + out.get("PATH", "")
    candidates.extend(os.path.join(d, "cl.exe")
                      for d in search_path.split(os.pathsep) if d)
    cl = next((path for path in candidates if os.path.isfile(path)), None)
    if cl is None:
        raise SystemExit("cl.exe not found via VCToolsInstallDir or MSVC PATH")
    tool_dir = os.path.dirname(cl)
    out["PATH"] = tool_dir + os.pathsep + search_path
    # cl.exe and link.exe implicitly consume these variables as additional
    # command-line arguments.  Keeping inherited values would make the actual
    # build differ from the explicit flags recorded in the recipe.
    for injected in ("CL", "_CL_", "LINK", "_LINK_"):
        out.pop(injected, None)
    return out, cl


def compile_one(args):
    cl, env, src, opt = args
    obj = os.path.join(OBJ, os.path.splitext(os.path.basename(src))[0] + ".obj")
    temporary = obj + ".%d.next.obj" % os.getpid()
    if os.path.exists(temporary):
        os.remove(temporary)
    cmd = ([cl] + compile_flags(opt) +
           ["/I", RUNTIME, "/I", GEN, "/Fo" + temporary, src])
    t0 = time.time()
    p = subprocess.run(cmd, env=env, stdout=subprocess.PIPE,
                       stderr=subprocess.STDOUT)
    dt = time.time() - t0
    txt = p.stdout.decode("cp866", errors="replace")
    rc = p.returncode
    if rc == 0 and not os.path.exists(temporary):
        rc = 2
        txt += "\ncompiler returned success but produced no temporary object\n"
    if rc == 0:
        os.replace(temporary, obj)
    elif os.path.exists(temporary):
        os.remove(temporary)
    size = os.path.getsize(obj) if rc == 0 else 0
    return (os.path.basename(src), rc, dt, size, txt)


def _main():
    os.makedirs(OBJ, exist_ok=True)
    env, cl = msvc_env()
    print("compiler : %s" % cl)

    try:
        generation_manifest = G.require_generation_manifest()
    except BC.BuildContractError as exc:
        print("generation contract FAILED: %s" % exc)
        print("run funcs.py, bounds.py, and gen_all.py before compiling")
        return 2
    srcs = generated_sources(generation_manifest)
    total_src = sum(os.path.getsize(s) for s in srcs)
    print("units    : %d   (%.1f MB of C)" % (len(srcs), total_src / 1048576.0))

    # Optimisation level is an argument because the level that matters is not
    # the same at every stage. /O2 is for the shipped ARM build; the PC
    # verification milestone only needs the code to be correct, and two units
    # cost 12 and 6.5 minutes at /O2 against a median under 3 seconds. Paying
    # 13 minutes per iteration to verify semantics is paying for the wrong
    # thing.
    opt = "/O2"
    args = [a for a in sys.argv[1:]]
    for a in list(args):
        if a.startswith("/O") or a.startswith("-O"):
            opt = "/" + a.lstrip("/-")
            args.remove(a)
    total_units = len(srcs)
    if not total_units:
        print("generation contract contains no C translation units")
        return 2
    limit = int(args[0]) if args else total_units
    srcs = srcs[:limit]
    if limit < total_units:
        print("building the first %d units only" % limit)
    else:
        print("building all %d units" % total_units)

    jobs = os.cpu_count() or 4
    print("optimise : %s" % opt)
    print("parallel : %d\n" % jobs)

    recipe_id, recipe_data = object_recipe(generation_manifest, opt, cl, env)
    current_incremental = recipe_data["params"]["incremental"]
    previous_manifest = None
    if os.path.exists(MANIFEST):
        try:
            previous_manifest = BC.load_manifest(MANIFEST)
        except BC.BuildContractError as exc:
            print("reuse     : 0 (cannot read previous manifest: %s)" % exc)
    try:
        if previous_manifest is None:
            reusable, reuse_reason = set(), "no previous successful manifest"
        else:
            reusable, reuse_reason = select_reusable_objects(
                current_incremental, previous_manifest,
                object_paths(generation_manifest),
            )
    except BC.BuildContractError as exc:
        print("current incremental contract FAILED: %s" % exc)
        return 2

    selected_objects = {
        os.path.splitext(os.path.basename(src))[0] + ".obj" for src in srcs
    }
    reusable &= selected_objects
    compile_srcs = [
        src for src in srcs
        if os.path.splitext(os.path.basename(src))[0] + ".obj" not in reusable
    ]
    print("reuse     : %d/%d (%s)" % (len(reusable), len(srcs), reuse_reason))

    # Invalidate first. A partial or failed build may leave valid old objects,
    # but no linker is allowed to accept a mixed set without this completion
    # record being recreated after every expected object succeeds.
    if os.path.exists(MANIFEST):
        os.remove(MANIFEST)

    t0 = time.time()
    ok = fail = 0
    obj_bytes = sum(os.path.getsize(os.path.join(OBJ, name))
                    for name in reusable)
    slowest = []
    failures = []
    with open(LOG, "w", encoding="utf-8") as log:
        log.write("=== reuse: %d/%d (%s)\n" %
                  (len(reusable), len(srcs), reuse_reason))
        with cf.ThreadPoolExecutor(max_workers=jobs) as ex:
            futs = [ex.submit(compile_one, (cl, env, s, opt))
                    for s in compile_srcs]
            for i, fu in enumerate(cf.as_completed(futs)):
                name, rc, dt, size, txt = fu.result()
                log.write("=== %s  rc=%d  %.1fs  %d B\n%s\n" % (name, rc, dt,
                                                                size, txt))
                if rc == 0:
                    ok += 1
                    obj_bytes += size
                else:
                    fail += 1
                    failures.append((name, txt))
                slowest.append((dt, name, size))
                if (i + 1) % 20 == 0:
                    print("   ... %d/%d  (%.0f s, %d ok, %d failed)"
                          % (i + 1, len(compile_srcs), time.time() - t0,
                             ok, fail))

    dt = time.time() - t0
    print("\n=== build complete in %.0f s (%.1f min) ===" % (dt, dt / 60.0))
    print("units reused    : %d" % len(reusable))
    print("units compiled  : %d" % ok)
    print("units FAILED    : %d" % fail)
    print("object bytes    : %d  (%.1f MB)" % (obj_bytes, obj_bytes / 1048576.0))
    if ok:
        print("mean per unit   : %.1f s" % (sum(d for d, _, _ in slowest) / len(slowest)))
        slowest.sort(reverse=True)
        print("\nslowest units:")
        for d, n, s in slowest[:6]:
            print("   %-18s %6.1f s   obj %.2f MB" % (n, d, s / 1048576.0))

    if failures:
        print("\nFIRST FAILURES (unfiltered -- a new toolchain's first run is"
              " read whole, not grepped):")
        for n, txt in failures[:3]:
            print("\n--- %s ---" % n)
            lines = [l for l in txt.splitlines() if l.strip()]
            for l in lines[:14]:
                print("   %s" % l)
    full_build = limit >= total_units and len(srcs) == total_units
    if not fail and full_build:
        try:
            # Detect normal concurrent edits/regeneration during compilation.
            # Publication remains fail-closed even though the C files are in a
            # shared scratch directory rather than immutable recipe folders.
            current_generation = G.require_generation_manifest()
            current_recipe_id, _ = object_recipe(
                current_generation, opt, cl, env
            )
            if current_recipe_id != recipe_id:
                raise BC.RecipeMismatchError(
                    "object inputs changed while compilation was running"
                )
            paths = object_paths(current_generation)
            outputs = BC.snapshot(OBJ, sorted(paths))
            # A reused object is accepted only if the publication snapshot is
            # still byte-identical to the prior successful manifest that tied
            # it to this source hash.  This closes the window between cache
            # selection and the final all-object snapshot.
            if reusable:
                previous_outputs = {
                    entry["path"]: entry
                    for entry in previous_manifest["outputs"]
                }
                current_outputs = {entry["path"]: entry for entry in outputs}
                for name in reusable:
                    if current_outputs.get(name) != previous_outputs.get(name):
                        raise BC.OutputMismatchError(
                            "reused object changed during build: %s" % name
                        )
            # Close the remaining snapshot window before publishing the stamp,
            # including guest.h, this script, compiler components and flags --
            # not only the generated C set.
            snapshot_generation = G.require_generation_manifest()
            snapshot_recipe_id, _ = object_recipe(
                snapshot_generation, opt, cl, env
            )
            if snapshot_recipe_id != recipe_id:
                raise BC.RecipeMismatchError(
                    "object inputs changed while outputs were snapshotted"
                )
        except BC.BuildContractError as exc:
            print("\nobject contract : NOT WRITTEN (%s)" % exc)
            print("\nfull log: %s" % LOG)
            return 2
        BC.write_manifest_atomic(
            MANIFEST,
            BC.make_manifest("objects", recipe_id, recipe_data, outputs),
        )
        print("\nobject contract : obj:%s  outputs:%s"
              % (recipe_id[:20], BC.set_id(outputs)[:20]))
    else:
        print("\nobject contract : NOT WRITTEN (partial or failed build)")
    print("\nfull log: %s" % LOG)
    return 1 if fail else 0


def main():
    with BC.exclusive_lock(os.path.join(os.path.dirname(OBJ), "pipeline.lock")):
        return _main()


if __name__ == "__main__":
    sys.exit(main())
