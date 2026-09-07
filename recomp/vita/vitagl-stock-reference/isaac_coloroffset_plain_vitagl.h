/* Included by optional patch 0013 after the 0008/0012 implementations.
 * All objects belong to one linked program; no source/pointer proof is cached
 * across draws. The plain shader is selected ONLY with the same-byte PLAIN
 * staging result. Failure preserves the existing neutral variant. */
#ifndef ISAAC_COLOROFFSET_PLAIN_VITAGL_H
#define ISAAC_COLOROFFSET_PLAIN_VITAGL_H

#define ISAAC_COLOROFFSET_PLAIN_CACHE_CAPACITY 4u
typedef struct {
	uint32_t blend_raw;
	uint8_t output_format;
	uint8_t multisample_mode;
	SceGxmFragmentProgram *fragment;
} isaac_coloroffset_plain_entry;

struct isaac_coloroffset_plain_state {
	SceGxmShaderPatcherId id;
	const SceGxmProgram *gxp;
	SceGxmShaderPatcherId stock_fragment_id;
	const SceGxmProgram *vertex_program;
	uint32_t source_generation;
	uint32_t vertex_generation;
	uint32_t count;
	isaac_coloroffset_plain_entry entries[ISAAC_COLOROFFSET_PLAIN_CACHE_CAPACITY];
#ifdef HAVE_ISAAC_COLOROFFSET_PLAIN_FP16
	uint32_t gxp_size;
	SceGxmShaderPatcherId fp16_id;
	const SceGxmProgram *fp16_gxp;
	uint32_t fp16_count;
	isaac_coloroffset_plain_entry fp16_entries[ISAAC_COLOROFFSET_PLAIN_CACHE_CAPACITY];
#endif
};

#ifdef HAVE_ISAAC_COLOROFFSET_PLAIN_FP16
static GLboolean isaac_coloroffset_plain_link_matches(
	const program *p, const isaac_coloroffset_plain_state *s);
#include "isaac_coloroffset_plain_fp16.h"
#endif

/* Private ABI 1: exactly three uint32_t words q,b,f, atomically with respect
 * to this single-threaded renderer's calls. q = all-vertex eligible requests;
 * b = selected plain fragments; f = requests falling back, so q == b+f.
 * Invalid arguments return 0 without consuming. No new public vitaGL ABI. */
static uint32_t isaac_coloroffset_plain_window[3];
uint32_t vglTakeIsaacColorOffsetPlainStats(uint32_t *out, uint32_t words) {
	if (!out || words != 3u)
		return 0u;
	vgl_fast_memcpy(out, isaac_coloroffset_plain_window, sizeof isaac_coloroffset_plain_window);
	vgl_memset(isaac_coloroffset_plain_window, 0, sizeof isaac_coloroffset_plain_window);
	return 1u;
}

static void isaac_coloroffset_plain_release(program *p) {
	isaac_coloroffset_plain_state *s = p->isaac_coloroffset_plain;
	if (!s)
		return;
#ifdef HAVE_ISAAC_COLOROFFSET_PLAIN_FP16
	isaac_coloroffset_plain_fp16_release(s);
#endif
	for (uint32_t i = 0; i < s->count; ++i)
		sceGxmShaderPatcherReleaseFragmentProgram(gxm_shader_patcher, s->entries[i].fragment);
	sceGxmShaderPatcherUnregisterProgram(gxm_shader_patcher, s->id);
	vgl_free((void *)s->gxp);
	vgl_free(s);
	p->isaac_coloroffset_plain = NULL;
}

static GLboolean isaac_coloroffset_plain_link_matches(
		const program *p, const isaac_coloroffset_plain_state *s) {
	int32_t index;
	if (!s || p->status != PROG_LINKED || !p->fshader || !p->vshader ||
			!p->isaac_coloroffset_link_vertex_exact ||
			s->stock_fragment_id != p->fshader->id ||
			s->vertex_program != p->vshader->prog ||
			s->vertex_generation != p->isaac_coloroffset_link_vertex_generation ||
			s->source_generation != p->isaac_coloroffset_link_source_generation)
		return GL_FALSE;
	index = isaac_coloroffset_shader_index(p->fshader);
	return index >= 0 && s->source_generation != 0u &&
		isaac_coloroffset_source_generation[index] == s->source_generation;
}

