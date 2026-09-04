"""Host oracle for the direct IAT import dispatch (ISAAC_VITA_IAT_DIRECT).

Builds runtime/host_vita_import_direct_oracle.c (which includes guest.c) with
the production host_vita_import_id.c under MSVC, in the audio-off, audio-on,
lua-on and lua-on + ISAAC_VITA_LUA_IMPORT_FASTDISPATCH family inventories plus
the ISAAC_VITA_PROFILE_IMPORT_KINDS census (the per-import array both routes
must note identically), and runs it.  Also proves the default-OFF scopes:
guest.c preprocessed without ISAAC_VITA_IMPORT_DIRECT keeps no direct table
and no guest_import_call, guest.h without GUEST_IMPORT_DIRECT spells the site
tokens as the plain guest_call, and host_vita_import_id.c without the Lua knob
keeps no typed binding.
"""

from __future__ import annotations

from pathlib import Path
import re
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import build_all as BA  # noqa: E402

RUNTIME = HERE / "runtime"
PASS_MARKER = "Vita IAT direct dispatch oracle: PASS"


def run(command, env):
    completed = subprocess.run(command, env=env, capture_output=True)
    if completed.returncode != 0:
        sys.stderr.write(completed.stdout.decode("cp866", errors="replace"))
        sys.stderr.write(completed.stderr.decode("cp866", errors="replace"))
        raise AssertionError(f"command failed: {command[0]} ...")
    return completed


def verify_header_scope() -> None:
    header = (RUNTIME / "guest.h").read_text(encoding="utf-8")
    # Join the header's line continuations so the spelling is matched whole.
    header = re.sub(" {2,}", " ", header.replace(chr(92) + chr(10), " "))
    off_block = re.search(
        r"#else\n#define GUEST_IMPORT_CALL\(target, slot_rva, import_id\) "
        r"guest_call\(c, \(target\)\)\n"
        r"#define GUEST_IMPORT_JMP\(target, slot_rva, import_id\) "
        r"guest_call\(c, \(target\)\)\n#endif", header)
    if off_block is None:
        raise AssertionError("guest.h lost the GUEST_IMPORT_DIRECT=0 spelling")
    on_block = re.search(
        r"#if GUEST_IMPORT_DIRECT\n#define GUEST_IMPORT_CALL\(target, slot_rva, "
        r"import_id\) guest_import_call\(c, \(target\), \(import_id\)\)\n"
        r"#define GUEST_IMPORT_JMP\(target, slot_rva, import_id\) "
        r"guest_import_call\(c, \(target\), \(import_id\)\)\n", header)
    if on_block is None:
        raise AssertionError("guest.h lost the GUEST_IMPORT_DIRECT=1 spelling")


def verify_build_gate() -> None:
    cmake = (HERE / "vita" / "CMakeLists.txt").read_text(encoding="utf-8")
    if 'option(ISAAC_VITA_IAT_DIRECT' not in cmake:
        raise AssertionError("CMake option ISAAC_VITA_IAT_DIRECT is missing")
    if len(re.findall(r"COMPILE_DEFINITIONS\s+ISAAC_VITA_IMPORT_DIRECT=1\)",
                      cmake)) != 1:
        raise AssertionError("guest.c must receive ISAAC_VITA_IMPORT_DIRECT=1 "
                             "exactly once")
    if len(re.findall(r"COMPILE_DEFINITIONS\s+GUEST_IMPORT_DIRECT=1\)",
                      cmake)) != 1:
        raise AssertionError("generated units must receive GUEST_IMPORT_DIRECT=1"
                             " exactly once")
    option = re.search(r'option\(ISAAC_VITA_IAT_DIRECT\s+"[^"]*"\s+(ON|OFF)\)',
                       cmake)
    if option is None or option.group(1) != "OFF":
        raise AssertionError("ISAAC_VITA_IAT_DIRECT must default OFF until the "
                             "device A/B is in")


