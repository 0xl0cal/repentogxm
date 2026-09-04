"""Focused coverage ABI/archive regression; no generation, build or game."""

import copy
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
import os
import struct
import tempfile
import threading

import guest_coverage as C


def canonical(value):
    return json.dumps(
        value, sort_keys=True, separators=(",", ":"), ensure_ascii=False,
        allow_nan=False,
    ).encode("utf-8")


def digest(value):
    return hashlib.sha256(value).hexdigest()


def fixture_document():
    functions = [
        {"id": 0, "rva": 0x1000, "symbol": "sub_00001000",
         "emitted_symbol": "sub_00001000", "manual": False, "stub": False},
        {"id": 1, "rva": 0x1100, "symbol": "sub_00001100",
         "emitted_symbol": "sub_00001100", "manual": False, "stub": True},
        {"id": 2, "rva": 0x1200, "symbol": "sub_00001200",
         "emitted_symbol": "sub_00001200", "manual": True, "stub": False},
    ]
    imports = [
        {"id": 0, "slot_rva": 0x5000, "name": "KERNEL32.dll!ExitProcess"},
        {"id": 1, "slot_rva": 0x5004, "name": "MSVCR120.dll!memcpy"},
    ]
    cases = [
        {"id": 0, "root_rva": 0x1000, "entry_rva": 0x1010,
         "executable": True, "edge_occurrences": 1, "hook_occurrences": 1},
        {"id": 1, "root_rva": 0x1000, "entry_rva": 0x1020,
         "executable": True, "edge_occurrences": 1, "hook_occurrences": 1},
        {"id": 2, "root_rva": 0x1100, "entry_rva": 0x1110,
         "executable": False, "edge_occurrences": 1, "hook_occurrences": 0},
    ]
    case_edges = [
        {"root_rva": 0x1000, "site_rva": 0x1008,
         "entry_rva": 0x1010, "case_id": 0, "executable": True},
        {"root_rva": 0x1000, "site_rva": 0x1008,
         "entry_rva": 0x1020, "case_id": 1, "executable": True},
        {"root_rva": 0x1100, "site_rva": 0x1108,
         "entry_rva": 0x1110, "case_id": 2, "executable": False},
    ]
    blocked = [{"id": 2, "root_rva": 0x1100, "entry_rva": 0x1110}]
    cross = [{"owner_rva": 0x1000, "target_rva": 0x1100,
              "function_id": 1}]
    function_bytes = b"".join(
        struct.pack("<I", item["rva"]) for item in functions
    )
    import_bytes = b"".join(
        struct.pack("<I", item["slot_rva"]) +
        item["name"].encode("utf-8") + b"\0" for item in imports
    )
    case_bytes = b"".join(
        struct.pack("<II", item["root_rva"], item["entry_rva"])
        for item in cases
    )
    executable_bytes = b"".join(
        struct.pack("<II", item["root_rva"], item["entry_rva"])
        for item in cases if item["executable"]
    )
    edge_bytes = b"".join(
        struct.pack("<III", item["root_rva"], item["site_rva"],
                    item["entry_rva"]) for item in case_edges
    )
    hook_bytes = b"".join(
        struct.pack("<III", item["root_rva"], item["site_rva"],
                    item["entry_rva"])
        for item in case_edges if item["executable"]
    )
    cross_pairs = [{"owner_rva": 0x1000, "target_rva": 0x1100}]
    semantic = {
        "abi_version": C.ABI_VERSION,
        "header_size": C.HEADER_SIZE,
        "schema_hash_rule": C.SCHEMA_HASH_RULE,
        "function_count": 3,
        "import_count": 2,
        "case_count": 3,
        "case_executable_count": 2,
        "case_blocked_count": 1,
        "case_edge_count": 3,
        "case_hook_count": 2,
        "function_generated_entry_hook_count": 2,
        "function_manual_hook_count": 1,
        "function_cross_root_hook_count": 1,
        "function_hook_count": 4,
        "function_rvas_sha256": digest(function_bytes),
        "imports_sha256": digest(import_bytes),
        "cases_sha256": digest(case_bytes),
        "case_executable_sha256": digest(executable_bytes),
        "case_edges_sha256": digest(edge_bytes),
        "case_hooks_sha256": digest(hook_bytes),
        "blocked_cases_sha256": digest(canonical(blocked)),
        "cross_root_pairs_sha256": digest(canonical(cross_pairs)),
        "cross_root_owners_sha256": digest(canonical([0x1000])),
        "cross_root_targets_sha256": digest(canonical([0x1100])),
        "functions": functions,
        "imports": imports,
        "cases": cases,
        "case_edges": case_edges,
        "blocked_case_ids": [2],
        "blocked_cases": blocked,
        "cross_root_hooks": cross,
    }
    build_id = "b" * 64
    return dict(
        semantic,
        schema_id=digest(canonical(semantic)),
        build_id=build_id,
        generation_id="gen:" + build_id,
    )


