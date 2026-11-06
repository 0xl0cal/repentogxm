#!/usr/bin/env python3
"""Hostile fixtures for the Vita raw-allocator compiler/link gate."""

from __future__ import annotations

import collections
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

import vita_raw_allocator_gate as gate
import vita_raw_allocator_profile_census as census


def must_fail(label, callback, needle: str) -> None:
    try:
        callback()
    except gate.GateError as exc:
        if needle not in str(exc):
            raise AssertionError(f"{label}: wrong failure: {exc}") from exc
    else:
        raise AssertionError(f"{label}: hostile fixture passed")


def compiler() -> str:
    for name in ("cc", "gcc", "clang"):
        found = shutil.which(name)
        if found:
            return found
    # Windows LLVM installations are commonly registered with the installer
    # but deliberately omitted from PATH in non-developer shells.
    candidates = (
        Path(os.environ.get("ProgramFiles", r"C:\Program Files")) /
        "LLVM" / "bin" / "clang.exe",
        Path(os.environ.get("ProgramFiles", r"C:\Program Files")) /
        "Microsoft Visual Studio" / "2022" / "Community" / "VC" /
        "Tools" / "Llvm" / "x64" / "bin" / "clang.exe",
    )
    for candidate in candidates:
        if candidate.is_file():
            return str(candidate)
    raise RuntimeError("no host C compiler found")


def poison_tests(root: Path, header: Path) -> None:
    cc = compiler()
    cases = {
        "malloc": "void *f(void){return malloc(4);}\n",
        "calloc": "void *f(void){return calloc(1,4);}\n",
        "realloc": "void *f(void*p){return realloc(p,4);}\n",
        "free": "void f(void*p){free(p);}\n",
        "memalign": "void *f(void){return memalign(8,8);}\n",
        "aligned_alloc": "void *f(void){return aligned_alloc(8,8);}\n",
        "strdup": "char *f(void){return strdup(\"x\");}\n",
    }
    common = [
        cc, "-std=gnu11", "-D__vita__=1",
        "-DISAAC_VITA_RAW_ALLOCATOR_GATE=1", "-include", str(header), "-c",
    ]
    for name, source in cases.items():
        path = root / f"poison-{name}.c"
        path.write_text(source, encoding="ascii")
        completed = subprocess.run(
            common + [str(path), "-o", str(root / f"poison-{name}.o")],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        )
        if completed.returncode == 0 or b"poison" not in completed.stderr.lower():
            raise AssertionError(f"raw {name} call was not poisoned")
    for name in cases:
        address = root / f"poison-address-{name}.c"
        address.write_text(
            f"void *f(void){{return (void *)&{name};}}\n", encoding="ascii"
        )
        completed = subprocess.run(
            common + [str(address), "-o", str(root / f"poison-address-{name}.o")],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        )
        if completed.returncode == 0 or b"poison" not in completed.stderr.lower():
            raise AssertionError(f"raw {name} address-taking was not poisoned")
    allowed = root / "exempt.c"
    allowed.write_text(
        "void *f(void*p){free(p);return malloc(4);}\n", encoding="ascii"
    )
    completed = subprocess.run(
        common[:4] + ["-DISAAC_VITA_RAW_ALLOCATOR_EXEMPT=1"] + common[4:] +
        [str(allowed), "-o", str(root / "exempt.o")],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE,
    )
    if completed.returncode != 0:
        raise AssertionError(
            "exact allocator exemption failed: " +
            completed.stderr.decode("utf-8", "replace")
        )


def closure_fixture(root: Path, poison: Path) -> tuple[set[Path], Path, Path]:
    source = (root / "source" / "recomp" / "vita" / "main.c").resolve()
    source.parent.mkdir(parents=True)
    source.write_text("int main(void){return 0;}\n", encoding="ascii")
    build = root / "build"
    build.mkdir()
    output = "CMakeFiles/isaac_first_arm_fault.dir/main.c.obj"
    obj = build / output
    obj.parent.mkdir(parents=True)
    obj.write_bytes(b"fixture")
    command = (
        f"cc -DISAAC_VITA_RAW_ALLOCATOR_GATE=1 -include "
        f"\"{poison.as_posix()}\" -o {output} -c \"{source.as_posix()}\""
    )
    commands = build / "compile_commands.json"
    commands.write_text(
        json.dumps([{
            "directory": str(build), "command": command,
            "file": str(source), "output": output,
        }]), encoding="utf-8"
    )
    ninja = build / "build.ninja"
    ninja.write_text(
        "build isaac_first_arm_fault: C_LINK " + output + "\n",
        encoding="utf-8",
    )
    return {source}, commands, ninja


def closure_tests(root: Path, poison: Path) -> None:
    expected, commands, ninja = closure_fixture(root, poison)
    records = gate.compile_closure(commands, expected, poison)
    gate.verify_link_closure(ninja, records)

    source = next(iter(expected))
    original_source = source.read_text(encoding="ascii")
    source.write_text("#define _GNU_SOURCE 1\n" + original_source,
                      encoding="ascii")
    must_fail("late feature-view macro",
              lambda: gate.compile_closure(commands, expected, poison),
              "feature-view directive conflicts")
    source.write_text(original_source, encoding="ascii")

    source.write_text(
        "#undef ISAAC_VITA_DIRECT_DEFAULT\n" + original_source,
        encoding="ascii",
    )
    must_fail(
        "source-local direct-default undef",
        lambda: gate.compile_closure(commands, expected, poison),
        "feature-view directive conflicts",
    )
    source.write_text(original_source, encoding="ascii")

    entries = json.loads(commands.read_text(encoding="utf-8"))
    original_command = entries[0]["command"]
    for label, undefinition in (
        ("joined", "-UISAAC_VITA_DIRECT_DEFAULT"),
        ("separate", "-U ISAAC_VITA_DIRECT_DEFAULT"),
    ):
        entries[0]["command"] = original_command.replace(
            " -o ", f" -DISAAC_VITA_DIRECT_DEFAULT=1 {undefinition} -o "
        )
        commands.write_text(json.dumps(entries), encoding="utf-8")
        must_fail(
            f"{label} direct-default command undef",
            lambda: gate.compile_closure(commands, expected, poison),
            "compile policy is undefined",
        )
    entries[0]["command"] = original_command
    commands.write_text(json.dumps(entries), encoding="utf-8")

    external = dict(entries[0])
    external["output"] = "CMakeFiles/external_dependency.dir/external.c.obj"
    entries.append(external)
    commands.write_text(json.dumps(entries), encoding="utf-8")
    must_fail("external poison leak",
              lambda: gate.compile_closure(commands, expected, poison),
              "leaked into external compile output")
    commands.write_text(json.dumps(entries[:1]), encoding="utf-8")

    original_ninja = ninja.read_text(encoding="utf-8")
    ninja.write_text(original_ninja.rstrip() + " stale.obj\n", encoding="utf-8")
    must_fail("stale link input", lambda: gate.verify_link_closure(ninja, records),
              "closure changed")
    ninja.write_text(original_ninja.rstrip() +
                     " CMakeFiles/isaac_first_arm_fault.dir/main.c.obj\n",
                     encoding="utf-8")
    must_fail("duplicate link input", lambda: gate.verify_link_closure(ninja, records),
              "duplicate")
    ninja.write_text(
        original_ninja.rstrip() +
        " CMakeFiles/isaac_first_arm_fault.dir/host_win32.c.obj\n",
        encoding="utf-8",
    )
    must_fail("forbidden link object",
              lambda: gate.verify_link_closure(ninja, records),
              "forbidden object entered production link")
    ninja.write_text(original_ninja, encoding="utf-8")

    entries = json.loads(commands.read_text(encoding="utf-8"))
    entries[0]["command"] = entries[0]["command"].replace(
        poison.as_posix(), (root / "unknown.h").as_posix()
    )
    commands.write_text(json.dumps(entries), encoding="utf-8")
    must_fail("unknown forced include",
              lambda: gate.compile_closure(commands, expected, poison),
              "forced-include closure")

    forbidden = (root / "source" / "recomp" / "runtime" / "host_win32.c").resolve()
    forbidden.parent.mkdir(parents=True, exist_ok=True)
    forbidden.write_text("int x;\n", encoding="ascii")
    entries[0]["file"] = str(forbidden)
    entries[0]["command"] = entries[0]["command"].replace(
        (root / "unknown.h").as_posix(), poison.as_posix()
    ).replace(next(iter(expected)).as_posix(), forbidden.as_posix())
    commands.write_text(json.dumps(entries), encoding="utf-8")
    must_fail("forbidden production source",
              lambda: gate.compile_closure(commands, {forbidden}, poison),
              "forbidden test/oracle/host source")


EXPECTED_SOURCE_FIXTURE_CACHE = {
        "ISAAC_VITA_HEAP_LEDGER_MEMBLOCK": "OFF",
        "ISAAC_VITA_HEAP_OVERFLOW_MSPACE": "OFF",
        "ISAAC_VITA_HEAP_SLAB_REJECT_FASTPATH": "OFF",
        "ISAAC_VITA_HEAP_STAGE_PROFILE": "OFF",
        "ISAAC_VITA_NATIVE_VORBIS_FREE_WATCH": "OFF",
        "ISAAC_VITA_HEAP_LEAN_IMPORT": "OFF",
        "ISAAC_VITA_HEAP_FREE_PREFILTER": "OFF",
        "ISAAC_VITA_HEAP_FREE_COST_PROFILE": "OFF",
        "ISAAC_VITA_NATIVE_VORBIS_FREE_WATCH_MASK": "OFF",
        "ISAAC_VITA_ROOM_ENTRY_SLAB": "OFF",
        "ISAAC_VITA_ROOM_ENTRY_HYBRID": "OFF",
        "ISAAC_VITA_ARCHIVE_FILE_CACHE": "OFF",
        "ISAAC_VITA_ARCHIVE_VALIDATION_RECEIPT": "OFF",
        "ISAAC_VITA_FIOS_CACHE": "OFF",
        "ISAAC_VITA_ASYNC_SAVE_WRITE": "OFF",
        "ISAAC_VITA_LOG_ASYNC": "OFF",
        "ISAAC_VITA_ANM2_SCRATCH": "OFF",
        "ISAAC_VITA_TEXEL_SCRATCH": "OFF",
        "ISAAC_VITA_KAGE": "ON",
        "ISAAC_VITA_GUEST_LINK_ID": "fixture-build",
        "ISAAC_VITA_AUDIO": "OFF",
        "ISAAC_VITA_AUDIO_STREAM_RECEIPT": "OFF",
        "ISAAC_VITA_LUA": "OFF",
        "ISAAC_VITA_IO_PROFILE": "OFF",
        "ISAAC_VITA_STALL_PROBE": "OFF",
        "ISAAC_VITA_PHASE_PROFILE": "OFF",
        "ISAAC_VITA_STATIC_SFX_PROFILE": "OFF",
        "ISAAC_VITA_BACKDROP_WATERGFX_GUARD": "OFF",
        "ISAAC_VITA_TROPHIES": "OFF",
        "ISAAC_VITA_DEBUG_OVERLAY": "OFF",
        "ISAAC_VITA_SIM_CADENCE_RECEIPT": "OFF",
        "ISAAC_VITA_WORLD_SEAM_DIAG": "OFF",
        "ISAAC_VITA_FULLSPEED_SCHEDULER": "OFF",
        "ISAAC_VITA_STABLE_30_PRESENTATION": "OFF",
        "ISAAC_VITA_RUNTIME_SETTINGS": "OFF",
        "ISAAC_VITA_MANAGER_SETTINGS_UI": "OFF",
        "ISAAC_VITA_RENDER_SCALE_DEFAULT": "OFF",
        "ISAAC_VITA_RENDERFRAME_FASTPATH": "OFF",
        "ISAAC_VITA_CONTINUE_PROFILE": "OFF",
        "ISAAC_VITA_EXIT_MENU_PROFILE": "OFF",
        "ISAAC_VITA_PNG_DECODE_PROFILE": "OFF",
        "ISAAC_VITA_PNG_NATIVE_UNFILTER": "OFF",
        "ISAAC_VITA_PNG_INFLATE_FLUSH_FASTPATH": "OFF",
        "ISAAC_VITA_PNG_CRC32_FASTPATH": "OFF",
        "ISAAC_VITA_ARCHIVE_MINIZ_FASTPATH": "OFF",
        "ISAAC_VITA_SAVE_CHECKSUM_FASTPATH": "OFF",
        "ISAAC_VITA_SAVE_READER_FASTPATH": "OFF",
        "ISAAC_VITA_SAVE_READ32_FUSED": "OFF",
        "ISAAC_VITA_SAVE_READER_DIRECT_EDGES": "OFF",
        "ISAAC_VITA_MEMSET_THUNK_FASTPATH": "OFF",
        "ISAAC_VITA_KAGE_MUTEX_SEAM": "OFF",
        "ISAAC_VITA_KAGE_REFCOUNT_SEAM": "OFF",
        "ISAAC_VITA_SYNC_INLINE_FASTPATH": "OFF",
        "ISAAC_VITA_FLOOR_THUNK_FASTPATH": "OFF",
        "ISAAC_VITA_TEXTURE_CHURN_PROFILE": "OFF",
        "ISAAC_VITA_COLOROFFSET_GPU_OPTIMIZATIONS": "OFF",
        "ISAAC_VITA_GUEST_LOOKUP_CACHE": "OFF",
        "ISAAC_VITA_SYNC_IMPORT_FASTPATH": "OFF",
        "ISAAC_VITA_GUEST_STACK_MEMBLOCK": "OFF",
        "ISAAC_VITA_DISPLAY_RASTER_720": "OFF",
        "ISAAC_VITA_GL_TYPED_STATE_CACHE": "OFF",
        "ISAAC_VITA_GAME_LOG_BATCH": "OFF",
        "ISAAC_VITA_GAME_LOG_ASYNC": "OFF",
        "ISAAC_VITA_RAW_GXM_PROBE": "OFF",
        "ISAAC_VITA_SCREENSHOT_PROBE": "OFF",
        "ISAAC_VITA_FIRST_FRAME_PROBE": "ON",
        "ISAAC_VITA_DIRECT_DEFAULT": "OFF",
        "ISAAC_VITA_VITAGL_STOCK_REFERENCE": "OFF",
        "ISAAC_VITA_TEXEL_OOM_DIAGNOSTIC": "OFF",
        "ISAAC_VITA_TEXTURE_ALIGN8_POLICY": "OFF",
        "ISAAC_VITA_NATIVE_PNG": "OFF",
        "ISAAC_VITA_NATIVE_THEORA": "OFF",
        "ISAAC_VITA_NATIVE_THEORA_RGB": "OFF",
        "ISAAC_VITA_INFLATE_TABLE_MEMO": "OFF",
        "ISAAC_VITA_UNIFORM_SET_RECEIPT": "OFF",
        "ISAAC_VITA_ARCHIVE_READ_TRAIL": "OFF",
        "ISAAC_VITA_ARCHIVE_VALIDATION_SIDECAR": "OFF",
        "ISAAC_VITA_NATIVE_INFLATE": "OFF",
        "ISAAC_VITA_LUA_GCCOLLECT_CLAMP": "OFF",
        "ISAAC_VITA_LUA_GC_PROFILE": "OFF",
        "ISAAC_VITA_LUA_GC_FINALIZER_PACE": "OFF",
        "ISAAC_VITA_LUA_INTEGER2STR": "OFF",
        "ISAAC_VITA_LUA_NATIVE_GETCLASS": "OFF",
        "ISAAC_VITA_LUA_BINDING_REPLAY": "OFF",
        "ISAAC_VITA_LUA_COST_PROFILE": "OFF",
        "ISAAC_VITA_LUA_LOADFILE_RETRY": "OFF",
        "ISAAC_VITA_LUA_CORE_ARM": "OFF",
        "ISAAC_VITA_KAGE_QUAD_FASTPATH": "OFF",
        "ISAAC_VITA_GFUEL_SEED_NATIVE": "OFF",
        "ISAAC_VITA_HASCOLLECTIBLE_NATIVE": "OFF",
        "ISAAC_VITA_SFX_FLOOR_PRELOAD": "OFF",
        "ISAAC_VITA_CAMERA_RING_NATIVE": "OFF",
        "ISAAC_VITA_UPDATE_PROBE": "OFF",
        "ISAAC_VITA_IMAGE_RETAIN": "OFF",
        "ISAAC_VITA_RENDER_CHAIN_NATIVE": "OFF",
        "ISAAC_VITA_SHADER_ATTRIB_FASTPATH": "OFF",
        "ISAAC_VITA_SHADER_SAMPLER_FASTPATH": "OFF",
        "ISAAC_VITA_KAGE_BIND_PACKET": "OFF",
        "ISAAC_VITA_GL_THREAD_RECORD": "OFF",
        "ISAAC_VITA_GL_THREAD_WORKER": "OFF",
        "ISAAC_VITA_GL_OWNER_CHECK": "OFF",
        "ISAAC_VITA_GL_RECORD_ADMISSION_CACHE_STANDALONE": "OFF",
        "ISAAC_VITA_SHADER_ATTRIB_DIRECT_STATE": "OFF",
        "ISAAC_VITA_GL_SHIM_FASTDISPATCH": "OFF",
        "ISAAC_VITA_GUEST_SAMPLER": "OFF",
        "ISAAC_VITA_GPU_FRAME_PROFILE": "OFF",
        "ISAAC_VITA_ACCESS_NEGATIVE_CACHE": "OFF",
        "ISAAC_VITA_LAUNCH_ARGS_FILE": "OFF",
        "ISAAC_VITA_AUDIO_WORKER": "OFF",
        "ISAAC_VITA_AUDIO_REFILL_RECEIPT": "OFF",
        "ISAAC_VITA_NATIVE_VORBIS": "OFF",
        "ISAAC_VITA_OGG_READAHEAD": "OFF",
        "ISAAC_VITA_ARCHIVE_XOR_FASTPATH": "OFF",
        "ISAAC_VITA_WAV_BUFFERED_REWIND": "OFF",
        "ISAAC_VITA_ROOM_GRID_INIT_NATIVE": "OFF",
        "ISAAC_VITA_PNG_PREMULTIPLY_NATIVE": "OFF",
        "ISAAC_VITA_PNG_TEXEL_INIT_ELISION": "OFF",
        "ISAAC_VITA_NATIVE_PNG_LIBDEFLATE_STRICT": "OFF",
        "ISAAC_VITA_NATIVE_PNG_REUSE": "OFF",
        "ISAAC_VITA_NATIVE_PNG_ADLER_NEON": "OFF",
        "ISAAC_VITA_THEORA_PRECACHE_FRAMES": "0",
        "ISAAC_VITA_THEORA_WORKER_PUMP": "0",
    }


