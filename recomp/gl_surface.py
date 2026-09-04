"""Generate and audit the exact guest OpenGL ABI surface.

This is deliberately not a libepoxy replacement.  The Windows executable
contains libepoxy's roughly 3,000-name registry, while the measured direct KAGE
path touches the original frozen 51-name set below.  Generated-code frontier
measurement adds 22 direct symbols, including ``glGetStringi`` and the
register-indirect ``glGenFramebuffers`` proved by exact resolver slots and call
sites.  The 171 original static call sites are a
conservative lower bound: 45 candidate roots were not decoded by the census
that produced it.  Consequently this module never calls the set "complete".

The useful product is smaller and stricter:

* one Khronos-derived prototype per measured name;
* an exact x86/APIENTRY stack layout;
* stable synthetic guest tokens, with collision checks;
* generated, typed backend callbacks and guest-stack adapters;
* a loud fault when a resolved symbol has no backend implementation.

No native ``FARPROC`` crosses this boundary.  Pointer arguments are represented
as 32-bit guest addresses even in the backend callback.  This prevents a
64-bit analysis host (or a future non-identity memory layout) from accidentally
treating a guest pointer array such as ``glShaderSource`` as native memory.

Usage::

    python recomp/gl_surface.py --write
    python recomp/gl_surface.py --check
    python recomp/gl_surface.py --check \
        --khronos-xml /path/to/OpenGL-Registry/xml/gl.xml \
        --pe /path/to/isaac-ng.exe.unpacked.exe
"""

from __future__ import print_function

import argparse
import hashlib
import json
import os
import re
import struct
import sys
import xml.etree.ElementTree as ET
import zlib
from collections import namedtuple


HERE = os.path.dirname(os.path.abspath(__file__))
RUNTIME = os.path.join(HERE, "runtime")
MANIFEST_PATH = os.path.join(HERE, "gl_surface_manifest.json")
HEADER_PATH = os.path.join(RUNTIME, "gl_surface_generated.h")
INC_PATH = os.path.join(RUNTIME, "gl_surface_generated.inc")

SCHEMA = 1
TOKEN_PREFIX = 0x7E000000
TOKEN_PAYLOAD_MASK = 0x00FFFFFF

# Direct-mapped token index for the Vita fast dispatcher
# (ISAAC_VITA_GL_SHIM_FASTDISPATCH): slot = ((token * MULTIPLIER) mod 2**32)
# >> (32 - BITS).  The multiplier was searched offline for the frozen 73-token
# registry and is collision-free over it; token_slot_table() fails loudly if a
# future surface change ever collides, so the O(1) lookup can never silently
# alias two names.  The runtime additionally re-compares the exact token of
# the indexed entry, so a collision could only cost a miss, never a wrong
# dispatch.
TOKEN_SLOT_MULTIPLIER = 0xAE061D23
TOKEN_SLOT_BITS = 8
TOKEN_SLOT_COUNT = 1 << TOKEN_SLOT_BITS

# Exact input binary measured by the graphics-seam census.  The site and root
# counts are evidence imported from that census, not recomputed by this file.
PE_SHA256 = "31846486979cfa07c8c968221553052c3ff603518681ca11912d445f96ca9404"
PE_SIZE = 8650240
MEASURED_STATIC_CALL_SITES_LOWER_BOUND = 171
MEASURED_UNDECODED_ROOTS = 45
ORIGINAL_MEASURED_NAMES = 51
SUPPLEMENTAL_DIRECT_GENERATED_NAMES = 22
EPOXY_GL_GET_STRINGI_SLOT = 0x307BFE78
EPOXY_GL_GET_STRINGI_CALL_RVA = 0x005992A6
GL_GEN_FRAMEBUFFERS_SLOT = 0x307C1AB4
GL_GEN_FRAMEBUFFERS_CALL_RVA = 0x00561A9D

# Independent ABI authority used for the semantic cross-check.  The generator
# does not need gl.xml during an ordinary build; --khronos-xml replays the check.
KHRONOS_REGISTRY_URL = "https://github.com/KhronosGroup/OpenGL-Registry"
KHRONOS_REGISTRY_COMMIT = "e8f7cd0e35ac8d6f5667a021ff83d04b1fec41ef"
KHRONOS_GL_XML_SHA256 = "d76ff1730ba90801a0c901fa38f643fcd1ad9da475a9c5a8344010be41c30687"

