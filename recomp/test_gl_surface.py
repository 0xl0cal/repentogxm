"""Focused structural and executable tests for the generated GL bridge."""

from __future__ import print_function

import argparse
import json
import os
import subprocess
import sys
import tempfile

from setuptools import msvc

import gl_surface as G


HERE = os.path.dirname(os.path.abspath(__file__))
RUNTIME = os.path.join(HERE, "runtime")


def check(condition, message):
    if not condition:
        raise AssertionError(message)


def structural_checks(cmds):
    G.check_outputs(G.generated_outputs(cmds))
    by_name = {cmd.name: cmd for cmd in cmds}
    check(len(cmds) == 73, "registry count")
    check(len({cmd.token for cmd in cmds}) == 73, "token uniqueness")
    check(all((cmd.token & 0xFF000000) == G.TOKEN_PREFIX for cmd in cmds),
          "token namespace")
    check(by_name["glGetString"].token == 0x7EBCF4BC,
          "stable glGetString token")
    check(by_name["glClearDepth"].stack_bytes == 8 and
          by_name["glClearDepth"].params[0].abi_kind == "f64",
          "GLdouble occupies two x86 slots")
    check(by_name["glCreateProgram"].stack_bytes == 0 and
          by_name["glCreateProgram"].return_kind == "u32",
          "zero-argument GLuint return")
    check([p.abi_kind for p in by_name["glShaderSource"].params] ==
          ["u32", "i32", "guest_addr", "guest_addr"],
          "shader source pointer arrays stay guest addresses")
    check(by_name["glTexImage2D"].stack_bytes == 36,
          "nine-slot glTexImage2D")
    check(by_name["glVertexAttribPointer"].params[3].abi_kind == "u8_slot" and
          by_name["glVertexAttribPointer"].stack_bytes == 24,
          "GLboolean still occupies one x86 stack slot")
    check(by_name["glGetString"].return_kind == "guest_addr",
          "pointer return stays a guest address")
    check(by_name["glGetStringi"].token == 0x7E707522 and
          by_name["glGetStringi"].stack_bytes == 8 and
          by_name["glGetStringi"].return_kind == "guest_addr",
          "epoxy glGetStringi ABI and stable token")
    check(by_name["glGenFramebuffers"].token == 0x7E98408F and
          by_name["glGenFramebuffers"].stack_bytes == 8 and
          [p.abi_kind for p in by_name["glGenFramebuffers"].params] ==
          ["i32", "guest_addr"],
          "register-indirect glGenFramebuffers ABI and stable token")

    with open(G.MANIFEST_PATH, "r", encoding="utf-8") as stream:
        manifest = json.load(stream)
    check(manifest["surface_complete"] is False,
          "conservative surface must not claim completeness")
    check(manifest["measurement"]["static_call_sites_lower_bound"] == 171,
          "frozen site lower bound")
    check(manifest["measurement"]["undecoded_candidate_roots"] == 45,
          "frozen undecoded-root caveat")
    check(manifest["measurement"]["original_measured_distinct_names"] == 51 and
          manifest["measurement"]["supplemental_direct_generated_distinct_names"] == 22,
          "original/supplemental registry split")
    check(manifest["measurement"]["supplemental_gl_get_stringi_evidence"] == {
        "name": "glGetStringi",
        "resolver_slot": "0x307bfe78",
        "call_rva": "0x005992a6",
        "arguments": ["GL_EXTENSIONS", "index"],
    }, "exact epoxy glGetStringi evidence")
    check(manifest["measurement"]["supplemental_gl_gen_framebuffers_evidence"] == {
        "name": "glGenFramebuffers",
        "resolver_slot": "0x307c1ab4",
        "call_rva": "0x00561a9d",
        "arguments": ["n", "framebuffers"],
    }, "exact glGenFramebuffers register-indirect evidence")
    check(manifest["abi_sha256"] == G.abi_sha256(cmds), "manifest ABI hash")