def write_document(path, document):
    with open(path, "w", encoding="utf-8", newline="\n") as stream:
        json.dump(document, stream, sort_keys=True, indent=2)
        stream.write("\n")


def mark(run, domain, index):
    offset = run.contract.offsets[domain] + index
    run.mapping[offset] = 1


def ready(run):
    struct.pack_into("<I", run.mapping, C.FLAGS_OFFSET, 1)


def artifact(path):
    identity = "a" * 64
    return {
        "path": os.path.abspath(path), "size": 1, "sha256": identity,
        "entry_recipe_id": identity, "entry_output_set_id": identity,
        "generation_recipe_id": identity, "object_output_set_id": identity,
    }


def expect_coverage_error(callback, contains):
    try:
        callback()
    except C.CoverageFormatError as exc:
        assert contains in str(exc), (contains, str(exc))
    else:
        raise AssertionError("expected CoverageFormatError: %s" % contains)


def check_manual_entry_hooks():
    here = os.path.dirname(os.path.abspath(__file__))
    specifications = {
        os.path.join(here, "runtime", "manual_kage.c"): (
            "00481280", "00560c60", "00560e30", "00560eb0", "00560f20",
            "00560f90", "00561830", "0056dd70",
        ),
        os.path.join(here, "runtime", "manual_portable.c"): (
            "00259650", "002597b0", "002599c0", "002c8700",
        ),
    }
    for path, symbols in specifications.items():
        with open(path, "r", encoding="utf-8") as stream:
            source = stream.read()
        for symbol in symbols:
            marker = "void sub_%s(CPU *__restrict c)" % symbol
            begin = source.find(marker)
            assert begin >= 0, marker
            body = source.find("{", begin)
            following = source.find("\nvoid sub_", body)
            if following < 0:
                following = len(source)
            wrapper = source[body + 1:following]
            hook = wrapper.find("guest_coverage_function(")
            assert hook >= 0 and wrapper.count("guest_coverage_function(") == 1
            prefix = wrapper[:hook]
            # C declarations may precede the hook, but no initializer, guest
            # load, delegation or faulting call may execute first.
            assert "=" not in prefix
            assert not any(token in prefix for token in (
                "ld8(", "ld16(", "ld32(", "use_original(",
                "guest_fault(", "gpop(", "gpush(",
            )), (symbol, prefix)