# Frozen order is alphabetical for auditability, but token identity is derived
# from the name rather than this position.  Future additions therefore cannot
# renumber an existing symbol.  These declarations were semantically checked
# against Khronos gl.xml at the commit above.
FROZEN_PROTOTYPES = r"""
void glActiveTexture(GLenum texture)
void glAlphaFunc(GLenum func, GLfloat ref)
void glAttachShader(GLuint program, GLuint shader)
void glBindFramebuffer(GLenum target, GLuint framebuffer)
void glBindRenderbuffer(GLenum target, GLuint renderbuffer)
void glBindTexture(GLenum target, GLuint texture)
void glBlendFuncSeparate(GLenum sfactorRGB, GLenum dfactorRGB, GLenum sfactorAlpha, GLenum dfactorAlpha)
GLenum glCheckFramebufferStatus(GLenum target)
void glClampColorARB(GLenum target, GLenum clamp)
void glClear(GLbitfield mask)
void glClearColor(GLfloat red, GLfloat green, GLfloat blue, GLfloat alpha)
void glClearDepth(GLdouble depth)
void glCompileShader(GLuint shader)
GLuint glCreateProgram()
GLuint glCreateShader(GLenum type)
void glCullFace(GLenum mode)
void glDeleteFramebuffers(GLsizei n, const GLuint *framebuffers)
void glDeleteProgram(GLuint program)
void glDeleteRenderbuffers(GLsizei n, const GLuint *renderbuffers)
void glDeleteShader(GLuint shader)
void glDeleteTextures(GLsizei n, const GLuint *textures)
void glDepthFunc(GLenum func)
void glDisableVertexAttribArray(GLuint index)
void glDrawElements(GLenum mode, GLsizei count, GLenum type, const void *indices)
void glEnable(GLenum cap)
void glEnableVertexAttribArray(GLuint index)
void glFramebufferRenderbuffer(GLenum target, GLenum attachment, GLenum renderbuffertarget, GLuint renderbuffer)
void glFramebufferTexture2D(GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level)
void glGenFramebuffers(GLsizei n, GLuint *framebuffers)
void glGenRenderbuffers(GLsizei n, GLuint *renderbuffers)
void glGenTextures(GLsizei n, GLuint *textures)
GLint glGetAttribLocation(GLuint program, const GLchar *name)
void glGetIntegerv(GLenum pname, GLint *data)
void glGetProgramInfoLog(GLuint program, GLsizei bufSize, GLsizei *length, GLchar *infoLog)
void glGetProgramiv(GLuint program, GLenum pname, GLint *params)
void glGetRenderbufferParameteriv(GLenum target, GLenum pname, GLint *params)
void glGetShaderInfoLog(GLuint shader, GLsizei bufSize, GLsizei *length, GLchar *infoLog)
void glGetShaderiv(GLuint shader, GLenum pname, GLint *params)
const GLubyte *glGetString(GLenum name)
const GLubyte *glGetStringi(GLenum name, GLuint index)
GLint glGetUniformLocation(GLuint program, const GLchar *name)
void glLinkProgram(GLuint program)
void glReadPixels(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, void *pixels)
void glRenderbufferStorage(GLenum target, GLenum internalformat, GLsizei width, GLsizei height)
void glShaderSource(GLuint shader, GLsizei count, const GLchar *const *string, const GLint *length)
void glTexImage2D(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLint border, GLenum format, GLenum type, const void *pixels)
void glTexParameteri(GLenum target, GLenum pname, GLint param)
void glTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, GLenum format, GLenum type, const void *pixels)
void glUniform1fv(GLint location, GLsizei count, const GLfloat *value)
void glUniform1i(GLint location, GLint v0)
void glUniform1iv(GLint location, GLsizei count, const GLint *value)
void glUniform1uiv(GLint location, GLsizei count, const GLuint *value)
void glUniform2fv(GLint location, GLsizei count, const GLfloat *value)
void glUniform2iv(GLint location, GLsizei count, const GLint *value)
void glUniform2uiv(GLint location, GLsizei count, const GLuint *value)
void glUniform3fv(GLint location, GLsizei count, const GLfloat *value)
void glUniform3iv(GLint location, GLsizei count, const GLint *value)
void glUniform3uiv(GLint location, GLsizei count, const GLuint *value)
void glUniform4fv(GLint location, GLsizei count, const GLfloat *value)
void glUniform4iv(GLint location, GLsizei count, const GLint *value)
void glUniform4uiv(GLint location, GLsizei count, const GLuint *value)
void glUniformMatrix2fv(GLint location, GLsizei count, GLboolean transpose, const GLfloat *value)
void glUniformMatrix2x3fv(GLint location, GLsizei count, GLboolean transpose, const GLfloat *value)
void glUniformMatrix2x4fv(GLint location, GLsizei count, GLboolean transpose, const GLfloat *value)
void glUniformMatrix3fv(GLint location, GLsizei count, GLboolean transpose, const GLfloat *value)
void glUniformMatrix3x2fv(GLint location, GLsizei count, GLboolean transpose, const GLfloat *value)
void glUniformMatrix3x4fv(GLint location, GLsizei count, GLboolean transpose, const GLfloat *value)
void glUniformMatrix4fv(GLint location, GLsizei count, GLboolean transpose, const GLfloat *value)
void glUniformMatrix4x2fv(GLint location, GLsizei count, GLboolean transpose, const GLfloat *value)
void glUniformMatrix4x3fv(GLint location, GLsizei count, GLboolean transpose, const GLfloat *value)
void glUseProgram(GLuint program)
void glVertexAttribPointer(GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride, const void *pointer)
void glViewport(GLint x, GLint y, GLsizei width, GLsizei height)
"""