def _mock_definition(cmd):
    params = ", ".join("%s %s" % (p.callback_type, p.name)
                       for p in cmd.params) or "void"
    lines = ["static %s mock_%s(%s)" %
             (cmd.callback_return, cmd.name, params), "{"]
    for param in cmd.params:
        lines.append("    (void)%s;" % param.name)
    if cmd.return_kind != "void":
        lines.append("    return (%s)0;" % cmd.callback_return)
    lines.extend(["}", ""])
    return lines


def render_harness(cmds):
    lines = [
        "#include <stdint.h>",
        "#include <stdio.h>",
        "#include <string.h>",
        '#include "gl_bridge.h"',
        "",
        "static double seen_depth;",
        "static float seen_ref;",
        "static uint32_t seen_func;",
        "static uint32_t seen_backend_return_rva;",
        "static guest_gl_int seen_uniform_location;",
        "static guest_gl_int seen_uniform_value;",
        "static guest_gl_enum seen_get_pname;",
        "static guest_gl_addr seen_get_data;",
        "static guest_gl_uint seen_vertex_index;",
        "static guest_gl_int seen_vertex_size;",
        "static guest_gl_enum seen_vertex_type;",
        "static guest_gl_boolean seen_vertex_normalized;",
        "static guest_gl_sizei seen_vertex_stride;",
        "static guest_gl_addr seen_vertex_pointer;",
        "static guest_gl_enum seen_tex_target;",
        "static guest_gl_int seen_tex_level;",
        "static guest_gl_int seen_tex_internalformat;",
        "static guest_gl_sizei seen_tex_width;",
        "static guest_gl_sizei seen_tex_height;",
        "static guest_gl_int seen_tex_border;",
        "static guest_gl_enum seen_tex_format;",
        "static guest_gl_enum seen_tex_type;",
        "static guest_gl_addr seen_tex_pixels;",
        "",
        "void guest_fault(CPU *__restrict c, uint32_t addr, const char *what)",
        "{",
        "    c->fault = what;",
        "    c->fault_addr = addr;",
        "}",
        "",
        "static void capture_clear_depth(guest_gl_double depth)",
        "{",
        "    seen_depth = depth;",
        "}",
        "",
        "static void capture_alpha(guest_gl_enum func, guest_gl_float ref)",
        "{",
        "    seen_func = func;",
        "    seen_ref = ref;",
        "    seen_backend_return_rva = guest_gl_backend_return_rva();",
        "}",
        "",
        "static void capture_uniform1i(guest_gl_int location, guest_gl_int v0)",
        "{",
        "    seen_uniform_location = location;",
        "    seen_uniform_value = v0;",
        "}",
        "",
        "static void capture_get_integer(guest_gl_enum pname, guest_gl_addr data)",
        "{",
        "    seen_get_pname = pname;",
        "    seen_get_data = data;",
        "}",
        "",
        "static void capture_vertex_attrib_pointer(",
        "    guest_gl_uint index, guest_gl_int size, guest_gl_enum type,",
        "    guest_gl_boolean normalized, guest_gl_sizei stride,",
        "    guest_gl_addr pointer)",
        "{",
        "    seen_vertex_index = index;",
        "    seen_vertex_size = size;",
        "    seen_vertex_type = type;",
        "    seen_vertex_normalized = normalized;",
        "    seen_vertex_stride = stride;",
        "    seen_vertex_pointer = pointer;",
        "}",
        "",
        "static void capture_tex_image_2d(",
        "    guest_gl_enum target, guest_gl_int level,",
        "    guest_gl_int internalformat, guest_gl_sizei width,",
        "    guest_gl_sizei height, guest_gl_int border,",
        "    guest_gl_enum format, guest_gl_enum type, guest_gl_addr pixels)",
        "{",
        "    seen_tex_target = target;",
        "    seen_tex_level = level;",
        "    seen_tex_internalformat = internalformat;",
        "    seen_tex_width = width;",
        "    seen_tex_height = height;",
        "    seen_tex_border = border;",
        "    seen_tex_format = format;",
        "    seen_tex_type = type;",
        "    seen_tex_pixels = pixels;",
        "}",
        "",
        "static guest_gl_uint return_program(void)",
        "{",
        "    return 0x13579bdfu;",
        "}",
        "",
        "static guest_gl_addr return_string(guest_gl_enum name)",
        "{",
        "    return name == 0x1f02u ? 0x30123456u : 0u;",
        "}",
        "",
        "static guest_gl_enum seen_stringi_name;",
        "static guest_gl_uint seen_stringi_index;",
        "static guest_gl_addr return_stringi(guest_gl_enum name,",
        "                                    guest_gl_uint index)",
        "{",
        "    seen_stringi_name = name;",
        "    seen_stringi_index = index;",
        "    return name == 0x1f03u && index == 7u ? 0x30abcdefu : 0u;",
        "}",
        "",
    ]
    for cmd in cmds:
        lines.extend(_mock_definition(cmd))
    lines.extend([
        "static void typecheck_every_callback(void)",
        "{",
        "    guest_gl_backend backend;",
        "    memset(&backend, 0, sizeof backend);",
    ])
    for cmd in cmds:
        lines.append("    backend.%s = mock_%s;" % (cmd.name, cmd.name))
    lines.extend([
        "    guest_gl_install_backend(&backend);",
        "}",
        "",
        "static int fail(const char *what)",
        "{",
        '    fprintf(stderr, "GL BRIDGE TEST FAIL: %s\\n", what);',
        "    return 1;",
        "}",
        "",
        "static int probe_backend_return_rva(",
        "    CPU *cpu, uint32_t start, uint32_t return_word,",
        "    uint32_t expected_rva)",
        "{",
        "    memset(cpu, 0, sizeof *cpu);",
        "    cpu->esp = start;",
        "    st32(start, return_word);",
        "    st32(start + 4u, 0x0203u);",
        "    st32(start + 8u, 0x3e800000u); /* 0.25f */",
        "    seen_backend_return_rva = UINT32_MAX;",
        "    if (!guest_gl_dispatch(cpu, guest_gl_resolve(\"glAlphaFunc\")))",
        "        return 0;",
        "    return seen_func == 0x0203u && seen_ref == 0.25f &&",
        "           seen_backend_return_rva == expected_rva &&",
        "           cpu->esp == start + 12u && !cpu->fault;",
        "}",
        "",
        "int main(void)",
        "{",
        "    CPU cpu;",
        "    CPU expected_cpu;",
        "    guest_gl_backend backend;",
        "    uint8_t stack[128];",
        "    char copied_name[GUEST_GL_PROCEDURE_NAME_MAX + 1U];",
        "    uint32_t start;",
        "    uint64_t bits64;",
        "    uint32_t token;",
        "    size_t count = 0u;",
        "    const guest_gl_symbol *symbols;",
        "",
        "    memset(&backend, 0, sizeof backend);",
        "    backend.glClearDepth = capture_clear_depth;",
        "    backend.glAlphaFunc = capture_alpha;",
        "    backend.glUniform1i = capture_uniform1i;",
        "    backend.glGetIntegerv = capture_get_integer;",
        "    backend.glVertexAttribPointer = capture_vertex_attrib_pointer;",
        "    backend.glTexImage2D = capture_tex_image_2d;",
        "    backend.glCreateProgram = return_program;",
        "    backend.glGetString = return_string;",
        "    backend.glGetStringi = return_stringi;",
        "    guest_gl_install_backend(&backend);",
        "",
        "    memset(&cpu, 0, sizeof cpu);",
        "    start = (uint32_t)(uintptr_t)&stack[0];",
        "    cpu.esp = start;",
        "    cpu.ecx = (uint32_t)(uintptr_t)\"glAlphaFunc\";",
        "    cpu.edx = 0x11223344u;",
        "    cpu.ebx = 0x22334455u;",
        "    cpu.esi = 0x33445566u;",
        "    cpu.edi = 0x44556677u;",
        "    cpu.ebp = 0x55667788u;",
        "    cpu.f_op = 0x99u;",
        "    st32(start, 0x00574237u);",
        "    st32(start + 4u, 0x986254a8u);",
        "    expected_cpu = cpu;",
        "    token = guest_gl_resolve(\"glAlphaFunc\");",
        "    expected_cpu.eax = token;",
        "    expected_cpu.esp += 4u;",
        "    if (!guest_gl_resolve_provider(&cpu, copied_name) ||",
        "        !token || strcmp(copied_name, \"glAlphaFunc\") != 0 ||",
        "        memcmp(&cpu, &expected_cpu, sizeof cpu) != 0 ||",
        "        ld32(cpu.esp) != 0x986254a8u)",
        '        return fail("provider token/plain-RET/callee state ABI");',
        "",
        "    memset(&cpu, 0, sizeof cpu);",
        "    cpu.esp = start;",
        "    cpu.ecx = (uint32_t)(uintptr_t)\"glDefinitelyMissing\";",
        "    cpu.eax = 0xa5a5a5a5u;",
        "    st32(start, 0x00574237u);",
        "    st32(start + 4u, 0x986254a8u);",
        "    expected_cpu = cpu;",
        "    if (guest_gl_resolve_provider(&cpu, copied_name) ||",
        "        strcmp(copied_name, \"glDefinitelyMissing\") != 0 ||",
        "        memcmp(&cpu, &expected_cpu, sizeof cpu) != 0)",
        '        return fail("unknown provider name mutated CPU");',
        "    if (guest_gl_resolve_guest(1u, copied_name) != 0u ||",
        "        copied_name[0] != '\\0')",
        '        return fail("ordinal-shaped provider name was read");',
        "",
        "    memset(&cpu, 0, sizeof cpu);",
        "    cpu.esp = start;",
        "    st32(start, 0xcafebabeu);",
        "    seen_depth = 0.0;",
        "    {",
        "        double input = 0.625;",
        "        memcpy(&bits64, &input, sizeof bits64);",
        "    }",
        "    st32(start + 4u, (uint32_t)bits64);",
        "    st32(start + 8u, (uint32_t)(bits64 >> 32));",
        '    token = guest_gl_resolve("glClearDepth");',
        "    if (!token || !guest_gl_dispatch(&cpu, token))",
        '        return fail("glClearDepth did not dispatch");',
        "    if (seen_depth != 0.625 || cpu.esp != start + 12u)",
        '        return fail("GLdouble decode or stdcall cleanup");',
        "",
        "    /* emit.py's production CALL convention stacks the canonical RVA.",
        "     * Keep relocated compatibility, but reject every word outside",
        "     * those two bounded image domains. */",
        "    if (!probe_backend_return_rva(",
        "            &cpu, start, 0x0056039du, 0x0056039du) ||",
        "        !probe_backend_return_rva(",
        "            &cpu, start, 0x0056d794u, 0x0056d794u) ||",
        "        !probe_backend_return_rva(",
        "            &cpu, start, GUEST_IMAGE_BASE + 0x0056039du,",
        "            0x0056039du) ||",
        "        !probe_backend_return_rva(",
        "            &cpu, start, GUEST_IMAGE_BASE + 0x0056d794u,",
        "            0x0056d794u) ||",
        "        !probe_backend_return_rva(",
        "            &cpu, start, 0x0085efffu, 0x0085efffu) ||",
        "        !probe_backend_return_rva(",
        "            &cpu, start, 0x0085f000u, 0u) ||",
        "        !probe_backend_return_rva(",
        "            &cpu, start, GUEST_IMAGE_BASE - 1u, 0u) ||",
        "        !probe_backend_return_rva(",
        "            &cpu, start, GUEST_IMAGE_BASE + 0x0085f000u, 0u) ||",
        "        !probe_backend_return_rva(",
        "            &cpu, start, UINT32_MAX, 0u) ||",
        "        guest_gl_backend_return_rva() != 0u)",
        '        return fail("canonical/relocated return RVA normalization");',
        "",
        "    memset(&cpu, 0, sizeof cpu);",
        "    cpu.esp = start;",
        "    st32(start, 0x44444444u);",
        "    st32(start + 4u, 0xfffffff9u);",
        "    st32(start + 8u, 0xf8a432ebu);",
        '    if (!guest_gl_dispatch(&cpu, guest_gl_resolve("glUniform1i")))',
        '        return fail("glUniform1i did not dispatch");',
        "    if (seen_uniform_location != -7 ||",
        "        seen_uniform_value != -123456789 || cpu.esp != start + 12u)",
        '        return fail("signed GLint decode or cleanup");',
        "",
        "    memset(&cpu, 0, sizeof cpu);",
        "    cpu.esp = start;",
        "    st32(start, 0x55555555u);",
        "    st32(start + 4u, 0x00000ba2u);",
        "    st32(start + 8u, 0x30abcdefu);",
        '    if (!guest_gl_dispatch(&cpu, guest_gl_resolve("glGetIntegerv")))',
        '        return fail("glGetIntegerv did not dispatch");',
        "    if (seen_get_pname != 0x00000ba2u ||",
        "        seen_get_data != 0x30abcdefu || cpu.esp != start + 12u)",
        '        return fail("guest output address pass-through or cleanup");',
        "",
        "    memset(&cpu, 0, sizeof cpu);",
        "    cpu.esp = start;",
        "    st32(start, 0x66666666u);",
        "    st32(start + 4u, 7u);",
        "    st32(start + 8u, 0xfffffffcu);",
        "    st32(start + 12u, 0x00001406u);",
        "    st32(start + 16u, 0xa5a5a501u);",
        "    st32(start + 20u, 0xffffffe0u);",
        "    st32(start + 24u, 0x30fedcbau);",
        "    if (!guest_gl_dispatch(&cpu,",
        '                           guest_gl_resolve("glVertexAttribPointer")))',
        '        return fail("glVertexAttribPointer did not dispatch");',
        "    if (seen_vertex_index != 7u || seen_vertex_size != -4 ||",
        "        seen_vertex_type != 0x00001406u ||",
        "        seen_vertex_normalized != 1u || seen_vertex_stride != -32 ||",
        "        seen_vertex_pointer != 0x30fedcbau || cpu.esp != start + 28u)",
        '        return fail("GLboolean slot, guest pointer, or cleanup");',
        "",
        "    memset(&cpu, 0, sizeof cpu);",
        "    cpu.esp = start;",
        "    st32(start, 0x77777777u);",
        "    st32(start + 4u, 0x00000de1u);",
        "    st32(start + 8u, 0xfffffffdu);",
        "    st32(start + 12u, 0x00001908u);",
        "    st32(start + 16u, 321u);",
        "    st32(start + 20u, 654u);",
        "    st32(start + 24u, 0xffffffffu);",
        "    st32(start + 28u, 0x000080e1u);",
        "    st32(start + 32u, 0x00001401u);",
        "    st32(start + 36u, 0x30c0ffeeu);",
        '    if (!guest_gl_dispatch(&cpu, guest_gl_resolve("glTexImage2D")))',
        '        return fail("glTexImage2D did not dispatch");',
        "    if (seen_tex_target != 0x00000de1u || seen_tex_level != -3 ||",
        "        seen_tex_internalformat != 0x00001908 ||",
        "        seen_tex_width != 321 || seen_tex_height != 654 ||",
        "        seen_tex_border != -1 || seen_tex_format != 0x000080e1u ||",
        "        seen_tex_type != 0x00001401u ||",
        "        seen_tex_pixels != 0x30c0ffeeu || cpu.esp != start + 40u)",
        '        return fail("nine-argument decode, offsets, or cleanup");',
        "",
        "    memset(&cpu, 0, sizeof cpu);",
        "    cpu.esp = start;",
        "    st32(start, 0x22222222u);",
        '    if (!guest_gl_dispatch(&cpu, guest_gl_resolve("glCreateProgram")))',
        '        return fail("glCreateProgram did not dispatch");',
        "    if (cpu.eax != 0x13579bdfu || cpu.esp != start + 4u)",
        '        return fail("GLuint return or zero-argument cleanup");',
        "",
        "    memset(&cpu, 0, sizeof cpu);",
        "    cpu.esp = start;",
        "    st32(start, 0x33333333u);",
        "    st32(start + 4u, 0x1f02u);",
        '    if (!guest_gl_dispatch(&cpu, guest_gl_resolve("glGetString")))',
        '        return fail("glGetString did not dispatch");',
        "    if (cpu.eax != 0x30123456u || cpu.esp != start + 8u)",
        '        return fail("guest-address return");',
        "",
        "    memset(&cpu, 0, sizeof cpu);",
        "    cpu.esp = start;",
        "    st32(start, 0x33333334u);",
        "    st32(start + 4u, 0x1f03u);",
        "    st32(start + 8u, 7u);",
        '    if (!guest_gl_dispatch(&cpu, guest_gl_resolve("glGetStringi")))',
        '        return fail("glGetStringi did not dispatch");',
        "    if (seen_stringi_name != 0x1f03u || seen_stringi_index != 7u ||",
        "        cpu.eax != 0x30abcdefu || cpu.esp != start + 12u)",
        '        return fail("glGetStringi arguments, return, or stdcall cleanup");',
        "",
        "    guest_gl_install_backend(NULL);",
        "    memset(&cpu, 0, sizeof cpu);",
        "    cpu.esp = start;",
        "    st32(start, 0x44444444u);",
        "    st32(start + 4u, 0x00004000u);",
        '    token = guest_gl_resolve("glClear");',
        "    if (!guest_gl_dispatch(&cpu, token))",
        '        return fail("known unsupported token was rejected");',
        "    if (!cpu.fault || cpu.fault_addr != token || cpu.esp != start ||",
        '        strcmp(cpu.fault, "unsupported GL backend symbol: glClear") != 0)',
        '        return fail("unsupported symbol was not exact and loud");',
        "",
        "    if (guest_gl_resolve(NULL) != 0u ||",
        '        guest_gl_resolve("glDefinitelyMissing") != 0u)',
        '        return fail("unknown name resolved");',
        "    if (guest_gl_dispatch(&cpu, 0x7effffffu) != 0)",
        '        return fail("arbitrary token dispatched");',
        "",
        "    symbols = guest_gl_symbols(&count);",
        "    if (!symbols || count != GUEST_GL_SURFACE_COUNT ||",
        '        strcmp(symbols[0].name, "glActiveTexture") != 0 ||',
        '        strcmp(symbols[count - 1u].name, "glViewport") != 0)',
        '        return fail("public metadata");',
        "",
        "    typecheck_every_callback();",
        '    printf("GL BRIDGE TEST PASS: %u typed symbols\\n",',
        "           (unsigned)GUEST_GL_SURFACE_COUNT);",
        "    return 0;",
        "}",
        "",
    ])
    return "\n".join(lines)