def expected_source_tests(root: Path) -> None:
    source_root = (root / "expected-source-root").resolve()
    cache = dict(EXPECTED_SOURCE_FIXTURE_CACHE)
    manifest = (root / "expected-generated" / "manifest.json").resolve()
    manifest.parent.mkdir(parents=True)
    manifest.write_text(
        json.dumps({
            "outputs": [
                {"path": "guest_0144.c"},
                {"path": "guest_stubs.c"},
                {"path": "guest_table.c"},
            ]
        }),
        encoding="utf-8",
    )
    runtime_root = source_root / "recomp" / "runtime"
    baseline = gate.expected_sources(source_root, manifest, cache)
    heap_parent = {**cache, "ISAAC_VITA_HEAP_OVERFLOW_MSPACE": "ON",
                   "ISAAC_VITA_ROOM_ENTRY_SLAB": "ON"}
    heap_off = gate.expected_sources(source_root, manifest, heap_parent)
    heap_on = {**heap_parent, "ISAAC_VITA_HEAP_SLAB_REJECT_FASTPATH": "ON"}
    if gate.expected_sources(source_root, manifest, heap_on) != heap_off:
        raise AssertionError("slab reject must keep the same audited source closure")
    for key in ("ISAAC_VITA_HEAP_OVERFLOW_MSPACE", "ISAAC_VITA_ROOM_ENTRY_SLAB"):
        must_fail("slab reject dependency " + key, lambda key=key: gate.expected_sources(
            source_root, manifest, {**heap_on, key: "OFF"}), "requires ROOM_ENTRY_SLAB")
    missing_heap_fastpath = dict(heap_parent)
    del missing_heap_fastpath["ISAAC_VITA_HEAP_SLAB_REJECT_FASTPATH"]
    must_fail("missing slab reject flag", lambda: gate.expected_sources(
        source_root, manifest, missing_heap_fastpath), "cache is missing")
    must_fail("malformed slab reject flag", lambda: gate.expected_sources(
        source_root, manifest, {**heap_parent, "ISAAC_VITA_HEAP_SLAB_REJECT_FASTPATH": "maybe"}), "is not ON/OFF")
    fast_key = "ISAAC_VITA_HEAP_SLAB_REJECT_FASTPATH"
    heap_record = {runtime_root / "host_vita_heap.c": {
        "source": runtime_root / "host_vita_heap.c",
        "definitions": {fast_key: ["1"]}, "undefinitions": []}}
    gate.verify_heap_slab_reject_compile_scope(heap_record, heap_on)
    must_fail("slab reject defined while OFF", lambda: gate.verify_heap_slab_reject_compile_scope(
        heap_record, heap_parent), "scope changed")
    wrong_record = {runtime_root / "guest_0144.c": {
        "source": runtime_root / "guest_0144.c", "definitions": {fast_key: ["1"]}}}
    must_fail("slab reject wrong owner", lambda: gate.verify_heap_slab_reject_compile_scope(
        wrong_record, heap_on), "scope changed")
    for values in (["0"], ["1", "1"], ["maybe"]):
        malformed = {runtime_root / "host_vita_heap.c": {
            "source": runtime_root / "host_vita_heap.c", "definitions": {fast_key: values}}}
        must_fail("slab reject noncanonical definition", lambda malformed=malformed:
            gate.verify_heap_slab_reject_compile_scope(malformed, heap_on), "non-canonical")
    must_fail("slab reject missing owner definition", lambda:
        gate.verify_heap_slab_reject_compile_scope({}, heap_on), "scope changed")
    undefined = {runtime_root / "host_vita_heap.c": {
        "source": runtime_root / "host_vita_heap.c", "definitions": {}, "undefinitions": [fast_key]}}
    must_fail("slab reject forced undefinition", lambda:
        gate.verify_heap_slab_reject_compile_scope(undefined, heap_parent), "undefined")
    stage_key = "ISAAC_VITA_HEAP_STAGE_PROFILE"
    stage_parent = {**cache, "ISAAC_VITA_HEAP_OVERFLOW_MSPACE": "ON",
                    "ISAAC_VITA_PHASE_PROFILE": "ON"}
    stage_on = {**stage_parent, stage_key: "ON"}
    if gate.expected_sources(source_root, manifest, stage_on) != \
            gate.expected_sources(source_root, manifest, stage_parent):
        raise AssertionError("heap stage profile must keep the same source closure")
    for key in ("ISAAC_VITA_HEAP_OVERFLOW_MSPACE", "ISAAC_VITA_PHASE_PROFILE"):
        must_fail("stage profile dependency " + key, lambda key=key: gate.expected_sources(
            source_root, manifest, {**stage_on, key: "OFF"}), "requires PHASE_PROFILE")
    stage_owners = {runtime_root / name: {"source": runtime_root / name,
                                          "definitions": {stage_key: ["1"]}}
                    for name in ("host_vita_heap.c", "kage_vita_phase_profile.c")}
    gate.verify_heap_stage_profile_compile_scope(stage_owners, stage_on)
    must_fail("stage profile defined while OFF", lambda:
        gate.verify_heap_stage_profile_compile_scope(stage_owners, stage_parent), "scope changed")
    one_owner = dict(list(stage_owners.items())[:1])
    must_fail("stage profile missing profiler owner", lambda:
        gate.verify_heap_stage_profile_compile_scope(one_owner, stage_on), "scope changed")
    stray = {**stage_owners, runtime_root / "guest_0144.c": {
        "source": runtime_root / "guest_0144.c", "definitions": {stage_key: ["1"]}}}
    must_fail("stage profile stray owner", lambda:
        gate.verify_heap_stage_profile_compile_scope(stray, stage_on), "scope changed")
    must_fail("stage profile noncanonical", lambda: gate.verify_heap_stage_profile_compile_scope(
        {runtime_root / "host_vita_heap.c": {"source": runtime_root / "host_vita_heap.c",
                                             "definitions": {stage_key: ["2"]}}}, stage_on),
        "non-canonical")
    lean_key = "ISAAC_VITA_HEAP_LEAN_IMPORT"
    must_fail("lean import without overflow mspace", lambda: gate.expected_sources(
        source_root, manifest, {**cache, lean_key: "ON"}), "requires HEAP_OVERFLOW_MSPACE")
    lean_on = {**stage_parent, lean_key: "ON"}
    if gate.expected_sources(source_root, manifest, lean_on) != \
            gate.expected_sources(source_root, manifest, stage_parent):
        raise AssertionError("lean import must keep the same source closure")
    heap_c = runtime_root / "host_vita_heap.c"
    lean_record = {heap_c: {"source": heap_c, "definitions": {lean_key: ["1"]}}}
    gate._verify_heap_define_scope(lean_record, lean_on, lean_key, "heap lean import", {"host_vita_heap.c"})
    must_fail("lean import defined while OFF", lambda: gate._verify_heap_define_scope(
        lean_record, stage_parent, lean_key, "heap lean import", {"host_vita_heap.c"}), "scope changed")
    must_fail("lean import stray owner", lambda: gate._verify_heap_define_scope(
        {**lean_record, runtime_root / "guest_0144.c": {"source": runtime_root / "guest_0144.c",
                                                         "definitions": {lean_key: ["1"]}}},
        lean_on, lean_key, "heap lean import", {"host_vita_heap.c"}), "scope changed")
    watch_key = "ISAAC_VITA_NATIVE_VORBIS_FREE_WATCH"
    must_fail("free watch without native vorbis", lambda: gate.expected_sources(
        source_root, manifest, {**cache, watch_key: "ON", "ISAAC_VITA_NATIVE_VORBIS": "OFF"}),
        "requires NATIVE_VORBIS")
    watch_on = {**cache, watch_key: "ON"}
    nv = runtime_root / "host_vita_native_vorbis.c"
    watch_record = {nv: {"source": nv, "definitions": {watch_key: ["1"]}}}
    gate.verify_native_vorbis_free_watch_compile_scope(watch_record, watch_on)
    must_fail("free watch defined while OFF", lambda:
        gate.verify_native_vorbis_free_watch_compile_scope(watch_record, cache), "scope changed")
    must_fail("free watch wrong owner", lambda: gate.verify_native_vorbis_free_watch_compile_scope(
        {runtime_root / "host_vita_heap.c": {"source": runtime_root / "host_vita_heap.c",
                                             "definitions": {watch_key: ["1"]}}}, watch_on),
        "scope changed")
    # W-heap: free prefilter (heap + OGG emergency, + ANM2/texel with their
    # knobs), Vorbis live mask (Vorbis unit), free cost profile (heap +
    # phase profile, + Vorbis with the mask).  No knob adds a unit.
    def owners_of(key, *names):
        return {runtime_root / name: {"source": runtime_root / name,
                                      "definitions": {key: ["1"]}}
                for name in names}

    pre_key = "ISAAC_VITA_HEAP_FREE_PREFILTER"
    pre_on = {**cache, pre_key: "ON"}
    if gate.expected_sources(source_root, manifest, pre_on) != baseline:
        raise AssertionError("free prefilter must keep the same source closure")
    missing_pre = dict(cache)
    del missing_pre[pre_key]
    must_fail("missing free prefilter flag", lambda: gate.expected_sources(
        source_root, manifest, missing_pre), "cache is missing")
    pre_base = owners_of(pre_key, "host_vita_heap.c", "host_vita_ogg_emergency.c")
    gate.verify_heap_free_prefilter_compile_scope(pre_base, pre_on)
    must_fail("free prefilter defined while OFF", lambda:
        gate.verify_heap_free_prefilter_compile_scope(pre_base, cache), "scope changed")
    must_fail("free prefilter without the OGG record owner", lambda:
        gate.verify_heap_free_prefilter_compile_scope(
            owners_of(pre_key, "host_vita_heap.c"), pre_on), "scope changed")
    pre_blocks = {**pre_on, "ISAAC_VITA_ANM2_SCRATCH": "ON",
                  "ISAAC_VITA_TEXEL_SCRATCH": "ON"}
    pre_all = owners_of(pre_key, "host_vita_heap.c", "host_vita_ogg_emergency.c",
                        "host_vita_anm2_scratch.c", "host_vita_texel_scratch.c")
    gate.verify_heap_free_prefilter_compile_scope(pre_all, pre_blocks)
    must_fail("free prefilter texel module missing its record define", lambda:
        gate.verify_heap_free_prefilter_compile_scope(
            owners_of(pre_key, "host_vita_heap.c", "host_vita_ogg_emergency.c",
                      "host_vita_anm2_scratch.c"), pre_blocks), "scope changed")
    must_fail("free prefilter module define without its module knob", lambda:
        gate.verify_heap_free_prefilter_compile_scope(pre_all, pre_on), "scope changed")
    must_fail("free prefilter stray owner", lambda:
        gate.verify_heap_free_prefilter_compile_scope(
            {**pre_base, **owners_of(pre_key, "guest_0144.c")}, pre_on), "scope changed")
    mask_key = "ISAAC_VITA_NATIVE_VORBIS_FREE_WATCH_MASK"
    must_fail("free watch mask without free watch", lambda: gate.expected_sources(
        source_root, manifest, {**cache, mask_key: "ON"}), "requires NATIVE_VORBIS_FREE_WATCH")
    nv_watch = {**cache, "ISAAC_VITA_NATIVE_VORBIS": "ON", watch_key: "ON"}
    mask_on = {**nv_watch, mask_key: "ON"}
    if gate.expected_sources(source_root, manifest, mask_on) != \
            gate.expected_sources(source_root, manifest, nv_watch):
        raise AssertionError("free watch mask must keep the same source closure")
    mask_record = owners_of(mask_key, "host_vita_native_vorbis.c")
    gate.verify_native_vorbis_free_watch_mask_compile_scope(mask_record, mask_on)
    must_fail("free watch mask defined while OFF", lambda:
        gate.verify_native_vorbis_free_watch_mask_compile_scope(mask_record, watch_on),
        "scope changed")
    must_fail("free watch mask wrong owner", lambda:
        gate.verify_native_vorbis_free_watch_mask_compile_scope(
            owners_of(mask_key, "host_vita_heap.c"), mask_on), "scope changed")
    cost_key = "ISAAC_VITA_HEAP_FREE_COST_PROFILE"
    must_fail("free cost profile without the stage profile", lambda: gate.expected_sources(
        source_root, manifest, {**stage_parent, cost_key: "ON"}), "requires HEAP_STAGE_PROFILE")
    cost_on = {**stage_on, cost_key: "ON"}
    if gate.expected_sources(source_root, manifest, cost_on) != \
            gate.expected_sources(source_root, manifest, stage_on):
        raise AssertionError("free cost profile must keep the same source closure")
    cost_owners = owners_of(cost_key, "host_vita_heap.c", "kage_vita_phase_profile.c")
    gate.verify_heap_free_cost_profile_compile_scope(cost_owners, cost_on)
    must_fail("free cost profile defined while OFF", lambda:
        gate.verify_heap_free_cost_profile_compile_scope(cost_owners, stage_on), "scope changed")
    must_fail("free cost profile missing profiler owner", lambda:
        gate.verify_heap_free_cost_profile_compile_scope(
            owners_of(cost_key, "host_vita_heap.c"), cost_on), "scope changed")
    cost_mask = {**cost_on, watch_key: "ON", mask_key: "ON"}
    cost_nv = owners_of(cost_key, "host_vita_heap.c", "kage_vita_phase_profile.c",
                        "host_vita_native_vorbis.c")
    gate.verify_heap_free_cost_profile_compile_scope(cost_nv, cost_mask)
    must_fail("free cost profile Vorbis census without the mask", lambda:
        gate.verify_heap_free_cost_profile_compile_scope(cost_nv, cost_on), "scope changed")
    must_fail("free cost profile mask without the Vorbis census", lambda:
        gate.verify_heap_free_cost_profile_compile_scope(cost_owners, cost_mask),
        "scope changed")
    must_fail("free cost profile noncanonical", lambda:
        gate.verify_heap_free_cost_profile_compile_scope(
            {runtime_root / "host_vita_heap.c": {
                "source": runtime_root / "host_vita_heap.c",
                "definitions": {cost_key: ["2"]}}}, cost_on), "non-canonical")
    sampler_parent = {**cache, "ISAAC_VITA_GL_SHIM_FASTDISPATCH": "ON",
                      "ISAAC_VITA_SHADER_ATTRIB_FASTPATH": "ON"}
    sampler_off = gate.expected_sources(source_root, manifest, sampler_parent)
    sampler_on_cache = {**sampler_parent, "ISAAC_VITA_SHADER_SAMPLER_FASTPATH": "ON"}
    sampler_on = gate.expected_sources(source_root, manifest, sampler_on_cache)
    if (sampler_on - sampler_off !=
            {runtime_root / "host_vita_shader_sampler_fastpath.c"} or
            sampler_off - sampler_on):
        raise AssertionError("sampler ON must add exactly its one audited owner")
    for key, value in (("ISAAC_VITA_KAGE", "OFF"),
                       ("ISAAC_VITA_GL_SHIM_FASTDISPATCH", "OFF"),
                       ("ISAAC_VITA_SHADER_ATTRIB_FASTPATH", "OFF"),
                       ("ISAAC_VITA_UNIFORM_SET_RECEIPT", "ON"),
                       ("ISAAC_VITA_GUEST_SAMPLER", "ON"),
                       ("ISAAC_VITA_STALL_PROBE", "ON")):
        must_fail("sampler dependency/conflict " + key,
                  lambda key=key, value=value: gate.expected_sources(
                      source_root, manifest, {**sampler_on_cache, key: value}),
                  "ISAAC_VITA_SHADER_SAMPLER_FASTPATH requires")
    missing_sampler = dict(sampler_parent)
    del missing_sampler["ISAAC_VITA_SHADER_SAMPLER_FASTPATH"]
    must_fail("missing sampler flag", lambda: gate.expected_sources(
        source_root, manifest, missing_sampler), "cache is missing")
    must_fail("malformed sampler flag", lambda: gate.expected_sources(
        source_root, manifest, {**sampler_parent, "ISAAC_VITA_SHADER_SAMPLER_FASTPATH": "maybe"}),
        "is not ON/OFF")
    packet_parent = {**cache, "ISAAC_VITA_GL_SHIM_FASTDISPATCH": "ON",
                     "ISAAC_VITA_SHADER_ATTRIB_FASTPATH": "ON",
                     "ISAAC_VITA_GL_THREAD_RECORD": "ON",
                     "ISAAC_VITA_GL_THREAD_WORKER": "ON"}
    packet_off = gate.expected_sources(source_root, manifest, packet_parent)
    packet_on_cache = {**packet_parent, "ISAAC_VITA_KAGE_BIND_PACKET": "ON"}
    packet_on = gate.expected_sources(source_root, manifest, packet_on_cache)
    if (packet_on - packet_off !=
            {runtime_root / "host_vita_kage_bind_packet.c"} or
            packet_off - packet_on):
        raise AssertionError("bind packet ON must add exactly its one audited owner")
    for key, value in (("ISAAC_VITA_KAGE", "OFF"),
                       ("ISAAC_VITA_GL_SHIM_FASTDISPATCH", "OFF"),
                       ("ISAAC_VITA_SHADER_ATTRIB_FASTPATH", "OFF"),
                       ("ISAAC_VITA_GL_THREAD_WORKER", "OFF"),
                       ("ISAAC_VITA_UNIFORM_SET_RECEIPT", "ON"),
                       ("ISAAC_VITA_STALL_PROBE", "ON")):
        must_fail("bind packet dependency/conflict " + key,
                  lambda key=key, value=value: gate.expected_sources(
                      source_root, manifest, {**packet_on_cache, key: value}),
                  "ISAAC_VITA_KAGE_BIND_PACKET requires")
    must_fail("bind packet without the recorder", lambda: gate.expected_sources(
        source_root, manifest, {**packet_on_cache, "ISAAC_VITA_GL_THREAD_RECORD": "OFF",
                                "ISAAC_VITA_GL_THREAD_WORKER": "OFF"}),
        "ISAAC_VITA_KAGE_BIND_PACKET requires")
    missing_packet = dict(packet_parent)
    del missing_packet["ISAAC_VITA_KAGE_BIND_PACKET"]
    must_fail("missing bind packet flag", lambda: gate.expected_sources(
        source_root, manifest, missing_packet), "cache is missing")
    must_fail("malformed bind packet flag", lambda: gate.expected_sources(
        source_root, manifest, {**packet_parent, "ISAAC_VITA_KAGE_BIND_PACKET": "maybe"}),
        "is not ON/OFF")
    access_on = gate.expected_sources(
        source_root, manifest, {**cache, "ISAAC_VITA_ACCESS_NEGATIVE_CACHE": "ON"})
    if (access_on - baseline != {runtime_root / "host_vita_access_cache.c"} or
            baseline - access_on):
        raise AssertionError("access cache ON must add exactly its one owner")
    missing_access = dict(cache)
    del missing_access["ISAAC_VITA_ACCESS_NEGATIVE_CACHE"]
    must_fail("missing access cache flag", lambda: gate.expected_sources(
        source_root, manifest, missing_access), "cache is missing")
    args_on = gate.expected_sources(
        source_root, manifest, {**cache, "ISAAC_VITA_LAUNCH_ARGS_FILE": "ON"})
    if (args_on - baseline != {runtime_root / "host_vita_launch_args.c"} or
            baseline - args_on):
        raise AssertionError("launch args ON must add exactly its one owner")
    phase_on = {**cache, "ISAAC_VITA_PHASE_PROFILE": "ON"}
    guest_off = gate.expected_sources(source_root, manifest, phase_on)
    guest_on = gate.expected_sources(
        source_root, manifest, {**phase_on, "ISAAC_VITA_GUEST_SAMPLER": "ON"})
    if (guest_on - guest_off != {runtime_root / "kage_vita_guest_sampler.c"} or
            guest_off - guest_on):
        raise AssertionError("guest sampler ON must add exactly its one owner")
    # GPU frame profile (added 2026-10-17): nested under
    # KAGE > PHASE_PROFILE like the sampler, one fixed-table owner.
    gpu_on = gate.expected_sources(
        source_root, manifest, {**phase_on, "ISAAC_VITA_GPU_FRAME_PROFILE": "ON"})
    if (gpu_on - guest_off != {runtime_root / "kage_vita_gpu_frame_profile.c"} or
            guest_off - gpu_on):
        raise AssertionError("GPU frame profile ON must add exactly its one owner")
    if gate.expected_sources(
            source_root, manifest, {**cache, "ISAAC_VITA_GPU_FRAME_PROFILE": "ON"}) != baseline:
        raise AssertionError("GPU frame profile without PHASE_PROFILE must add nothing")
    missing_gpu = dict(phase_on)
    del missing_gpu["ISAAC_VITA_GPU_FRAME_PROFILE"]
    must_fail("missing GPU frame profile flag", lambda: gate.expected_sources(
        source_root, manifest, missing_gpu), "cache is missing")
    for key in ("ISAAC_VITA_KAGE", "ISAAC_VITA_PHASE_PROFILE"):
        must_fail("guest sampler dependency " + key,
                  lambda key=key: gate.expected_sources(
                      source_root, manifest,
                      {**phase_on, "ISAAC_VITA_GUEST_SAMPLER": "ON", key: "OFF"}),
                  "ISAAC_VITA_GUEST_SAMPLER requires")
    record_on_cache = {**cache, "ISAAC_VITA_GL_THREAD_RECORD": "ON"}
    record_on = gate.expected_sources(source_root, manifest, record_on_cache)
    if (record_on - baseline != {runtime_root / name for name in (
            "gl_record_backend.c", "gl_record_ring.c", "gl_record_locations.c")} or
            baseline - record_on):
        raise AssertionError("GL recorder ON must add exactly its three audited owners")
    for key, value in (("ISAAC_VITA_KAGE", "OFF"),
                       ("ISAAC_VITA_SHADER_ATTRIB_DIRECT_STATE", "ON")):
        must_fail("recorder dependency/conflict " + key,
                  lambda key=key, value=value: gate.expected_sources(
                      source_root, manifest, {**record_on_cache, key: value}),
                  "ISAAC_VITA_GL_THREAD_RECORD requires")
    owner_cache = {**record_on_cache, "ISAAC_VITA_GL_THREAD_WORKER": "ON",
                   "ISAAC_VITA_GL_OWNER_CHECK": "ON"}
    worker_only = gate.expected_sources(
        source_root, manifest, {**record_on_cache, "ISAAC_VITA_GL_THREAD_WORKER": "ON"})
    owner_on = gate.expected_sources(source_root, manifest, owner_cache)
    if worker_only != record_on:
        raise AssertionError("GL render-thread worker must add no unit")
    if owner_on - record_on != {runtime_root / "gl_owner_check.c"} or record_on - owner_on:
        raise AssertionError("GL owner check must add exactly its one audited owner")
    must_fail("owner check without worker", lambda: gate.expected_sources(
        source_root, manifest, {**owner_cache, "ISAAC_VITA_GL_THREAD_WORKER": "OFF"}),
        "ISAAC_VITA_GL_OWNER_CHECK requires")
    must_fail("worker without recorder", lambda: gate.expected_sources(
        source_root, manifest, {**cache, "ISAAC_VITA_GL_THREAD_WORKER": "ON"}),
        "ISAAC_VITA_GL_THREAD_WORKER requires")
    missing_owner = dict(cache)
    del missing_owner["ISAAC_VITA_GL_OWNER_CHECK"]
    must_fail("missing owner check flag", lambda: gate.expected_sources(
        source_root, manifest, missing_owner), "cache is missing")
    # Recorder admission cache without the overlay:
    # the recorder's own free wrap only when the overlay does not own it.
    worker_cache = {**record_on_cache, "ISAAC_VITA_GL_THREAD_WORKER": "ON"}
    standalone_cache = {**worker_cache,
                        "ISAAC_VITA_GL_RECORD_ADMISSION_CACHE_STANDALONE": "ON"}
    overlay_worker_cache = {**worker_cache, "ISAAC_VITA_RUNTIME_SETTINGS": "ON",
                            "ISAAC_VITA_DEBUG_OVERLAY": "ON"}
    free_wrap_source = runtime_root / "gl_record_free_wrap.c"
    standalone_on = gate.expected_sources(source_root, manifest, standalone_cache)
    if standalone_on - worker_only != {free_wrap_source} or worker_only - standalone_on:
        raise AssertionError(
            "admission cache standalone without the overlay must add exactly "
            "gl_record_free_wrap.c")
    overlay_worker = gate.expected_sources(source_root, manifest, overlay_worker_cache)
    standalone_overlay = gate.expected_sources(
        source_root, manifest,
        {**overlay_worker_cache, "ISAAC_VITA_GL_RECORD_ADMISSION_CACHE_STANDALONE": "ON"})
    if standalone_overlay != overlay_worker or free_wrap_source in standalone_overlay:
        raise AssertionError(
            "admission cache standalone with the overlay must add no unit")
    must_fail("admission cache standalone without worker", lambda: gate.expected_sources(
        source_root, manifest,
        {**record_on_cache, "ISAAC_VITA_GL_RECORD_ADMISSION_CACHE_STANDALONE": "ON"}),
        "ISAAC_VITA_GL_RECORD_ADMISSION_CACHE_STANDALONE requires")
    missing_standalone = dict(cache)
    del missing_standalone["ISAAC_VITA_GL_RECORD_ADMISSION_CACHE_STANDALONE"]
    must_fail("missing admission cache standalone flag", lambda: gate.expected_sources(
        source_root, manifest, missing_standalone), "cache is missing")
    missing_record = dict(cache)
    del missing_record["ISAAC_VITA_GL_THREAD_RECORD"]
    must_fail("missing recorder flag", lambda: gate.expected_sources(
        source_root, manifest, missing_record), "cache is missing")
    must_fail("malformed recorder flag", lambda: gate.expected_sources(
        source_root, manifest, {**cache, "ISAAC_VITA_GL_THREAD_RECORD": "maybe"}),
        "is not ON/OFF")
    theora_on = dict(cache)
    theora_on["ISAAC_VITA_NATIVE_THEORA"] = "ON"
    theora_on["ISAAC_VITA_NATIVE_THEORA_RGB"] = "ON"
    theora_on["ISAAC_VITA_THEORA_PRECACHE_FRAMES"] = "3"
    theora_on["ISAAC_VITA_THEORA_WORKER_PUMP"] = "1"
    theora_sources = gate.expected_sources(source_root, manifest, theora_on)
    if theora_sources - baseline != {
            runtime_root / "host_vita_native_theora.c",
            runtime_root / "host_vita_native_yuv.c",
            runtime_root / "host_vita_theora_precache.c",
            runtime_root / "host_vita_theora_worker_pump.c"}:
        raise AssertionError(
            "theora knobs must select exactly their four runtime units: "
            f"{sorted(str(p) for p in theora_sources - baseline)}")
    miniz_parent = {
        "ISAAC_VITA_HEAP_OVERFLOW_MSPACE": "ON",
        "ISAAC_VITA_ARCHIVE_MINIZ_FASTPATH": "ON",
    }
    inventory_cases = (
        ({"ISAAC_VITA_LUA": "ON"}, {"ISAAC_VITA_LUA_GCCOLLECT_CLAMP": "ON"},
         {"host_vita_lua_gc.c"}),
        ({"ISAAC_VITA_LUA": "ON"}, {"ISAAC_VITA_LUA_GC_PROFILE": "ON"},
         {"host_vita_lua_gc.c"}),
        ({"ISAAC_VITA_LUA": "ON"}, {"ISAAC_VITA_LUA_NATIVE_GETCLASS": "ON"},
         {"host_vita_lua_getclass.c"}),
        ({"ISAAC_VITA_LUA": "ON", "ISAAC_VITA_LUA_NATIVE_GETCLASS": "ON"},
         {"ISAAC_VITA_LUA_BINDING_REPLAY": "ON"},
         {"host_vita_lua_binding_replay.c"}),
        ({"ISAAC_VITA_LUA": "ON"}, {"ISAAC_VITA_LUA_COST_PROFILE": "ON"},
         {"host_vita_lua_cost.c"}),
        ({"ISAAC_VITA_LUA": "ON"}, {"ISAAC_VITA_LUA_LOADFILE_RETRY": "ON"},
         {"host_vita_lua_loadfile.c"}),
        # the finalizer pace swaps lgc.c inside the Lua archive only
        # (lua_derived_sources); no runtime unit joins the closure
        ({"ISAAC_VITA_LUA": "ON"}, {"ISAAC_VITA_LUA_GC_FINALIZER_PACE": "ON"},
         set()),
        # integer2str swaps lobject.c inside the Lua archive; its boot
        # self-test unit joins only with the cost profile (next case)
        ({"ISAAC_VITA_LUA": "ON"}, {"ISAAC_VITA_LUA_INTEGER2STR": "ON"},
         set()),
        ({"ISAAC_VITA_LUA": "ON", "ISAAC_VITA_LUA_COST_PROFILE": "ON"},
         {"ISAAC_VITA_LUA_INTEGER2STR": "ON"},
         {"host_vita_lua_integer2str_selftest.c"}),
        ({"ISAAC_VITA_FLOOR_THUNK_FASTPATH": "ON"},
         {"ISAAC_VITA_KAGE_QUAD_FASTPATH": "ON"},
         {"host_vita_kage_quad_fastpath.c"}),
        ({}, {"ISAAC_VITA_SHADER_ATTRIB_FASTPATH": "ON"},
         {"host_vita_shader_attrib_fastpath.c"}),
        ({}, {"ISAAC_VITA_GFUEL_SEED_NATIVE": "ON"},
         {"host_vita_gfuel_seed_native.c"}),
        ({}, {"ISAAC_VITA_HASCOLLECTIBLE_NATIVE": "ON"},
         {"host_vita_hascoll_native.c"}),
        ({}, {"ISAAC_VITA_SFX_FLOOR_PRELOAD": "ON"},
         {"host_vita_sfx_floor_preload.c"}),
        ({}, {"ISAAC_VITA_CAMERA_RING_NATIVE": "ON"},
         {"host_vita_camera_ring_native.c"}),
        ({}, {"ISAAC_VITA_UPDATE_PROBE": "ON"},
         {"host_vita_update_probe.c"}),
        ({}, {"ISAAC_VITA_IMAGE_RETAIN": "ON"},
         {"host_vita_image_retain.c"}),
        ({}, {"ISAAC_VITA_RENDER_CHAIN_NATIVE": "ON"},
         {"host_vita_render_chain_native.c"}),
        ({"ISAAC_VITA_AUDIO": "ON"}, {"ISAAC_VITA_AUDIO_WORKER": "ON"},
         {"host_vita_audio_refill.c"}),
        ({"ISAAC_VITA_AUDIO": "ON"}, {"ISAAC_VITA_AUDIO_REFILL_RECEIPT": "ON"},
         {"host_vita_audio_refill.c"}),
        ({"ISAAC_VITA_AUDIO": "ON"}, {"ISAAC_VITA_NATIVE_VORBIS": "ON"},
         {"host_vita_native_vorbis.c"}),
        ({"ISAAC_VITA_AUDIO": "ON", "ISAAC_VITA_NATIVE_VORBIS": "ON"},
         {"ISAAC_VITA_OGG_READAHEAD": "ON"},
         {"host_vita_ogg_readahead.c"}),
        (miniz_parent, {"ISAAC_VITA_ARCHIVE_XOR_FASTPATH": "ON"},
         {"host_vita_archive_xor.c"}),
        (miniz_parent, {"ISAAC_VITA_WAV_BUFFERED_REWIND": "ON"},
         {"host_vita_wav_buffered_rewind.c"}),
        (miniz_parent, {"ISAAC_VITA_ROOM_GRID_INIT_NATIVE": "ON"},
         {"host_vita_room_grid_init.c"}),
        (miniz_parent, {"ISAAC_VITA_PNG_PREMULTIPLY_NATIVE": "ON"},
         {"host_vita_png_premultiply.c"}),
        (miniz_parent, {"ISAAC_VITA_PNG_TEXEL_INIT_ELISION": "ON"},
         {"host_vita_png_texel_init.c"}),
        ({"ISAAC_VITA_NATIVE_PNG": "ON"},
         {"ISAAC_VITA_NATIVE_PNG_LIBDEFLATE_STRICT": "ON"},
         {"host_vita_native_png_libdeflate.c"}),
        ({"ISAAC_VITA_NATIVE_PNG": "ON"}, {"ISAAC_VITA_NATIVE_PNG_REUSE": "ON"},
         {"host_vita_png_reuse.c"}),
        ({"ISAAC_VITA_NATIVE_PNG": "ON"}, {"ISAAC_VITA_NATIVE_PNG_ADLER_NEON": "ON"},
         {"host_vita_native_png_adler.c", "host_vita_native_png_miniz.c"}),
    )
    for parent, knob, units in inventory_cases:
        parent_sources = gate.expected_sources(
            source_root, manifest, {**cache, **parent})
        knob_sources = gate.expected_sources(
            source_root, manifest, {**cache, **parent, **knob})
        added = {path.name for path in knob_sources - parent_sources}
        if added != units or (knob_sources - parent_sources) - {
                runtime_root / name for name in units}:
            raise AssertionError(
                f"{sorted(knob)} must add exactly {sorted(units)} on top of "
                f"{sorted(parent)}: got {sorted(added)}")
        # the sub-knob without its parent selects nothing (the quad replay
        # knob is refused outright, as CMake refuses it without the floor thunk)
        if "ISAAC_VITA_KAGE_QUAD_FASTPATH" in knob:
            must_fail(
                "KAGE quad fast path without the floor thunk fast path",
                lambda: gate.expected_sources(
                    source_root, manifest, {**cache, **knob}),
                "requires ISAAC_VITA_FLOOR_THUNK_FASTPATH=ON",
            )
        elif parent:
            orphan = gate.expected_sources(
                source_root, manifest, {**cache, **knob})
            if orphan != baseline:
                raise AssertionError(
                    f"{sorted(knob)} without {sorted(parent)} must select no unit")
    must_fail(
        "LuaBridge binding replay without the native getClass seam",
        lambda: gate.expected_sources(
            source_root, manifest,
            {**cache, "ISAAC_VITA_LUA": "ON", "ISAAC_VITA_LUA_BINDING_REPLAY": "ON"},
        ),
        "requires ISAAC_VITA_LUA_NATIVE_GETCLASS=ON",
    )
    must_fail(
        "inflate table memo without the archive MiniZ fast path",
        lambda: gate.expected_sources(
            source_root, manifest,
            {**cache, "ISAAC_VITA_INFLATE_TABLE_MEMO": "ON"},
        ),
        "requires ISAAC_VITA_ARCHIVE_MINIZ_FASTPATH=ON",
    )
    miniz_on = {
        **cache,
        "ISAAC_VITA_HEAP_OVERFLOW_MSPACE": "ON",
        "ISAAC_VITA_ARCHIVE_MINIZ_FASTPATH": "ON",
    }
    miniz_on_sources = gate.expected_sources(source_root, manifest, miniz_on)
    memo_on_sources = gate.expected_sources(
        source_root, manifest,
        {**miniz_on, "ISAAC_VITA_INFLATE_TABLE_MEMO": "ON"},
    )
    if (memo_on_sources - miniz_on_sources !=
            {runtime_root / "host_vita_inflate_table_memo.c"} or
            miniz_on_sources - memo_on_sources):
        raise AssertionError(
            "inflate table memo must select exactly its one runtime unit "
            "over the archive MiniZ fast path: "
            f"{sorted(str(p) for p in memo_on_sources - miniz_on_sources)}")
    must_fail(
        "uniform-set receipt without the translated KAGE target",
        lambda: gate.expected_sources(
            source_root, manifest,
            {**cache, "ISAAC_VITA_KAGE": "OFF",
             "ISAAC_VITA_UNIFORM_SET_RECEIPT": "ON"},
        ),
        "ISAAC_VITA_UNIFORM_SET_RECEIPT requires ISAAC_VITA_KAGE=ON",
    )
    uniform_receipt_sources = gate.expected_sources(
        source_root, manifest,
        {**cache, "ISAAC_VITA_UNIFORM_SET_RECEIPT": "ON"},
    )
    if (uniform_receipt_sources - baseline !=
            {runtime_root / "host_vita_uniform_set_receipt.c"} or
            baseline - uniform_receipt_sources):
        raise AssertionError(
            "uniform-set receipt must select exactly its one runtime unit: "
            f"{sorted(str(p) for p in uniform_receipt_sources - baseline)}")
    must_fail(
        "archive read trail without the CRT archive file cache",
        lambda: gate.expected_sources(
            source_root, manifest,
            {**cache, "ISAAC_VITA_PHASE_PROFILE": "ON",
             "ISAAC_VITA_ARCHIVE_READ_TRAIL": "ON"},
        ),
        "ISAAC_VITA_ARCHIVE_READ_TRAIL requires "
        "ISAAC_VITA_ARCHIVE_FILE_CACHE=ON",
    )
    must_fail(
        "archive read trail without the phase profiler",
        lambda: gate.expected_sources(
            source_root, manifest,
            {**cache, "ISAAC_VITA_ARCHIVE_FILE_CACHE": "ON",
             "ISAAC_VITA_ARCHIVE_READ_TRAIL": "ON"},
        ),
        "ISAAC_VITA_ARCHIVE_READ_TRAIL requires ISAAC_VITA_PHASE_PROFILE=ON",
    )
    must_fail(
        "archive read trail without the translated KAGE target",
        lambda: gate.expected_sources(
            source_root, manifest,
            {**cache, "ISAAC_VITA_KAGE": "OFF",
             "ISAAC_VITA_ARCHIVE_FILE_CACHE": "ON",
             "ISAAC_VITA_PHASE_PROFILE": "ON",
             "ISAAC_VITA_ARCHIVE_READ_TRAIL": "ON"},
        ),
        "ISAAC_VITA_ARCHIVE_READ_TRAIL requires ISAAC_VITA_KAGE=ON",
    )
    trail_base = {
        **cache,
        "ISAAC_VITA_ARCHIVE_FILE_CACHE": "ON",
        "ISAAC_VITA_PHASE_PROFILE": "ON",
    }
    trail_base_sources = gate.expected_sources(
        source_root, manifest, trail_base
    )
    trail_on_sources = gate.expected_sources(
        source_root, manifest,
        {**trail_base, "ISAAC_VITA_ARCHIVE_READ_TRAIL": "ON"},
    )
    if (trail_on_sources - trail_base_sources !=
            {runtime_root / "host_vita_archive_trail.c"} or
            trail_base_sources - trail_on_sources):
        raise AssertionError(
            "archive read trail must select exactly its one runtime unit "
            "over the archive file cache and the phase profiler: "
            f"{sorted(str(p) for p in trail_on_sources - trail_base_sources)}")
    # Static SFX window profile: diagnostic builds compile its TU under
    # KAGE and CMake refuses it without PHASE_PROFILE or AUDIO; the gate
    # must know the unit and refuse the knob exactly where CMake does.
    must_fail(
        "static SFX profile without the phase profiler",
        lambda: gate.expected_sources(
            source_root, manifest,
            {**cache, "ISAAC_VITA_AUDIO": "ON",
             "ISAAC_VITA_STATIC_SFX_PROFILE": "ON"},
        ),
        "ISAAC_VITA_STATIC_SFX_PROFILE requires ISAAC_VITA_PHASE_PROFILE=ON",
    )
    must_fail(
        "static SFX profile without audio",
        lambda: gate.expected_sources(
            source_root, manifest,
            {**cache, "ISAAC_VITA_PHASE_PROFILE": "ON",
             "ISAAC_VITA_STATIC_SFX_PROFILE": "ON"},
        ),
        "ISAAC_VITA_STATIC_SFX_PROFILE requires ISAAC_VITA_AUDIO=ON",
    )
    must_fail(
        "static SFX profile without the translated KAGE target",
        lambda: gate.expected_sources(
            source_root, manifest,
            {**cache, "ISAAC_VITA_KAGE": "OFF",
             "ISAAC_VITA_AUDIO": "ON",
             "ISAAC_VITA_PHASE_PROFILE": "ON",
             "ISAAC_VITA_STATIC_SFX_PROFILE": "ON"},
        ),
        "ISAAC_VITA_STATIC_SFX_PROFILE requires ISAAC_VITA_KAGE=ON",
    )
    sfx_base = {
        **cache,
        "ISAAC_VITA_AUDIO": "ON",
        "ISAAC_VITA_PHASE_PROFILE": "ON",
    }
    sfx_base_sources = gate.expected_sources(source_root, manifest, sfx_base)
    sfx_on_sources = gate.expected_sources(
        source_root, manifest,
        {**sfx_base, "ISAAC_VITA_STATIC_SFX_PROFILE": "ON"},
    )
    if (sfx_on_sources - sfx_base_sources !=
            {runtime_root / "host_vita_static_sfx_profile.c"} or
            sfx_base_sources - sfx_on_sources):
        raise AssertionError(
            "static SFX profile must select exactly its one runtime unit "
            "over audio and the phase profiler: "
            f"{sorted(str(p) for p in sfx_on_sources - sfx_base_sources)}")
    must_fail(
        "missing static SFX profile feature selection",
        lambda: gate.expected_sources(
            source_root, manifest,
            {k: v for k, v in cache.items()
             if k != "ISAAC_VITA_STATIC_SFX_PROFILE"},
        ),
        "CMake cache is missing ISAAC_VITA_STATIC_SFX_PROFILE",
    )
    # Backdrop watergfx guard: CMake compiles its log helper in every
    # translated build with the knob ON (no KAGE nesting); the gate must
    # know that one unit and nothing else (the int2l ARM build of 2026-09-27 failed
    # with extra=[kage_vita_backdrop_watergfx.c] before it did).
    watergfx_on_sources = gate.expected_sources(
        source_root, manifest,
        {**cache, "ISAAC_VITA_BACKDROP_WATERGFX_GUARD": "ON"},
    )
    if (watergfx_on_sources - baseline !=
            {runtime_root / "kage_vita_backdrop_watergfx.c"} or
            baseline - watergfx_on_sources):
        raise AssertionError(
            "backdrop watergfx guard must select exactly its one runtime "
            "unit: "
            f"{sorted(str(p) for p in watergfx_on_sources - baseline)}")
    must_fail(
        "missing backdrop watergfx guard feature selection",
        lambda: gate.expected_sources(
            source_root, manifest,
            {k: v for k, v in cache.items()
             if k != "ISAAC_VITA_BACKDROP_WATERGFX_GUARD"},
        ),
        "CMake cache is missing ISAAC_VITA_BACKDROP_WATERGFX_GUARD",
    )
    # PS Vita trophies: the TryUnlock owners are generated units already in
    # the closure; the trophy runtime is the one new unit.
    trophies_on_sources = gate.expected_sources(
        source_root, manifest,
        {**cache, "ISAAC_VITA_TROPHIES": "ON"},
    )
    if (trophies_on_sources - baseline !=
            {runtime_root / "host_vita_trophies.c"} or
            baseline - trophies_on_sources):
        raise AssertionError(
            "trophies must select exactly their one runtime unit: "
            f"{sorted(str(p) for p in trophies_on_sources - baseline)}")
    must_fail(
        "missing trophies feature selection",
        lambda: gate.expected_sources(
            source_root, manifest,
            {k: v for k, v in cache.items()
             if k != "ISAAC_VITA_TROPHIES"},
        ),
        "CMake cache is missing ISAAC_VITA_TROPHIES",
    )
    # The loadfile retry key is read only inside the Lua block.
    must_fail(
        "missing Lua loadfile retry feature selection",
        lambda: gate.expected_sources(
            source_root, manifest,
            {k: v for k, v in {**cache, "ISAAC_VITA_LUA": "ON"}.items()
             if k != "ISAAC_VITA_LUA_LOADFILE_RETRY"},
        ),
        "CMake cache is missing ISAAC_VITA_LUA_LOADFILE_RETRY",
    )
    must_fail(
        "archive validation sidecar without the validation receipt",
        lambda: gate.expected_sources(
            source_root, manifest,
            {**cache, "ISAAC_VITA_ARCHIVE_VALIDATION_SIDECAR": "ON"},
        ),
        "ISAAC_VITA_ARCHIVE_VALIDATION_SIDECAR requires "
        "ISAAC_VITA_ARCHIVE_VALIDATION_RECEIPT=ON",
    )
    must_fail(
        "archive validation sidecar without the translated KAGE target",
        lambda: gate.expected_sources(
            source_root, manifest,
            {**cache, "ISAAC_VITA_KAGE": "OFF",
             "ISAAC_VITA_ARCHIVE_VALIDATION_RECEIPT": "ON",
             "ISAAC_VITA_ARCHIVE_VALIDATION_SIDECAR": "ON"},
        ),
        "ISAAC_VITA_ARCHIVE_VALIDATION_SIDECAR requires ISAAC_VITA_KAGE=ON",
    )
    receipt_on = {**cache, "ISAAC_VITA_ARCHIVE_VALIDATION_RECEIPT": "ON"}
    receipt_on_sources = gate.expected_sources(
        source_root, manifest, receipt_on
    )
    sidecar_on_sources = gate.expected_sources(
        source_root, manifest,
        {**receipt_on, "ISAAC_VITA_ARCHIVE_VALIDATION_SIDECAR": "ON"},
    )
    if (sidecar_on_sources - receipt_on_sources !=
            {runtime_root / "host_vita_archive_sidecar.c"} or
            receipt_on_sources - sidecar_on_sources):
        raise AssertionError(
            "archive validation sidecar must select exactly its one runtime "
            "unit over the validation receipt: "
            f"{sorted(str(p) for p in sidecar_on_sources - receipt_on_sources)}")
    must_fail(
        "game log write-behind without the archive file cache",
        lambda: gate.expected_sources(
            source_root, manifest,
            {**cache, "ISAAC_VITA_GAME_LOG_ASYNC": "ON"},
        ),
        "requires ISAAC_VITA_ARCHIVE_FILE_CACHE=ON",
    )
    must_fail(
        "game log write-behind without the asynchronous logger",
        lambda: gate.expected_sources(
            source_root, manifest,
            {**cache, "ISAAC_VITA_ARCHIVE_FILE_CACHE": "ON",
             "ISAAC_VITA_GAME_LOG_ASYNC": "ON"},
        ),
        "requires ISAAC_VITA_LOG_ASYNC=ON",
    )
    game_log_deps = {
        **cache,
        "ISAAC_VITA_ARCHIVE_FILE_CACHE": "ON",
        "ISAAC_VITA_LOG_ASYNC": "ON",
    }
    game_log_deps_sources = gate.expected_sources(
        source_root, manifest, game_log_deps
    )
    game_log_on_sources = gate.expected_sources(
        source_root, manifest,
        {**game_log_deps, "ISAAC_VITA_GAME_LOG_ASYNC": "ON"},
    )
    if (game_log_on_sources - game_log_deps_sources !=
            {runtime_root / "host_vita_game_log.c"} or
            game_log_deps_sources - game_log_on_sources):
        raise AssertionError(
            "game log write-behind must select exactly its one runtime unit "
            "over the archive file cache and the asynchronous logger: "
            f"{sorted(str(p) for p in game_log_on_sources - game_log_deps_sources)}")
    bad_pump = dict(cache)
    bad_pump["ISAAC_VITA_THEORA_WORKER_PUMP"] = "one"
    must_fail(
        "non-numeric theora pump knob",
        lambda: gate.expected_sources(source_root, manifest, bad_pump),
        "is not a whole number",
    )
    missing_audio_receipt = dict(cache)
    del missing_audio_receipt["ISAAC_VITA_AUDIO_STREAM_RECEIPT"]
    must_fail(
        "missing audio-stream receipt feature selection",
        lambda: gate.expected_sources(
            source_root, manifest, missing_audio_receipt
        ),
        "CMake cache is missing ISAAC_VITA_AUDIO_STREAM_RECEIPT",
    )
    must_fail(
        "malformed audio-stream receipt feature selection",
        lambda: gate.expected_sources(
            source_root,
            manifest,
            {**cache, "ISAAC_VITA_AUDIO_STREAM_RECEIPT": "AUTO"},
        ),
        "CMake cache ISAAC_VITA_AUDIO_STREAM_RECEIPT is not ON/OFF",
    )
    must_fail(
        "audio-stream receipt without audio",
        lambda: gate.expected_sources(
            source_root,
            manifest,
            {**cache, "ISAAC_VITA_AUDIO_STREAM_RECEIPT": "ON"},
        ),
        "requires ISAAC_VITA_KAGE=ON and ISAAC_VITA_AUDIO=ON",
    )
    audio_on_sources = gate.expected_sources(
        source_root,
        manifest,
        {**cache, "ISAAC_VITA_AUDIO": "ON"},
    )
    receipt_on_sources = gate.expected_sources(
        source_root,
        manifest,
        {
            **cache,
            "ISAAC_VITA_AUDIO": "ON",
            "ISAAC_VITA_AUDIO_STREAM_RECEIPT": "ON",
        },
    )
    if receipt_on_sources != audio_on_sources:
        raise AssertionError(
            "audio-stream receipt unexpectedly changed source closure"
        )
    missing_png_native = dict(cache)
    del missing_png_native["ISAAC_VITA_PNG_NATIVE_UNFILTER"]
    must_fail(
        "missing PNG native feature selection",
        lambda: gate.expected_sources(source_root, manifest, missing_png_native),
        "CMake cache is missing ISAAC_VITA_PNG_NATIVE_UNFILTER",
    )
    must_fail(
        "malformed PNG native feature selection",
        lambda: gate.expected_sources(
            source_root, manifest,
            {**cache, "ISAAC_VITA_PNG_NATIVE_UNFILTER": "AUTO"},
        ),
        "CMake cache ISAAC_VITA_PNG_NATIVE_UNFILTER is not ON/OFF",
    )
    native_without_overflow = {
        **cache,
        "ISAAC_VITA_PNG_NATIVE_UNFILTER": "ON",
    }
    must_fail(
        "PNG native feature without overflow range ledger",
        lambda: gate.expected_sources(
            source_root, manifest, native_without_overflow
        ),
        "requires ISAAC_VITA_HEAP_OVERFLOW_MSPACE=ON",
    )
    native_base = {
        **cache,
        "ISAAC_VITA_HEAP_OVERFLOW_MSPACE": "ON",
    }
    native_off_sources = gate.expected_sources(
        source_root, manifest, native_base
    )
    native_on_sources = gate.expected_sources(
        source_root, manifest,
        {**native_base, "ISAAC_VITA_PNG_NATIVE_UNFILTER": "ON"},
    )
    expected_native_delta = {
        (runtime_root / "host_vita_png_unfilter_native.c").resolve(),
        (runtime_root / "host_vita_png_unfilter_guest.c").resolve(),
    }
    if (native_on_sources - native_off_sources != expected_native_delta or
            native_off_sources - native_on_sources):
        raise AssertionError(
            "PNG native allocator closure lost its exact two-file delta"
        )
    inflate_on_sources = gate.expected_sources(
        source_root, manifest,
        {
            **native_base,
            "ISAAC_VITA_PNG_INFLATE_FLUSH_FASTPATH": "ON",
        },
    )
    expected_inflate_delta = {
        (runtime_root / "host_vita_zlib_inflate_flush_native.c").resolve(),
        (runtime_root / "host_vita_zlib_inflate_flush_guest.c").resolve(),
    }
    if (inflate_on_sources - native_off_sources != expected_inflate_delta or
            native_off_sources - inflate_on_sources):
        raise AssertionError(
            "PNG inflate-flush allocator closure lost its exact two-file delta"
        )
    missing_crc32 = dict(cache)
    del missing_crc32["ISAAC_VITA_PNG_CRC32_FASTPATH"]
    must_fail(
        "missing PNG CRC32 feature selection",
        lambda: gate.expected_sources(source_root, manifest, missing_crc32),
        "CMake cache is missing ISAAC_VITA_PNG_CRC32_FASTPATH",
    )
    must_fail(
        "malformed PNG CRC32 feature selection",
        lambda: gate.expected_sources(
            source_root, manifest,
            {**cache, "ISAAC_VITA_PNG_CRC32_FASTPATH": "AUTO"},
        ),
        "CMake cache ISAAC_VITA_PNG_CRC32_FASTPATH is not ON/OFF",
    )
    crc32_on_sources = gate.expected_sources(
        source_root, manifest,
        {**cache, "ISAAC_VITA_PNG_CRC32_FASTPATH": "ON"},
    )
    expected_crc32_delta = {
        (runtime_root / "host_vita_png_crc32_native.c").resolve(),
        (runtime_root / "host_vita_png_crc32_guest.c").resolve(),
    }
    if (crc32_on_sources - baseline != expected_crc32_delta or
            baseline - crc32_on_sources):
        raise AssertionError(
            "PNG CRC32 allocator closure lost its exact two-file delta"
        )
    checksum_on_sources = gate.expected_sources(
        source_root, manifest,
        {**cache, "ISAAC_VITA_SAVE_CHECKSUM_FASTPATH": "ON"},
    )
    expected_checksum_delta = {
        (runtime_root / "host_vita_save_checksum_native.c").resolve(),
        (runtime_root / "host_vita_save_checksum_guest.c").resolve(),
    }
    if (checksum_on_sources - baseline != expected_checksum_delta or
            baseline - checksum_on_sources):
        raise AssertionError(
            "save-checksum allocator closure lost its exact two-file delta"
        )
    reader_on_sources = gate.expected_sources(
        source_root, manifest,
        {**cache, "ISAAC_VITA_SAVE_READER_FASTPATH": "ON"},
    )
    expected_reader_delta = {
        (runtime_root / "host_vita_save_reader_guest.c").resolve(),
    }
    if (reader_on_sources - baseline != expected_reader_delta or
            baseline - reader_on_sources):
        raise AssertionError(
            "save-reader allocator closure lost its exact one-file delta"
        )
    read32_on_sources = gate.expected_sources(
        source_root, manifest,
        {
            **cache,
            "ISAAC_VITA_SAVE_CHECKSUM_FASTPATH": "ON",
            "ISAAC_VITA_SAVE_READER_FASTPATH": "ON",
            "ISAAC_VITA_SAVE_READ32_FUSED": "ON",
        },
    )
    expected_read32_delta = (
        expected_checksum_delta | expected_reader_delta | {
            (runtime_root / "host_vita_save_read32_guest.c").resolve(),
        }
    )
    if (read32_on_sources - baseline != expected_read32_delta or
            baseline - read32_on_sources):
        raise AssertionError(
            "save-read32 allocator closure lost its exact four-file delta"
        )
    direct_on_sources = gate.expected_sources(
        source_root, manifest,
        {
            **cache,
            "ISAAC_VITA_SAVE_READER_FASTPATH": "ON",
            "ISAAC_VITA_SAVE_READER_DIRECT_EDGES": "ON",
        },
    )
    expected_direct_delta = expected_reader_delta | {
        (runtime_root / "host_vita_save_reader_direct.c").resolve(),
    }
    if (direct_on_sources - baseline != expected_direct_delta or
            baseline - direct_on_sources):
        raise AssertionError(
            "save-reader direct allocator closure lost its exact two-file delta"
        )
    memset_on_sources = gate.expected_sources(
        source_root, manifest,
        {**cache, "ISAAC_VITA_MEMSET_THUNK_FASTPATH": "ON"},
    )
    expected_memset_delta = {
        (runtime_root / "host_vita_memset_thunk_direct.c").resolve(),
    }
    if (memset_on_sources - baseline != expected_memset_delta or
            baseline - memset_on_sources):
        raise AssertionError(
            "memset-thunk allocator closure lost its exact one-file delta"
        )
    must_fail(
        "KAGE mutex seam without the inline sync fast path",
        lambda: gate.expected_sources(
            source_root, manifest,
            {**cache, "ISAAC_VITA_KAGE_MUTEX_SEAM": "ON"},
        ),
        "requires ISAAC_VITA_SYNC_INLINE_FASTPATH=ON",
    )
    kage_mutex_on_sources = gate.expected_sources(
        source_root, manifest,
        {**cache, "ISAAC_VITA_KAGE_MUTEX_SEAM": "ON",
         "ISAAC_VITA_SYNC_INLINE_FASTPATH": "ON"},
    )
    expected_kage_mutex_delta = {
        (runtime_root / "host_vita_kage_mutex_seam.c").resolve(),
    }
    if (kage_mutex_on_sources - baseline != expected_kage_mutex_delta or
            baseline - kage_mutex_on_sources):
        raise AssertionError(
            "KAGE mutex seam allocator closure lost its exact one-file delta"
        )
    must_fail(
        "KAGE refcount seam without the mutex seam",
        lambda: gate.expected_sources(
            source_root, manifest,
            {**cache, "ISAAC_VITA_KAGE_REFCOUNT_SEAM": "ON",
             "ISAAC_VITA_SYNC_INLINE_FASTPATH": "ON"},
        ),
        "requires ISAAC_VITA_KAGE_MUTEX_SEAM=ON",
    )
    must_fail(
        "KAGE refcount seam without the inline sync fast path",
        lambda: gate.expected_sources(
            source_root, manifest,
            {**cache, "ISAAC_VITA_KAGE_REFCOUNT_SEAM": "ON",
             "ISAAC_VITA_KAGE_MUTEX_SEAM": "ON"},
        ),
        "requires ISAAC_VITA_SYNC_INLINE_FASTPATH=ON",
    )
    for key in ("ISAAC_VITA_TRANSLATED_CPU",
                "ISAAC_VITA_TRANSLATED_CPU_FLAGS_LOCAL"):
        must_fail(
            f"KAGE refcount seam with {key}=OFF in the cache",
            lambda key=key: gate.expected_sources(
                source_root, manifest,
                {**cache, "ISAAC_VITA_KAGE_REFCOUNT_SEAM": "ON",
                 "ISAAC_VITA_KAGE_MUTEX_SEAM": "ON",
                 "ISAAC_VITA_SYNC_INLINE_FASTPATH": "ON",
                 "ISAAC_VITA_TRANSLATED_CPU": "ON",
                 "ISAAC_VITA_TRANSLATED_CPU_FLAGS_LOCAL": "ON",
                 key: "OFF"},
            ),
            f"requires {key}=ON",
        )
    # With both keys present and ON (the real cache) the closure is the
    # exact one-file delta over the mutex-ON set; the keys add no source.
    kage_refcount_on_sources = gate.expected_sources(
        source_root, manifest,
        {**cache, "ISAAC_VITA_KAGE_REFCOUNT_SEAM": "ON",
         "ISAAC_VITA_KAGE_MUTEX_SEAM": "ON",
         "ISAAC_VITA_SYNC_INLINE_FASTPATH": "ON",
         "ISAAC_VITA_TRANSLATED_CPU": "ON",
         "ISAAC_VITA_TRANSLATED_CPU_FLAGS_LOCAL": "ON"},
    )
    expected_kage_refcount_delta = {
        (runtime_root / "host_vita_kage_refcount_seam.c").resolve(),
    }
    if (kage_refcount_on_sources - kage_mutex_on_sources !=
            expected_kage_refcount_delta or
            kage_mutex_on_sources - kage_refcount_on_sources):
        raise AssertionError(
            "KAGE refcount seam allocator closure lost its exact one-file "
            "delta over the mutex seam"
        )
    floor_on_sources = gate.expected_sources(
        source_root, manifest,
        {**cache, "ISAAC_VITA_FLOOR_THUNK_FASTPATH": "ON"},
    )
    expected_floor_delta = {
        (runtime_root / "host_vita_floor_thunk_direct.c").resolve(),
    }
    if (floor_on_sources - baseline != expected_floor_delta or
            baseline - floor_on_sources):
        raise AssertionError(
            "floor-thunk allocator closure lost its exact one-file delta"
        )
    bundle3_features = (
        (
            "ISAAC_VITA_ARCHIVE_VALIDATION_RECEIPT",
            "archive-validation receipt",
            "host_vita_archive_receipt.c",
        ),
        (
            "ISAAC_VITA_RENDERFRAME_FASTPATH",
            "RenderFrame fast path",
            "host_vita_renderframe_fastpath.c",
        ),
        (
            "ISAAC_VITA_CONTINUE_PROFILE",
            "Continue profile",
            "kage_vita_continue_profile.c",
        ),
        (
            "ISAAC_VITA_EXIT_MENU_PROFILE",
            "Exit-menu profile",
            "kage_vita_exit_menu_profile.c",
        ),
        (
            "ISAAC_VITA_PNG_DECODE_PROFILE",
            "PNG decode profile",
            "kage_vita_png_decode_profile.c",
        ),
        (
            "ISAAC_VITA_SIM_CADENCE_RECEIPT",
            "simulation-cadence receipt",
            "kage_vita_sim_cadence_receipt.c",
        ),
        (
            "ISAAC_VITA_WORLD_SEAM_DIAG",
            "world-seam diagnostic",
            "kage_vita_world_seam_diag.c",
        ),
    )
    bundle3_sources = set()
    for feature, label, filename in bundle3_features:
        source = (runtime_root / filename).resolve()
        bundle3_sources.add(source)
        if source in baseline:
            raise AssertionError(f"{label} source entered the closure while OFF")
        cache[feature] = "ON"
        enabled = gate.expected_sources(source_root, manifest, cache)
        if enabled - baseline != {source} or baseline - enabled:
            raise AssertionError(
                f"{label} allocator closure lost its exact one-file delta"
            )
        cache[feature] = "OFF"

        missing = dict(cache)
        del missing[feature]
        must_fail(
            f"missing {label} feature selection",
            lambda missing=missing: gate.expected_sources(
                source_root, manifest, missing
            ),
            f"CMake cache is missing {feature}",
        )
        must_fail(
            f"malformed {label} feature selection",
            lambda feature=feature: gate.expected_sources(
                source_root, manifest, {**cache, feature: "AUTO"}
            ),
            f"CMake cache {feature} is not ON/OFF",
        )

    bundle3_all_on_cache = {
        **cache,
        **{feature: "ON" for feature, _, _ in bundle3_features},
    }
    bundle3_all_on = gate.expected_sources(
        source_root, manifest, bundle3_all_on_cache
    )
    if (bundle3_all_on - baseline != bundle3_sources or
            baseline - bundle3_all_on or len(bundle3_all_on) != 49):
        raise AssertionError(
            "diagnostic all-on allocator closure is not the exact 49-source set"
        )

    kage_off_cache = {**cache, "ISAAC_VITA_KAGE": "OFF"}
    kage_off = gate.expected_sources(source_root, manifest, kage_off_cache)
    kage_required_features = tuple(
        row for row in bundle3_features
        if row[0] not in {
            "ISAAC_VITA_ARCHIVE_VALIDATION_RECEIPT",
            "ISAAC_VITA_EXIT_MENU_PROFILE",
        }
    )
    for feature, label, filename in kage_required_features:
        source = (runtime_root / filename).resolve()
        if source in kage_off:
            raise AssertionError(f"{label} source entered the KAGE-OFF closure")
        must_fail(
            f"{label} without KAGE",
            lambda feature=feature: gate.expected_sources(
                source_root, manifest, {**kage_off_cache, feature: "ON"}
            ),
            f"{feature} requires ISAAC_VITA_KAGE=ON",
        )
    archive_source = (runtime_root / "host_vita_archive_receipt.c").resolve()
    archive_kage_off = gate.expected_sources(
        source_root,
        manifest,
        {
            **kage_off_cache,
            "ISAAC_VITA_ARCHIVE_VALIDATION_RECEIPT": "ON",
        },
    )
    if archive_kage_off - kage_off != {archive_source} or \
            kage_off - archive_kage_off:
        raise AssertionError(
            "archive-validation receipt gained an accidental KAGE dependency"
        )

    cache["ISAAC_VITA_FIOS_CACHE"] = "ON"
    fios = gate.expected_sources(source_root, manifest, cache)
    fios_source = (runtime_root / "host_vita_fios_cache.c").resolve()
    if fios - baseline != {fios_source} or baseline - fios:
        raise AssertionError("FIOS cache source closure is not exact")
    cache["ISAAC_VITA_FIOS_CACHE"] = "OFF"
    cache["ISAAC_VITA_STALL_PROBE"] = "ON"
    probed = gate.expected_sources(source_root, manifest, cache)
    expected_delta = {
        (runtime_root / "kage_vita_stall_probe.c").resolve(),
        (runtime_root / "kage_vita_preloop_phase.c").resolve(),
    }
    if probed - baseline != expected_delta or baseline - probed:
        raise AssertionError(
            "STALL_PROBE allocator source closure lost its exact two-file delta"
        )

    cache["ISAAC_VITA_STALL_PROBE"] = "OFF"
    phase_off = gate.expected_sources(source_root, manifest, cache)
    phase_source = (runtime_root / "kage_vita_phase_profile.c").resolve()
    if phase_source in phase_off:
        raise AssertionError(
            "PHASE_PROFILE source entered the allocator closure while OFF"
        )
    cache["ISAAC_VITA_PHASE_PROFILE"] = "ON"
    phase_on = gate.expected_sources(source_root, manifest, cache)
    if phase_on - phase_off != {phase_source} or phase_off - phase_on:
        raise AssertionError(
            "PHASE_PROFILE allocator source closure lost its exact one-file delta"
        )
    must_fail(
        "simulation-cadence receipt with phase profile",
        lambda: gate.expected_sources(
            source_root, manifest,
            {**cache, "ISAAC_VITA_SIM_CADENCE_RECEIPT": "ON"},
        ),
        "requires ISAAC_VITA_PHASE_PROFILE=OFF",
    )
    missing_phase = dict(cache)
    del missing_phase["ISAAC_VITA_PHASE_PROFILE"]
    must_fail(
        "missing phase-profile feature selection",
        lambda: gate.expected_sources(source_root, manifest, missing_phase),
        "CMake cache is missing ISAAC_VITA_PHASE_PROFILE",
    )
    must_fail(
        "malformed phase-profile feature selection",
        lambda: gate.expected_sources(
            source_root, manifest,
            {**cache, "ISAAC_VITA_PHASE_PROFILE": "AUTO"},
        ),
        "CMake cache ISAAC_VITA_PHASE_PROFILE is not ON/OFF",
    )
    missing_texture_churn = dict(cache)
    del missing_texture_churn["ISAAC_VITA_TEXTURE_CHURN_PROFILE"]
    must_fail(
        "missing texture-churn feature selection",
        lambda: gate.expected_sources(
            source_root, manifest, missing_texture_churn
        ),
        "CMake cache is missing ISAAC_VITA_TEXTURE_CHURN_PROFILE",
    )
    must_fail(
        "malformed texture-churn feature selection",
        lambda: gate.expected_sources(
            source_root, manifest,
            {**cache, "ISAAC_VITA_TEXTURE_CHURN_PROFILE": "AUTO"},
        ),
        "CMake cache ISAAC_VITA_TEXTURE_CHURN_PROFILE is not ON/OFF",
    )
    must_fail(
        "texture-churn without phase profile",
        lambda: gate.expected_sources(
            source_root, manifest,
            {
                **cache,
                "ISAAC_VITA_PHASE_PROFILE": "OFF",
                "ISAAC_VITA_TEXTURE_CHURN_PROFILE": "ON",
                "ISAAC_VITA_VITAGL_STOCK_REFERENCE": "ON",
            },
        ),
        "requires ISAAC_VITA_PHASE_PROFILE=ON",
    )
    must_fail(
        "texture-churn without stock vitaGL",
        lambda: gate.expected_sources(
            source_root, manifest,
            {
                **cache,
                "ISAAC_VITA_TEXTURE_CHURN_PROFILE": "ON",
                "ISAAC_VITA_VITAGL_STOCK_REFERENCE": "OFF",
            },
        ),
        "requires ISAAC_VITA_VITAGL_STOCK_REFERENCE=ON",
    )
    texture_churn_on = gate.expected_sources(
        source_root, manifest,
        {
            **cache,
            "ISAAC_VITA_TEXTURE_CHURN_PROFILE": "ON",
            "ISAAC_VITA_VITAGL_STOCK_REFERENCE": "ON",
        },
    )
    if texture_churn_on != phase_on:
        raise AssertionError(
            "texture-churn profile changed the phase-profile source closure"
        )
    cache["ISAAC_VITA_PHASE_PROFILE"] = "OFF"
    scheduler_off = gate.expected_sources(source_root, manifest, cache)
    scheduler_source = (
        runtime_root / "kage_vita_fullspeed_scheduler.c"
    ).resolve()
    if scheduler_source in scheduler_off:
        raise AssertionError(
            "FULLSPEED_SCHEDULER source entered the allocator closure while OFF"
        )
    cache["ISAAC_VITA_FULLSPEED_SCHEDULER"] = "ON"
    scheduler_on = gate.expected_sources(source_root, manifest, cache)
    if scheduler_on - scheduler_off != {scheduler_source} or \
            scheduler_off - scheduler_on:
        raise AssertionError(
            "FULLSPEED_SCHEDULER allocator closure lost its exact one-file delta"
        )
    missing_scheduler = dict(cache)
    del missing_scheduler["ISAAC_VITA_FULLSPEED_SCHEDULER"]
    must_fail(
        "missing fullspeed-scheduler feature selection",
        lambda: gate.expected_sources(
            source_root, manifest, missing_scheduler
        ),
        "CMake cache is missing ISAAC_VITA_FULLSPEED_SCHEDULER",
    )
    must_fail(
        "malformed fullspeed-scheduler feature selection",
        lambda: gate.expected_sources(
            source_root, manifest,
            {**cache, "ISAAC_VITA_FULLSPEED_SCHEDULER": "AUTO"},
        ),
        "CMake cache ISAAC_VITA_FULLSPEED_SCHEDULER is not ON/OFF",
    )
    cache["ISAAC_VITA_FULLSPEED_SCHEDULER"] = "OFF"
    stable30_off = gate.expected_sources(source_root, manifest, cache)
    stable30_source = (runtime_root / "kage_vita_stable30.c").resolve()
    if stable30_source in stable30_off:
        raise AssertionError(
            "STABLE_30_PRESENTATION source entered allocator closure while OFF"
        )
    cache["ISAAC_VITA_STABLE_30_PRESENTATION"] = "ON"
    stable30_on = gate.expected_sources(source_root, manifest, cache)
    if stable30_on - stable30_off != {stable30_source} or \
            stable30_off - stable30_on:
        raise AssertionError(
            "STABLE_30_PRESENTATION closure lost its exact one-file delta"
        )
    cache["ISAAC_VITA_FULLSPEED_SCHEDULER"] = "ON"
    must_fail(
        "stable30/fullspeed conflict",
        lambda: gate.expected_sources(source_root, manifest, cache),
        "are exclusive",
    )
    cache["ISAAC_VITA_FULLSPEED_SCHEDULER"] = "OFF"
    cache["ISAAC_VITA_STABLE_30_PRESENTATION"] = "OFF"
    missing_stable30 = dict(cache)
    del missing_stable30["ISAAC_VITA_STABLE_30_PRESENTATION"]
    must_fail(
        "missing stable30 feature selection",
        lambda: gate.expected_sources(
            source_root, manifest, missing_stable30
        ),
        "CMake cache is missing ISAAC_VITA_STABLE_30_PRESENTATION",
    )
    must_fail(
        "malformed stable30 feature selection",
        lambda: gate.expected_sources(
            source_root, manifest,
            {**cache, "ISAAC_VITA_STABLE_30_PRESENTATION": "AUTO"},
        ),
        "CMake cache ISAAC_VITA_STABLE_30_PRESENTATION is not ON/OFF",
    )
    settings_off = gate.expected_sources(source_root, manifest, cache)
    settings_sources = {
        (runtime_root / "host_vita_settings_parse.c").resolve(),
        (runtime_root / "host_vita_runtime_settings.c").resolve(),
    }
    if settings_sources & settings_off:
        raise AssertionError(
            "RUNTIME_SETTINGS sources entered the allocator closure while OFF"
        )
    cache["ISAAC_VITA_RUNTIME_SETTINGS"] = "ON"
    settings_on = gate.expected_sources(source_root, manifest, cache)
    if settings_on - settings_off != settings_sources or \
            settings_off - settings_on:
        raise AssertionError(
            "RUNTIME_SETTINGS closure lost its exact two-file delta"
        )
    cache["ISAAC_VITA_FULLSPEED_SCHEDULER"] = "ON"
    settings_scheduler_on = gate.expected_sources(source_root, manifest, cache)
    if settings_scheduler_on - settings_on != {scheduler_source} or \
            settings_on - settings_scheduler_on:
        raise AssertionError(
            "RUNTIME_SETTINGS + FULLSPEED_SCHEDULER closure is not the "
            "exact three-file delta"
        )
    cache["ISAAC_VITA_FULLSPEED_SCHEDULER"] = "OFF"
    must_fail(
        "runtime settings without KAGE",
        lambda: gate.expected_sources(
            source_root, manifest, {**cache, "ISAAC_VITA_KAGE": "OFF"}
        ),
        "ISAAC_VITA_RUNTIME_SETTINGS requires ISAAC_VITA_KAGE=ON",
    )
    must_fail(
        "runtime settings with the io profile",
        lambda: gate.expected_sources(
            source_root, manifest, {**cache, "ISAAC_VITA_IO_PROFILE": "ON"}
        ),
        "ISAAC_VITA_RUNTIME_SETTINGS requires ISAAC_VITA_IO_PROFILE=OFF",
    )
    cache["ISAAC_VITA_RUNTIME_SETTINGS"] = "OFF"
    missing_settings = dict(cache)
    del missing_settings["ISAAC_VITA_RUNTIME_SETTINGS"]
    must_fail(
        "missing runtime-settings feature selection",
        lambda: gate.expected_sources(
            source_root, manifest, missing_settings
        ),
        "CMake cache is missing ISAAC_VITA_RUNTIME_SETTINGS",
    )
    must_fail(
        "malformed runtime-settings feature selection",
        lambda: gate.expected_sources(
            source_root, manifest,
            {**cache, "ISAAC_VITA_RUNTIME_SETTINGS": "AUTO"},
        ),
        "CMake cache ISAAC_VITA_RUNTIME_SETTINGS is not ON/OFF",
    )
    # Manager settings page: a fail-closed key that selects no eboot source
    # (its units go to isaac_manager only) and requires the runtime knob.
    if gate.expected_sources(source_root, manifest, cache) != settings_off:
        raise AssertionError(
            "MANAGER_SETTINGS_UI=OFF changed the eboot closure"
        )
    manager_ui_on = gate.expected_sources(
        source_root, manifest,
        {**cache, "ISAAC_VITA_RUNTIME_SETTINGS": "ON",
         "ISAAC_VITA_MANAGER_SETTINGS_UI": "ON"},
    )
    if manager_ui_on != settings_on:
        raise AssertionError(
            "MANAGER_SETTINGS_UI=ON added or removed an eboot source"
        )
    must_fail(
        "manager settings page without runtime settings",
        lambda: gate.expected_sources(
            source_root, manifest,
            {**cache, "ISAAC_VITA_MANAGER_SETTINGS_UI": "ON"},
        ),
        "ISAAC_VITA_MANAGER_SETTINGS_UI requires ISAAC_VITA_RUNTIME_SETTINGS=ON",
    )
    missing_manager_ui = dict(cache)
    del missing_manager_ui["ISAAC_VITA_MANAGER_SETTINGS_UI"]
    must_fail(
        "missing manager-settings feature selection",
        lambda: gate.expected_sources(
            source_root, manifest, missing_manager_ui
        ),
        "CMake cache is missing ISAAC_VITA_MANAGER_SETTINGS_UI",
    )
    must_fail(
        "malformed manager-settings feature selection",
        lambda: gate.expected_sources(
            source_root, manifest,
            {**cache, "ISAAC_VITA_MANAGER_SETTINGS_UI": "AUTO"},
        ),
        "CMake cache ISAAC_VITA_MANAGER_SETTINGS_UI is not ON/OFF",
    )
    # Render-scale default: a runtime-settings sub-knob that selects
    # exactly one extra loader-side unit and requires the runtime knob.
    render_scale_source = (
        runtime_root / "host_vita_render_scale.c"
    ).resolve()
    if render_scale_source in settings_on:
        raise AssertionError(
            "RENDER_SCALE_DEFAULT source entered the closure while OFF"
        )
    render_scale_on = gate.expected_sources(
        source_root, manifest,
        {**cache, "ISAAC_VITA_RUNTIME_SETTINGS": "ON",
         "ISAAC_VITA_RENDER_SCALE_DEFAULT": "ON"},
    )
    if render_scale_on - settings_on != {render_scale_source} or \
            settings_on - render_scale_on:
        raise AssertionError(
            "RENDER_SCALE_DEFAULT closure lost its exact one-file delta"
        )
    must_fail(
        "render-scale default without runtime settings",
        lambda: gate.expected_sources(
            source_root, manifest,
            {**cache, "ISAAC_VITA_RENDER_SCALE_DEFAULT": "ON"},
        ),
        "ISAAC_VITA_RENDER_SCALE_DEFAULT requires "
        "ISAAC_VITA_RUNTIME_SETTINGS=ON",
    )
    missing_render_scale = dict(cache)
    del missing_render_scale["ISAAC_VITA_RENDER_SCALE_DEFAULT"]
    must_fail(
        "missing render-scale feature selection",
        lambda: gate.expected_sources(
            source_root, manifest, missing_render_scale
        ),
        "CMake cache is missing ISAAC_VITA_RENDER_SCALE_DEFAULT",
    )
    # On-screen debug overlay: one unit inside the runtime-settings block;
    # the ARM build's ninja ALL gate must know it (int2l-style extra=[...] guard).
    overlay_source = (runtime_root / "host_vita_debug_overlay.c").resolve()
    if overlay_source in settings_on:
        raise AssertionError("DEBUG_OVERLAY source entered the closure while OFF")
    overlay_on = gate.expected_sources(
        source_root, manifest,
        {**cache, "ISAAC_VITA_RUNTIME_SETTINGS": "ON",
         "ISAAC_VITA_DEBUG_OVERLAY": "ON"},
    )
    if overlay_on - settings_on != {overlay_source} or \
            settings_on - overlay_on:
        raise AssertionError(
            "DEBUG_OVERLAY closure lost its exact one-file delta: "
            f"{sorted(str(p) for p in overlay_on ^ settings_on)}")
    must_fail(
        "debug overlay without runtime settings",
        lambda: gate.expected_sources(
            source_root, manifest,
            {**cache, "ISAAC_VITA_DEBUG_OVERLAY": "ON"},
        ),
        "ISAAC_VITA_DEBUG_OVERLAY requires ISAAC_VITA_RUNTIME_SETTINGS=ON",
    )
    must_fail(
        "debug overlay without KAGE",
        lambda: gate.expected_sources(
            source_root, manifest,
            {**cache, "ISAAC_VITA_DEBUG_OVERLAY": "ON",
             "ISAAC_VITA_KAGE": "OFF"},
        ),
        "ISAAC_VITA_DEBUG_OVERLAY requires ISAAC_VITA_KAGE=ON",
    )
    missing_overlay = dict(cache)
    del missing_overlay["ISAAC_VITA_DEBUG_OVERLAY"]
    must_fail(
        "missing debug-overlay feature selection",
        lambda: gate.expected_sources(source_root, manifest, missing_overlay),
        "CMake cache is missing ISAAC_VITA_DEBUG_OVERLAY",
    )
    coloroffset_off = gate.expected_sources(source_root, manifest, cache)
    coloroffset_source = (
        runtime_root / "gl_vita_coloroffset_source.c"
    ).resolve()
    if coloroffset_source in coloroffset_off:
        raise AssertionError(
            "COLOROFFSET_GPU source entered the allocator closure while OFF"
        )
    cache["ISAAC_VITA_COLOROFFSET_GPU_OPTIMIZATIONS"] = "ON"
    coloroffset_on = gate.expected_sources(source_root, manifest, cache)
    if coloroffset_on - coloroffset_off != {coloroffset_source} or \
            coloroffset_off - coloroffset_on:
        raise AssertionError(
            "COLOROFFSET_GPU allocator closure lost its exact one-file delta"
        )
    missing_coloroffset = dict(cache)
    del missing_coloroffset["ISAAC_VITA_COLOROFFSET_GPU_OPTIMIZATIONS"]
    must_fail(
        "missing ColorOffset GPU feature selection",
        lambda: gate.expected_sources(
            source_root, manifest, missing_coloroffset
        ),
        "CMake cache is missing ISAAC_VITA_COLOROFFSET_GPU_OPTIMIZATIONS",
    )
    must_fail(
        "malformed ColorOffset GPU feature selection",
        lambda: gate.expected_sources(
            source_root, manifest,
            {**cache, "ISAAC_VITA_COLOROFFSET_GPU_OPTIMIZATIONS": "AUTO"},
        ),
        "CMake cache ISAAC_VITA_COLOROFFSET_GPU_OPTIMIZATIONS is not ON/OFF",
    )
    cache["ISAAC_VITA_FULLSPEED_SCHEDULER"] = "ON"
    scheduler_coloroffset_on = gate.expected_sources(
        source_root, manifest, cache
    )
    if scheduler_coloroffset_on - coloroffset_off != {
            scheduler_source, coloroffset_source} or \
            coloroffset_off - scheduler_coloroffset_on:
        raise AssertionError(
            "scheduler+ColorOffset allocator closure lost its exact two-file delta"
        )
    cache["ISAAC_VITA_FULLSPEED_SCHEDULER"] = "OFF"
    cache["ISAAC_VITA_COLOROFFSET_GPU_OPTIMIZATIONS"] = "OFF"
    cache_off = gate.expected_sources(source_root, manifest, cache)
    cache["ISAAC_VITA_GUEST_LOOKUP_CACHE"] = "ON"
    cache_on = gate.expected_sources(source_root, manifest, cache)
    if cache_on != cache_off:
        raise AssertionError(
            "GUEST_LOOKUP_CACHE changed the allocator source closure"
        )
    cache["ISAAC_VITA_FULLSPEED_SCHEDULER"] = "ON"
    combined_on = gate.expected_sources(source_root, manifest, cache)
    if combined_on != scheduler_on:
        raise AssertionError(
            "GUEST_LOOKUP_CACHE changed the fullspeed allocator closure"
        )
    cache["ISAAC_VITA_FULLSPEED_SCHEDULER"] = "OFF"
    missing_cache = dict(cache)
    del missing_cache["ISAAC_VITA_GUEST_LOOKUP_CACHE"]
    must_fail(
        "missing guest-lookup-cache feature selection",
        lambda: gate.expected_sources(source_root, manifest, missing_cache),
        "CMake cache is missing ISAAC_VITA_GUEST_LOOKUP_CACHE",
    )
    must_fail(
        "malformed guest-lookup-cache feature selection",
        lambda: gate.expected_sources(
            source_root, manifest,
            {**cache, "ISAAC_VITA_GUEST_LOOKUP_CACHE": "AUTO"},
        ),
        "CMake cache ISAAC_VITA_GUEST_LOOKUP_CACHE is not ON/OFF",
    )
    cache["ISAAC_VITA_GUEST_LOOKUP_CACHE"] = "OFF"
    sync_import_off = gate.expected_sources(source_root, manifest, cache)
    cache["ISAAC_VITA_SYNC_IMPORT_FASTPATH"] = "ON"
    sync_import_on = gate.expected_sources(source_root, manifest, cache)
    if sync_import_on != sync_import_off:
        raise AssertionError(
            "SYNC_IMPORT_FASTPATH changed the allocator source closure"
        )
    missing_sync_import = dict(cache)
    del missing_sync_import["ISAAC_VITA_SYNC_IMPORT_FASTPATH"]
    must_fail(
        "missing sync-import-fastpath feature selection",
        lambda: gate.expected_sources(
            source_root, manifest, missing_sync_import
        ),
        "CMake cache is missing ISAAC_VITA_SYNC_IMPORT_FASTPATH",
    )
    must_fail(
        "malformed sync-import-fastpath feature selection",
        lambda: gate.expected_sources(
            source_root, manifest,
            {**cache, "ISAAC_VITA_SYNC_IMPORT_FASTPATH": "AUTO"},
        ),
        "CMake cache ISAAC_VITA_SYNC_IMPORT_FASTPATH is not ON/OFF",
    )
    cache["ISAAC_VITA_SYNC_IMPORT_FASTPATH"] = "OFF"
    guest_stack_off = gate.expected_sources(source_root, manifest, cache)
    guest_stack_on = gate.expected_sources(
        source_root, manifest,
        {**cache, "ISAAC_VITA_GUEST_STACK_MEMBLOCK": "ON"},
    )
    if guest_stack_on - guest_stack_off != {
            runtime_root / "guest_stack_memblock.c"} or \
            guest_stack_off - guest_stack_on:
        raise AssertionError(
            "GUEST_STACK_MEMBLOCK must add exactly guest_stack_memblock.c: "
            f"{sorted(str(p) for p in guest_stack_on ^ guest_stack_off)}"
        )
    missing_guest_stack = dict(cache)
    del missing_guest_stack["ISAAC_VITA_GUEST_STACK_MEMBLOCK"]
    must_fail(
        "missing guest-stack-memblock feature selection",
        lambda: gate.expected_sources(
            source_root, manifest, missing_guest_stack
        ),
        "CMake cache is missing ISAAC_VITA_GUEST_STACK_MEMBLOCK",
    )
    must_fail(
        "malformed guest-stack-memblock feature selection",
        lambda: gate.expected_sources(
            source_root, manifest,
            {**cache, "ISAAC_VITA_GUEST_STACK_MEMBLOCK": "AUTO"},
        ),
        "CMake cache ISAAC_VITA_GUEST_STACK_MEMBLOCK is not ON/OFF",
    )
    display_off = gate.expected_sources(source_root, manifest, cache)
    cache["ISAAC_VITA_DISPLAY_RASTER_720"] = "ON"
    display_on = gate.expected_sources(source_root, manifest, cache)
    if display_on != display_off:
        raise AssertionError(
            "DISPLAY_RASTER_720 changed the allocator source closure"
        )
    missing_display = dict(cache)
    del missing_display["ISAAC_VITA_DISPLAY_RASTER_720"]
    must_fail(
        "missing display-raster feature selection",
        lambda: gate.expected_sources(source_root, manifest, missing_display),
        "CMake cache is missing ISAAC_VITA_DISPLAY_RASTER_720",
    )
    must_fail(
        "malformed display-raster feature selection",
        lambda: gate.expected_sources(
            source_root, manifest,
            {**cache, "ISAAC_VITA_DISPLAY_RASTER_720": "AUTO"},
        ),
        "CMake cache ISAAC_VITA_DISPLAY_RASTER_720 is not ON/OFF",
    )
    cache["ISAAC_VITA_DISPLAY_RASTER_720"] = "OFF"
    typed_off = gate.expected_sources(source_root, manifest, cache)
    cache["ISAAC_VITA_GL_TYPED_STATE_CACHE"] = "ON"
    typed_on = gate.expected_sources(source_root, manifest, cache)
    if typed_on != typed_off:
        raise AssertionError(
            "GL_TYPED_STATE_CACHE changed the allocator source closure"
        )
    missing_typed = dict(cache)
    del missing_typed["ISAAC_VITA_GL_TYPED_STATE_CACHE"]
    must_fail(
        "missing typed-state-cache feature selection",
        lambda: gate.expected_sources(source_root, manifest, missing_typed),
        "CMake cache is missing ISAAC_VITA_GL_TYPED_STATE_CACHE",
    )
    must_fail(
        "malformed typed-state-cache feature selection",
        lambda: gate.expected_sources(
            source_root, manifest,
            {**cache, "ISAAC_VITA_GL_TYPED_STATE_CACHE": "AUTO"},
        ),
        "CMake cache ISAAC_VITA_GL_TYPED_STATE_CACHE is not ON/OFF",
    )
    cache["ISAAC_VITA_GL_TYPED_STATE_CACHE"] = "OFF"
    log_batch_off = gate.expected_sources(source_root, manifest, cache)
    cache["ISAAC_VITA_GAME_LOG_BATCH"] = "ON"
    log_batch_on = gate.expected_sources(source_root, manifest, cache)
    if log_batch_on != log_batch_off:
        raise AssertionError(
            "GAME_LOG_BATCH changed the allocator source closure"
        )
    missing_log_batch = dict(cache)
    del missing_log_batch["ISAAC_VITA_GAME_LOG_BATCH"]
    must_fail(
        "missing game-log-batch feature selection",
        lambda: gate.expected_sources(source_root, manifest, missing_log_batch),
        "CMake cache is missing ISAAC_VITA_GAME_LOG_BATCH",
    )
    must_fail(
        "malformed game-log-batch feature selection",
        lambda: gate.expected_sources(
            source_root, manifest,
            {**cache, "ISAAC_VITA_GAME_LOG_BATCH": "AUTO"},
        ),
        "CMake cache ISAAC_VITA_GAME_LOG_BATCH is not ON/OFF",
    )
    cache["ISAAC_VITA_GAME_LOG_BATCH"] = "OFF"
    raw_gxm_baseline = gate.expected_sources(source_root, manifest, cache)
    cache["ISAAC_VITA_RAW_GXM_PROBE"] = "ON"
    raw_gxm_probed = gate.expected_sources(source_root, manifest, cache)
    raw_gxm_source = (runtime_root / "kage_vita_raw_gxm_probe.c").resolve()
    if (raw_gxm_probed - raw_gxm_baseline != {raw_gxm_source} or
            raw_gxm_baseline - raw_gxm_probed):
        raise AssertionError(
            "RAW_GXM_PROBE allocator source closure lost its exact one-file delta"
        )
    missing_raw_gxm = dict(cache)
    del missing_raw_gxm["ISAAC_VITA_RAW_GXM_PROBE"]
    must_fail(
        "missing raw GXM feature selection",
        lambda: gate.expected_sources(
            source_root, manifest, missing_raw_gxm
        ),
        "CMake cache is missing ISAAC_VITA_RAW_GXM_PROBE",
    )
    must_fail(
        "malformed raw GXM feature selection",
        lambda: gate.expected_sources(
            source_root, manifest,
            {**cache, "ISAAC_VITA_RAW_GXM_PROBE": "AUTO"},
        ),
        "CMake cache ISAAC_VITA_RAW_GXM_PROBE is not ON/OFF",
    )
    cache["ISAAC_VITA_RAW_GXM_PROBE"] = "OFF"
    screenshot_baseline = gate.expected_sources(source_root, manifest, cache)
    cache["ISAAC_VITA_SCREENSHOT_PROBE"] = "ON"
    screenshot_probed = gate.expected_sources(source_root, manifest, cache)
    screenshot_source = (
        runtime_root / "kage_vita_screenshot_probe.c"
    ).resolve()
    if (screenshot_probed - screenshot_baseline != {screenshot_source} or
            screenshot_baseline - screenshot_probed):
        raise AssertionError(
            "SCREENSHOT_PROBE allocator source closure lost its exact one-file delta"
        )
    missing_screenshot = dict(cache)
    del missing_screenshot["ISAAC_VITA_SCREENSHOT_PROBE"]
    must_fail(
        "missing screenshot feature selection",
        lambda: gate.expected_sources(
            source_root, manifest, missing_screenshot
        ),
        "CMake cache is missing ISAAC_VITA_SCREENSHOT_PROBE",
    )
    must_fail(
        "malformed screenshot feature selection",
        lambda: gate.expected_sources(
            source_root, manifest,
            {**cache, "ISAAC_VITA_SCREENSHOT_PROBE": "AUTO"},
        ),
        "CMake cache ISAAC_VITA_SCREENSHOT_PROBE is not ON/OFF",
    )
    cache["ISAAC_VITA_SCREENSHOT_PROBE"] = "OFF"
    cache["ISAAC_VITA_STALL_PROBE"] = "ON"

    derived = (root / "detached override" / "guest_0144.c").resolve()
    overridden = gate.expected_sources(
        source_root, manifest, cache, {"guest_0144.c": derived}
    )
    canonical = (manifest.parent / "guest_0144.c").resolve()
    if (derived not in overridden or canonical in overridden or
            len(overridden) != len(probed)):
        raise AssertionError("generated source override did not preserve closure")
    must_fail(
        "manifest-unowned generated override",
        lambda: gate.expected_sources(
            source_root, manifest, cache, {"guest_9999.c": derived}
        ),
        "no manifest owner",
    )
    must_fail(
        "generated override physical alias",
        lambda: gate.expected_sources(
            source_root,
            manifest,
            cache,
            {"guest_0144.c": manifest.parent / "guest_stubs.c"},
        ),
        "aliases another physical source",
    )
    must_fail(
        "runtime-colliding generated override",
        lambda: gate.expected_sources(
            source_root,
            manifest,
            cache,
            {"guest_0144.c": source_root / "recomp" / "vita" / "main.c"},
        ),
        "collides with a runtime source",
    )