Param = namedtuple("Param", "ctype name abi_kind callback_type stack_bytes")
Command = namedtuple("Command", "return_type name params return_kind callback_return token stack_bytes")

_SCALAR_TYPES = {
    "GLenum": ("u32", "guest_gl_enum", 4),
    "GLbitfield": ("u32", "guest_gl_bitfield", 4),
    "GLuint": ("u32", "guest_gl_uint", 4),
    "GLint": ("i32", "guest_gl_int", 4),
    "GLsizei": ("i32", "guest_gl_sizei", 4),
    "GLboolean": ("u8_slot", "guest_gl_boolean", 4),
    "GLfloat": ("f32", "guest_gl_float", 4),
    "GLdouble": ("f64", "guest_gl_double", 8),
}

_RETURN_TYPES = {
    "void": ("void", "void"),
    "GLenum": ("u32", "guest_gl_enum"),
    "GLuint": ("u32", "guest_gl_uint"),
    "GLint": ("i32", "guest_gl_int"),
}


def _clean_type(value):
    """Canonicalise harmless C whitespace without changing qualifiers."""
    value = re.sub(r"\s+", " ", value.strip())
    value = re.sub(r"\s*\*\s*", " *", value)
    return value.strip()


def _split_params(value):
    if not value.strip():
        return []
    # The frozen surface has no function-pointer parameters; commas therefore
    # have one unambiguous meaning.  Reject a future declaration that changes
    # that assumption instead of silently parsing it incorrectly.
    if "(" in value or ")" in value:
        raise ValueError("nested declarator requires an explicit parser: %r" % value)
    return [part.strip() for part in value.split(",")]


def _parse_param(text):
    text = _clean_type(text)
    match = re.match(r"^(.*?)([A-Za-z_]\w*)$", text)
    if not match:
        raise ValueError("cannot parse GL parameter %r" % text)
    ctype = _clean_type(match.group(1))
    name = match.group(2)
    if "*" in ctype:
        # Preserve the Khronos spelling in the manifest, but keep guest
        # addresses opaque at the platform callback boundary.
        return Param(ctype, name, "guest_addr", "guest_gl_addr", 4)
    try:
        kind, callback, width = _SCALAR_TYPES[ctype]
    except KeyError:
        raise ValueError("unsupported GL scalar parameter type %r" % ctype)
    return Param(ctype, name, kind, callback, width)


def _token_for_name(name):
    # CRC32 is an identity scheme, not a security primitive.  A hard collision
    # check below makes the 24-bit truncation safe for this registry.
    return TOKEN_PREFIX | (zlib.crc32(name.encode("ascii")) & TOKEN_PAYLOAD_MASK)


def parse_prototype(line):
    line = line.strip()
    match = re.match(r"^(.+?)(gl[A-Za-z0-9_]+)\((.*)\)$", line)
    if not match:
        raise ValueError("cannot parse GL prototype %r" % line)
    return_type = _clean_type(match.group(1))
    name = match.group(2)
    params = tuple(_parse_param(part) for part in _split_params(match.group(3)))
    if "*" in return_type:
        return_kind, callback_return = "guest_addr", "guest_gl_addr"
    else:
        try:
            return_kind, callback_return = _RETURN_TYPES[return_type]
        except KeyError:
            raise ValueError("unsupported GL return type %r" % return_type)
    return Command(return_type, name, params, return_kind, callback_return,
                   _token_for_name(name), sum(p.stack_bytes for p in params))


def commands():
    result = tuple(parse_prototype(line) for line in FROZEN_PROTOTYPES.splitlines()
                   if line.strip())
    names = [cmd.name for cmd in result]
    if names != sorted(names):
        raise ValueError("frozen GL surface must remain alphabetically auditable")
    if len(result) != (ORIGINAL_MEASURED_NAMES +
                       SUPPLEMENTAL_DIRECT_GENERATED_NAMES) or \
            len(set(names)) != len(result):
        raise ValueError("expected exactly 51 original + 22 supplemental GL names")
    tokens = {}
    for cmd in result:
        if cmd.token in tokens:
            raise ValueError("synthetic token collision: %s and %s -> 0x%08x" %
                             (tokens[cmd.token], cmd.name, cmd.token))
        tokens[cmd.token] = cmd.name
    return result