def _msvc_env():
    raw = {key.upper(): value for key, value in
           msvc.msvc14_get_vc_env("x86").items()}
    # setuptools may preserve an inherited lower-case `path` after vcvarsall;
    # VCToolsInstallDir is the unambiguous toolchain identity on this host.
    env = {key.upper(): value for key, value in os.environ.items()}
    for key, value in raw.items():
        if key != "PATH":
            env[key] = value
    for key in ("SYSTEMROOT", "TEMP", "TMP", "PATHEXT", "COMSPEC"):
        if key in os.environ:
            env.setdefault(key, os.environ[key])
    candidates = []
    vc_tools = raw.get("VCTOOLSINSTALLDIR")
    if vc_tools:
        candidates.extend([
            os.path.join(vc_tools, "bin", "HostX86", "x86", "cl.exe"),
            os.path.join(vc_tools, "bin", "HostX64", "x86", "cl.exe"),
        ])
    search_path = raw.get("PATH", "") + os.pathsep + env.get("PATH", "")
    candidates.extend(os.path.join(directory, "cl.exe")
                      for directory in search_path.split(os.pathsep) if directory)
    compiler = next((path for path in candidates if os.path.isfile(path)), None)
    if not compiler:
        raise RuntimeError("MSVC x86 cl.exe not found")
    env["PATH"] = os.path.dirname(compiler) + os.pathsep + search_path
    for injected in ("CL", "_CL_", "LINK", "_LINK_"):
        env.pop(injected, None)
    return compiler, env