def check_same_parent_parallel_runs(contract, temp):
    """Four same-PID runs overlap and retain one exact cumulative OR."""
    root = os.path.join(temp, "parallel-coverage")
    create_gate = threading.Barrier(4)

    def create_run(_index):
        create_gate.wait(timeout=5.0)
        return C.CoverageRun(
            contract, root=root, timestamp_ns=777, pid=31337,
            publish_latest_stdout=False,
        )

    with ThreadPoolExecutor(max_workers=4) as pool:
        runs = list(pool.map(create_run, range(4)))

    stems = {run.run_stem for run in runs}
    expected_base = "19700101T000000.000000777Z-p31337"
    assert stems == {
        expected_base,
        expected_base + "-n0001",
        expected_base + "-n0002",
        expected_base + "-n0003",
    }
    for field in ("coverage_path", "stdout_path", "summary_path"):
        assert len({getattr(run, field) for run in runs}) == 4
    assert all(not run.publish_latest_stdout for run in runs)

    marks = (
        ((0,), (0,), (0,)),
        ((1,), (1,), (1,)),
        ((2,), (0,), (0,)),
        ((0,), (1,), (1,)),
    )
    for index, run in enumerate(runs):
        ready(run)
        for domain, ids in zip(("functions", "imports", "cases"),
                               marks[index]):
            for marked_id in ids:
                mark(run, domain, marked_id)
        with open(run.stdout_path, "wb") as stream:
            stream.write(("parallel worker %d\n" % index).encode("ascii"))

    start_gate = threading.Barrier(4)
    callback_entered = [threading.Event() for _ in runs]
    callback_release = [threading.Event() for _ in runs]
    returned = [threading.Event() for _ in runs]
    completion_order = []
    completion_guard = threading.Lock()

    def finish_run(index):
        def callback(summary):
            assert summary["run"] == {
                "functions": len(marks[index][0]),
                "imports": len(marks[index][1]),
                "cases": len(marks[index][2]),
            }
            callback_entered[index].set()
            if not callback_release[index].wait(timeout=2.0):
                raise RuntimeError("parallel callback release timed out")

        start_gate.wait(timeout=5.0)
        summary = runs[index].finish({
            "returncode": 0,
            "output": ("parallel worker %d\n" % index).encode("ascii"),
            "stop_reason": "controller",
            "matched_line": None,
            "controller_error": None,
            "artifact": artifact(os.path.join(temp, "fixture.exe")),
        }, runs[index].stdout_path, summary_callback=callback)
        with completion_guard:
            completion_order.append(index)
        returned[index].set()
        return summary

    try:
        with ThreadPoolExecutor(max_workers=4) as pool:
            futures = [pool.submit(finish_run, index) for index in range(4)]
            # Reaching all four callbacks proves all finalizers crossed the
            # nested local/cross-process lock without a same-process deadlock.
            for event in callback_entered:
                assert event.wait(timeout=5.0), "coverage finalizer deadlocked"
            # Finish in the reverse of dispatch order.  This also proves the
            # immutable per-run archives are not coupled to completion order.
            for index in reversed(range(4)):
                callback_release[index].set()
                assert returned[index].wait(timeout=5.0)
            summaries = [future.result(timeout=5.0) for future in futures]
    finally:
        for event in callback_release:
            event.set()
        for run in runs:
            run.close()

    assert completion_order == [3, 2, 1, 0]
    assert sum(item["new"]["functions"] for item in summaries) == 3
    assert sum(item["new"]["imports"] for item in summaries) == 2
    assert sum(item["new"]["cases"] for item in summaries) == 2
    for index, summary in enumerate(summaries):
        assert summary["coverage_path"] == runs[index].coverage_path
        assert summary["stdout_path"] == runs[index].stdout_path
        assert summary["started_ns"] == 777
        assert summary["stop_classification"] == "controller"
        assert summary["controller_error"] is None
        with open(runs[index].summary_path, "r", encoding="utf-8") as stream:
            assert json.load(stream) == summary
        with open(runs[index].coverage_path, "rb") as stream:
            coverage_bytes = stream.read()
            snapshot = C.snapshot_capture(
                coverage_bytes, contract,
                require_build=True, require_ready=True,
            )
        assert summary["coverage_sha256"] == digest(coverage_bytes)
        with open(runs[index].stdout_path, "rb") as stream:
            stdout_bytes = stream.read()
        assert summary["stdout_sha256"] == digest(stdout_bytes)
        assert snapshot.marked_ids("functions") == marks[index][0]
        assert snapshot.marked_ids("imports") == marks[index][1]
        assert snapshot.marked_ids("cases") == marks[index][2]
        assert snapshot.cases[contract.blocked_case_ids[0]] == 0

    cumulative = C.read_cumulative_snapshot(contract, root)
    assert cumulative.counts == {"functions": 3, "imports": 2, "cases": 2}
    assert cumulative.cases[contract.blocked_case_ids[0]] == 0
    with open(runs[0].cumulative_summary_path, "r", encoding="utf-8") \
            as stream:
        cumulative_summary = json.load(stream)
    with open(runs[0].cumulative_path, "rb") as stream:
        assert cumulative_summary["cumulative_sha256"] == digest(stream.read())
    candidates = []
    for summary in summaries:
        candidate = copy.deepcopy(summary)
        for field in (
                "coverage_path", "coverage_sha256",
                "stdout_path", "stdout_sha256"):
            candidate.pop(field)
        candidates.append(candidate)
    assert cumulative_summary in candidates