def token_slot(token):
    return ((token * TOKEN_SLOT_MULTIPLIER) & 0xFFFFFFFF) >> (32 - TOKEN_SLOT_BITS)


def token_slot_table(cmds):
    """Return the direct-mapped slot table: 0 = empty, else 1 + entry index.

    Entry indices follow the frozen alphabetical ``s_guest_gl_entries`` order.
    Any two registry tokens sharing a slot is a hard generation failure."""
    table = [0] * TOKEN_SLOT_COUNT
    for index, cmd in enumerate(cmds):
        slot = token_slot(cmd.token)
        if table[slot]:
            raise ValueError(
                "token slot collision: %s and %s -> slot %d; choose a new "
                "TOKEN_SLOT_MULTIPLIER" %
                (cmds[table[slot] - 1].name, cmd.name, slot))
        table[slot] = index + 1
    return table


def _canonical_command(cmd):
    return {
        "name": cmd.name,
        "token": "0x%08x" % cmd.token,
        "return": {
            "ctype": cmd.return_type,
            "abi_kind": cmd.return_kind,
            "callback_type": cmd.callback_return,
        },
        "params": [
            {
                "name": p.name,
                "ctype": p.ctype,
                "abi_kind": p.abi_kind,
                "callback_type": p.callback_type,
                "stack_bytes": p.stack_bytes,
            }
            for p in cmd.params
        ],
        "x86_api_entry": "stdcall",
        "x86_stack_bytes": cmd.stack_bytes,
    }


def _abi_payload(cmds):
    # Evidence counts and external source locations do not affect generated C
    # ABI identity.  Signatures, tokens, and stack layouts do.
    return [_canonical_command(cmd) for cmd in cmds]


def abi_sha256(cmds):
    raw = json.dumps(_abi_payload(cmds), sort_keys=True,
                     separators=(",", ":")).encode("utf-8")
    return hashlib.sha256(raw).hexdigest()


def render_manifest(cmds):
    doc = {
        "schema": SCHEMA,
        "surface_complete": False,
        "warning": (
            "Measured conservative registry only; 45 candidate roots were "
            "undecoded, so an absent symbol is unsupported, not unreachable. "
            "Counts distinguish 51 original measured names from 22 "
            "supplemental direct-generated names."
        ),
        "abi_sha256": abi_sha256(cmds),
        "measurement": {
            "pe_sha256": PE_SHA256,
            "pe_size": PE_SIZE,
            "distinct_names": len(cmds),
            "original_measured_distinct_names": ORIGINAL_MEASURED_NAMES,
            "supplemental_direct_generated_distinct_names":
                SUPPLEMENTAL_DIRECT_GENERATED_NAMES,
            "supplemental_gl_get_stringi_evidence": {
                "name": "glGetStringi",
                "resolver_slot": "0x%08x" % EPOXY_GL_GET_STRINGI_SLOT,
                "call_rva": "0x%08x" % EPOXY_GL_GET_STRINGI_CALL_RVA,
                "arguments": ["GL_EXTENSIONS", "index"],
            },
            "supplemental_gl_gen_framebuffers_evidence": {
                "name": "glGenFramebuffers",
                "resolver_slot": "0x%08x" % GL_GEN_FRAMEBUFFERS_SLOT,
                "call_rva": "0x%08x" % GL_GEN_FRAMEBUFFERS_CALL_RVA,
                "arguments": ["n", "framebuffers"],
            },
            "static_call_sites_lower_bound": MEASURED_STATIC_CALL_SITES_LOWER_BOUND,
            "undecoded_candidate_roots": MEASURED_UNDECODED_ROOTS,
        },
        "authority": {
            "repository": KHRONOS_REGISTRY_URL,
            "commit": KHRONOS_REGISTRY_COMMIT,
            "gl_xml_sha256": KHRONOS_GL_XML_SHA256,
        },
        "commands": _abi_payload(cmds),
    }
    return json.dumps(doc, indent=2, sort_keys=True) + "\n"


def _callback_params(cmd):
    return ", ".join("%s %s" % (p.callback_type, p.name)
                     for p in cmd.params) or "void"