NATIVE_INFLATE_FIXTURE_CACHE = {
    "ISAAC_VITA_NATIVE_INFLATE": "ON",
}


def native_inflate_tests(root: Path, poison: Path) -> None:
    """ISAAC_VITA_NATIVE_INFLATE: the six units enter the expected set with the
    knob, each carries exactly its iz_ prefix header after the poison header,
    and the prefix headers may only rename (no allocator name, no feature-view
    macro, no foreign include)."""
    source_root = (root / "native-inflate-root").resolve()
    runtime_root = source_root / "recomp" / "runtime"
    vendor_root = runtime_root / "vendor" / "zlib114"
    vendor_root.mkdir(parents=True)
    (source_root / "recomp" / "vita").mkdir(parents=True)
    root_header = vendor_root / "izlib114_prefix.h"
    root_header.write_text(
        "/* renames */\n#ifndef IZLIB114_PREFIX_H\n#define IZLIB114_PREFIX_H\n"
        "#define adler32 iz_adler32\n#define inflate_fast iz_inflate_fast\n"
        "#define zcalloc iz_zcalloc\n#endif /* IZLIB114_PREFIX_H */\n",
        encoding="ascii",
    )
    (vendor_root / "izlib114_prefix_codes.h").write_text(
        "#ifndef IZLIB114_PREFIX_CODES_H\n#define IZLIB114_PREFIX_CODES_H\n"
        '#include "izlib114_prefix.h"\n#undef inflate_fast\n'
        "#define inflate_fast isaac_ni_hook_inflate_fast\n#endif\n",
        encoding="ascii",
    )
    (vendor_root / "izlib114_prefix_util.h").write_text(
        "#ifndef IZLIB114_PREFIX_UTIL_H\n#define IZLIB114_PREFIX_UTIL_H\n"
        '#include "izlib114_prefix.h"\n#define NO_MEMCPY 1\n'
        "#define zmemcpy isaac_ni_hook_zmemcpy\n#endif\n",
        encoding="ascii",
    )
    units = {
        runtime_root / "host_vita_native_inflate.c": root_header,
        runtime_root / "host_vita_native_inflate_hooks.c": root_header,
        runtime_root / "host_vita_native_inflate_zlib114_codes.c":
            vendor_root / "izlib114_prefix_codes.h",
        vendor_root / "inffast.c": root_header,
        vendor_root / "adler32.c": root_header,
        vendor_root / "infutil.c": vendor_root / "izlib114_prefix_util.h",
    }
    for unit in units:
        unit.write_text("int fixture;\n", encoding="ascii")

    manifest = (root / "native-inflate-generated" / "manifest.json").resolve()
    manifest.parent.mkdir(parents=True)
    manifest.write_text(
        json.dumps({"outputs": [{"path": "guest_0144.c"}, {"path": "guest_stubs.c"},
                                {"path": "guest_table.c"}]}),
        encoding="utf-8",
    )
    cache_off = dict(EXPECTED_SOURCE_FIXTURE_CACHE)
    cache_on = {**cache_off, **NATIVE_INFLATE_FIXTURE_CACHE}
    baseline = gate.expected_sources(source_root, manifest, cache_off)
    with_inflate = gate.expected_sources(source_root, manifest, cache_on)
    if with_inflate - baseline != {unit.resolve() for unit in units}:
        raise AssertionError(
            "ISAAC_VITA_NATIVE_INFLATE must select exactly its six units: "
            f"{sorted(str(p) for p in with_inflate - baseline)}")
    if gate.native_inflate_forced_includes(cache_off, source_root):
        raise AssertionError("prefix includes must be empty while the knob is OFF")
    mapping = gate.native_inflate_forced_includes(cache_on, source_root)
    if mapping != {unit.resolve(): header.resolve() for unit, header in units.items()}:
        raise AssertionError(f"prefix include mapping drifted: {mapping}")

    # One compile record per unit: poison first, then exactly its prefix header.
    build = root / "native-inflate-build"
    build.mkdir()
    entries = []
    for unit, header in units.items():
        output = f"CMakeFiles/isaac_first_arm_fault.dir/{unit.name}.obj"
        obj = build / output
        obj.parent.mkdir(parents=True, exist_ok=True)
        obj.write_bytes(b"fixture")
        entries.append({
            "directory": str(build),
            "command": (
                f"cc -DISAAC_VITA_RAW_ALLOCATOR_GATE=1 -include \"{poison.as_posix()}\" "
                f"-include \"{header.as_posix()}\" -o {output} -c \"{unit.as_posix()}\""
            ),
            "file": str(unit), "output": output,
        })
    commands = build / "compile_commands.json"
    commands.write_text(json.dumps(entries), encoding="utf-8")
    expected = {unit.resolve() for unit in units}
    records = gate.compile_closure(commands, expected, poison, forced_includes=mapping)
    if set(records) != expected:
        raise AssertionError("native inflate units with their prefix headers must pass")
    must_fail("prefix include without the knob",
              lambda: gate.compile_closure(commands, expected, poison),
              "forced-include closure")
    swapped = json.loads(commands.read_text(encoding="utf-8"))
    swapped[0]["command"] = swapped[0]["command"].replace(
        root_header.as_posix(), (vendor_root / "izlib114_prefix_codes.h").as_posix())
    commands.write_text(json.dumps(swapped), encoding="utf-8")
    must_fail("wrong prefix header on a unit",
              lambda: gate.compile_closure(commands, expected, poison,
                                           forced_includes=mapping),
              "forced-include closure")
    commands.write_text(json.dumps(entries), encoding="utf-8")

    original = root_header.read_text(encoding="ascii")
    for label, hostile, needle in (
        ("allocator rename target", "#define zcalloc malloc\n", "allocator name"),
        ("allocator rename name", "#define malloc iz_malloc\n", "allocator name"),
        ("feature-view macro", "#define _GNU_SOURCE 1\n", "feature-view"),
        ("foreign include", "#include <stdlib.h>\n", "includes"),
        ("code line", "static int leak;\n", "non-directive line"),
        ("gate marker", "#undef ISAAC_VITA_RAW_ALLOCATOR_GATE\n", "feature-view/gate"),
    ):
        root_header.write_text(original.replace("#endif", hostile + "#endif"),
                               encoding="ascii")
        must_fail(f"hostile prefix header: {label}",
                  lambda: gate.native_inflate_forced_includes(cache_on, source_root),
                  needle)
    root_header.write_text(original.replace("#ifndef IZLIB114_PREFIX_H\n", "")
                           .replace("#define IZLIB114_PREFIX_H\n", "")
                           .replace("#endif /* IZLIB114_PREFIX_H */\n", ""),
                           encoding="ascii")
    must_fail("prefix header without a guard",
              lambda: gate.native_inflate_forced_includes(cache_on, source_root),
              "include guard")
    root_header.write_text(original, encoding="ascii")