def main():
    check_manual_entry_hooks()
    with tempfile.TemporaryDirectory(prefix="guest-coverage-") as temp:
        contract_path = os.path.join(temp, "guest_coverage.json")
        document = fixture_document()
        write_document(contract_path, document)
        contract = C.load_contract(contract_path)
        assert contract.total_size == C.HEADER_SIZE + 8
        assert contract.blocked_case_ids == (2,)
        padded = bytearray(C.encode_header(contract))
        padded.extend(b"\0" * (contract.total_size - C.HEADER_SIZE))
        padded[C.BUILD_ID_OFFSET + C.IDENTITY_BYTES] = 1
        expect_coverage_error(lambda: C.parse_header(padded), "padding")

        # Exact schema hash and record digest are separate guards: changing a
        # record without either update, or updating only the outer schema,
        # must both fail.
        tampered = copy.deepcopy(document)
        tampered["functions"][0]["rva"] += 4
        write_document(contract_path, tampered)
        expect_coverage_error(
            lambda: C.load_contract(contract_path), "schema hash"
        )
        semantic = {key: value for key, value in tampered.items()
                    if key not in C.DOCUMENT_FIELDS}
        tampered["schema_id"] = digest(canonical(semantic))
        write_document(contract_path, tampered)
        expect_coverage_error(
            lambda: C.load_contract(contract_path), "record digest"
        )
        write_document(contract_path, document)
        contract = C.load_contract(contract_path)

        check_same_parent_parallel_runs(contract, temp)

        root = os.path.join(temp, "coverage")
        run1 = C.CoverageRun(contract, root=root, timestamp_ns=1, pid=7)
        assert run1.publish_latest_stdout
        try:
            C.CoverageRun(
                contract, root=root, timestamp_ns=1, pid=7,
                publish_latest_stdout=1,
            )
        except TypeError as exc:
            assert "publish_latest_stdout" in str(exc)
        else:
            raise AssertionError("non-boolean latest-stdout policy accepted")
        collision = C.CoverageRun(
            contract, root=root, timestamp_ns=1, pid=7
        )
        assert collision.run_stem == run1.run_stem + "-n0001"
        assert collision.coverage_path != run1.coverage_path
        collision.close()
        try:
            assert not C.parse_header(run1.mapping)["writer_ready"]
            empty_live = run1.snapshot(require_ready=False)
            assert not run1.snapshot(require_ready=None).writer_ready
            assert empty_live.counts == {
                "functions": 0, "imports": 0, "cases": 0,
            }
            expect_coverage_error(
                lambda: run1.snapshot(require_ready=True), "writer-ready"
            )
            empty_cumulative = run1.cumulative_snapshot()
            assert empty_cumulative.counts == empty_live.counts
            assert not empty_cumulative.writer_ready
            ready(run1)
            assert run1.snapshot(require_ready=None).writer_ready
            mark(run1, "functions", 0)
            mark(run1, "imports", 1)
            mark(run1, "cases", 0)
            live = run1.snapshot(require_ready=True)
            assert live.counts == {
                "functions": 1, "imports": 1, "cases": 1,
            }
            assert live.marked_ids("functions") == (0,)
            assert live.new_ids(empty_live, "functions") == (0,)
            assert live.require_monotonic_after(empty_live) is live
            stdout1 = os.path.join(temp, "live1.log")
            with open(stdout1, "wb") as stream:
                stream.write(b"normal child\n")
            summary1 = run1.finish({
                "returncode": 0, "output": b"normal child\n",
                "stop_reason": None, "matched_line": None,
                "artifact": artifact(os.path.join(temp, "fixture.exe")),
            }, stdout1)
        finally:
            run1.close()
        assert summary1["run"] == {"functions": 1, "imports": 1, "cases": 1}
        assert summary1["new"] == summary1["run"]
        assert summary1["stop_classification"] == "normal"
        assert summary1["artifact"] == artifact(
            os.path.join(temp, "fixture.exe")
        )
        assert os.path.isfile(summary1["coverage_path"])
        aggregate1 = C.read_cumulative_snapshot(contract, root)
        assert aggregate1.counts == summary1["cumulative"]
        assert aggregate1.marked_ids("imports") == (1,)

        regressed = C.CoverageSnapshot(
            schema_id=live.schema_id, build_id=live.build_id,
            writer_ready=True, functions=bytes(contract.function_count),
            imports=live.imports, cases=live.cases,
        )
        expect_coverage_error(
            lambda: regressed.require_monotonic_after(live), "regressed"
        )
        assert regressed.new_ids(live, "functions") == ()
        wrong_build = C.CoverageSnapshot(
            schema_id=live.schema_id, build_id="c" * 64,
            writer_ready=True, functions=live.functions,
            imports=live.imports, cases=live.cases,
        )
        expect_coverage_error(
            lambda: wrong_build.require_monotonic_after(live), "builds"
        )
        lost_ready = C.CoverageSnapshot(
            schema_id=live.schema_id, build_id=live.build_id,
            writer_ready=False, functions=live.functions,
            imports=live.imports, cases=live.cases,
        )
        expect_coverage_error(
            lambda: lost_ready.require_monotonic_after(live), "writer-ready"
        )

        run2 = C.CoverageRun(contract, root=root, timestamp_ns=2, pid=7)
        ready(run2)
        mark(run2, "functions", 0)
        mark(run2, "functions", 1)
        mark(run2, "imports", 0)
        mark(run2, "cases", 1)
        stdout2 = os.path.join(temp, "live2.log")
        with open(stdout2, "wb") as stream:
            stream.write(b"first startup fault         : 00001010 fixture\n")
        summary2 = run2.finish({
            "returncode": 1,
            "output": b"first startup fault         : 00001010 fixture\n",
            "stop_reason": "timeout", "matched_line": None,
            "artifact": artifact(os.path.join(temp, "fixture.exe")),
        }, stdout2)
        assert summary2["stop_classification"] == "guest-fault"
        assert summary2["new"] == {"functions": 1, "imports": 1, "cases": 1}
        assert summary2["cumulative"] == {
            "functions": 2, "imports": 2, "cases": 2,
        }

        callback_fault = C.CoverageRun(
            contract, root=root, timestamp_ns=42, pid=7
        )
        ready(callback_fault)
        callback_stdout = os.path.join(temp, "callback-fault.log")
        with open(callback_stdout, "wb") as stream:
            stream.write(
                b"first startup fault         : 00001010 fixture\n"
            )

        def broken_summary_callback(_summary):
            _summary["run"]["functions"] = 999
            assert C.read_cumulative_snapshot(contract, root).counts == {
                "functions": 2, "imports": 2, "cases": 2,
            }
            raise RuntimeError("summary callback boom")

        callback_summary = callback_fault.finish({
            "returncode": 1,
            "output": b"first startup fault         : 00001010 fixture\n",
            "stop_reason": "controller-error", "matched_line": None,
            "controller_error": "earlier controller error",
            "artifact": artifact(os.path.join(temp, "fixture.exe")),
        }, callback_stdout, summary_callback=broken_summary_callback)
        assert callback_summary["stop_classification"] == "guest-fault"
        assert "earlier controller error" in \
            callback_summary["controller_error"]
        assert "summary callback boom" in callback_summary["controller_error"]
        assert callback_summary["run"]["functions"] == 0
        with open(callback_fault.summary_path, "r", encoding="utf-8") \
                as stream:
            archived_callback = json.load(stream)
        assert archived_callback["controller_error"] == \
            callback_summary["controller_error"]

        callback_clean = C.CoverageRun(
            contract, root=root, timestamp_ns=43, pid=7
        )
        ready(callback_clean)
        clean_summary = callback_clean.finish({
            "returncode": 1, "output": b"",
            "stop_reason": "controller", "matched_line": None,
            "artifact": artifact(os.path.join(temp, "fixture.exe")),
        }, callback_stdout, summary_callback=broken_summary_callback)
        assert clean_summary["stop_classification"] == "controller-error"
        assert "summary callback boom" in clean_summary["controller_error"]

        # Two processes can share a time_ns value.  A late callback failure
        # from the older PID must not overwrite the newer cumulative summary.
        collision_outer = C.CoverageRun(
            contract, root=root, timestamp_ns=44, pid=7
        )
        ready(collision_outer)

        def collision_callback(_summary):
            collision_inner = C.CoverageRun(
                contract, root=root, timestamp_ns=44, pid=8
            )
            ready(collision_inner)
            mark(collision_inner, "functions", 2)
            inner_stdout = os.path.join(temp, "collision-inner.log")
            with open(inner_stdout, "wb") as stream:
                stream.write(b"newer same-tick run\n")
            collision_inner.finish({
                "returncode": 0, "output": b"newer same-tick run\n",
                "stop_reason": "controller", "matched_line": None,
                "artifact": artifact(os.path.join(temp, "fixture.exe")),
            }, inner_stdout)
            raise RuntimeError("older same-tick callback boom")

        collision_outer.finish({
            "returncode": 0, "output": b"",
            "stop_reason": "controller", "matched_line": None,
            "artifact": artifact(os.path.join(temp, "fixture.exe")),
        }, callback_stdout, summary_callback=collision_callback)
        with open(collision_outer.cumulative_summary_path, "r",
                  encoding="utf-8") as stream:
            same_tick_cumulative = json.load(stream)
        assert same_tick_cumulative["started_ns"] == 44
        assert same_tick_cumulative["controller_error"] is None
        assert same_tick_cumulative["cumulative"]["functions"] == 3

        stale = C.CoverageRun(contract, root=root, timestamp_ns=3, pid=7)
        stdout3 = os.path.join(temp, "live3.log")
        with open(stdout3, "wb") as stream:
            stream.write(b"stale binary\n")
        expect_coverage_error(
            lambda: stale.finish({"returncode": 0, "output": b"",
                                  "stop_reason": None,
                                  "matched_line": None,
                                  "artifact": artifact(os.path.join(
                                      temp, "fixture.exe"))}, stdout3),
            "writer-ready",
        )
        stale.close()

        blocked = C.CoverageRun(contract, root=root, timestamp_ns=4, pid=7)
        ready(blocked)
        mark(blocked, "cases", 2)
        expect_coverage_error(
            lambda: blocked.finish({"returncode": 0, "output": b"",
                                    "stop_reason": None,
                                    "matched_line": None,
                                    "artifact": artifact(os.path.join(
                                        temp, "fixture.exe"))}, stdout3),
            "blocked loud-stub",
        )
        blocked.close()

        unattributed = C.CoverageRun(
            contract, root=root, timestamp_ns=41, pid=7
        )
        ready(unattributed)
        expect_coverage_error(
            lambda: unattributed.finish({
                "returncode": 0, "output": b"", "stop_reason": None,
                "matched_line": None,
            }, stdout3),
            "artifact identity",
        )
        unattributed.close()

        # Existing aggregate size is checked before mmap, under the schema
        # lock.  This cannot be mistaken for a new empty cumulative file.
        with open(run2.cumulative_path, "r+b") as stream:
            stream.truncate(contract.total_size - 1)
        sized = C.CoverageRun(contract, root=root, timestamp_ns=5, pid=7)
        ready(sized)
        expect_coverage_error(
            lambda: sized.finish({"returncode": 0, "output": b"",
                                  "stop_reason": None,
                                  "matched_line": None,
                                  "artifact": artifact(os.path.join(
                                      temp, "fixture.exe"))}, stdout3),
            "cumulative coverage file size",
        )
        sized.close()

        assert C.classify_stop({
            "returncode": 1, "stop_reason": "matched",
            "output": b"host exception              : code=c0000005\n",
        }) == "host-exception"
        assert C.classify_stop({
            "returncode": 1, "stop_reason": "timeout", "output": b"",
        }) == "timeout"
        assert C.classify_stop({
            "returncode": 1, "stop_reason": "controller", "output": b"",
        }) == "controller"
        assert C.classify_stop({
            "returncode": 1, "stop_reason": "controller-error",
            "output": b"",
        }) == "controller-error"
        assert C.classify_stop({
            "returncode": 1, "stop_reason": "controller-error",
            "output": b"first startup fault         : 00001010 fixture\n",
        }) == "guest-fault"

    print("guest semantic coverage: schema/header/archive/negative contracts PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