def render_header(cmds):
    digest = abi_sha256(cmds)
    out = [
        "/* Generated by recomp/gl_surface.py.  Do not edit. */",
        "#ifndef REPENTOGXM_GL_SURFACE_GENERATED_H",
        "#define REPENTOGXM_GL_SURFACE_GENERATED_H",
        "",
        "#ifndef REPENTOGXM_GL_BRIDGE_TYPES_READY",
        '#error "include gl_bridge.h, not gl_surface_generated.h directly"',
        "#endif",
        "",
        "#define GUEST_GL_SURFACE_COUNT %du" % len(cmds),
        "#define GUEST_GL_ORIGINAL_MEASURED_COUNT %du" %
        ORIGINAL_MEASURED_NAMES,
        "#define GUEST_GL_SUPPLEMENTAL_DIRECT_COUNT %du" %
        SUPPLEMENTAL_DIRECT_GENERATED_NAMES,
        '#define GUEST_GL_SURFACE_ABI_SHA256 "%s"' % digest,
        "#define GUEST_GL_STATIC_CALL_SITES_LOWER_BOUND %du" %
        MEASURED_STATIC_CALL_SITES_LOWER_BOUND,
        "#define GUEST_GL_UNDECODED_CANDIDATE_ROOTS %du" %
        MEASURED_UNDECODED_ROOTS,
        "",
        "typedef struct guest_gl_backend {",
    ]
    for cmd in cmds:
        out.append("    %s (*%s)(%s);" %
                   (cmd.callback_return, cmd.name, _callback_params(cmd)))
    out.extend([
        "} guest_gl_backend;",
        "",
        "#endif",
        "",
    ])
    return "\n".join(out)


def _read_expr(param, offset):
    if param.abi_kind == "u32":
        return "(%s)guest_gl_arg_u32(c, %du)" % (param.callback_type, offset)
    if param.abi_kind == "i32":
        return "(%s)(int32_t)guest_gl_arg_u32(c, %du)" % (
            param.callback_type, offset)
    if param.abi_kind == "u8_slot":
        return "(%s)(uint8_t)guest_gl_arg_u32(c, %du)" % (
            param.callback_type, offset)
    if param.abi_kind == "f32":
        return "guest_gl_arg_f32(c, %du)" % offset
    if param.abi_kind == "f64":
        return "guest_gl_arg_f64(c, %du)" % offset
    if param.abi_kind == "guest_addr":
        return "(guest_gl_addr)guest_gl_arg_u32(c, %du)" % offset
    raise ValueError("no C read expression for %s" % (param.abi_kind,))


def _render_adapter_tail(cmd):
    """Backend presence check, typed call and return-register store shared by
    the legacy and the ISAAC_VITA_GL_SHIM_RAW_ARGS adapter bodies."""
    out = [
        "    if (!s_guest_gl_backend.%s) {" % cmd.name,
        "        guest_gl_unsupported(c, 0x%08xu," % cmd.token,
        '            "unsupported GL backend symbol: %s");' % cmd.name,
        "        return;",
        "    }",
    ]
    args = ", ".join(p.name for p in cmd.params)
    call = "s_guest_gl_backend.%s(%s)" % (cmd.name, args)
    if cmd.return_kind == "void":
        out.append("    %s;" % call)
    elif cmd.return_kind in ("u32", "guest_addr"):
        out.append("    c->eax = (uint32_t)%s;" % call)
    elif cmd.return_kind == "i32":
        out.append("    c->eax = (uint32_t)(int32_t)%s;" % call)
    else:
        raise ValueError("no C return expression for %s" % cmd.return_kind)
    return out


def _render_adapter(cmd):
    out = ["static void guest_gl_adapter_%s(CPU *__restrict c)" % cmd.name,
           "{"]
    offset = 0
    for param in cmd.params:
        out.append("    %s %s = %s;" %
                   (param.callback_type, param.name,
                    _read_expr(param, offset)))
        offset += param.stack_bytes
    out.extend(_render_adapter_tail(cmd))
    out.extend(["    guest_gl_stdcall_return(c, %du);" % cmd.stack_bytes,
                "}", ""])
    return out


def _raw_read_expr(param, offset):
    """Plain-load spelling of _read_expr for a frame guest_gl_frame_ok
    already validated: `frame` is the guest address of argument byte 0."""
    word = "ld32(frame + %du)" % offset
    if param.abi_kind == "u32":
        return "(%s)%s" % (param.callback_type, word)
    if param.abi_kind == "i32":
        return "(%s)(int32_t)%s" % (param.callback_type, word)
    if param.abi_kind == "u8_slot":
        return "(%s)(uint8_t)%s" % (param.callback_type, word)
    if param.abi_kind == "f32":
        return "guest_gl_f32_from_bits(%s)" % word
    if param.abi_kind == "f64":
        return "guest_gl_f64_from_bits(%s, ld32(frame + %du))" % (
            word, offset + 4)
    if param.abi_kind == "guest_addr":
        return "(guest_gl_addr)%s" % word
    raise ValueError("no raw C read expression for %s" % (param.abi_kind,))