def png_decode_profile_scope_tests(root: Path) -> None:
    scope = (root / "png-profile-scope").resolve()
    scope.mkdir()
    owner_roots = {
        "guest_0001.c": "sub_005a0c50",
        "guest_0002.c": "sub_005a0cd0",
        "guest_0003.c": "sub_005b1500",
        "guest_0004.c": "sub_005c5fb0",
        "guest_0005.c": "sub_005c6fa0",
        "guest_0006.c": "sub_005d6d80",
    }
    records: dict[Path, dict[str, object]] = {}
    for name, function in owner_roots.items():
        source = (scope / name).resolve()
        source.write_text(
            f"void {function}(CPU *__restrict c){{(void)c;}}\n",
            encoding="utf-8",
        )
        records[source] = {
            "source": source, "definitions": {}, "undefinitions": [],
        }
    runtime = (scope / "kage_vita_png_decode_profile.c").resolve()
    runtime.write_text("int png_profile_runtime_fixture;\n", encoding="utf-8")
    records[runtime] = {
        "source": runtime, "definitions": {}, "undefinitions": [],
    }
    native_shim = (scope / "host_vita_png_unfilter_guest.c").resolve()
    native_shim.write_text("int png_native_shim_fixture;\n", encoding="utf-8")
    records[native_shim] = {
        "source": native_shim, "definitions": {}, "undefinitions": [],
    }
    unrelated = (scope / "guest_9999.c").resolve()
    unrelated.write_text("int unrelated_guest_fixture;\n", encoding="utf-8")
    records[unrelated] = {
        "source": unrelated, "definitions": {}, "undefinitions": [],
    }

    inactive = {
        "ISAAC_VITA_PNG_DECODE_PROFILE": "OFF",
        "ISAAC_VITA_PNG_NATIVE_UNFILTER": "OFF",
        "ISAAC_VITA_TEXTURE_CHURN_PROFILE": "OFF",
        "ISAAC_VITA_GUEST_LINK_ID": "fixture-build",
    }
    gate.verify_png_decode_profile_compile_scope(records, inactive)
    gate.verify_png_decode_profile_compile_scope(
        records, {"ISAAC_VITA_PNG_DECODE_PROFILE": "OFF"}
    )
    gate.verify_png_decode_profile_compile_scope(
        records, {
            "ISAAC_VITA_PNG_DECODE_PROFILE": "OFF",
            "ISAAC_VITA_GUEST_LINK_ID": "unrelated;custom",
        },
    )
    missing = dict(inactive)
    del missing["ISAAC_VITA_PNG_DECODE_PROFILE"]
    must_fail(
        "missing PNG profile compile selection",
        lambda: gate.verify_png_decode_profile_compile_scope(records, missing),
        "CMake cache is missing ISAAC_VITA_PNG_DECODE_PROFILE",
    )
    must_fail(
        "malformed PNG profile compile selection",
        lambda: gate.verify_png_decode_profile_compile_scope(
            records, {
                "ISAAC_VITA_PNG_DECODE_PROFILE": "AUTO",
                "ISAAC_VITA_GUEST_LINK_ID": "fixture-build",
            }
        ),
        "is not ON/OFF",
    )
    missing_build_id = {
        "ISAAC_VITA_PNG_DECODE_PROFILE": "ON",
    }
    must_fail(
        "missing PNG profile guest build ID",
        lambda: gate.verify_png_decode_profile_compile_scope(
            records, missing_build_id
        ),
        "CMake cache is missing ISAAC_VITA_GUEST_LINK_ID",
    )
    must_fail(
        "unsafe PNG profile guest build ID",
        lambda: gate.verify_png_decode_profile_compile_scope(
            records, {
                "ISAAC_VITA_PNG_DECODE_PROFILE": "ON",
                "ISAAC_VITA_GUEST_LINK_ID": "bad;build",
            }
        ),
        "is not a controlled build ID",
    )

    runtime_definitions = records[runtime]["definitions"]
    assert isinstance(runtime_definitions, dict)
    runtime_definitions["ISAAC_VITA_PNG_DECODE_PROFILE"] = ["1"]
    must_fail(
        "PNG profile macro present while OFF",
        lambda: gate.verify_png_decode_profile_compile_scope(records, inactive),
        "compile-definition scope changed",
    )
    runtime_definitions.clear()

    active = {
        "ISAAC_VITA_PNG_DECODE_PROFILE": "ON",
        "ISAAC_VITA_PNG_NATIVE_UNFILTER": "OFF",
        "ISAAC_VITA_GUEST_LINK_ID": "fixture-build",
    }
    for name in owner_roots:
        definitions = records[(scope / name).resolve()]["definitions"]
        assert isinstance(definitions, dict)
        definitions["ISAAC_VITA_PNG_DECODE_PROFILE"] = ["1"]
    runtime_definitions["ISAAC_VITA_PNG_DECODE_PROFILE"] = ["1"]
    runtime_definitions[gate.PNG_DECODE_PROFILE_BUILD_ID_MACRO] = [
        '"fixture-build"'
    ]
    gate.verify_png_decode_profile_compile_scope(records, active)

    combined = {
        **active,
        "ISAAC_VITA_PNG_NATIVE_UNFILTER": "ON",
    }
    native_definitions = records[native_shim]["definitions"]
    assert isinstance(native_definitions, dict)
    must_fail(
        "PNG native shim missing profile telemetry scope",
        lambda: gate.verify_png_decode_profile_compile_scope(records, combined),
        "compile-definition scope changed",
    )
    native_definitions["ISAAC_VITA_PNG_DECODE_PROFILE"] = ["1"]
    gate.verify_png_decode_profile_compile_scope(records, combined)
    native_definitions.clear()

    first_owner = records[(scope / "guest_0001.c").resolve()]
    first_definitions = first_owner["definitions"]
    assert isinstance(first_definitions, dict)
    first_definitions["ISAAC_VITA_PNG_DECODE_PROFILE"] = ["0"]
    must_fail(
        "non-canonical PNG profile owner definition",
        lambda: gate.verify_png_decode_profile_compile_scope(records, active),
        "non-canonical PNG profile definition",
    )
    first_definitions["ISAAC_VITA_PNG_DECODE_PROFILE"] = ["1"]
    first_owner["undefinitions"] = ["ISAAC_VITA_PNG_DECODE_PROFILE"]
    must_fail(
        "undefined PNG profile owner policy",
        lambda: gate.verify_png_decode_profile_compile_scope(records, active),
        "compile policy is undefined",
    )
    first_owner["undefinitions"] = []

    runtime_definitions[gate.PNG_DECODE_PROFILE_BUILD_ID_MACRO] = [
        "fixture-build"
    ]
    must_fail(
        "unquoted PNG profile build ID",
        lambda: gate.verify_png_decode_profile_compile_scope(records, active),
        "build ID disagrees with ISAAC_VITA_GUEST_LINK_ID",
    )
    runtime_definitions[gate.PNG_DECODE_PROFILE_BUILD_ID_MACRO] = [
        '"other-build"'
    ]
    must_fail(
        "wrong but quoted PNG profile build ID",
        lambda: gate.verify_png_decode_profile_compile_scope(records, active),
        "build ID disagrees with ISAAC_VITA_GUEST_LINK_ID",
    )
    runtime_definitions[gate.PNG_DECODE_PROFILE_BUILD_ID_MACRO] = [
        '"fixture-build"'
    ]
    first_definitions[gate.PNG_DECODE_PROFILE_BUILD_ID_MACRO] = [
        '"fixture-build"'
    ]
    must_fail(
        "PNG profile build ID leaked into generated owner",
        lambda: gate.verify_png_decode_profile_compile_scope(records, active),
        "build-ID compile-definition scope changed",
    )
    first_definitions.pop(gate.PNG_DECODE_PROFILE_BUILD_ID_MACRO)
    runtime_definitions.pop(gate.PNG_DECODE_PROFILE_BUILD_ID_MACRO)
    must_fail(
        "missing PNG profile runtime build ID",
        lambda: gate.verify_png_decode_profile_compile_scope(records, active),
        "build-ID compile-definition scope changed",
    )
    runtime_definitions[gate.PNG_DECODE_PROFILE_BUILD_ID_MACRO] = [
        '"fixture-build"'
    ]

    first_source = first_owner["source"]
    assert isinstance(first_source, Path)
    original = first_source.read_text(encoding="utf-8")
    first_source.write_text(
        original + "void sub_005a0c50(CPU *__restrict c){(void)c;}\n",
        encoding="utf-8",
    )
    must_fail(
        "duplicate PNG generated owner definition",
        lambda: gate.verify_png_decode_profile_compile_scope(records, active),
        "generated definition census changed",
    )
    first_source.write_text(original, encoding="utf-8")