def _run(command, env, cwd):
    result = subprocess.run(command, cwd=cwd, env=env,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    output = result.stdout.decode("cp866", errors="replace")
    if output.strip():
        print(output.rstrip())
    if result.returncode:
        raise RuntimeError("command failed (%d): %s" %
                           (result.returncode, " ".join(command)))


def compile_and_run(cmds):
    compiler, env = _msvc_env()
    with tempfile.TemporaryDirectory(prefix="repentogxm-gl-") as scratch:
        harness = os.path.join(scratch, "gl_bridge_test.c")
        with open(harness, "w", newline="\n", encoding="utf-8") as stream:
            stream.write(render_harness(cmds))
        common = [compiler, "/nologo", "/c", "/std:c11", "/W4", "/WX",
                  "/wd4310", "/GS-", "/I", RUNTIME]
        bridge_obj = os.path.join(scratch, "gl_bridge.obj")
        harness_obj = os.path.join(scratch, "gl_bridge_test.obj")
        stack_stub_obj = os.path.join(scratch, "guest_stack_legacy_oracle_stub.obj")
        _run(common + ["/Fo:" + bridge_obj,
                       os.path.join(RUNTIME, "gl_bridge.c")], env, scratch)
        _run(common + ["/Fo:" + harness_obj, harness], env, scratch)
        _run(common + ["/Fo:" + stack_stub_obj,
                       os.path.join(RUNTIME,
                                    "guest_stack_legacy_oracle_stub.c")],
             env, scratch)
        executable = os.path.join(scratch, "gl_bridge_test.exe")
        _run([compiler, "/nologo", bridge_obj, harness_obj, stack_stub_obj,
              "/Fe:" + executable], env, scratch)
        _run([executable], env, scratch)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--khronos-xml")
    parser.add_argument("--pe")
    parser.add_argument("--no-compile", action="store_true")
    args = parser.parse_args(argv)
    try:
        cmds = G.commands()
        structural_checks(cmds)
        if args.khronos_xml:
            result = G.verify_khronos(args.khronos_xml, cmds)
            check(result["is_pinned_snapshot"],
                  "Khronos XML differs from pinned audited snapshot")
            print("Khronos semantic ABI: %d/%d exact" %
                  (result["commands"], len(cmds)))
        if args.pe:
            result = G.verify_pe(args.pe, cmds)
            print("PE evidence: %d/%d names present; %d embedded GL-like names" %
                  (result["measured_names_present"], len(cmds),
                   result["embedded_gl_like_names"]))
        if not args.no_compile:
            compile_and_run(cmds)
    except (AssertionError, G.ET.ParseError, IOError, OSError,
            RuntimeError, ValueError) as exc:
        print("GL SURFACE TEST FAIL: %s" % exc, file=sys.stderr)
        return 1
    print("GL SURFACE TEST PASS: ABI %s" % G.abi_sha256(cmds))
    return 0


if __name__ == "__main__":
    sys.exit(main())