def _render_raw_adapter(cmd):
    """ISAAC_VITA_GL_SHIM_RAW_ARGS body (wf/opt-gltok).  The hot adapter
    validates the whole frame (return word + argument bytes) once with
    guest_gl_frame_ok, reads the arguments with plain ld32 and retires the
    frame with one esp store (guest_gl_stdcall_return_raw, which falls back
    to the legacy gpop + adjust pair if esp moved under a nested dispatch).
    A rejected frame goes to the out-of-line _slow twin, the legacy body
    unchanged, so the first rejected word faults exactly as before and the
    hot function stays compact (no cold per-word validators inline)."""
    out = ["static GUEST_GL_NOINLINE void guest_gl_adapter_%s_slow("
           "CPU *__restrict c)" % cmd.name,
           "{"]
    offset = 0
    for param in cmd.params:
        out.append("    %s %s = %s;" %
                   (param.callback_type, param.name,
                    _read_expr(param, offset)))
        offset += param.stack_bytes
    out.extend(_render_adapter_tail(cmd))
    out.extend(["    guest_gl_stdcall_return(c, %du);" % cmd.stack_bytes,
                "}", ""])
    out.extend(["static void guest_gl_adapter_%s(CPU *__restrict c)" % cmd.name,
                "{",
                "    const uint32_t frame = c->esp + 4u;",
                "    if (GUEST_STACK_UNLIKELY(!guest_gl_frame_ok(c, %du))) {" %
                cmd.stack_bytes,
                "        guest_gl_adapter_%s_slow(c);" % cmd.name,
                "        return;",
                "    }"])
    offset = 0
    for param in cmd.params:
        out.append("    %s %s = %s;" %
                   (param.callback_type, param.name,
                    _raw_read_expr(param, offset)))
        offset += param.stack_bytes
    out.extend(_render_adapter_tail(cmd))
    out.extend(["    guest_gl_stdcall_return_raw(c, frame, %du);" %
                cmd.stack_bytes,
                "}", ""])
    return out


def _render_table_trampolines(cmds):
    """ISAAC_VITA_GL_SHIM_TABLE_TOKENS (wf/opt-gltok): one guest_fn-shaped
    trampoline per registry entry plus the token/function arrays guest.c
    inserts into the boot-built dispatch table."""
    out = [
        "#if defined(ISAAC_VITA_GL_SHIM_TABLE_TOKENS)",
        "/* ISAAC_VITA_GL_SHIM_TABLE_TOKENS (wf/opt-gltok): one guest_fn-shaped",
        " * trampoline per registry entry, the function a dispatch-table hit for",
        " * that token calls (guest.c inserts the token/trampoline pairs).  The",
        " * probe passes only the CPU, so the entry is bound here; the shared",
        " * guest_gl_table_dispatch (gl_bridge.c) counts the dynamic call and runs",
        " * the ownership-guarded adapter. */",
    ]
    for index, cmd in enumerate(cmds):
        out.extend([
            "static void guest_gl_table_%s(CPU *__restrict c)" % cmd.name,
            "{",
            "    guest_gl_table_dispatch(c, &s_guest_gl_entries[%d]);" % index,
            "}",
            "",
        ])
    out.append("static const uint32_t "
               "s_guest_gl_table_tokens[GUEST_GL_SURFACE_COUNT] = {")
    for row in range(0, len(cmds), 6):
        out.append("    " + ", ".join(
            "0x%08xu" % cmd.token for cmd in cmds[row:row + 6]) + ",")
    out.extend(["};", "",
                "static const guest_fn "
                "s_guest_gl_table_functions[GUEST_GL_SURFACE_COUNT] = {"])
    for cmd in cmds:
        out.append("    guest_gl_table_%s," % cmd.name)
    out.extend(["};", "#endif", ""])
    return out