def png_native_unfilter_scope_tests(root: Path) -> None:
    scope = (root / "png-native-scope").resolve()
    scope.mkdir()
    sources = {
        name: (scope / name).resolve()
        for name in (
            "guest_0042.c",
            "host_vita_heap.c",
            "host_vita_png_unfilter_native.c",
            "host_vita_png_unfilter_guest.c",
            "guest_9999.c",
        )
    }
    sources["guest_0042.c"].write_text(
        "void sub_005c6bf0(CPU *__restrict c){(void)c;}\n",
        encoding="utf-8",
    )
    for name in sources:
        if name != "guest_0042.c":
            sources[name].write_text(f"int fixture_{len(name)};\n", encoding="utf-8")
    records = {
        path: {"source": path, "definitions": {}, "undefinitions": []}
        for path in sources.values()
    }
    inactive = {"ISAAC_VITA_PNG_NATIVE_UNFILTER": "OFF"}
    gate.verify_png_native_unfilter_compile_scope(records, inactive)
    must_fail(
        "missing PNG native compile selection",
        lambda: gate.verify_png_native_unfilter_compile_scope(records, {}),
        "CMake cache is missing ISAAC_VITA_PNG_NATIVE_UNFILTER",
    )
    must_fail(
        "malformed PNG native compile selection",
        lambda: gate.verify_png_native_unfilter_compile_scope(
            records, {"ISAAC_VITA_PNG_NATIVE_UNFILTER": "AUTO"}
        ),
        "is not ON/OFF",
    )

    active = {"ISAAC_VITA_PNG_NATIVE_UNFILTER": "ON"}
    expected_names = (
        "guest_0042.c",
        "host_vita_heap.c",
        "host_vita_png_unfilter_native.c",
        "host_vita_png_unfilter_guest.c",
    )
    for name in expected_names:
        definitions = records[sources[name]]["definitions"]
        assert isinstance(definitions, dict)
        definitions["ISAAC_VITA_PNG_NATIVE_UNFILTER"] = ["1"]
    shim_definitions = records[
        sources["host_vita_png_unfilter_guest.c"]
    ]["definitions"]
    assert isinstance(shim_definitions, dict)
    shim_definitions["ISAAC_VITA_HEAP_RANGE_LEASE"] = ["1"]
    gate.verify_png_native_unfilter_compile_scope(records, active)

    core_definitions = records[
        sources["host_vita_png_unfilter_native.c"]
    ]["definitions"]
    assert isinstance(core_definitions, dict)
    core_definitions["ISAAC_VITA_PNG_NATIVE_UNFILTER"] = ["0"]
    must_fail(
        "non-canonical PNG native definition",
        lambda: gate.verify_png_native_unfilter_compile_scope(records, active),
        "non-canonical PNG native-unfilter definition",
    )
    core_definitions["ISAAC_VITA_PNG_NATIVE_UNFILTER"] = ["1"]

    unrelated_definitions = records[sources["guest_9999.c"]]["definitions"]
    assert isinstance(unrelated_definitions, dict)
    unrelated_definitions["ISAAC_VITA_PNG_NATIVE_UNFILTER"] = ["1"]
    must_fail(
        "PNG native definition leaked into unrelated owner",
        lambda: gate.verify_png_native_unfilter_compile_scope(records, active),
        "compile-definition scope changed",
    )
    unrelated_definitions.clear()

    owner_record = records[sources["guest_0042.c"]]
    owner_record["undefinitions"] = ["ISAAC_VITA_PNG_NATIVE_UNFILTER"]
    must_fail(
        "undefined PNG native owner policy",
        lambda: gate.verify_png_native_unfilter_compile_scope(records, active),
        "policy is undefined",
    )
    owner_record["undefinitions"] = []

    shim_definitions.pop("ISAAC_VITA_HEAP_RANGE_LEASE")
    must_fail(
        "PNG native shim missing range lease",
        lambda: gate.verify_png_native_unfilter_compile_scope(records, active),
        "lost its heap range lease",
    )
    shim_definitions["ISAAC_VITA_HEAP_RANGE_LEASE"] = ["1"]

    owner_source = sources["guest_0042.c"]
    original = owner_source.read_text(encoding="utf-8")
    owner_source.write_text("int missing_owner;\n", encoding="utf-8")
    must_fail(
        "missing PNG native generated owner",
        lambda: gate.verify_png_native_unfilter_compile_scope(records, active),
        "generated definition census changed",
    )
    owner_source.write_text(original + original, encoding="utf-8")
    must_fail(
        "duplicate PNG native generated owner",
        lambda: gate.verify_png_native_unfilter_compile_scope(records, active),
        "generated definition census changed",
    )
    owner_source.write_text(original, encoding="utf-8")


def png_crc32_fastpath_scope_tests(root: Path) -> None:
    scope = (root / "png-crc32-scope").resolve()
    scope.mkdir()
    owner = (scope / "guest_0042.c").resolve()
    native = (scope / "host_vita_png_crc32_native.c").resolve()
    shim = (scope / "host_vita_png_crc32_guest.c").resolve()
    unrelated = (scope / "guest_9999.c").resolve()
    owner_text = (
        "void sub_005c3320(CPU *__restrict c){(void)c;}\n"
        "if (isaac_vita_png_crc32_guest_try(c)) {}\n"
    )
    owner.write_text(owner_text, encoding="utf-8")
    native.write_text("int png_crc32_native_fixture;\n", encoding="utf-8")
    shim.write_text("int png_crc32_shim_fixture;\n", encoding="utf-8")
    unrelated.write_text("int unrelated_fixture;\n", encoding="utf-8")
    records = {
        path: {"source": path, "definitions": {}, "undefinitions": []}
        for path in (owner, native, shim, unrelated)
    }

    inactive = {"ISAAC_VITA_PNG_CRC32_FASTPATH": "OFF"}
    gate.verify_png_crc32_fastpath_compile_scope(records, inactive)
    must_fail(
        "missing PNG CRC32 compile selection",
        lambda: gate.verify_png_crc32_fastpath_compile_scope(records, {}),
        "CMake cache is missing ISAAC_VITA_PNG_CRC32_FASTPATH",
    )
    must_fail(
        "malformed PNG CRC32 compile selection",
        lambda: gate.verify_png_crc32_fastpath_compile_scope(
            records, {"ISAAC_VITA_PNG_CRC32_FASTPATH": "AUTO"}
        ),
        "is not ON/OFF",
    )

    active = {"ISAAC_VITA_PNG_CRC32_FASTPATH": "ON"}
    owner_definitions = records[owner]["definitions"]
    shim_definitions = records[shim]["definitions"]
    native_definitions = records[native]["definitions"]
    unrelated_definitions = records[unrelated]["definitions"]
    assert isinstance(owner_definitions, dict)
    assert isinstance(shim_definitions, dict)
    assert isinstance(native_definitions, dict)
    assert isinstance(unrelated_definitions, dict)
    owner_definitions["ISAAC_VITA_PNG_CRC32_FASTPATH"] = ["1"]
    shim_definitions["ISAAC_VITA_PNG_CRC32_FASTPATH"] = ["1"]
    gate.verify_png_crc32_fastpath_compile_scope(records, active)

    shim_definitions["ISAAC_VITA_PNG_CRC32_FASTPATH"] = ["0"]
    must_fail(
        "non-canonical PNG CRC32 shim definition",
        lambda: gate.verify_png_crc32_fastpath_compile_scope(records, active),
        "non-canonical PNG CRC32 definition",
    )
    shim_definitions["ISAAC_VITA_PNG_CRC32_FASTPATH"] = ["1"]

    native_definitions["ISAAC_VITA_PNG_CRC32_FASTPATH"] = ["1"]
    must_fail(
        "PNG CRC32 definition leaked into native leaf",
        lambda: gate.verify_png_crc32_fastpath_compile_scope(records, active),
        "compile-definition scope changed",
    )
    native_definitions.clear()

    unrelated_definitions["ISAAC_VITA_PNG_CRC32_FASTPATH"] = ["1"]
    must_fail(
        "PNG CRC32 definition leaked into unrelated owner",
        lambda: gate.verify_png_crc32_fastpath_compile_scope(records, active),
        "compile-definition scope changed",
    )
    unrelated_definitions.clear()

    records[owner]["undefinitions"] = ["ISAAC_VITA_PNG_CRC32_FASTPATH"]
    must_fail(
        "undefined PNG CRC32 owner policy",
        lambda: gate.verify_png_crc32_fastpath_compile_scope(records, active),
        "policy is undefined",
    )
    records[owner]["undefinitions"] = []

    owner.write_text("int missing_crc32_owner;\n", encoding="utf-8")
    must_fail(
        "missing PNG CRC32 generated owner",
        lambda: gate.verify_png_crc32_fastpath_compile_scope(records, active),
        "generated owner/seam census changed",
    )
    owner.write_text(owner_text + owner_text, encoding="utf-8")
    must_fail(
        "duplicate PNG CRC32 generated owner",
        lambda: gate.verify_png_crc32_fastpath_compile_scope(records, active),
        "generated owner/seam census changed",
    )
    owner.write_text(owner_text, encoding="utf-8")


def save_reader_fastpath_scope_tests(root: Path) -> None:
    scope = (root / "save-reader-scope").resolve()
    scope.mkdir()
    owner = (scope / "guest_0042.c").resolve()
    helper = (scope / "host_vita_save_reader_guest.c").resolve()
    unrelated = (scope / "guest_9999.c").resolve()
    owner_text = (
        "void sub_0025ba60(CPU *__restrict c){(void)c;}\n"
        "/* Frozen memory Save Reader native fast path */\n"
    )
    owner.write_text(owner_text, encoding="utf-8")
    helper.write_text("int save_reader_fixture;\n", encoding="utf-8")
    unrelated.write_text("int unrelated_fixture;\n", encoding="utf-8")
    records = {
        path: {"source": path, "definitions": {}, "undefinitions": []}
        for path in (owner, helper, unrelated)
    }
    inactive = {"ISAAC_VITA_SAVE_READER_FASTPATH": "OFF"}
    gate.verify_save_reader_fastpath_compile_scope(records, inactive)

    active = {"ISAAC_VITA_SAVE_READER_FASTPATH": "ON"}
    for path in (owner, helper):
        definitions = records[path]["definitions"]
        assert isinstance(definitions, dict)
        definitions["ISAAC_VITA_SAVE_READER_FASTPATH"] = ["1"]
    gate.verify_save_reader_fastpath_compile_scope(records, active)

    helper_definitions = records[helper]["definitions"]
    assert isinstance(helper_definitions, dict)
    helper_definitions["ISAAC_VITA_SAVE_READER_FASTPATH"] = ["0"]
    must_fail(
        "non-canonical save-reader definition",
        lambda: gate.verify_save_reader_fastpath_compile_scope(records, active),
        "non-canonical save-reader definition",
    )
    helper_definitions["ISAAC_VITA_SAVE_READER_FASTPATH"] = ["1"]

    unrelated_definitions = records[unrelated]["definitions"]
    assert isinstance(unrelated_definitions, dict)
    unrelated_definitions["ISAAC_VITA_SAVE_READER_FASTPATH"] = ["1"]
    must_fail(
        "save-reader definition leaked into unrelated owner",
        lambda: gate.verify_save_reader_fastpath_compile_scope(records, active),
        "compile-definition scope changed",
    )
    unrelated_definitions.clear()

    owner.write_text("int missing_save_reader_owner;\n", encoding="utf-8")
    must_fail(
        "missing save-reader generated owner",
        lambda: gate.verify_save_reader_fastpath_compile_scope(records, active),
        "generated owner/seam census changed",
    )
    owner.write_text(owner_text, encoding="utf-8")


def sim_cadence_receipt_scope_tests(root: Path) -> None:
    scope = (root / "sim-cadence-scope").resolve()
    scope.mkdir()
    owner = (scope / "guest_0001.c").resolve()
    owner_text = (
        "void sub_002cdcf0(CPU *__restrict c)\n"
        "{\n"
        "    extern void kage_vita_sim_cadence_note_game_update(\n"
        "        uint32_t, uint32_t);\n"
        "    kage_vita_sim_cadence_note_game_update(c->ecx, 0);\n"
        "}\n"
    )
    owner.write_text(owner_text, encoding="utf-8")
    backend = (scope / "kage_vita_backend.c").resolve()
    backend.write_text("int cadence_backend_fixture;\n", encoding="utf-8")
    runtime = (scope / "kage_vita_sim_cadence_receipt.c").resolve()
    runtime.write_text("int cadence_runtime_fixture;\n", encoding="utf-8")
    unrelated = (scope / "guest_9999.c").resolve()
    unrelated.write_text("int cadence_unrelated_fixture;\n", encoding="utf-8")
    records: dict[Path, dict[str, object]] = {
        source: {
            "source": source,
            "definitions": {},
            "undefinitions": [],
        }
        for source in (owner, backend, runtime, unrelated)
    }
    inactive = {
        "ISAAC_VITA_SIM_CADENCE_RECEIPT": "OFF",
        "ISAAC_VITA_GUEST_LINK_ID": "fixture-build",
    }
    gate.verify_sim_cadence_receipt_compile_scope(records, inactive)
    missing = dict(inactive)
    del missing["ISAAC_VITA_SIM_CADENCE_RECEIPT"]
    must_fail(
        "missing simulation-cadence compile selection",
        lambda: gate.verify_sim_cadence_receipt_compile_scope(records, missing),
        "CMake cache is missing ISAAC_VITA_SIM_CADENCE_RECEIPT",
    )
    must_fail(
        "malformed simulation-cadence compile selection",
        lambda: gate.verify_sim_cadence_receipt_compile_scope(
            records,
            {**inactive, "ISAAC_VITA_SIM_CADENCE_RECEIPT": "AUTO"},
        ),
        "is not ON/OFF",
    )
    active = {
        **inactive,
        "ISAAC_VITA_SIM_CADENCE_RECEIPT": "ON",
    }
    must_fail(
        "simulation-cadence active without guest link ID",
        lambda: gate.verify_sim_cadence_receipt_compile_scope(
            records,
            {"ISAAC_VITA_SIM_CADENCE_RECEIPT": "ON"},
        ),
        "CMake cache is missing ISAAC_VITA_GUEST_LINK_ID",
    )
    must_fail(
        "simulation-cadence active with hostile guest link ID",
        lambda: gate.verify_sim_cadence_receipt_compile_scope(
            records,
            {
                "ISAAC_VITA_SIM_CADENCE_RECEIPT": "ON",
                "ISAAC_VITA_GUEST_LINK_ID": "hostile;id",
            },
        ),
        "is not a controlled build ID",
    )
    must_fail(
        "missing simulation-cadence owner definitions",
        lambda: gate.verify_sim_cadence_receipt_compile_scope(records, active),
        "compile-definition scope changed",
    )
    for source in (owner, backend, runtime):
        definitions = records[source]["definitions"]
        assert isinstance(definitions, dict)
        definitions["ISAAC_VITA_SIM_CADENCE_RECEIPT"] = ["1"]
    runtime_definitions = records[runtime]["definitions"]
    assert isinstance(runtime_definitions, dict)
    runtime_definitions["ISAAC_VITA_SIM_CADENCE_RECEIPT_BUILD_ID"] = [
        '"fixture-build"'
    ]
    gate.verify_sim_cadence_receipt_compile_scope(records, active)

    owner_definitions = records[owner]["definitions"]
    assert isinstance(owner_definitions, dict)
    owner_definitions["ISAAC_VITA_SIM_CADENCE_RECEIPT"] = ["0"]
    must_fail(
        "non-canonical simulation-cadence definition",
        lambda: gate.verify_sim_cadence_receipt_compile_scope(records, active),
        "non-canonical simulation-cadence definition",
    )
    owner_definitions["ISAAC_VITA_SIM_CADENCE_RECEIPT"] = ["1"]
    records[backend]["undefinitions"] = ["ISAAC_VITA_SIM_CADENCE_RECEIPT"]
    must_fail(
        "undefined simulation-cadence policy",
        lambda: gate.verify_sim_cadence_receipt_compile_scope(records, active),
        "simulation-cadence compile policy is undefined",
    )
    records[backend]["undefinitions"] = []
    unrelated_definitions = records[unrelated]["definitions"]
    assert isinstance(unrelated_definitions, dict)
    unrelated_definitions["ISAAC_VITA_SIM_CADENCE_RECEIPT"] = ["1"]
    must_fail(
        "simulation-cadence definition leaked into unrelated owner",
        lambda: gate.verify_sim_cadence_receipt_compile_scope(records, active),
        "compile-definition scope changed",
    )
    unrelated_definitions.clear()
    runtime_definitions["ISAAC_VITA_SIM_CADENCE_RECEIPT_BUILD_ID"] = [
        '"wrong-build"'
    ]
    must_fail(
        "simulation-cadence build ID mismatch",
        lambda: gate.verify_sim_cadence_receipt_compile_scope(records, active),
        "build ID disagrees",
    )
    runtime_definitions["ISAAC_VITA_SIM_CADENCE_RECEIPT_BUILD_ID"] = [
        '"fixture-build"'
    ]
    owner.write_text(
        owner_text.replace(
            "void sub_002cdcf0(CPU *__restrict c)",
            "void sub_002cdcf1(CPU *__restrict c)",
        ),
        encoding="utf-8",
    )
    must_fail(
        "missing simulation-cadence generated owner",
        lambda: gate.verify_sim_cadence_receipt_compile_scope(records, active),
        "generated owner/seam census changed",
    )
    owner.write_text(owner_text, encoding="utf-8")
    runtime_definitions.pop("ISAAC_VITA_SIM_CADENCE_RECEIPT_BUILD_ID")
    for source in (owner, backend, runtime):
        definitions = records[source]["definitions"]
        assert isinstance(definitions, dict)
        definitions.pop("ISAAC_VITA_SIM_CADENCE_RECEIPT")
    gate.verify_sim_cadence_receipt_compile_scope(records, inactive)


def _define(records: dict[Path, dict[str, object]], source: Path,
            macro: str, values: list[str]) -> None:
    definitions = records[source]["definitions"]
    assert isinstance(definitions, dict)
    definitions[macro] = values


def _undefine(records: dict[Path, dict[str, object]], source: Path,
              macro: str) -> None:
    definitions = records[source]["definitions"]
    assert isinstance(definitions, dict)
    definitions.pop(macro, None)