static void isaac_coloroffset_plain_link(program *p) {
	shader probe;
	isaac_coloroffset_plain_state *s;
	const SceGxmProgram *compiled;
	SceGxmProgram *copy = NULL;
	GLenum saved_sema;
	GLboolean saved_first;
	binds_map saved_bindings;
	const char *failure = "allocation";
	uint32_t size = 0u, hash = 0u;
	if (isaac_coloroffset_plain_link_matches(p, p->isaac_coloroffset_plain))
		return;
	if (p->isaac_coloroffset_plain) {
		sceGxmFinish(gxm_context); /* Only relink, never draw-time. */
		isaac_coloroffset_plain_release(p);
	}
	/* Retain a working neutral fallback and its authenticated paired VS. */
	if (!p->isaac_coloroffset_link_vertex_exact ||
			p->isaac_coloroffset_fs_probe_state != ISAAC_COLOROFFSET_FS_PROBE_STATE_READY)
		return;
	s = vglMalloc(sizeof(*s));
	if (!s)
		goto report;
	vgl_memset(s, 0, sizeof(*s));
	if (!is_shark_online && !start_shader_compiler()) {
		failure = "compiler";
		goto fail;
	}
	vgl_memset(&probe, 0, sizeof(probe));
	probe.type = GL_FRAGMENT_SHADER;
	probe.valid = GL_TRUE;
	probe.is_glsl = GL_TRUE;
	/* Same existing Color0*Texture0 source, but NOT diagnostic mode 1:
	 * all-vertex PLAIN proof is mandatory before this program is selected. */
	probe.size = (uint32_t)(sizeof isaac_coloroffset_fs_probe_trivial_source - 1u);
	probe.source = vglMalloc(probe.size + 1u);
	if (!probe.source)
		goto fail;
	vgl_fast_memcpy(probe.source, isaac_coloroffset_fs_probe_trivial_source, probe.size + 1u);
	saved_sema = glsl_sema_mode;
	saved_first = glsl_is_first_shader;
	saved_bindings = glsl_bindings_map;
	glsl_sema_mode = VGL_MODE_SHADER_PAIR;
	glsl_translator_set_process(p->vshader, &probe);
	glsl_sema_mode = saved_sema;
	glsl_is_first_shader = saved_first;
	glsl_bindings_map = saved_bindings;
	compiled = shark_compile_shader_extended((const char *)probe.source,
		&probe.size, SHARK_FRAGMENT_SHADER, compiler_opts, compiler_fastmath,
		compiler_fastprecision, compiler_fastint);
	vgl_free(probe.source);
	failure = "compile";
	if (compiled && probe.size) {
		failure = "allocation";
		copy = vglMalloc(probe.size);
		if (copy)
			vgl_fast_memcpy(copy, compiled, probe.size);
	}
	shark_clear_output();
#ifdef HAVE_SHARK_LOG
	if (shark_log) {
		vgl_free(shark_log);
		shark_log = NULL;
	}
#endif
	if (!copy)
		goto fail;
	failure = "check";
	/* Reuse the fixed complete-header minimum 0x9c; no >=512-byte bug. */
	if (!isaac_coloroffset_fs_probe_program_is_exact(copy, probe.size, p->fshader))
		goto fail;
	failure = "register";
	if (sceGxmShaderPatcherRegisterProgram(gxm_shader_patcher, copy, &s->id))
		goto fail;
	s->gxp = copy;
#ifdef HAVE_ISAAC_COLOROFFSET_PLAIN_FP16
	s->gxp_size = probe.size;
#endif
	s->stock_fragment_id = p->fshader->id;
	s->vertex_program = p->vshader->prog;
	s->source_generation = p->isaac_coloroffset_link_source_generation;
	s->vertex_generation = p->isaac_coloroffset_link_vertex_generation;
	p->isaac_coloroffset_plain = s;
	size = probe.size;
	hash = isaac_coloroffset_fnv1a(copy, size);
	failure = "none";
	goto report;
fail:
	if (copy)
		vgl_free(copy);
	vgl_free(s);
report:
	sceClibPrintf("KAGE VITA COLOROFFSET PLAIN LINK: prog=%u ready=%u gxp=%u/%08x fail=%s\n",
		(uint32_t)(p - progs) + 1u, (unsigned)(p->isaac_coloroffset_plain != NULL), size, hash, failure);
#ifdef HAVE_ISAAC_COLOROFFSET_PLAIN_FP16
	isaac_coloroffset_plain_fp16_link(p);
#endif
}

static SceGxmFragmentProgram *isaac_coloroffset_plain_select(program *p) {
	isaac_coloroffset_plain_state *s = p->isaac_coloroffset_plain;
	isaac_coloroffset_plain_entry *entry;
	uint8_t output = is_fbo_float ? SCE_GXM_OUTPUT_REGISTER_FORMAT_HALF4 :
		SCE_GXM_OUTPUT_REGISTER_FORMAT_UCHAR4;
	++isaac_coloroffset_plain_window[0];
#ifdef HAVE_ISAAC_COLOROFFSET_PLAIN_FP16
	{
		SceGxmFragmentProgram *half = isaac_coloroffset_plain_fp16_select(p);
		if (half) {
			++isaac_coloroffset_plain_window[1];
			return half;
		}
	}
#endif
	if (!isaac_coloroffset_plain_link_matches(p, s))
		goto fallback;
	for (uint32_t i = 0; i < s->count; ++i) {
		entry = &s->entries[i];
		if (entry->blend_raw == blend_info.raw && entry->output_format == output &&
				entry->multisample_mode == (uint8_t)msaa_mode)
			goto selected;
	}
	if (s->count >= ISAAC_COLOROFFSET_PLAIN_CACHE_CAPACITY)
		goto fallback;
	entry = &s->entries[s->count];
	if (sceGxmShaderPatcherCreateFragmentProgram(gxm_shader_patcher, s->id,
			(SceGxmOutputRegisterFormat)output, msaa_mode, &blend_info.info,
			p->vshader->prog, &entry->fragment))
		goto fallback;
	entry->blend_raw = blend_info.raw;
	entry->output_format = output;
	entry->multisample_mode = (uint8_t)msaa_mode;
	++s->count;
selected:
	++isaac_coloroffset_plain_window[1];
	return entry->fragment;
fallback:
	++isaac_coloroffset_plain_window[2];
	return NULL;
}

#endif