def render_inc(cmds):
    out = [
        "/* Generated by recomp/gl_surface.py.  Included by gl_bridge.c. */",
        "#if !defined(ISAAC_VITA_GL_SHIM_RAW_ARGS)",
    ]
    for cmd in cmds:
        out.extend(_render_adapter(cmd))
    out.extend([
        "#else",
        "/* ISAAC_VITA_GL_SHIM_RAW_ARGS (wf/opt-gltok) adapters: the same decode,",
        " * backend check, call and stdcall retirement, with the whole frame",
        " * [esp, esp + 4 + argument bytes) validated once by guest_gl_frame_ok",
        " * (gl_bridge.c) and read with plain loads; a rejected frame takes the",
        " * out-of-line _slow twin (the legacy per-word readers) and faults",
        " * exactly where they fault. */",
        "",
    ])
    for cmd in cmds:
        out.extend(_render_raw_adapter(cmd))
    out.extend([
        "#endif",
        "static const guest_gl_entry s_guest_gl_entries[] = {",
    ])
    return_enums = {
        "void": "GUEST_GL_RETURN_VOID",
        "u32": "GUEST_GL_RETURN_U32",
        "i32": "GUEST_GL_RETURN_I32",
        "guest_addr": "GUEST_GL_RETURN_GUEST_ADDR",
    }
    for cmd in cmds:
        out.append(
            '    { 0x%08xu, "%s", %du, %du, %s, guest_gl_adapter_%s },' %
            (cmd.token, cmd.name, cmd.stack_bytes, len(cmd.params),
             return_enums[cmd.return_kind], cmd.name))
    out.extend(["};", "", "static const guest_gl_symbol s_guest_gl_symbols[] = {"])
    for cmd in cmds:
        out.append(
            '    { 0x%08xu, "%s", %du, %du, %s },' %
            (cmd.token, cmd.name, cmd.stack_bytes, len(cmd.params),
             return_enums[cmd.return_kind]))
    out.extend(["};", "",
                "#if !defined(ISAAC_VITA_GL_SHIM_FASTDISPATCH)",
                "/* Legacy exact dispatch.  The fast build indexes s_guest_gl_entries",
                " * through s_guest_gl_token_index (below) and never references this",
                " * switch, so it is compiled out rather than left unused. */",
                "static int guest_gl_dispatch_generated(",
                "    CPU *__restrict c, uint32_t token)", "{",
                "    switch (token) {"])
    for cmd in sorted(cmds, key=lambda item: item.token):
        out.extend([
            "    case 0x%08xu:" % cmd.token,
            "        guest_gl_adapter_%s(c);" % cmd.name,
            "        return 1;",
        ])
    out.extend(["    default:", "        return 0;", "    }", "}", "#endif", ""])
    table = token_slot_table(cmds)
    out.extend([
        "#if defined(ISAAC_VITA_GL_SHIM_FASTDISPATCH)",
        "/* Direct-mapped token index (gl_surface.py token_slot_table): the slot",
        " * holds 1 + the s_guest_gl_entries index, or 0.  Collision-free over the",
        " * frozen registry by generation-time check; the reader re-compares the",
        " * exact token, so an unregistered token can only miss. */",
        "#define GUEST_GL_TOKEN_SLOT_MULTIPLIER 0x%08xu" % TOKEN_SLOT_MULTIPLIER,
        "#define GUEST_GL_TOKEN_SLOT_BITS %du" % TOKEN_SLOT_BITS,
        "#define GUEST_GL_TOKEN_SLOT_COUNT %du" % TOKEN_SLOT_COUNT,
        "#define GUEST_GL_TOKEN_SLOT(token) \\",
        "    ((uint32_t)((uint32_t)(token) * GUEST_GL_TOKEN_SLOT_MULTIPLIER) >> \\",
        "     (32u - GUEST_GL_TOKEN_SLOT_BITS))",
        "static const uint8_t s_guest_gl_token_index[GUEST_GL_TOKEN_SLOT_COUNT] = {",
    ])
    for row in range(0, TOKEN_SLOT_COUNT, 16):
        out.append("    " + ", ".join(
            "%2d" % value for value in table[row:row + 16]) + ",")
    out.extend(["};", "#endif", ""])
    out.extend(_render_table_trampolines(cmds))
    return "\n".join(out)


def generated_outputs(cmds):
    return {
        MANIFEST_PATH: render_manifest(cmds),
        HEADER_PATH: render_header(cmds),
        INC_PATH: render_inc(cmds),
    }


def _sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as stream:
        while True:
            block = stream.read(1024 * 1024)
            if not block:
                break
            h.update(block)
    return h.hexdigest()


def _semantic_from_xml(path, wanted):
    root = ET.parse(path).getroot()
    found = {}
    for node in root.findall("./commands/command"):
        proto = node.find("proto")
        if proto is None:
            continue
        name_node = proto.find("name")
        if name_node is None or name_node.text not in wanted:
            continue
        name = name_node.text
        proto_text = _clean_type("".join(proto.itertext()))
        return_type = _clean_type(proto_text[:proto_text.rfind(name)])
        params = []
        for param_node in node.findall("param"):
            param_name_node = param_node.find("name")
            if param_name_node is None:
                raise ValueError("Khronos parameter without a name in %s" % name)
            param_name = param_name_node.text
            full = _clean_type("".join(param_node.itertext()))
            split = full.rfind(param_name)
            params.append(_parse_param(_clean_type(full[:split]) + " " + param_name))
        if "*" in return_type:
            return_kind, callback_return = "guest_addr", "guest_gl_addr"
        else:
            return_kind, callback_return = _RETURN_TYPES[return_type]
        found[name] = Command(
            return_type, name, tuple(params), return_kind, callback_return,
            _token_for_name(name), sum(p.stack_bytes for p in params))
    missing = sorted(set(wanted) - set(found))
    if missing:
        raise ValueError("Khronos registry lacks measured names: %s" %
                         ", ".join(missing))
    return found