def runtime_settings_scope_tests(root: Path) -> None:
    scope = (root / "runtime-settings-scope").resolve()
    scope.mkdir()
    entry = (scope / "entry_vita.c").resolve()
    scheduler = (scope / "kage_vita_fullspeed_scheduler.c").resolve()
    parser = (scope / "host_vita_settings_parse.c").resolve()
    loader = (scope / "host_vita_runtime_settings.c").resolve()
    unrelated = (scope / "guest_9999.c").resolve()
    backend = (scope / "kage_vita_backend.c").resolve()
    gl_backend = (scope / "gl_vita_backend.c").resolve()
    for source in (entry, scheduler, parser, loader, unrelated, backend,
                   gl_backend):
        source.write_text("int runtime_settings_fixture;\n", encoding="utf-8")

    def fresh_records(
            with_scheduler: bool) -> dict[Path, dict[str, object]]:
        sources = [entry, parser, loader, unrelated, backend, gl_backend]
        if with_scheduler:
            sources.append(scheduler)
        return {
            source: {
                "source": source,
                "definitions": {},
                "undefinitions": [],
            }
            for source in sources
        }

    inactive = {
        "ISAAC_VITA_RUNTIME_SETTINGS": "OFF",
        "ISAAC_VITA_FULLSPEED_SCHEDULER": "OFF",
    }
    gate.verify_runtime_settings_compile_scope(fresh_records(False), inactive)
    gate.verify_runtime_settings_compile_scope(
        fresh_records(True), {**inactive, "ISAAC_VITA_FULLSPEED_SCHEDULER": "ON"}
    )
    must_fail(
        "malformed runtime-settings compile policy",
        lambda: gate.verify_runtime_settings_compile_scope(
            fresh_records(False),
            {**inactive, "ISAAC_VITA_RUNTIME_SETTINGS": "AUTO"},
        ),
        "is not ON/OFF",
    )
    must_fail(
        "runtime-settings scope without the scheduler selection",
        lambda: gate.verify_runtime_settings_compile_scope(
            fresh_records(False), {"ISAAC_VITA_RUNTIME_SETTINGS": "OFF"}
        ),
        "CMake cache is missing ISAAC_VITA_FULLSPEED_SCHEDULER",
    )

    # Active without a scheduler: three owners, the loader without the
    # nested scheduler definition.
    active = {
        "ISAAC_VITA_RUNTIME_SETTINGS": "ON",
        "ISAAC_VITA_FULLSPEED_SCHEDULER": "OFF",
    }
    records = fresh_records(False)
    must_fail(
        "missing runtime-settings owner definitions",
        lambda: gate.verify_runtime_settings_compile_scope(records, active),
        "compile-definition scope changed",
    )
    for source in (entry, parser, loader):
        _define(records, source, "ISAAC_VITA_RUNTIME_SETTINGS", ["1"])
    # The loader must carry the build raster width: 960 while the
    # scaled-raster block defines none on the graphics owners, else exactly
    # the owners' value (by design, the receipt's raster= is the build
    # raster; the runtime-settings CMake block forwards the width alone).
    must_fail(
        "loader without the display-raster width",
        lambda: gate.verify_runtime_settings_compile_scope(records, active),
        "display-raster width definition scope changed",
    )
    _define(records, loader, "ISAAC_VITA_DISPLAY_RASTER_WIDTH", ["720"])
    must_fail(
        "loader width without a scaled raster",
        lambda: gate.verify_runtime_settings_compile_scope(records, active),
        "display-raster width definition scope changed",
    )
    _define(records, loader, "ISAAC_VITA_DISPLAY_RASTER_WIDTH", ["960"])
    gate.verify_runtime_settings_compile_scope(records, active)
    for owner in (backend, gl_backend):
        _define(records, owner, "ISAAC_VITA_DISPLAY_RASTER_WIDTH", ["720"])
    must_fail(
        "loader width differs from the graphics owners",
        lambda: gate.verify_runtime_settings_compile_scope(records, active),
        "display-raster width definition scope changed",
    )
    _define(records, loader, "ISAAC_VITA_DISPLAY_RASTER_WIDTH", ["720"])
    gate.verify_runtime_settings_compile_scope(records, active)
    _define(records, gl_backend, "ISAAC_VITA_DISPLAY_RASTER_WIDTH", ["480"])
    must_fail(
        "graphics owners disagree on the width",
        lambda: gate.verify_runtime_settings_compile_scope(records, active),
        "differs between the graphics owners",
    )
    for owner in (backend, gl_backend):
        _undefine(records, owner, "ISAAC_VITA_DISPLAY_RASTER_WIDTH")
    _define(records, loader, "ISAAC_VITA_DISPLAY_RASTER_WIDTH", ["960"])
    gate.verify_runtime_settings_compile_scope(records, active)
    must_fail(
        "runtime-settings definitions while OFF",
        lambda: gate.verify_runtime_settings_compile_scope(records, inactive),
        "compile-definition scope changed",
    )
    off_records = fresh_records(False)
    _define(off_records, loader, "ISAAC_VITA_DISPLAY_RASTER_WIDTH", ["960"])
    must_fail(
        "loader width while OFF",
        lambda: gate.verify_runtime_settings_compile_scope(
            off_records, inactive
        ),
        "carries a display-raster width while",
    )
    _define(records, loader, "ISAAC_VITA_FULLSPEED_SCHEDULER", ["1"])
    must_fail(
        "loader learns a scheduler that is not compiled",
        lambda: gate.verify_runtime_settings_compile_scope(records, active),
        "fullspeed-scheduler definition scope changed",
    )
    _undefine(records, loader, "ISAAC_VITA_FULLSPEED_SCHEDULER")
    _define(records, unrelated, "ISAAC_VITA_RUNTIME_SETTINGS", ["1"])
    must_fail(
        "unrelated runtime-settings owner",
        lambda: gate.verify_runtime_settings_compile_scope(records, active),
        "compile-definition scope changed",
    )
    _undefine(records, unrelated, "ISAAC_VITA_RUNTIME_SETTINGS")
    _define(records, entry, "ISAAC_VITA_RUNTIME_SETTINGS", ["0"])
    must_fail(
        "non-canonical runtime-settings definition",
        lambda: gate.verify_runtime_settings_compile_scope(records, active),
        "non-canonical runtime-settings definition",
    )
    _define(records, entry, "ISAAC_VITA_RUNTIME_SETTINGS", ["1"])
    records[entry]["undefinitions"] = ["ISAAC_VITA_RUNTIME_SETTINGS"]
    must_fail(
        "runtime-settings undefinition",
        lambda: gate.verify_runtime_settings_compile_scope(records, active),
        "is undefined by",
    )
    records[entry]["undefinitions"] = []
    gate.verify_runtime_settings_compile_scope(records, active)

    # Active with the scheduler: four owners and the nested definition on
    # the loader; the scheduler owner may not drop out.
    active_scheduler = {
        "ISAAC_VITA_RUNTIME_SETTINGS": "ON",
        "ISAAC_VITA_FULLSPEED_SCHEDULER": "ON",
    }
    records = fresh_records(True)
    for source in (entry, scheduler, parser, loader):
        _define(records, source, "ISAAC_VITA_RUNTIME_SETTINGS", ["1"])
    _define(records, loader, "ISAAC_VITA_DISPLAY_RASTER_WIDTH", ["960"])
    must_fail(
        "loader without the nested scheduler definition",
        lambda: gate.verify_runtime_settings_compile_scope(
            records, active_scheduler
        ),
        "fullspeed-scheduler definition scope changed",
    )
    _define(records, loader, "ISAAC_VITA_FULLSPEED_SCHEDULER", ["1"])
    gate.verify_runtime_settings_compile_scope(records, active_scheduler)
    _undefine(records, scheduler, "ISAAC_VITA_RUNTIME_SETTINGS")
    must_fail(
        "scheduler owner lost its runtime-settings definition",
        lambda: gate.verify_runtime_settings_compile_scope(
            records, active_scheduler
        ),
        "compile-definition scope changed",
    )
    records = fresh_records(True)
    del records[loader]
    for source in (entry, scheduler, parser):
        _define(records, source, "ISAAC_VITA_RUNTIME_SETTINGS", ["1"])
    must_fail(
        "loader missing from the closure",
        lambda: gate.verify_runtime_settings_compile_scope(
            records, active_scheduler
        ),
        "compile-definition scope changed",
    )


def manager_settings_ui_scope_tests(root: Path) -> None:
    """The manager page knob never reaches an eboot compile record."""
    scope = (root / "manager-settings-ui-scope").resolve()
    scope.mkdir()
    entry = (scope / "entry_vita.c").resolve()
    storage = (scope / "manager_storage.c").resolve()
    unrelated = (scope / "guest_0001.c").resolve()
    for source in (entry, storage, unrelated):
        source.write_text("int x;\n", encoding="utf-8")

    def fresh_records() -> dict[Path, dict[str, object]]:
        return {
            source: {
                "source": source,
                "definitions": {},
                "undefinitions": [],
            }
            for source in (entry, storage, unrelated)
        }

    off = {"ISAAC_VITA_MANAGER_SETTINGS_UI": "OFF"}
    on = {"ISAAC_VITA_MANAGER_SETTINGS_UI": "ON"}
    gate.verify_manager_settings_ui_compile_scope(fresh_records(), off)
    gate.verify_manager_settings_ui_compile_scope(fresh_records(), on)
    must_fail(
        "manager-settings scope without the feature selection",
        lambda: gate.verify_manager_settings_ui_compile_scope(
            fresh_records(), {}
        ),
        "CMake cache is missing ISAAC_VITA_MANAGER_SETTINGS_UI",
    )
    must_fail(
        "malformed manager-settings compile policy",
        lambda: gate.verify_manager_settings_ui_compile_scope(
            fresh_records(), {"ISAAC_VITA_MANAGER_SETTINGS_UI": "AUTO"}
        ),
        "is not ON/OFF",
    )
    for selection in (off, on):
        # A manager-side unit that leaked into the eboot closure with the
        # per-target definition, and the same with an unrelated unit.
        for source in (storage, unrelated, entry):
            records = fresh_records()
            _define(records, source, "MANAGER_SETTINGS_UI", ["1"])
            must_fail(
                f"manager-settings definition on {source.name}",
                lambda: gate.verify_manager_settings_ui_compile_scope(
                    records, selection
                ),
                "reached an eboot unit",
            )
            records = fresh_records()
            records[source]["undefinitions"] = ["MANAGER_SETTINGS_UI"]
            must_fail(
                f"manager-settings undefinition on {source.name}",
                lambda: gate.verify_manager_settings_ui_compile_scope(
                    records, selection
                ),
                "reached an eboot unit",
            )
    # The runtime knob's own definition is not the manager's.
    records = fresh_records()
    _define(records, entry, "ISAAC_VITA_RUNTIME_SETTINGS", ["1"])
    gate.verify_manager_settings_ui_compile_scope(records, on)
    records = fresh_records()
    del records[entry]["definitions"]
    must_fail(
        "manager-settings scope with a malformed record",
        lambda: gate.verify_manager_settings_ui_compile_scope(records, on),
        "compile record is missing source definitions",
    )


def manager_ui2_game_skin_scope_tests(root: Path) -> None:
    """The manager game skin knob, its definition and its units never reach
    an eboot compile record; ON needs the UI2 knob."""
    scope = (root / "manager-game-skin-scope").resolve()
    scope.mkdir()
    entry = (scope / "entry_vita.c").resolve()
    unrelated = (scope / "guest_0001.c").resolve()
    for source in (entry, unrelated):
        source.write_text("int x;\n", encoding="utf-8")

    def fresh_records(*extra: Path) -> dict[Path, dict[str, object]]:
        return {
            source: {
                "source": source,
                "definitions": {},
                "undefinitions": [],
            }
            for source in (entry, unrelated, *extra)
        }

    off = {"ISAAC_VITA_MANAGER_UI2_GAME_SKIN": "OFF"}
    on = {"ISAAC_VITA_MANAGER_UI2_GAME_SKIN": "ON",
          "ISAAC_VITA_MANAGER_UI2": "ON"}
    gate.verify_manager_ui2_game_skin_compile_scope(fresh_records(), off)
    gate.verify_manager_ui2_game_skin_compile_scope(fresh_records(), on)
    must_fail(
        "manager game skin scope without the feature selection",
        lambda: gate.verify_manager_ui2_game_skin_compile_scope(
            fresh_records(), {}
        ),
        "CMake cache is missing ISAAC_VITA_MANAGER_UI2_GAME_SKIN",
    )
    must_fail(
        "malformed manager game skin selection",
        lambda: gate.verify_manager_ui2_game_skin_compile_scope(
            fresh_records(), {"ISAAC_VITA_MANAGER_UI2_GAME_SKIN": "AUTO"}
        ),
        "is not ON/OFF",
    )
    must_fail(
        "manager game skin without UI2",
        lambda: gate.verify_manager_ui2_game_skin_compile_scope(
            fresh_records(), {"ISAAC_VITA_MANAGER_UI2_GAME_SKIN": "ON",
                              "ISAAC_VITA_MANAGER_UI2": "OFF"}
        ),
        "requires ISAAC_VITA_MANAGER_UI2=ON",
    )
    must_fail(
        "manager game skin with the UI2 selection missing",
        lambda: gate.verify_manager_ui2_game_skin_compile_scope(
            fresh_records(), {"ISAAC_VITA_MANAGER_UI2_GAME_SKIN": "ON"}
        ),
        "CMake cache is missing ISAAC_VITA_MANAGER_UI2",
    )
    for selection in (off, on):
        for source in (entry, unrelated):
            records = fresh_records()
            _define(records, source, "MANAGER_UI2_GAME_SKIN", ["1"])
            must_fail(
                f"manager game skin definition on {source.name}",
                lambda: gate.verify_manager_ui2_game_skin_compile_scope(
                    records, selection
                ),
                "reached an eboot unit",
            )
            records = fresh_records()
            records[source]["undefinitions"] = ["MANAGER_UI2_GAME_SKIN"]
            must_fail(
                f"manager game skin undefinition on {source.name}",
                lambda: gate.verify_manager_ui2_game_skin_compile_scope(
                    records, selection
                ),
                "reached an eboot unit",
            )
        # Every skin unit is refused as an eboot source, definition or not.
        for unit in sorted(gate.MANAGER_UI2_GAME_SKIN_UNITS):
            leaked = (scope / unit).resolve()
            leaked.write_text("int x;\n", encoding="utf-8")
            must_fail(
                f"manager game skin unit {unit} in the eboot closure",
                lambda: gate.verify_manager_ui2_game_skin_compile_scope(
                    fresh_records(leaked), selection
                ),
                "manager game skin unit reached the eboot closure",
            )
    # Other manager definitions are not the skin's.
    records = fresh_records()
    _define(records, entry, "MANAGER_UI2", ["1"])
    gate.verify_manager_ui2_game_skin_compile_scope(records, on)
    records = fresh_records()
    del records[entry]["definitions"]
    must_fail(
        "manager game skin scope with a malformed record",
        lambda: gate.verify_manager_ui2_game_skin_compile_scope(records, on),
        "compile record is missing source definitions",
    )


def manager_trophy_import_scope_tests(root: Path) -> None:
    """The manager trophy import knob never reaches an eboot compile record;
    ON needs the UI2 knob."""
    scope = (root / "manager-trophy-import-scope").resolve()
    scope.mkdir()
    entry = (scope / "entry_vita.c").resolve()
    trophies = (scope / "host_vita_trophies.c").resolve()
    for source in (entry, trophies):
        source.write_text("int x;\n", encoding="utf-8")

    def fresh_records() -> dict[Path, dict[str, object]]:
        return {
            source: {
                "source": source,
                "definitions": {},
                "undefinitions": [],
            }
            for source in (entry, trophies)
        }

    off = {"ISAAC_VITA_MANAGER_TROPHY_IMPORT": "OFF"}
    on = {"ISAAC_VITA_MANAGER_TROPHY_IMPORT": "ON",
          "ISAAC_VITA_MANAGER_UI2": "ON"}
    gate.verify_manager_trophy_import_compile_scope(fresh_records(), off)
    gate.verify_manager_trophy_import_compile_scope(fresh_records(), on)
    must_fail(
        "manager trophy import scope without the feature selection",
        lambda: gate.verify_manager_trophy_import_compile_scope(
            fresh_records(), {}
        ),
        "CMake cache is missing ISAAC_VITA_MANAGER_TROPHY_IMPORT",
    )
    must_fail(
        "malformed manager trophy import selection",
        lambda: gate.verify_manager_trophy_import_compile_scope(
            fresh_records(), {"ISAAC_VITA_MANAGER_TROPHY_IMPORT": "AUTO"}
        ),
        "is not ON/OFF",
    )
    must_fail(
        "manager trophy import without UI2",
        lambda: gate.verify_manager_trophy_import_compile_scope(
            fresh_records(), {"ISAAC_VITA_MANAGER_TROPHY_IMPORT": "ON",
                              "ISAAC_VITA_MANAGER_UI2": "OFF"}
        ),
        "requires ISAAC_VITA_MANAGER_UI2=ON",
    )
    for selection in (off, on):
        for source in (entry, trophies):
            records = fresh_records()
            _define(records, source, "MANAGER_TROPHY_IMPORT", ["1"])
            must_fail(
                f"manager trophy import definition on {source.name}",
                lambda: gate.verify_manager_trophy_import_compile_scope(
                    records, selection
                ),
                "reached an eboot unit",
            )
            records = fresh_records()
            records[source]["undefinitions"] = ["MANAGER_TROPHY_IMPORT"]
            must_fail(
                f"manager trophy import undefinition on {source.name}",
                lambda: gate.verify_manager_trophy_import_compile_scope(
                    records, selection
                ),
                "reached an eboot unit",
            )
    # The game runtime's own trophy definition is not the manager's.
    records = fresh_records()
    _define(records, trophies, "ISAAC_VITA_TROPHIES", ["1"])
    gate.verify_manager_trophy_import_compile_scope(records, on)


def manager_debug_overlay_row_scope_tests(root: Path) -> None:
    """The manager debug overlay row knob never reaches an eboot compile
    record; ON needs the UI2 knob."""
    scope = (root / "manager-debug-overlay-row-scope").resolve()
    scope.mkdir()
    entry = (scope / "entry_vita.c").resolve()
    overlay = (scope / "host_vita_debug_overlay.c").resolve()
    for source in (entry, overlay):
        source.write_text("int x;\n", encoding="utf-8")

    def fresh_records() -> dict[Path, dict[str, object]]:
        return {
            source: {
                "source": source,
                "definitions": {},
                "undefinitions": [],
            }
            for source in (entry, overlay)
        }

    off = {"ISAAC_VITA_MANAGER_DEBUG_OVERLAY_ROW": "OFF"}
    on = {"ISAAC_VITA_MANAGER_DEBUG_OVERLAY_ROW": "ON",
          "ISAAC_VITA_MANAGER_UI2": "ON"}
    gate.verify_manager_debug_overlay_row_compile_scope(fresh_records(), off)
    gate.verify_manager_debug_overlay_row_compile_scope(fresh_records(), on)
    must_fail(
        "manager debug overlay row scope without the feature selection",
        lambda: gate.verify_manager_debug_overlay_row_compile_scope(
            fresh_records(), {}
        ),
        "CMake cache is missing ISAAC_VITA_MANAGER_DEBUG_OVERLAY_ROW",
    )
    must_fail(
        "malformed manager debug overlay row selection",
        lambda: gate.verify_manager_debug_overlay_row_compile_scope(
            fresh_records(), {"ISAAC_VITA_MANAGER_DEBUG_OVERLAY_ROW": "AUTO"}
        ),
        "is not ON/OFF",
    )
    must_fail(
        "manager debug overlay row without UI2",
        lambda: gate.verify_manager_debug_overlay_row_compile_scope(
            fresh_records(), {"ISAAC_VITA_MANAGER_DEBUG_OVERLAY_ROW": "ON",
                              "ISAAC_VITA_MANAGER_UI2": "OFF"}
        ),
        "requires ISAAC_VITA_MANAGER_UI2=ON",
    )
    for selection in (off, on):
        for source in (entry, overlay):
            records = fresh_records()
            _define(records, source, "MANAGER_DEBUG_OVERLAY_ROW", ["1"])
            must_fail(
                f"manager debug overlay row definition on {source.name}",
                lambda: gate.verify_manager_debug_overlay_row_compile_scope(
                    records, selection
                ),
                "reached an eboot unit",
            )
            records = fresh_records()
            records[source]["undefinitions"] = ["MANAGER_DEBUG_OVERLAY_ROW"]
            must_fail(
                f"manager debug overlay row undefinition on {source.name}",
                lambda: gate.verify_manager_debug_overlay_row_compile_scope(
                    records, selection
                ),
                "reached an eboot unit",
            )
    records = fresh_records()
    del records[entry]["definitions"]
    must_fail(
        "manager debug overlay row scope with a malformed record",
        lambda: gate.verify_manager_debug_overlay_row_compile_scope(
            records, on),
        "compile record is missing source definitions",
    )


def stable30_scope_tests(root: Path) -> None:
    scope = (root / "stable30-scope").resolve()
    scope.mkdir()
    app = (scope / "guest_0001.c").resolve()
    manager = (scope / "guest_0002.c").resolve()
    backend = (scope / "kage_vita_backend.c").resolve()
    hooks = (scope / "kage_vita_generated_hooks.c").resolve()
    runtime = (scope / "kage_vita_stable30.c").resolve()
    unrelated = (scope / "guest_9999.c").resolve()
    app_text = (
        "void sub_0048bc50(CPU *__restrict c)\n"
        "if (kage_pc_backend_stable30_bypass_limiter())\n"
    )
    manager_text = (
        "void sub_004b0010(CPU *__restrict c)\n"
        "if (kage_pc_backend_stable30_manager_entry(c->ecx))\n"
        "void sub_004b0600(CPU *__restrict c)\n"
        "if (kage_pc_backend_stable30_plan_render(\n"
    )
    app.write_text(app_text, encoding="utf-8")
    manager.write_text(manager_text, encoding="utf-8")
    for source in (backend, hooks, runtime, unrelated):
        source.write_text("int stable30_fixture;\n", encoding="utf-8")
    records: dict[Path, dict[str, object]] = {
        source: {
            "source": source,
            "definitions": {},
            "undefinitions": [],
        }
        for source in (app, manager, backend, hooks, runtime, unrelated)
    }
    inactive = {"ISAAC_VITA_STABLE_30_PRESENTATION": "OFF"}
    gate.verify_stable30_compile_scope(records, inactive)
    must_fail(
        "malformed stable30 compile policy",
        lambda: gate.verify_stable30_compile_scope(
            records, {"ISAAC_VITA_STABLE_30_PRESENTATION": "AUTO"}
        ),
        "is not ON/OFF",
    )
    must_fail(
        "stable30 active without guest link ID",
        lambda: gate.verify_stable30_compile_scope(
            records, {"ISAAC_VITA_STABLE_30_PRESENTATION": "ON"}
        ),
        "CMake cache is missing ISAAC_VITA_GUEST_LINK_ID",
    )
    must_fail(
        "stable30 active with hostile guest link ID",
        lambda: gate.verify_stable30_compile_scope(
            records,
            {
                "ISAAC_VITA_STABLE_30_PRESENTATION": "ON",
                "ISAAC_VITA_GUEST_LINK_ID": "hostile;id",
            },
        ),
        "is not a controlled build ID",
    )
    active = {
        "ISAAC_VITA_STABLE_30_PRESENTATION": "ON",
        "ISAAC_VITA_GUEST_LINK_ID": "fixture-build",
    }
    must_fail(
        "missing stable30 owner definitions",
        lambda: gate.verify_stable30_compile_scope(records, active),
        "compile-definition scope changed",
    )
    owners = (app, manager, backend, hooks, runtime)
    for source in owners:
        definitions = records[source]["definitions"]
        assert isinstance(definitions, dict)
        definitions["ISAAC_VITA_STABLE_30_PRESENTATION"] = ["1"]
    runtime_definitions = records[runtime]["definitions"]
    assert isinstance(runtime_definitions, dict)
    runtime_definitions["ISAAC_VITA_STABLE30_BUILD_ID"] = [
        '"fixture-build"'
    ]
    gate.verify_stable30_compile_scope(records, active)
    unrelated_definitions = records[unrelated]["definitions"]
    assert isinstance(unrelated_definitions, dict)
    unrelated_definitions["ISAAC_VITA_STABLE30_BUILD_ID"] = [
        '"fixture-build"'
    ]
    must_fail(
        "stable30 build ID leaked into unrelated owner",
        lambda: gate.verify_stable30_compile_scope(records, active),
        "build-ID scope changed",
    )
    unrelated_definitions.clear()

    app_definitions = records[app]["definitions"]
    assert isinstance(app_definitions, dict)
    app_definitions["ISAAC_VITA_STABLE_30_PRESENTATION"] = ["0"]
    must_fail(
        "non-canonical stable30 definition",
        lambda: gate.verify_stable30_compile_scope(records, active),
        "non-canonical stable30 definition",
    )
    app_definitions["ISAAC_VITA_STABLE_30_PRESENTATION"] = ["1"]
    records[backend]["undefinitions"] = [
        "ISAAC_VITA_STABLE_30_PRESENTATION"
    ]
    must_fail(
        "undefined stable30 policy",
        lambda: gate.verify_stable30_compile_scope(records, active),
        "stable30 policy is undefined",
    )
    records[backend]["undefinitions"] = []
    unrelated_definitions["ISAAC_VITA_STABLE_30_PRESENTATION"] = ["1"]
    must_fail(
        "stable30 definition leaked into unrelated owner",
        lambda: gate.verify_stable30_compile_scope(records, active),
        "compile-definition scope changed",
    )
    unrelated_definitions.clear()
    runtime_definitions["ISAAC_VITA_STABLE30_BUILD_ID"] = ['"wrong-build"']
    must_fail(
        "stable30 build ID mismatch",
        lambda: gate.verify_stable30_compile_scope(records, active),
        "build ID disagrees",
    )
    runtime_definitions["ISAAC_VITA_STABLE30_BUILD_ID"] = [
        '"fixture-build"'
    ]
    manager.write_text(
        manager_text.replace(
            "if (kage_pc_backend_stable30_plan_render(\n", ""
        ),
        encoding="utf-8",
    )
    must_fail(
        "missing stable30 generated seam",
        lambda: gate.verify_stable30_compile_scope(records, active),
        "generated owner/seam census changed",
    )
    manager.write_text(manager_text, encoding="utf-8")
    runtime_definitions.pop("ISAAC_VITA_STABLE30_BUILD_ID")
    for source in owners:
        definitions = records[source]["definitions"]
        assert isinstance(definitions, dict)
        definitions.pop("ISAAC_VITA_STABLE_30_PRESENTATION")
    gate.verify_stable30_compile_scope(records, inactive)


def guest_stack_memblock_scope_tests(root: Path) -> None:
    scope = root / "guest-stack-memblock-scope"
    scope.mkdir()
    guest = (scope / "guest.c").resolve()
    entry = (scope / "entry_vita.c").resolve()
    unrelated = (scope / "host_vita_heap.c").resolve()
    for source in (guest, entry, unrelated):
        source.write_text("int guest_stack_memblock_fixture;\n", encoding="utf-8")
    records: dict[Path, dict[str, object]] = {
        source: {
            "source": source,
            "definitions": {},
            "undefinitions": [],
        }
        for source in (guest, entry, unrelated)
    }
    inactive = {"ISAAC_VITA_GUEST_STACK_MEMBLOCK": "OFF"}
    active = {"ISAAC_VITA_GUEST_STACK_MEMBLOCK": "ON"}
    gate.verify_guest_stack_memblock_compile_scope(records, inactive)
    must_fail(
        "missing guest-stack-memblock compile policy",
        lambda: gate.verify_guest_stack_memblock_compile_scope(records, {}),
        "CMake cache is missing ISAAC_VITA_GUEST_STACK_MEMBLOCK",
    )
    must_fail(
        "malformed guest-stack-memblock compile policy",
        lambda: gate.verify_guest_stack_memblock_compile_scope(
            records, {"ISAAC_VITA_GUEST_STACK_MEMBLOCK": "AUTO"}
        ),
        "is not ON/OFF",
    )
    must_fail(
        "guest-stack-memblock ON without owner definitions",
        lambda: gate.verify_guest_stack_memblock_compile_scope(records, active),
        "compile-definition scope changed",
    )
    for source in (guest, entry):
        definitions = records[source]["definitions"]
        assert isinstance(definitions, dict)
        definitions["ISAAC_VITA_GUEST_STACK_MEMBLOCK"] = ["1"]
    gate.verify_guest_stack_memblock_compile_scope(records, active)
    must_fail(
        "guest-stack-memblock define leaked while OFF",
        lambda: gate.verify_guest_stack_memblock_compile_scope(records, inactive),
        "compile-definition scope changed",
    )
    guest_definitions = records[guest]["definitions"]
    assert isinstance(guest_definitions, dict)
    guest_definitions["ISAAC_VITA_GUEST_STACK_MEMBLOCK"] = ["0"]
    must_fail(
        "non-canonical guest-stack-memblock definition",
        lambda: gate.verify_guest_stack_memblock_compile_scope(records, active),
        "non-canonical guest-stack-memblock definition",
    )
    guest_definitions["ISAAC_VITA_GUEST_STACK_MEMBLOCK"] = ["1"]
    unrelated_definitions = records[unrelated]["definitions"]
    assert isinstance(unrelated_definitions, dict)
    unrelated_definitions["ISAAC_VITA_GUEST_STACK_MEMBLOCK"] = ["1"]
    must_fail(
        "guest-stack-memblock define leaked into an unrelated owner",
        lambda: gate.verify_guest_stack_memblock_compile_scope(records, active),
        "compile-definition scope changed",
    )
    unrelated_definitions.clear()
    del records[entry]["definitions"]["ISAAC_VITA_GUEST_STACK_MEMBLOCK"]  # type: ignore[union-attr]
    must_fail(
        "guest-stack-memblock define missing on the receipt owner",
        lambda: gate.verify_guest_stack_memblock_compile_scope(records, active),
        "missing=['entry_vita.c']",
    )
    records[entry]["definitions"] = {"ISAAC_VITA_GUEST_STACK_MEMBLOCK": ["1"]}
    records[unrelated]["undefinitions"] = ["ISAAC_VITA_GUEST_STACK_MEMBLOCK"]
    must_fail(
        "undefined guest-stack-memblock policy",
        lambda: gate.verify_guest_stack_memblock_compile_scope(records, active),
        "guest-stack-memblock compile policy is undefined",
    )
    records[unrelated]["undefinitions"] = []
    gate.verify_guest_stack_memblock_compile_scope(records, active)


def audio_stream_receipt_scope_tests(root: Path) -> None:
    scope = (root / "audio-stream-receipt-scope").resolve()
    scope.mkdir()
    owner = (scope / "host_vita_audio_cooperative.c").resolve()
    owner.write_text("int audio_stream_receipt_owner;\n", encoding="utf-8")
    unrelated = (scope / "guest_9999.c").resolve()
    unrelated.write_text("int audio_stream_receipt_unrelated;\n", encoding="utf-8")
    records: dict[Path, dict[str, object]] = {
        source: {
            "source": source,
            "definitions": {},
            "undefinitions": [],
        }
        for source in (owner, unrelated)
    }
    inactive = {"ISAAC_VITA_AUDIO_STREAM_RECEIPT": "OFF"}
    gate.verify_audio_stream_receipt_compile_scope(records, inactive)

    must_fail(
        "missing audio-stream receipt compile selection",
        lambda: gate.verify_audio_stream_receipt_compile_scope(records, {}),
        "CMake cache is missing ISAAC_VITA_AUDIO_STREAM_RECEIPT",
    )
    must_fail(
        "malformed audio-stream receipt compile selection",
        lambda: gate.verify_audio_stream_receipt_compile_scope(
            records, {"ISAAC_VITA_AUDIO_STREAM_RECEIPT": "AUTO"}
        ),
        "is not ON/OFF",
    )

    active = {"ISAAC_VITA_AUDIO_STREAM_RECEIPT": "ON"}
    must_fail(
        "missing audio-stream receipt owner definition",
        lambda: gate.verify_audio_stream_receipt_compile_scope(records, active),
        "compile-definition scope changed",
    )
    owner_definitions = records[owner]["definitions"]
    assert isinstance(owner_definitions, dict)
    owner_definitions["ISAAC_VITA_AUDIO_STREAM_RECEIPT"] = ["1"]
    gate.verify_audio_stream_receipt_compile_scope(records, active)

    owner_definitions["ISAAC_VITA_AUDIO_STREAM_RECEIPT"] = ["0"]
    must_fail(
        "non-canonical audio-stream receipt definition",
        lambda: gate.verify_audio_stream_receipt_compile_scope(records, active),
        "non-canonical audio-stream receipt definition",
    )
    owner_definitions["ISAAC_VITA_AUDIO_STREAM_RECEIPT"] = ["1"]
    records[owner]["undefinitions"] = ["ISAAC_VITA_AUDIO_STREAM_RECEIPT"]
    must_fail(
        "undefined audio-stream receipt policy",
        lambda: gate.verify_audio_stream_receipt_compile_scope(records, active),
        "audio-stream receipt compile policy is undefined",
    )
    records[owner]["undefinitions"] = []
    unrelated_definitions = records[unrelated]["definitions"]
    assert isinstance(unrelated_definitions, dict)
    unrelated_definitions["ISAAC_VITA_AUDIO_STREAM_RECEIPT"] = ["1"]
    must_fail(
        "audio-stream receipt definition leaked into unrelated owner",
        lambda: gate.verify_audio_stream_receipt_compile_scope(records, active),
        "compile-definition scope changed",
    )
    unrelated_definitions.clear()
    must_fail(
        "audio-stream receipt definition survived OFF selection",
        lambda: gate.verify_audio_stream_receipt_compile_scope(records, inactive),
        "compile-definition scope changed",
    )


