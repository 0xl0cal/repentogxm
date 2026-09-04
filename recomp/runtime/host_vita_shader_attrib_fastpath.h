#ifndef ISAAC_HOST_VITA_SHADER_ATTRIB_FASTPATH_H
#define ISAAC_HOST_VITA_SHADER_ATTRIB_FASTPATH_H

#include "guest.h"

/* ISAAC_VITA_SHADER_ATTRIB_FASTPATH (wf/opt-attrib): host owners of the two
 * authenticated KAGE::Graphics::Shader attribute-plumbing bodies.
 *
 *   sub_005672d0  Shader::EnableAttribs(this=ecx, base, unused, stride)
 *                 per attribute: glGetAttribLocation(program, name) ->
 *                 glEnableVertexAttribArray(loc) ->
 *                 glVertexAttribPointer(loc, ncomp(format), GL_FLOAT, 0,
 *                                       stride, base + running offset)
 *   sub_005673f0  Shader::DisableAttribs(this=ecx, -, -, -)
 *                 per attribute: glGetAttribLocation(program, name) ->
 *                 glDisableVertexAttribArray(loc)
 *
 * Each `_try` is called by the generated body marker at the root instruction
 * (gen_all.py VITA_SHADER_ATTRIB_FASTPATH_ROOTS) with the CPU exactly as the
 * caller's `call [vtable+0x14/0x18]` left it: ECX = this, ESP at the pushed
 * return word, the three thiscall arguments above it.  HANDLED (1) means the
 * complete translated sequence ran into the same typed backend wrappers the
 * generated adapters call, in the same order with the same arguments, and
 * the `ret 0xc` was performed (ESP += 16, EAX/ECX/EDX as the x86 leaves
 * them, callee-saved registers untouched, lazy flags untouched: the body's
 * flags are dead at its RET).  REJECTED (0) leaves the CPU, guest memory,
 * the GL ownership word and every census untouched so the translated body
 * that follows the marker decides, including every fault the original path
 * would raise (foreign/nested GL owner, missing backend symbol, unresolved
 * slot word, stack violation, unknown attribute format). */
int isaac_vita_shader_attribs_enable_try(CPU *__restrict c);
int isaac_vita_shader_attribs_disable_try(CPU *__restrict c);

/* Frozen facts shared with the oracle (recomp/runtime/
 * host_vita_shader_attrib_fastpath_oracle.c).  RVAs are image-relative. */
#define ISAAC_VITA_SHADER_ATTRIB_ENABLE_RVA       0x005672D0u
#define ISAAC_VITA_SHADER_ATTRIB_DISABLE_RVA      0x005673F0u
#define ISAAC_VITA_SHADER_ATTRIB_VTABLE_RVA       0x0075E0A8u
#define ISAAC_VITA_SHADER_ATTRIB_SLOT_GET_LOCATION_RVA 0x007C294Cu
#define ISAAC_VITA_SHADER_ATTRIB_SLOT_ENABLE_RVA  0x007BF9B0u
#define ISAAC_VITA_SHADER_ATTRIB_SLOT_POINTER_RVA 0x007C2968u
#define ISAAC_VITA_SHADER_ATTRIB_SLOT_DISABLE_RVA 0x007BF9C0u
/* Return words the translated CALLs push (RVAs, see commit 2cd6417). */
#define ISAAC_VITA_SHADER_ATTRIB_ENABLE_RET_GET    0x005672F5u
#define ISAAC_VITA_SHADER_ATTRIB_ENABLE_RET_ENABLE 0x005672FFu
#define ISAAC_VITA_SHADER_ATTRIB_ENABLE_RET_POINTER 0x00567355u
#define ISAAC_VITA_SHADER_ATTRIB_DISABLE_RET_GET   0x0056740Fu
#define ISAAC_VITA_SHADER_ATTRIB_DISABLE_RET_DISABLE 0x00567416u
/* Shader object layout read by both bodies. */
#define ISAAC_VITA_SHADER_ATTRIB_OFF_ATTRIBS      0x08u
#define ISAAC_VITA_SHADER_ATTRIB_OFF_COUNT        0x0Cu
#define ISAAC_VITA_SHADER_ATTRIB_OFF_PROGRAM      0x28u
/* Attribute formats 1..8 (frozen jump tables at RVA 0x5673ac/0x5673cc):
 * component counts 1,2,3,4,3,4,2,1 and byte advances 4*ncomp.  Any other
 * format takes the translated body (its "Unknown attribute format" log). */
#define ISAAC_VITA_SHADER_ATTRIB_FORMAT_COUNT     8u
/* Fail-closed bound on the attribute count (vitaGL exposes 16 attributes;
 * the frozen shaders declare at most 7). */
#define ISAAC_VITA_SHADER_ATTRIB_MAX_COUNT        16u
#define ISAAC_VITA_SHADER_ATTRIB_GL_FLOAT         0x1406u

#endif