def verify_khronos(path, cmds):
    found = _semantic_from_xml(path, {cmd.name for cmd in cmds})
    mismatches = []
    for expected in cmds:
        actual = found[expected.name]
        if actual != expected:
            mismatches.append("%s\n  frozen: %r\n  xml:    %r" %
                              (expected.name, expected, actual))
    if mismatches:
        raise ValueError("Khronos ABI mismatch:\n" + "\n".join(mismatches))
    digest = _sha256_file(path)
    return {
        "commands": len(found),
        "sha256": digest,
        "pinned_sha256": KHRONOS_GL_XML_SHA256,
        "is_pinned_snapshot": digest == KHRONOS_GL_XML_SHA256,
    }


def verify_pe(path, cmds):
    if os.path.getsize(path) != PE_SIZE:
        raise ValueError("PE size mismatch: %d != %d" %
                         (os.path.getsize(path), PE_SIZE))
    digest = _sha256_file(path)
    if digest != PE_SHA256:
        raise ValueError("PE SHA-256 mismatch: %s != %s" % (digest, PE_SHA256))
    with open(path, "rb") as stream:
        raw = stream.read()
    strings = set(match.group(1).decode("ascii") for match in re.finditer(
        rb"(?<![A-Za-z0-9_])(gl[A-Z][A-Za-z0-9_]{2,})\x00", raw))
    missing = sorted(cmd.name for cmd in cmds if cmd.name not in strings)
    if missing:
        raise ValueError("measured GL names missing from exact PE: %s" %
                         ", ".join(missing))
    return {
        "sha256": digest,
        "embedded_gl_like_names": len(strings),
        "measured_names_present": len(cmds),
        "static_call_sites_lower_bound": MEASURED_STATIC_CALL_SITES_LOWER_BOUND,
        "undecoded_candidate_roots": MEASURED_UNDECODED_ROOTS,
        "surface_complete": False,
    }


def write_outputs(outputs):
    for path, data in outputs.items():
        directory = os.path.dirname(path)
        if directory and not os.path.isdir(directory):
            os.makedirs(directory)
        with open(path, "w", newline="\n", encoding="utf-8") as stream:
            stream.write(data)
        print("wrote %s" % os.path.relpath(path, HERE))


def check_outputs(outputs):
    stale = []
    for path, expected in outputs.items():
        try:
            with open(path, "r", encoding="utf-8") as stream:
                actual = stream.read()
        except IOError:
            actual = None
        if actual != expected:
            stale.append(os.path.relpath(path, HERE))
    if stale:
        raise ValueError("stale/missing generated GL surface: %s; run --write" %
                         ", ".join(stale))


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--write", action="store_true",
                      help="rewrite deterministic generated products")
    mode.add_argument("--check", action="store_true",
                      help="fail if a generated product is stale")
    parser.add_argument("--khronos-xml",
                        help="semantically verify all prototypes against gl.xml")
    parser.add_argument("--pe",
                        help="verify exact PE identity and embedded measured names")
    args = parser.parse_args(argv)

    try:
        cmds = commands()
        outputs = generated_outputs(cmds)
        if args.write:
            write_outputs(outputs)
        else:
            check_outputs(outputs)
        print("GL ABI: %d names (%d original + %d supplemental direct), "
              "%d static sites (lower bound), %d undecoded roots" %
              (len(cmds), ORIGINAL_MEASURED_NAMES,
               SUPPLEMENTAL_DIRECT_GENERATED_NAMES,
               MEASURED_STATIC_CALL_SITES_LOWER_BOUND,
               MEASURED_UNDECODED_ROOTS))
        print("ABI SHA-256: %s" % abi_sha256(cmds))
        if args.khronos_xml:
            print("Khronos: %s" % json.dumps(
                verify_khronos(args.khronos_xml, cmds), sort_keys=True))
        if args.pe:
            print("PE: %s" % json.dumps(verify_pe(args.pe, cmds), sort_keys=True))
    except (IOError, OSError, ValueError, ET.ParseError) as exc:
        print("GL SURFACE FAIL: %s" % exc, file=sys.stderr)
        return 1
    print("GL SURFACE PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