def world_seam_diag_scope_tests(root: Path) -> None:
    scope = (root / "world-seam-scope").resolve()
    scope.mkdir()
    owner = (scope / "guest_0001.c").resolve()
    owner_text = (
        "void sub_002ce770(CPU *__restrict c)\n"
        "{\n"
        "    extern void kage_vita_world_seam_diag_begin(uint32_t, uint32_t);\n"
        "    kage_vita_world_seam_diag_begin(8, 1);\n"
        "    kage_vita_world_seam_diag_post_room();\n"
        "    kage_vita_world_seam_diag_post_lua();\n"
        "    kage_vita_world_seam_diag_pre_hud();\n"
        "}\n"
    )
    owner.write_text(owner_text, encoding="utf-8")
    backend = (scope / "gl_vita_backend.c").resolve()
    backend.write_text("int world_seam_backend_fixture;\n", encoding="utf-8")
    runtime = (scope / "kage_vita_world_seam_diag.c").resolve()
    runtime.write_text("int world_seam_runtime_fixture;\n", encoding="utf-8")
    unrelated = (scope / "guest_9999.c").resolve()
    unrelated.write_text("int world_seam_unrelated_fixture;\n", encoding="utf-8")
    records: dict[Path, dict[str, object]] = {
        source: {
            "source": source,
            "definitions": {},
            "undefinitions": [],
        }
        for source in (owner, backend, runtime, unrelated)
    }
    inactive = {
        "ISAAC_VITA_WORLD_SEAM_DIAG": "OFF",
        "ISAAC_VITA_GUEST_LINK_ID": "fixture-build",
    }
    gate.verify_world_seam_diag_compile_scope(records, inactive)
    missing = dict(inactive)
    del missing["ISAAC_VITA_WORLD_SEAM_DIAG"]
    must_fail(
        "missing world-seam compile selection",
        lambda: gate.verify_world_seam_diag_compile_scope(records, missing),
        "CMake cache is missing ISAAC_VITA_WORLD_SEAM_DIAG",
    )
    must_fail(
        "malformed world-seam compile selection",
        lambda: gate.verify_world_seam_diag_compile_scope(
            records, {**inactive, "ISAAC_VITA_WORLD_SEAM_DIAG": "AUTO"}
        ),
        "is not ON/OFF",
    )
    active = {**inactive, "ISAAC_VITA_WORLD_SEAM_DIAG": "ON"}
    must_fail(
        "world-seam active without guest link ID",
        lambda: gate.verify_world_seam_diag_compile_scope(
            records, {"ISAAC_VITA_WORLD_SEAM_DIAG": "ON"}
        ),
        "CMake cache is missing ISAAC_VITA_GUEST_LINK_ID",
    )
    must_fail(
        "world-seam active with hostile guest link ID",
        lambda: gate.verify_world_seam_diag_compile_scope(
            records,
            {
                "ISAAC_VITA_WORLD_SEAM_DIAG": "ON",
                "ISAAC_VITA_GUEST_LINK_ID": "hostile;id",
            },
        ),
        "is not a controlled build ID",
    )
    must_fail(
        "missing world-seam owner definitions",
        lambda: gate.verify_world_seam_diag_compile_scope(records, active),
        "compile-definition scope changed",
    )
    for source in (owner, backend, runtime):
        definitions = records[source]["definitions"]
        assert isinstance(definitions, dict)
        definitions["ISAAC_VITA_WORLD_SEAM_DIAG"] = ["1"]
    runtime_definitions = records[runtime]["definitions"]
    assert isinstance(runtime_definitions, dict)
    runtime_definitions["ISAAC_VITA_WORLD_SEAM_DIAG_BUILD_ID"] = [
        '"fixture-build"'
    ]
    gate.verify_world_seam_diag_compile_scope(records, active)

    owner_definitions = records[owner]["definitions"]
    assert isinstance(owner_definitions, dict)
    owner_definitions["ISAAC_VITA_WORLD_SEAM_DIAG"] = ["0"]
    must_fail(
        "non-canonical world-seam definition",
        lambda: gate.verify_world_seam_diag_compile_scope(records, active),
        "non-canonical world-seam definition",
    )
    owner_definitions["ISAAC_VITA_WORLD_SEAM_DIAG"] = ["1"]
    records[backend]["undefinitions"] = ["ISAAC_VITA_WORLD_SEAM_DIAG"]
    must_fail(
        "undefined world-seam policy",
        lambda: gate.verify_world_seam_diag_compile_scope(records, active),
        "world-seam policy is undefined",
    )
    records[backend]["undefinitions"] = []
    unrelated_definitions = records[unrelated]["definitions"]
    assert isinstance(unrelated_definitions, dict)
    unrelated_definitions["ISAAC_VITA_WORLD_SEAM_DIAG"] = ["1"]
    must_fail(
        "world-seam definition leaked into unrelated owner",
        lambda: gate.verify_world_seam_diag_compile_scope(records, active),
        "compile-definition scope changed",
    )
    unrelated_definitions.clear()
    runtime_definitions["ISAAC_VITA_WORLD_SEAM_DIAG_BUILD_ID"] = [
        '"wrong-build"'
    ]
    must_fail(
        "world-seam build ID mismatch",
        lambda: gate.verify_world_seam_diag_compile_scope(records, active),
        "build ID disagrees",
    )
    runtime_definitions["ISAAC_VITA_WORLD_SEAM_DIAG_BUILD_ID"] = [
        '"fixture-build"'
    ]
    owner.write_text(
        owner_text.replace("kage_vita_world_seam_diag_pre_hud();\n", ""),
        encoding="utf-8",
    )
    must_fail(
        "missing world-seam generated seam",
        lambda: gate.verify_world_seam_diag_compile_scope(records, active),
        "generated owner/seam census changed",
    )
    owner.write_text(owner_text, encoding="utf-8")
    runtime_definitions.pop("ISAAC_VITA_WORLD_SEAM_DIAG_BUILD_ID")
    for source in (owner, backend, runtime):
        definitions = records[source]["definitions"]
        assert isinstance(definitions, dict)
        definitions.pop("ISAAC_VITA_WORLD_SEAM_DIAG")
    gate.verify_world_seam_diag_compile_scope(records, inactive)


def direct_default_tests(root: Path) -> None:
    build = (root / "direct-default-build").resolve()
    output = (build / gate.DIRECT_DEFAULT_OUTPUT_RELATIVE).resolve()
    output.parent.mkdir(parents=True)
    fixture = b"derived fixture\n"
    output.write_bytes(fixture)
    cache = {
        "ISAAC_VITA_DIRECT_DEFAULT": "OFF",
        "ISAAC_VITA_RENDER_SURFACE_NATIVE": "OFF",
        "ISAAC_VITA_FIRST_FRAME_PROBE": "ON",
        "ISAAC_VITA_VITAGL_STOCK_REFERENCE": "OFF",
        "ISAAC_VITA_DISPLAY_RASTER_720": "OFF",
        "ISAAC_VITA_PHASE_PROFILE": "OFF",
        "ISAAC_VITA_GL_TYPED_STATE_CACHE": "OFF",
        "ISAAC_VITA_GAME_LOG_BATCH": "OFF",
        "ISAAC_VITA_GAME_LOG_ASYNC": "OFF",
        "ISAAC_VITA_CONTINUE_PROFILE": "OFF",
        "ISAAC_VITA_EXIT_MENU_PROFILE": "OFF",
        "ISAAC_VITA_PNG_DECODE_PROFILE": "OFF",
        "ISAAC_VITA_PNG_NATIVE_UNFILTER": "OFF",
        "ISAAC_VITA_TEXTURE_CHURN_PROFILE": "OFF",
        "ISAAC_VITA_GUEST_LINK_ID": "fixture-build",
    }
    if gate.direct_default_overrides(cache, None, build):
        raise AssertionError("OFF direct-default unexpectedly has an override")
    must_fail(
        "ON direct-default without override",
        lambda: gate.direct_default_overrides(
            {**cache, "ISAAC_VITA_DIRECT_DEFAULT": "ON"}, None, build
        ),
        "selection disagrees",
    )
    must_fail(
        "OFF direct-default with override",
        lambda: gate.direct_default_overrides(cache, output, build),
        "selection disagrees",
    )
    must_fail(
        "relative direct-default override",
        lambda: gate.direct_default_overrides(
            {**cache, "ISAAC_VITA_DIRECT_DEFAULT": "ON"},
            Path(gate.DIRECT_DEFAULT_SOURCE_NAME),
            build,
        ),
        "not absolute",
    )

    saved_size = gate.DIRECT_DEFAULT_OUTPUT_SIZE
    saved_sha = gate.DIRECT_DEFAULT_OUTPUT_SHA256
    gate.DIRECT_DEFAULT_OUTPUT_SIZE = len(fixture)
    gate.DIRECT_DEFAULT_OUTPUT_SHA256 = hashlib.sha256(fixture).hexdigest()
    active = {**cache, "ISAAC_VITA_DIRECT_DEFAULT": "ON"}
    try:
        overrides = gate.direct_default_overrides(active, output, build)
        if overrides != {gate.DIRECT_DEFAULT_SOURCE_NAME: output}:
            raise AssertionError("direct-default physical override changed")
        elsewhere = (root / "elsewhere" / gate.DIRECT_DEFAULT_SOURCE_NAME)
        elsewhere.parent.mkdir(parents=True)
        elsewhere.write_bytes(fixture)
        must_fail(
            "wrong build-owned override path",
            lambda: gate.direct_default_overrides(
                active, elsewhere, build
            ),
            "wrong build-owned path",
        )
        output.write_bytes(fixture + b"mutation")
        must_fail(
            "wrong direct-default output identity",
            lambda: gate.direct_default_overrides(active, output, build),
            "identity changed",
        )
        output.write_bytes(fixture)

        # The render-surface-native mode pins its own derived identity; the
        # direct-default bytes must be rejected under it and vice versa.
        native_fixture = b"render-surface-native fixture\n"
        saved_native = (
            gate.RENDER_SURFACE_NATIVE_OUTPUT_SIZE,
            gate.RENDER_SURFACE_NATIVE_OUTPUT_SHA256,
        )
        gate.RENDER_SURFACE_NATIVE_OUTPUT_SIZE = len(native_fixture)
        gate.RENDER_SURFACE_NATIVE_OUTPUT_SHA256 = hashlib.sha256(
            native_fixture
        ).hexdigest()
        native = {**active, "ISAAC_VITA_RENDER_SURFACE_NATIVE": "ON"}
        try:
            must_fail(
                "direct-default bytes under render-surface-native",
                lambda: gate.direct_default_overrides(native, output, build),
                "identity changed (render-surface-native=ON)",
            )
            output.write_bytes(native_fixture)
            if gate.direct_default_overrides(native, output, build) != {
                gate.DIRECT_DEFAULT_SOURCE_NAME: output
            }:
                raise AssertionError(
                    "render-surface-native override was not accepted"
                )
            must_fail(
                "render-surface-native bytes under direct-default",
                lambda: gate.direct_default_overrides(active, output, build),
                "identity changed (render-surface-native=OFF)",
            )
            missing_mode = dict(active)
            del missing_mode["ISAAC_VITA_RENDER_SURFACE_NATIVE"]
            must_fail(
                "missing render-surface-native selection",
                lambda: gate.direct_default_overrides(
                    missing_mode, output, build
                ),
                "CMake cache is missing ISAAC_VITA_RENDER_SURFACE_NATIVE",
            )
        finally:
            (
                gate.RENDER_SURFACE_NATIVE_OUTPUT_SIZE,
                gate.RENDER_SURFACE_NATIVE_OUTPUT_SHA256,
            ) = saved_native
        output.write_bytes(fixture)
        foreign = output.parent / "foreign.txt"
        foreign.write_bytes(b"unowned\n")
        must_fail(
            "unowned direct-default sibling",
            lambda: gate.direct_default_overrides(active, output, build),
            "directory inventory changed",
        )
        foreign.unlink()

        symlink_build = (root / "symlink-direct-default-build").resolve()
        symlink_build.mkdir()
        outside_owner = (root / "outside-direct-default-owner").resolve()
        (outside_owner / "direct-default").mkdir(parents=True)
        linked_owner = symlink_build / "isaac-generated-overrides"
        try:
            os.symlink(outside_owner, linked_owner, target_is_directory=True)
        except OSError:
            pass
        else:
            linked_output = (
                linked_owner / "direct-default" /
                gate.DIRECT_DEFAULT_SOURCE_NAME
            )
            linked_output.write_bytes(fixture)
            must_fail(
                "symlinked build-owned override ancestor",
                lambda: gate.direct_default_overrides(
                    active, linked_output, symlink_build
                ),
                "symlinked build-owned directory",
            )
    finally:
        gate.DIRECT_DEFAULT_OUTPUT_SIZE = saved_size
        gate.DIRECT_DEFAULT_OUTPUT_SHA256 = saved_sha

    owners = gate.DIRECT_DEFAULT_DEFINITION_OWNERS
    records: dict[Path, dict[str, object]] = {}
    for name in (*sorted(owners),
                 *sorted(gate.STOCK_REFERENCE_DEFINITION_OWNERS),
                 "kage_vita_phase_profile.c",
                 "kage_vita_continue_profile.c",
                 "kage_vita_exit_menu_profile.c",
                 "host_vita_crt.c",
                 *sorted(name for name in gate.GAME_LOG_ASYNC_DEFINITION_OWNERS
                         if name != "host_vita_crt.c"),
                 gate.DIRECT_DEFAULT_SOURCE_NAME):
        source = (root / "scope" / name).resolve()
        records[source] = {"source": source, "definitions": {}}
    gate.verify_direct_default_compile_scope(records, cache)
    gate.verify_stock_reference_compile_scope(records, cache)
    gate.verify_display_raster_compile_scope(records, cache)
    gate.verify_typed_state_cache_compile_scope(records, cache)
    gate.verify_game_log_batch_compile_scope(records, cache)
    gate.verify_exit_menu_profile_compile_scope(records, cache)
    gate.verify_png_decode_profile_compile_scope(records, cache)
    missing_display = dict(cache)
    del missing_display["ISAAC_VITA_DISPLAY_RASTER_720"]
    must_fail(
        "missing display-raster cache selection",
        lambda: gate.verify_display_raster_compile_scope(
            records, missing_display
        ),
        "CMake cache is missing ISAAC_VITA_DISPLAY_RASTER_720",
    )
    must_fail(
        "malformed display-raster cache selection",
        lambda: gate.verify_display_raster_compile_scope(
            records,
            {**cache, "ISAAC_VITA_DISPLAY_RASTER_720": "AUTO"},
        ),
        "is not ON/OFF",
    )
    display_active = {**cache, "ISAAC_VITA_DISPLAY_RASTER_720": "ON"}
    for record in records.values():
        source = record["source"]
        definitions = record["definitions"]
        assert isinstance(source, Path)
        assert isinstance(definitions, dict)
        if source.name in gate.DISPLAY_RASTER_DEFINITION_OWNERS:
            definitions["ISAAC_VITA_DISPLAY_RASTER_720"] = ["1"]
    gate.verify_display_raster_compile_scope(records, display_active)
    display_owner = next(
        record for record in records.values()
        if isinstance(record["source"], Path) and
        record["source"].name in gate.DISPLAY_RASTER_DEFINITION_OWNERS
    )
    display_definitions = display_owner["definitions"]
    assert isinstance(display_definitions, dict)
    display_definitions["ISAAC_VITA_DISPLAY_RASTER_720"] = ["0"]
    must_fail(
        "non-canonical display-raster owner definition",
        lambda: gate.verify_display_raster_compile_scope(
            records, display_active
        ),
        "non-canonical display-raster definition",
    )
    display_definitions["ISAAC_VITA_DISPLAY_RASTER_720"] = ["1"]
    display_guest = next(
        record for record in records.values()
        if isinstance(record["source"], Path) and
        record["source"].name == gate.DIRECT_DEFAULT_SOURCE_NAME
    )
    display_guest_definitions = display_guest["definitions"]
    assert isinstance(display_guest_definitions, dict)
    display_guest_definitions["ISAAC_VITA_DISPLAY_RASTER_720"] = ["1"]
    must_fail(
        "display-raster definition leaked into derived guest",
        lambda: gate.verify_display_raster_compile_scope(
            records, display_active
        ),
        "scope changed",
    )
    for record in records.values():
        definitions = record["definitions"]
        assert isinstance(definitions, dict)
        definitions.pop("ISAAC_VITA_DISPLAY_RASTER_720", None)
    missing_typed = dict(cache)
    del missing_typed["ISAAC_VITA_GL_TYPED_STATE_CACHE"]
    must_fail(
        "missing typed-state-cache selection",
        lambda: gate.verify_typed_state_cache_compile_scope(
            records, missing_typed
        ),
        "CMake cache is missing ISAAC_VITA_GL_TYPED_STATE_CACHE",
    )
    must_fail(
        "malformed typed-state-cache selection",
        lambda: gate.verify_typed_state_cache_compile_scope(
            records,
            {**cache, "ISAAC_VITA_GL_TYPED_STATE_CACHE": "AUTO"},
        ),
        "is not ON/OFF",
    )
    typed_active = {**cache, "ISAAC_VITA_GL_TYPED_STATE_CACHE": "ON"}
    typed_backend = next(
        record for record in records.values()
        if isinstance(record["source"], Path) and
        record["source"].name == "gl_vita_backend.c"
    )
    typed_backend_definitions = typed_backend["definitions"]
    assert isinstance(typed_backend_definitions, dict)
    typed_backend_definitions["ISAAC_VITA_GL_TYPED_STATE_CACHE"] = ["1"]
    gate.verify_typed_state_cache_compile_scope(records, typed_active)
    typed_backend_definitions["ISAAC_VITA_GL_TYPED_STATE_CACHE"] = ["0"]
    must_fail(
        "non-canonical typed-state-cache owner definition",
        lambda: gate.verify_typed_state_cache_compile_scope(
            records, typed_active
        ),
        "non-canonical typed-state-cache definition",
    )
    typed_backend_definitions["ISAAC_VITA_GL_TYPED_STATE_CACHE"] = ["1"]
    typed_backend["undefinitions"] = ["ISAAC_VITA_GL_TYPED_STATE_CACHE"]
    must_fail(
        "typed-state-cache owner undefinition",
        lambda: gate.verify_typed_state_cache_compile_scope(
            records, typed_active
        ),
        "typed-state-cache compile policy is undefined",
    )
    typed_backend.pop("undefinitions")
    typed_guest_definitions = display_guest["definitions"]
    assert isinstance(typed_guest_definitions, dict)
    typed_guest_definitions["ISAAC_VITA_GL_TYPED_STATE_CACHE"] = ["1"]
    must_fail(
        "typed-state-cache definition leaked into derived guest",
        lambda: gate.verify_typed_state_cache_compile_scope(
            records, typed_active
        ),
        "scope changed",
    )
    typed_guest_definitions.pop("ISAAC_VITA_GL_TYPED_STATE_CACHE")
    typed_phase_active = {
        **typed_active,
        "ISAAC_VITA_PHASE_PROFILE": "ON",
    }
    typed_phase = next(
        record for record in records.values()
        if isinstance(record["source"], Path) and
        record["source"].name == "kage_vita_phase_profile.c"
    )
    typed_phase_definitions = typed_phase["definitions"]
    assert isinstance(typed_phase_definitions, dict)
    must_fail(
        "missing typed-state-cache phase-profile owner definition",
        lambda: gate.verify_typed_state_cache_compile_scope(
            records, typed_phase_active
        ),
        "scope changed",
    )
    typed_phase_definitions["ISAAC_VITA_GL_TYPED_STATE_CACHE"] = ["1"]
    gate.verify_typed_state_cache_compile_scope(records, typed_phase_active)
    typed_backend_definitions.pop("ISAAC_VITA_GL_TYPED_STATE_CACHE")
    typed_phase_definitions.pop("ISAAC_VITA_GL_TYPED_STATE_CACHE")
    missing_texture_churn = dict(cache)
    del missing_texture_churn["ISAAC_VITA_TEXTURE_CHURN_PROFILE"]
    must_fail(
        "missing texture-churn compile selection",
        lambda: gate.verify_texture_churn_profile_compile_scope(
            records, missing_texture_churn
        ),
        "CMake cache is missing ISAAC_VITA_TEXTURE_CHURN_PROFILE",
    )
    must_fail(
        "malformed texture-churn compile selection",
        lambda: gate.verify_texture_churn_profile_compile_scope(
            records,
            {**cache, "ISAAC_VITA_TEXTURE_CHURN_PROFILE": "AUTO"},
        ),
        "is not ON/OFF",
    )
    texture_churn_active = {
        **cache,
        "ISAAC_VITA_PHASE_PROFILE": "ON",
        "ISAAC_VITA_VITAGL_STOCK_REFERENCE": "ON",
        "ISAAC_VITA_TEXTURE_CHURN_PROFILE": "ON",
    }
    must_fail(
        "missing texture-churn owner definitions",
        lambda: gate.verify_texture_churn_profile_compile_scope(
            records, texture_churn_active
        ),
        "scope changed",
    )
    typed_backend_definitions["ISAAC_VITA_TEXTURE_CHURN_PROFILE"] = ["1"]
    typed_phase_definitions["ISAAC_VITA_TEXTURE_CHURN_PROFILE"] = ["1"]
    gate.verify_texture_churn_profile_compile_scope(
        records, texture_churn_active
    )
    typed_backend_definitions["ISAAC_VITA_TEXTURE_CHURN_PROFILE"] = ["0"]
    must_fail(
        "non-canonical texture-churn definition",
        lambda: gate.verify_texture_churn_profile_compile_scope(
            records, texture_churn_active
        ),
        "non-canonical texture-churn definition",
    )
    typed_backend_definitions["ISAAC_VITA_TEXTURE_CHURN_PROFILE"] = ["1"]
    typed_backend["undefinitions"] = ["ISAAC_VITA_TEXTURE_CHURN_PROFILE"]
    must_fail(
        "undefined texture-churn policy",
        lambda: gate.verify_texture_churn_profile_compile_scope(
            records, texture_churn_active
        ),
        "texture-churn compile policy is undefined",
    )
    typed_backend.pop("undefinitions")
    typed_guest_definitions["ISAAC_VITA_TEXTURE_CHURN_PROFILE"] = ["1"]
    must_fail(
        "texture-churn definition leaked into generated guest",
        lambda: gate.verify_texture_churn_profile_compile_scope(
            records, texture_churn_active
        ),
        "scope changed",
    )
    typed_guest_definitions.pop("ISAAC_VITA_TEXTURE_CHURN_PROFILE")
    must_fail(
        "texture-churn definition present while OFF",
        lambda: gate.verify_texture_churn_profile_compile_scope(
            records, cache
        ),
        "scope changed",
    )
    typed_backend_definitions.pop("ISAAC_VITA_TEXTURE_CHURN_PROFILE")
    typed_phase_definitions.pop("ISAAC_VITA_TEXTURE_CHURN_PROFILE")
    gate.verify_texture_churn_profile_compile_scope(records, cache)
    missing_log_batch = dict(cache)
    del missing_log_batch["ISAAC_VITA_GAME_LOG_BATCH"]
    must_fail(
        "missing game-log-batch selection",
        lambda: gate.verify_game_log_batch_compile_scope(
            records, missing_log_batch
        ),
        "CMake cache is missing ISAAC_VITA_GAME_LOG_BATCH",
    )
    must_fail(
        "malformed game-log-batch selection",
        lambda: gate.verify_game_log_batch_compile_scope(
            records,
            {**cache, "ISAAC_VITA_GAME_LOG_BATCH": "AUTO"},
        ),
        "is not ON/OFF",
    )
    log_batch_active = {**cache, "ISAAC_VITA_GAME_LOG_BATCH": "ON"}
    log_crt = next(
        record for record in records.values()
        if isinstance(record["source"], Path) and
        record["source"].name == "host_vita_crt.c"
    )
    log_crt_definitions = log_crt["definitions"]
    assert isinstance(log_crt_definitions, dict)
    log_crt_definitions["ISAAC_VITA_GAME_LOG_BATCH"] = ["1"]
    gate.verify_game_log_batch_compile_scope(records, log_batch_active)
    log_continue_active = {
        **log_batch_active,
        "ISAAC_VITA_CONTINUE_PROFILE": "ON",
    }
    log_continue = next(
        record for record in records.values()
        if isinstance(record["source"], Path) and
        record["source"].name == "kage_vita_continue_profile.c"
    )
    log_continue_definitions = log_continue["definitions"]
    assert isinstance(log_continue_definitions, dict)
    must_fail(
        "missing game-log-batch Continue owner definition",
        lambda: gate.verify_game_log_batch_compile_scope(
            records, log_continue_active
        ),
        "scope changed",
    )
    log_continue_definitions["ISAAC_VITA_GAME_LOG_BATCH"] = ["1"]
    gate.verify_game_log_batch_compile_scope(records, log_continue_active)
    log_crt_definitions["ISAAC_VITA_GAME_LOG_BATCH"] = ["0"]
    must_fail(
        "non-canonical game-log-batch owner definition",
        lambda: gate.verify_game_log_batch_compile_scope(
            records, log_continue_active
        ),
        "non-canonical game-log-batch definition",
    )
    log_crt_definitions.pop("ISAAC_VITA_GAME_LOG_BATCH")
    log_continue_definitions.pop("ISAAC_VITA_GAME_LOG_BATCH")
    gate.verify_game_log_async_compile_scope(records, cache)
    missing_log_async = dict(cache)
    del missing_log_async["ISAAC_VITA_GAME_LOG_ASYNC"]
    must_fail(
        "missing game-log-async selection",
        lambda: gate.verify_game_log_async_compile_scope(
            records, missing_log_async
        ),
        "CMake cache is missing ISAAC_VITA_GAME_LOG_ASYNC",
    )
    must_fail(
        "malformed game-log-async selection",
        lambda: gate.verify_game_log_async_compile_scope(
            records,
            {**cache, "ISAAC_VITA_GAME_LOG_ASYNC": "AUTO"},
        ),
        "is not ON/OFF",
    )
    log_async_active = {**cache, "ISAAC_VITA_GAME_LOG_ASYNC": "ON"}
    must_fail(
        "game-log-async active without owner definitions",
        lambda: gate.verify_game_log_async_compile_scope(
            records, log_async_active
        ),
        "scope changed",
    )
    log_async_owner_definitions: list[dict[str, object]] = []
    for owner_name in sorted(gate.GAME_LOG_ASYNC_DEFINITION_OWNERS):
        owner = next(
            record for record in records.values()
            if isinstance(record["source"], Path) and
            record["source"].name == owner_name
        )
        owner_definitions = owner["definitions"]
        assert isinstance(owner_definitions, dict)
        owner_definitions["ISAAC_VITA_GAME_LOG_ASYNC"] = ["1"]
        log_async_owner_definitions.append(owner_definitions)
    gate.verify_game_log_async_compile_scope(records, log_async_active)
    must_fail(
        "game-log-async owner definitions while the knob is OFF",
        lambda: gate.verify_game_log_async_compile_scope(records, cache),
        "scope changed",
    )
    log_async_owner_definitions[0].pop("ISAAC_VITA_GAME_LOG_ASYNC")
    must_fail(
        "missing game-log-async owner definition",
        lambda: gate.verify_game_log_async_compile_scope(
            records, log_async_active
        ),
        "scope changed",
    )
    log_async_owner_definitions[0]["ISAAC_VITA_GAME_LOG_ASYNC"] = ["0"]
    must_fail(
        "non-canonical game-log-async owner definition",
        lambda: gate.verify_game_log_async_compile_scope(
            records, log_async_active
        ),
        "non-canonical game-log-async definition",
    )
    for owner_definitions in log_async_owner_definitions:
        owner_definitions.pop("ISAAC_VITA_GAME_LOG_ASYNC")
    gate.verify_game_log_async_compile_scope(records, cache)
    missing_exit_menu = dict(cache)
    del missing_exit_menu["ISAAC_VITA_EXIT_MENU_PROFILE"]
    must_fail(
        "missing Exit-menu compile selection",
        lambda: gate.verify_exit_menu_profile_compile_scope(
            records, missing_exit_menu
        ),
        "CMake cache is missing ISAAC_VITA_EXIT_MENU_PROFILE",
    )
    must_fail(
        "malformed Exit-menu compile selection",
        lambda: gate.verify_exit_menu_profile_compile_scope(
            records,
            {**cache, "ISAAC_VITA_EXIT_MENU_PROFILE": "AUTO"},
        ),
        "is not ON/OFF",
    )
    exit_active = {**cache, "ISAAC_VITA_EXIT_MENU_PROFILE": "ON"}
    exit_runtime = next(
        record for record in records.values()
        if isinstance(record["source"], Path) and
        record["source"].name == "kage_vita_exit_menu_profile.c"
    )
    exit_runtime_definitions = exit_runtime["definitions"]
    assert isinstance(exit_runtime_definitions, dict)
    exit_guest_source = display_guest["source"]
    assert isinstance(exit_guest_source, Path)
    exit_guest_source.parent.mkdir(parents=True, exist_ok=True)
    exit_guest_source.write_text(
        "\n".join(
            f"void {name}(CPU *__restrict c){{(void)c;}}"
            for name in (
                "sub_0025b340", "sub_004b0010", "sub_004b43d0",
                "sub_004b4500", "sub_004b47a0",
            )
        ) + "\n",
        encoding="utf-8",
    )
    for definitions in (
            exit_runtime_definitions, log_crt_definitions,
            typed_guest_definitions):
        definitions["ISAAC_VITA_EXIT_MENU_PROFILE"] = ["1"]
    gate.verify_exit_menu_profile_compile_scope(records, exit_active)
    exit_runtime_definitions["ISAAC_VITA_EXIT_MENU_PROFILE"] = ["0"]
    must_fail(
        "non-canonical Exit-menu owner definition",
        lambda: gate.verify_exit_menu_profile_compile_scope(
            records, exit_active
        ),
        "non-canonical Exit-menu definition",
    )
    exit_runtime_definitions["ISAAC_VITA_EXIT_MENU_PROFILE"] = ["1"]
    log_continue_definitions["ISAAC_VITA_EXIT_MENU_PROFILE"] = ["1"]
    must_fail(
        "Exit-menu definition leaked into foreign owner",
        lambda: gate.verify_exit_menu_profile_compile_scope(
            records, exit_active
        ),
        "scope changed",
    )
    log_continue_definitions.pop("ISAAC_VITA_EXIT_MENU_PROFILE")
    for definitions in (
            exit_runtime_definitions, log_crt_definitions,
            typed_guest_definitions):
        definitions.pop("ISAAC_VITA_EXIT_MENU_PROFILE")
    gate.verify_exit_menu_profile_compile_scope(records, cache)
    missing_stock = dict(cache)
    del missing_stock["ISAAC_VITA_VITAGL_STOCK_REFERENCE"]
    must_fail(
        "missing stock-reference cache selection",
        lambda: gate.verify_stock_reference_compile_scope(
            records, missing_stock
        ),
        "CMake cache is missing ISAAC_VITA_VITAGL_STOCK_REFERENCE",
    )
    must_fail(
        "malformed stock-reference cache selection",
        lambda: gate.verify_stock_reference_compile_scope(
            records,
            {**cache, "ISAAC_VITA_VITAGL_STOCK_REFERENCE": "AUTO"},
        ),
        "is not ON/OFF",
    )
    for record in records.values():
        source = record["source"]
        assert isinstance(source, Path)
        if source.name in owners:
            definitions = record["definitions"]
            assert isinstance(definitions, dict)
            definitions["ISAAC_VITA_DIRECT_DEFAULT"] = ["1"]
    gate.verify_direct_default_compile_scope(records, active)

    first_owner = next(
        record for record in records.values()
        if isinstance(record["source"], Path) and
        record["source"].name in owners
    )
    definitions = first_owner["definitions"]
    assert isinstance(definitions, dict)
    definitions.pop("ISAAC_VITA_DIRECT_DEFAULT")
    must_fail(
        "missing direct-default owner definition",
        lambda: gate.verify_direct_default_compile_scope(records, active),
        "scope changed",
    )
    definitions["ISAAC_VITA_DIRECT_DEFAULT"] = ["1"]
    definitions["ISAAC_VITA_DIRECT_DEFAULT"] = ["0"]
    must_fail(
        "non-canonical direct-default owner definition",
        lambda: gate.verify_direct_default_compile_scope(records, active),
        "non-canonical direct-default definition",
    )
    definitions["ISAAC_VITA_DIRECT_DEFAULT"] = ["1"]
    guest_record = next(
        record for record in records.values()
        if isinstance(record["source"], Path) and
        record["source"].name == gate.DIRECT_DEFAULT_SOURCE_NAME
    )
    guest_definitions = guest_record["definitions"]
    assert isinstance(guest_definitions, dict)
    guest_definitions["ISAAC_VITA_DIRECT_DEFAULT"] = ["1"]
    must_fail(
        "direct-default definition leaked into derived guest",
        lambda: gate.verify_direct_default_compile_scope(records, active),
        "scope changed",
    )
    guest_definitions.clear()
    must_fail(
        "direct-default without first-frame probe",
        lambda: gate.verify_direct_default_compile_scope(
            records,
            {**active, "ISAAC_VITA_FIRST_FRAME_PROBE": "OFF"},
        ),
        "requires ISAAC_VITA_FIRST_FRAME_PROBE=ON",
    )

    stock_active = {
        **active,
        "ISAAC_VITA_FIRST_FRAME_PROBE": "OFF",
        "ISAAC_VITA_VITAGL_STOCK_REFERENCE": "ON",
    }
    for record in records.values():
        definitions = record["definitions"]
        source = record["source"]
        assert isinstance(definitions, dict)
        assert isinstance(source, Path)
        definitions.pop("ISAAC_VITA_DIRECT_DEFAULT", None)
        if source.name in gate.STOCK_REFERENCE_DEFINITION_OWNERS:
            definitions["ISAAC_VITA_VITAGL_STOCK_REFERENCE"] = ["1"]
    gate.verify_direct_default_compile_scope(records, stock_active)
    gate.verify_stock_reference_compile_scope(records, stock_active)
    must_fail(
        "stock reference without direct-default",
        lambda: gate.verify_direct_default_compile_scope(
            records,
            {**stock_active, "ISAAC_VITA_DIRECT_DEFAULT": "OFF"},
        ),
        "requires direct-default",
    )
    must_fail(
        "stock reference with first-frame probe",
        lambda: gate.verify_direct_default_compile_scope(
            records,
            {**stock_active, "ISAAC_VITA_FIRST_FRAME_PROBE": "ON"},
        ),
        "forbids first-frame probe",
    )
    stock_owner = next(
        record for record in records.values()
        if isinstance(record["source"], Path) and
        record["source"].name in gate.STOCK_REFERENCE_DEFINITION_OWNERS
    )
    stock_definitions = stock_owner["definitions"]
    assert isinstance(stock_definitions, dict)
    stock_definitions.pop("ISAAC_VITA_VITAGL_STOCK_REFERENCE")
    must_fail(
        "missing stock-reference owner definition",
        lambda: gate.verify_stock_reference_compile_scope(
            records, stock_active
        ),
        "scope changed",
    )