def main() -> int:
    verify_header_scope()
    verify_build_gate()
    env, compiler = BA.msvc_env()
    common = [
        compiler, "/nologo", "/W4", "/WX", "/O2", "/Gy", "/std:c11",
        "/wd4310", "/wd4702", "/wd4996",
        "/DISAAC_VITA_IMPORT_ID_DISPATCH=1",
        "/DISAAC_VITA_IMPORT_DIRECT=1",
        "/DISAAC_VITA_GUEST_LOOKUP_CACHE=1",
        "/DISAAC_VITA_PHASE_PROFILE=1",
        "/DGUEST_IMAGE_BASE=0x98000000u",
        "/I", str(RUNTIME),
    ]
    with tempfile.TemporaryDirectory(prefix="isaac-vita-iat-direct-") as tmp:
        root = Path(tmp)

        preprocessed = root / "guest-iat-direct-off.i"
        run([compiler, "/nologo", "/P", "/std:c11", "/wd4310", "/wd4996",
             "/DISAAC_VITA_IMPORT_ID_DISPATCH=1", "/I", str(RUNTIME),
             str(RUNTIME / "guest.c"), "/Fi:" + str(preprocessed)], env)
        off_text = preprocessed.read_text(encoding="utf-8", errors="replace")
        # guest.h keeps the prototype (a generated unit with
        # GUEST_IMPORT_DIRECT=1 names it); the table, its rebuild hook and
        # the definition body must be gone.
        leaked = [token for token in ("g_guest_import_direct",
                                      "guest_import_direct_rebuild",
                                      "entry->fn(c, entry->local_index")
                  if token in off_text]
        if leaked:
            raise AssertionError(f"default-OFF guest.c retained the direct "
                                 f"table: {leaked}")

        # ISAAC_VITA_LUA_IMPORT_FASTDISPATCH (host_vita_import_id.c binds the
        # thirteen typed Lua endpoints): preprocessed without the knob the
        # binder keeps no typed-table reference, and the binding hook must sit
        # in both routes (guest_host_import_id, ..._direct_binding).
        preprocessed = root / "import-id-lua-fast-off.i"
        run([compiler, "/nologo", "/P", "/std:c11", "/wd4310", "/wd4996",
             "/DISAAC_VITA_LUA=1", "/DISAAC_VITA_AUDIO=1",
             "/DISAAC_VITA_XINPUT=1", "/DISAAC_VITA_GUEST_DISPATCH_TABLE=1",
             "/I", str(RUNTIME), str(RUNTIME / "host_vita_import_id.c"),
             "/Fi:" + str(preprocessed)], env)
        off_text = preprocessed.read_text(encoding="utf-8", errors="replace")
        leaked = [token for token in ("isaac_vita_lua_import_fast",
                                      "g_isaac_vita_lua_import_fast_by_index",
                                      "fast_ =")
                  if token in off_text]
        if leaked:
            raise AssertionError(f"default-OFF host_vita_import_id.c retained "
                                 f"the typed Lua binding: {leaked}")
        binder = (RUNTIME / "host_vita_import_id.c").read_text(encoding="utf-8")
        if binder.count("ISAAC_VITA_LUA_FAST_BINDING(entry->kind, "
                        "entry->local_index, ") != 2:
            raise AssertionError("the typed Lua binding hook must be applied "
                                 "in exactly the two dispatch routes")
        if binder.count("ISAAC_VITA_LUA_FAST_PREPARE(0);") != 1 or \
                binder.count("ISAAC_VITA_LUA_FAST_PREPARE(1);") != 1:
            raise AssertionError("registration must clear then rebind the "
                                 "typed Lua table exactly once each")

        census_marker = ("Vita IAT direct dispatch oracle: per-import census "
                         "compared on every differential run")
        for label, flags in (
                ("audio-off", ["/DISAAC_VITA_AUDIO=0", "/DISAAC_VITA_XINPUT=0",
                               "/DISAAC_VITA_LUA=0"]),
                ("audio-on", ["/DISAAC_VITA_AUDIO=1", "/DISAAC_VITA_XINPUT=1",
                              "/DISAAC_VITA_LUA=0"]),
                ("census", ["/DISAAC_VITA_AUDIO=1", "/DISAAC_VITA_XINPUT=1",
                            "/DISAAC_VITA_LUA=0",
                            "/DISAAC_VITA_PROFILE_IMPORT_KINDS=1"]),
                ("lua-on", ["/DISAAC_VITA_AUDIO=1", "/DISAAC_VITA_XINPUT=1",
                            "/DISAAC_VITA_LUA=1"]),
                ("lua-fast", ["/DISAAC_VITA_AUDIO=1", "/DISAAC_VITA_XINPUT=1",
                              "/DISAAC_VITA_LUA=1",
                              "/DISAAC_VITA_LUA_IMPORT_FASTDISPATCH=1"])):
            executable = root / f"iat-direct-oracle-{label}.exe"
            run(common + flags + [
                str(RUNTIME / "host_vita_import_direct_oracle.c"),
                str(RUNTIME / "host_vita_import_id.c"),
                "/Fe:" + str(executable),
                "/Fo:" + str(root) + "\\",
                "/link", "/OPT:REF", "/INCREMENTAL:NO"], env)
            ran = run([str(executable)], env)
            output = ran.stdout.decode("cp866", errors="replace")
            if PASS_MARKER not in output:
                raise AssertionError(f"{label}: unexpected oracle output: "
                                     f"{output!r}")
            if (label == "census") != (census_marker in output):
                raise AssertionError(f"{label}: per-import census coverage "
                                     f"does not match the build: {output!r}")
            sys.stdout.write(output)
    print(PASS_MARKER)
    return 0


if __name__ == "__main__":
    sys.exit(main())