def _cmake_external_object(build: Path, source: Path) -> Path:
    # CMake embeds an external source's absolute path below ``<target>.dir``.
    # Remove only host path syntax that cannot be a Windows filename; retain all
    # root components so this fixture catches accidental semantic path leakage.
    parts = [
        part for part in source.resolve().as_posix().replace(":", "").split("/")
        if part
    ]
    path = build / "CMakeFiles" / f"{gate.TARGET}.dir" / Path(*parts)
    return path.with_name(path.name + ".obj").resolve()


def _logical_closure_fixture(
    root: Path, *, source_dir: str, generated_dir: str,
) -> tuple[
    Path, Path, dict[Path, dict[str, object]],
    dict[Path, collections.Counter[gate.RelocKey]], list[Path],
]:
    source_root = (root / source_dir).resolve()
    generated_root = (root / generated_dir).resolve()
    build = (root / "build [external-objects]").resolve()
    logical_sources = (
        "recomp/runtime/guest.c",
        "recomp/runtime/host_vita_heap.c",
        "generated/guest_0000.c",
    )
    physical_sources = {
        logical: (
            generated_root / logical.removeprefix("generated/")
            if logical.startswith("generated/")
            else source_root / logical
        ).resolve()
        for logical in logical_sources
    }
    records: dict[Path, dict[str, object]] = {}
    raw: dict[Path, collections.Counter[gate.RelocKey]] = {}
    for logical in reversed(logical_sources):
        source = physical_sources[logical]
        source.parent.mkdir(parents=True, exist_ok=True)
        source.write_text("int fixture;\n", encoding="ascii")
        obj = _cmake_external_object(build, source)
        obj.parent.mkdir(parents=True, exist_ok=True)
        obj.write_bytes(b"object fixture\n")
        records[source] = {
            "source": source,
            "object": obj,
            "exempt": logical != "generated/guest_0000.c",
        }
        raw[source] = collections.Counter()
    raw[physical_sources["recomp/runtime/guest.c"]][
        ("guest_owner", "malloc", "R_ARM_THM_CALL")
    ] = 1
    link_order = (
        "recomp/runtime/host_vita_heap.c",
        "generated/guest_0000.c",
        "recomp/runtime/guest.c",
    )
    linked: list[Path] = []
    for logical in link_order:
        value = records[physical_sources[logical]]["object"]
        assert isinstance(value, Path)
        linked.append(value)
    return source_root, generated_root, records, raw, linked


def logical_closure_tests(root: Path) -> None:
    # The physical source/generated ordering is deliberately reversed between
    # roots, and names include spaces plus regex metacharacters.
    fixture_a = _logical_closure_fixture(
        root / "relocated A [short].+", source_dir="z source (runtime)$",
        generated_dir="a generated [code]^",
    )
    fixture_b = _logical_closure_fixture(
        root / "relocated B much longer [root](with spaces).+^$",
        source_dir="a source [runtime]^", generated_dir="z generated (code)$",
    )
    fixture_nested = _logical_closure_fixture(
        root / "relocated C nested [root].+^$",
        source_dir="source root [owner]^",
        generated_dir="source root [owner]^/build/gen (inside)$",
    )

    closures: list[dict[str, object]] = []
    physical_orders: list[list[str]] = []
    for source_root, generated_root, records, raw, linked in (
        fixture_a, fixture_b, fixture_nested
    ):
        physical_orders.append([
            gate._logical_source(source, source_root, generated_root)
            for source in sorted(records)
        ])
        for source, record in records.items():
            obj = record["object"]
            assert isinstance(obj, Path)
            owner = (generated_root if source.is_relative_to(generated_root)
                     else source_root)
            if owner.name not in obj.parts:
                raise AssertionError(
                    "CMake-shaped fixture lost its absolute source root"
                )
            embedded_source = source.as_posix().replace(":", "").lstrip("/")
            if not obj.as_posix().endswith(f"/{embedded_source}.obj"):
                raise AssertionError(
                    "CMake-shaped fixture does not contain its full source path"
                )
        closure = gate.logical_closure(
            records, raw, linked, source_root, generated_root
        )
        payload = gate._canonical_bytes(closure)
        for physical_root in (source_root, generated_root):
            root_spellings = {
                physical_root.as_posix(),
                physical_root.as_posix().replace(":", "").lstrip("/"),
            }
            if any(value.encode("utf-8") in payload for value in root_spellings):
                raise AssertionError("physical root leaked into logical closure")
        closure_sources = closure["sources"]
        assert isinstance(closure_sources, list)
        for row in closure_sources:
            if row["object"] != gate._logical_object(row["source"]):
                raise AssertionError("logical object is not derived from its source")
        closures.append(closure)

    if physical_orders[0] == physical_orders[1]:
        raise AssertionError("fixture failed to reverse physical source ordering")
    if any(value != closures[0] for value in closures[1:]):
        raise AssertionError("relocated roots changed the logical closure")
    closure_hashes = [
        hashlib.sha256(gate._canonical_bytes(value)).hexdigest()
        for value in closures
    ]
    if any(value != closure_hashes[0] for value in closure_hashes[1:]):
        raise AssertionError("relocated roots changed the logical closure hash")

    expected_sources = sorted((
        "generated/guest_0000.c",
        "recomp/runtime/guest.c",
        "recomp/runtime/host_vita_heap.c",
    ))
    first_sources = closures[0]["sources"]
    nested_closure_sources = closures[2]["sources"]
    assert isinstance(first_sources, list)
    assert isinstance(nested_closure_sources, list)
    actual_sources = [row["source"] for row in first_sources]
    if actual_sources != expected_sources:
        raise AssertionError("logical source rows are not canonically ordered")
    nested_sources = [row["source"] for row in nested_closure_sources]
    if nested_sources[0] != "generated/guest_0000.c":
        raise AssertionError("nested generated root lost its logical prefix")
    expected_linked = [
        "objects/recomp/runtime/host_vita_heap.c.obj",
        "objects/generated/guest_0000.c.obj",
        "objects/recomp/runtime/guest.c.obj",
    ]
    if closures[0]["linked_objects"] != expected_linked:
        raise AssertionError("physical link order was not translated exactly")

    source_root, generated_root, records, raw, linked = fixture_a
    canonical = next(
        source for source in records if source.name == "guest_0000.c"
    )
    override = (root / "detached generated override" / "guest_0000.c").resolve()
    override.parent.mkdir(parents=True)
    override.write_text("int fixture;\n", encoding="ascii")
    aliased_records = dict(records)
    aliased_record = dict(aliased_records.pop(canonical))
    aliased_record["source"] = override
    aliased_records[override] = aliased_record
    aliased_raw = dict(raw)
    aliased_raw[override] = aliased_raw.pop(canonical)
    aliased = gate.logical_closure(
        aliased_records,
        aliased_raw,
        linked,
        source_root,
        generated_root,
        {override: "generated/guest_0000.c"},
    )
    if aliased != closures[0]:
        raise AssertionError("physical generated override changed logical closure")
    must_fail(
        "unaliased detached generated override",
        lambda: gate.logical_closure(
            aliased_records, aliased_raw, linked, source_root, generated_root
        ),
        "no logical closure owner",
    )
    must_fail(
        "malformed generated source alias",
        lambda: gate.logical_closure(
            aliased_records,
            aliased_raw,
            linked,
            source_root,
            generated_root,
            {override: "generated/not-a-guest.c"},
        ),
        "malformed logical path",
    )
    unknown_alias = (root / "unknown alias" / "guest_0001.c").resolve()
    must_fail(
        "unknown generated source alias",
        lambda: gate.logical_closure(
            aliased_records,
            aliased_raw,
            linked,
            source_root,
            generated_root,
            {
                override: "generated/guest_0000.c",
                unknown_alias: "generated/guest_0001.c",
            },
        ),
        "no compile-source mapping",
    )

    boundary_root = (root / "logical ownership boundary").resolve()
    boundary_source = (boundary_root / "source").resolve()
    boundary_generated = (boundary_source / "build" / "generated").resolve()
    if gate._logical_source(
        boundary_source / "recomp" / "runtime" / "owner.c",
        boundary_source, boundary_generated,
    ) != "recomp/runtime/owner.c":
        raise AssertionError("exact source-root ownership changed")
    if gate._logical_source(
        boundary_generated / "guest_0001.c",
        boundary_source, boundary_generated,
    ) != "generated/guest_0001.c":
        raise AssertionError("longest nested generated-root ownership changed")
    if gate._logical_source(
        boundary_source / "build" / "generated-sibling" / "owner.c",
        boundary_source, boundary_generated,
    ) != "build/generated-sibling/owner.c":
        raise AssertionError("generated-root sibling crossed a path boundary")
    must_fail(
        "source-root sibling ownership",
        lambda: gate._logical_source(
            boundary_root / "source-sibling" / "owner.c",
            boundary_source, boundary_generated,
        ),
        "no logical closure owner",
    )
    must_fail(
        "duplicate logical roots",
        lambda: gate._logical_source(
            boundary_source / "owner.c", boundary_source, boundary_source
        ),
        "resolve to the same directory",
    )

    reversed_link = gate.logical_closure(
        records, raw, list(reversed(linked)), source_root, generated_root
    )
    if (reversed_link["linked_objects"] == closures[0]["linked_objects"] or
            hashlib.sha256(gate._canonical_bytes(reversed_link)).digest() ==
            hashlib.sha256(gate._canonical_bytes(closures[0])).digest()):
        raise AssertionError("link-order mutation was not observable")

    unknown = (root / "unknown build" / "unknown.c.obj").resolve()
    must_fail(
        "unknown logical linked object",
        lambda: gate.logical_closure(
            records, raw, [*linked[:-1], unknown], source_root, generated_root
        ),
        "no compile-source mapping",
    )
    must_fail(
        "duplicate logical linked object",
        lambda: gate.logical_closure(
            records, raw, [linked[0], linked[0], *linked[2:]],
            source_root, generated_root,
        ),
        "duplicate physical linked object",
    )
    must_fail(
        "missing logical linked object",
        lambda: gate.logical_closure(
            records, raw, linked[:-1], source_root, generated_root
        ),
        "missing compiled objects",
    )

    original_logical_source = gate._logical_source
    try:
        gate._logical_source = lambda *unused: "recomp/runtime/duplicate.c"
        must_fail(
            "duplicate logical compile source",
            lambda: gate.logical_closure(
                records, raw, linked, source_root, generated_root
            ),
            "duplicate logical compile source",
        )
    finally:
        gate._logical_source = original_logical_source

    original_logical_object = gate._logical_object
    try:
        gate._logical_object = lambda unused: "objects/duplicate.c.obj"
        must_fail(
            "duplicate logical compile object",
            lambda: gate.logical_closure(
                records, raw, linked, source_root, generated_root
            ),
            "duplicate logical compile object",
        )
    finally:
        gate._logical_object = original_logical_object


def relocation_tests() -> None:
    fixture = """
Relocation section '.rel.text.owner' at offset 0x10 contains 2 entries:
 Offset     Info    Type                Sym. Value  Symbol's Name
00000002  0000010a R_ARM_THM_CALL         00000000   malloc
00000008  0000022f R_ARM_THM_MOVW_ABS_NC  00000000   free
"""
    actual = gate.parse_raw_relocations(fixture)
    expected = gate._counter((
        (1, "owner", "malloc", "R_ARM_THM_CALL"),
        (1, "owner", "free", "R_ARM_THM_MOVW_ABS_NC"),
    ))
    if actual != expected:
        raise AssertionError("raw relocation parser lost call/address records")
    extra = actual.copy()
    extra[("owner", "free", "R_ARM_THM_MOVW_ABS_NC")] += 1
    if not (extra - expected):
        raise AssertionError("extra exempt relocation was not observable")
    wrong = actual.copy()
    del wrong[("owner", "malloc", "R_ARM_THM_CALL")]
    wrong[("wrong_owner", "malloc", "R_ARM_THM_CALL")] += 1
    if not (expected - wrong) or not (wrong - expected):
        raise AssertionError("wrong exempt function relocation was not observable")
    guest_record = {
        "source": Path("runtime") / "guest.c",
        "exempt": True,
        "definitions": {},
    }
    guest_expected = gate.EXPECTED_FIXED["guest.c"].copy()
    guest_extra = guest_expected.copy()
    guest_extra[("guest_fs_base", "calloc", "R_ARM_THM_CALL")] += 1
    must_fail(
        "extra exempt call",
        lambda: gate.verify_record_relocations(guest_record, guest_extra),
        "extra=",
    )
    guest_wrong = guest_expected.copy()
    guest_wrong[("guest_fs_base", "calloc", "R_ARM_THM_CALL")] -= 1
    guest_wrong[("wrong_function", "calloc", "R_ARM_THM_CALL")] += 1
    must_fail(
        "wrong exempt function",
        lambda: gate.verify_record_relocations(guest_record, guest_wrong),
        "missing=",
    )
    # ISAAC_VITA_GUEST_STACK_MEMBLOCK: the guest.c record carrying the define
    # expects the canonical table minus the three newlib stack edges, and
    # the OFF record keeps them; each table rejects the other's object.
    newlib_stack = gate.GUEST_STACK_NEWLIB_RELOCATIONS
    if sum(newlib_stack.values()) != 3 or not (newlib_stack <= guest_expected):
        raise AssertionError(
            "GUEST_STACK_NEWLIB_RELOCATIONS must be three rows of the guest.c table"
        )
    if gate.expected_relocations(guest_record) != guest_expected:
        raise AssertionError("guest.c OFF record lost its canonical table")
    guest_on_record = {
        "source": Path("runtime") / "guest.c",
        "exempt": True,
        "definitions": {"ISAAC_VITA_GUEST_STACK_MEMBLOCK": ["1"]},
    }
    guest_on_expected = gate.expected_relocations(guest_on_record)
    if guest_on_expected != guest_expected - newlib_stack:
        raise AssertionError(
            "guest.c ON record must drop exactly the newlib stack rows"
        )
    if any(function in ("guest_stack_init", "guest_stack_free")
           for function, unused_symbol, unused_kind in guest_on_expected):
        raise AssertionError("guest.c ON table still names a stack owner")
    gate.verify_record_relocations(guest_on_record, guest_on_expected)
    must_fail(
        "guest stack memblock ON object keeps newlib stack edges",
        lambda: gate.verify_record_relocations(guest_on_record, guest_expected),
        "extra=",
    )
    must_fail(
        "guest stack memblock OFF object lost newlib stack edges",
        lambda: gate.verify_record_relocations(guest_record, guest_on_expected),
        "missing=",
    )
    if gate.expected_relocations({
            "source": Path("runtime") / "guest.c",
            "exempt": True,
            "definitions": {"ISAAC_VITA_GUEST_STACK_MEMBLOCK": ["0"]},
    }) != guest_expected:
        raise AssertionError("only the canonical =1 definition selects the ON table")
    must_fail(
        "ordinary object raw relocation",
        lambda: gate.verify_record_relocations(
            {"source": Path("runtime") / "manual_portable.c", "exempt": False},
            gate._counter(((1, "bad", "malloc", "R_ARM_THM_CALL"),)),
        ),
        "ordinary source retained",
    )
    must_fail(
        "non-function allocator address",
        lambda: gate.parse_raw_relocations(
            "Relocation section '.rel.data.fp' at offset 0x10 contains 1 entry:\n"
            "00000000  00000102 R_ARM_ABS32 00000000 malloc\n"
        ),
        "outside a function section",
    )

    heap_canonical = {
        "source": Path("runtime") / "host_vita_heap.c",
        "exempt": True,
        "definitions": {
            name: ["1"] for name in gate.HEAP_CANONICAL_DEFINES
        },
    }
    gate.verify_record_relocations(
        heap_canonical, gate.EXPECTED_HEAP_CANONICAL.copy(),
        gate.PROFILE_CANONICAL,
    )
    # The pool-size STRING knob (ISAAC_VITA_HEAP_OVERFLOW_MSPACE_BYTES) is
    # not a feature define: a non-default token on host_vita_heap.c is part
    # of the receipt closure but leaves the allocator profile canonical.
    heap_canonical_pool = {
        "source": Path("runtime") / "host_vita_heap.c",
        "exempt": True,
        "definitions": {
            **{name: ["1"] for name in gate.HEAP_CANONICAL_DEFINES},
            "ISAAC_VITA_HEAP_OVERFLOW_MSPACE_BYTES": ["0x017fd000U"],
        },
    }
    if gate.heap_profile(heap_canonical_pool) != gate.PROFILE_CANONICAL:
        raise AssertionError(
            "pool-size knob token changed the host_vita_heap.c profile"
        )
    canonical_missing = gate.EXPECTED_HEAP_CANONICAL.copy()
    canonical_missing[
        ("vita_heap_guest_free_impl", "free", "R_ARM_THM_CALL")
    ] -= 1
    must_fail(
        "canonical heap missing call",
        lambda: gate.verify_record_relocations(
            heap_canonical, canonical_missing, gate.PROFILE_CANONICAL
        ),
        "missing=",
    )

    heap_diagnostic = {
        "source": Path("runtime") / "host_vita_heap.c",
        "exempt": True,
        "definitions": {"ISAAC_VITA_OGG_EMERGENCY": ["1"]},
    }
    legitimate_subset = gate._counter((
        (1, "vita_heap_guest_malloc_with_reason", "malloc",
         "R_ARM_THM_CALL"),
        (1, "vita_heap_ledger_reserve.constprop.7", "free",
         "R_ARM_THM_CALL"),
    ))
    gate.verify_record_relocations(
        heap_diagnostic, legitimate_subset, gate.PROFILE_DIAGNOSTIC
    )
    hostile_diagnostic = {
        "new function": gate._counter((
            (1, "new_owner", "malloc", "R_ARM_THM_CALL"),
        )),
        "wrong symbol": gate._counter((
            (1, "vita_heap_guest_malloc_with_reason", "calloc",
             "R_ARM_THM_CALL"),
        )),
        "wrong relocation type": gate._counter((
            (1, "vita_heap_guest_malloc_with_reason", "malloc",
             "R_ARM_ABS32"),
        )),
        "per-key count overflow": gate._counter((
            (3, "vita_heap_guest_malloc_with_reason", "free",
             "R_ARM_THM_CALL"),
        )),
    }
    for label, hostile in hostile_diagnostic.items():
        must_fail(
            "diagnostic heap " + label,
            lambda hostile=hostile: gate.verify_record_relocations(
                heap_diagnostic, hostile, gate.PROFILE_DIAGNOSTIC
            ),
            "allowlist changed",
        )
    mutually_exclusive = gate._counter((
        (1, "vita_heap_ledger_rehash", "free", "R_ARM_THM_CALL"),
        (1, "vita_heap_ledger_reserve", "free", "R_ARM_THM_CALL"),
    ))
    must_fail(
        "diagnostic mutually-exclusive owners",
        lambda: gate.verify_record_relocations(
            heap_diagnostic, mutually_exclusive, gate.PROFILE_DIAGNOSTIC
        ),
        "mutually-exclusive",
    )
    must_fail(
        "diagnostic receipt profile on canonical heap",
        lambda: gate.verify_record_relocations(
            heap_canonical, gate.EXPECTED_HEAP_CANONICAL.copy(),
            gate.PROFILE_DIAGNOSTIC,
        ),
        "profile disagrees",
    )
    malformed_features = dict(heap_diagnostic)
    malformed_features["definitions"] = {
        "ISAAC_VITA_HEAP_LEDGER_MEMBLOCK": ["0"]
    }
    must_fail(
        "non-canonical heap feature token",
        lambda: gate.heap_profile(malformed_features),
        "non-canonical feature definition",
    )


def census_depfile_tests() -> None:
    parsed = census._parse_depfile_text(
        "obj.o: /sdk/stdlib.h relative\\ path.h \\\n"
        " /repo/host_vita_heap.h\n"
    )
    if parsed != [
        "/sdk/stdlib.h", "relative path.h", "/repo/host_vita_heap.h"
    ]:
        raise AssertionError(f"census depfile parser changed: {parsed}")
    stripped = census._without_dependency_flags([
        "cc", "-MD", "-MF", "old.d", "-MTold.o", "-MQ", "quoted.o",
        "-c", "heap.c",
    ])
    if stripped != ["cc", "-c", "heap.c"]:
        raise AssertionError(f"dependency option stripping changed: {stripped}")
    for label, hostile in (
        ("missing rule", "no-colon\n"),
        ("multiple rules", "a: one.h\nb: two.h\n"),
        ("empty dependency closure", "a:\n"),
    ):
        try:
            census._parse_depfile_text(hostile)
        except census.CensusError:
            pass
        else:
            raise AssertionError(f"hostile census depfile passed: {label}")


def main() -> int:
    here = Path(__file__).resolve().parent
    poison = here / "isaac_vita_raw_allocator_poison.h"
    with tempfile.TemporaryDirectory(prefix="isaac-vita-raw-alloc-gate-") as value:
        root = Path(value)
        poison_tests(root, poison)
        closure_tests(root, poison)
        expected_source_tests(root)
        native_inflate_tests(root, poison)
        png_decode_profile_scope_tests(root)
        png_native_unfilter_scope_tests(root)
        png_crc32_fastpath_scope_tests(root)
        save_reader_fastpath_scope_tests(root)
        guest_stack_memblock_scope_tests(root)
        audio_stream_receipt_scope_tests(root)
        sim_cadence_receipt_scope_tests(root)
        stable30_scope_tests(root)
        runtime_settings_scope_tests(root)
        manager_settings_ui_scope_tests(root)
        manager_ui2_game_skin_scope_tests(root)
        manager_trophy_import_scope_tests(root)
        manager_debug_overlay_row_scope_tests(root)
        world_seam_diag_scope_tests(root)
        direct_default_tests(root)
        logical_closure_tests(root)
        relocation_tests()
        census_depfile_tests()
    print(
        "Vita raw allocator gate hostile self-test: PASS; "
        "all 7 names/calls/address-taking/feature-view/external-leak/"
        "physical-and-logical-closure/path-relocation/nested-root-boundary/"
        "link-order/feature-source-inventory-and-bundle3-all-on-closure/"
        "static-sfx-profile-source-and-requirements/"
        "phase-profile-fullspeed-and-"
        "guest-cache-sync-import-display-raster-and-typed-state-source-policy/"
        "texture-churn-source-and-definition-scope/"
        "audio-stream-receipt-cache-and-definition-scope/"
        "simulation-cadence-source-build-ID-and-definition-scope/"
        "stable30-source-build-ID-and-definition-scope/"
        "runtime-settings-source-owner-and-nested-scheduler-scope/"
        "manager-settings-page-zero-eboot-source-and-definition-scope/"
        "manager-game-skin-zero-eboot-unit-and-definition-scope/"
        "manager-trophy-import-zero-eboot-definition-scope/"
        "manager-debug-overlay-row-zero-eboot-definition-scope/"
        "world-seam-source-build-ID-and-definition-scope/"
        "png-decode-profile-source-and-definition-scope/"
        "png-native-unfilter-source-and-definition-scope/"
        "png-crc32-source-and-definition-scope/"
        "save-reader-source-and-definition-scope/"
        "guest-stack-memblock-source-rows-and-definition-scope/"
        "exit-menu-profile-source-and-definition-scope/"
        "direct-default-override-and-definition-scope/"
        "native-inflate-units-and-prefix-includes/"
        "release-sub-knob-unit-inventory/"
        "exact-and-diagnostic-exempt-relocations/depfiles rejected"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
