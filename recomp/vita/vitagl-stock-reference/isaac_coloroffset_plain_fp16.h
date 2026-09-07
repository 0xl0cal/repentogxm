#ifndef ISAAC_COLOROFFSET_PLAIN_FP16_H
#define ISAAC_COLOROFFSET_PLAIN_FP16_H

#if !defined(HAVE_ISAAC_COLOROFFSET_PLAIN_FP16) || HAVE_ISAAC_COLOROFFSET_PLAIN_FP16 != 1
#error "Private P2 FP16 candidate requires its opt-in flag"
#endif
#ifdef HAVE_ISAAC_COLOROFFSET_PLAIN_VERTEX_PAIR
#error "Private P2 FP16 candidate is isolated from the paired vertex candidate"
#endif

/* Private P2 source only. All declarations/UVs remain float; only the two
 * local colours are half. The pinned translator skips compound *= instead
 * of rewriting it to its float-only vglMul overloads (glsl_utils.c:922).
 * No global precision/translator/compiler setting is changed. */
static const char isaac_coloroffset_plain_fp16_source[] =
	"#ifdef GL_ES\nprecision highp float;\n#endif\n"
	"#if __VERSION__ >= 140\n"
	"in vec4 Color0;\nin vec2 TexCoord0;\nout vec4 fragColor;\n"
	"#else\nvarying vec4 Color0;\nvarying vec2 TexCoord0;\n"
	"#define fragColor gl_FragColor\n#define texture texture2D\n#endif\n"
	"uniform sampler2D Texture0;\n"
	"void main(void) {\n"
	" half4 sampleColor = half4(texture(Texture0, TexCoord0));\n"
	" half4 tintColor = half4(Color0);\n"
	" tintColor *= sampleColor;\n"
	" fragColor = vec4(tintColor);\n}\n";

/* Private ABI 1, exactly two uint32 words, single render thread.
 * q = proven PLAIN requests consulting this option; b = half fragment
 * selected for the final bind. q-b = requests using the unchanged baseline
 * selection/fallback path. This is not native-bind return-status telemetry. */
static uint32_t isaac_coloroffset_plain_fp16_window[2];
uint32_t vglTakeIsaacColorOffsetPlainFp16Stats(uint32_t *out, uint32_t words) {
	if (!out || words != 2u)
		return 0u;
	vgl_fast_memcpy(out, isaac_coloroffset_plain_fp16_window, sizeof isaac_coloroffset_plain_fp16_window);
	vgl_memset(isaac_coloroffset_plain_fp16_window, 0, sizeof isaac_coloroffset_plain_fp16_window);
	return 1u;
}

/* Byte views, not a guessed native C struct. Layout is published in
 * Vita3K gxm/types.h: SceGxmProgram (varyings relative field at 0x2c),
 * SceGxmProgramVertexVaryings (32 bytes, count at +12, relative descriptor
 * field at +16) and SceGxmProgramAttributeDescriptor (four uint32 fields).
 * Comparing ALL descriptor bytes preserves attribute_info, resource_index,
 * size and component_info, including iterator width/precision/register map.
 * Unknown block fields are compared, not masked by an assumed meaning.
 * Only the descriptor table's relocated address is excluded. */
typedef struct {
	const uint8_t *block;
	const uint8_t *descriptors;
	uint16_t count;
} isaac_plain_fp16_interface;

static int isaac_plain_fp16_interface_view(const SceGxmProgram *gxp,
		uint32_t available, isaac_plain_fp16_interface *view) {
	const uint8_t *bytes = (const uint8_t *)gxp;
	uint32_t size, relative, block, table_field, table;
	uint16_t count;
	if (!gxp || !view || available < 0x9cu)
		return 0;
	size = isaac_coloroffset_load_u32(bytes + 8u);
	if (size < 0x9cu || size > available)
		return 0;
	relative = isaac_coloroffset_load_u32(bytes + 0x2cu);
	if (!relative || relative > size - 0x2cu - 32u)
		return 0;
	block = 0x2cu + relative;
	vgl_fast_memcpy(&count, bytes + block + 12u, sizeof count);
	if (!count || count > 32u)
		return 0; /* Bounded supported interface, not a generic GXP decoder. */
	table_field = block + 16u;
	relative = isaac_coloroffset_load_u32(bytes + table_field);
	if (!relative || relative > size - table_field)
		return 0;
	table = table_field + relative;
	if ((uint32_t)count > (size - table) / 16u)
		return 0;
	view->block = bytes + block;
	view->descriptors = bytes + table;
	view->count = count;
	return 1;
}

static int isaac_plain_fp16_interface_matches(const SceGxmProgram *candidate,
		uint32_t candidate_size, const SceGxmProgram *baseline,
		uint32_t baseline_size) {
	isaac_plain_fp16_interface a, b;
	if (!isaac_plain_fp16_interface_view(candidate, candidate_size, &a) ||
			!isaac_plain_fp16_interface_view(baseline, baseline_size, &b) ||
			a.count != b.count)
		return 0;
	/* Program/buffer/texture-unit flags have the same external behavior;
	 * code size, code offsets and temporary-register counts may differ. */
	return memcmp((const uint8_t *)candidate + 0x14u,
			(const uint8_t *)baseline + 0x14u, 16u) == 0 &&
		memcmp(a.block, b.block, 16u) == 0 &&
		memcmp(a.block + 20u, b.block + 20u, 12u) == 0 &&
		memcmp(a.descriptors, b.descriptors, (size_t)a.count * 16u) == 0;
}

/* Same caller-owned Finish boundary as the parent P2 release. */
static void isaac_coloroffset_plain_fp16_release(isaac_coloroffset_plain_state *s) {
	if (!s->fp16_id)
		return;
	ISAAC_GXM_SHADER_STATE_INVALIDATE();
	for (uint32_t i = 0; i < s->fp16_count; ++i)
		sceGxmShaderPatcherReleaseFragmentProgram(gxm_shader_patcher, s->fp16_entries[i].fragment);
	sceGxmShaderPatcherUnregisterProgram(gxm_shader_patcher, s->fp16_id);
	vgl_free((void *)s->fp16_gxp);
	s->fp16_id = NULL;
	s->fp16_gxp = NULL;
	s->fp16_count = 0u;
}

static void isaac_coloroffset_plain_fp16_link(program *p) {
	isaac_coloroffset_plain_state *s = p->isaac_coloroffset_plain;
	shader probe;
	const SceGxmProgram *compiled;
	SceGxmProgram *copy = NULL;
	SceGxmShaderPatcherId id = NULL;
	GLenum saved_sema;
	GLboolean saved_first;
	binds_map saved_bindings;
	const char *failure = "allocation";
	uint32_t size = 0u, hash = 0u;
	if (!isaac_coloroffset_plain_link_matches(p, s) || s->fp16_id)
		return;
	if (!is_shark_online && !start_shader_compiler()) {
		failure = "compiler";
		goto report;
	}
	vgl_memset(&probe, 0, sizeof probe);
	probe.type = GL_FRAGMENT_SHADER;
	probe.valid = GL_TRUE;
	probe.is_glsl = GL_TRUE;
	probe.size = sizeof isaac_coloroffset_plain_fp16_source - 1u;
	probe.source = vglMalloc(probe.size + 1u);
	if (!probe.source)
		goto report;
	vgl_fast_memcpy(probe.source, isaac_coloroffset_plain_fp16_source, probe.size + 1u);
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
	failure = "abi";
	if (!isaac_coloroffset_fs_probe_program_is_exact(copy, probe.size, p->fshader) ||
			!isaac_plain_fp16_interface_matches(copy, probe.size, s->gxp, s->gxp_size))
		goto fail;
	failure = "register";
	if (sceGxmShaderPatcherRegisterProgram(gxm_shader_patcher, copy, &id) || !id)
		goto fail;
	s->fp16_id = id;
	s->fp16_gxp = copy;
	size = probe.size;
	hash = isaac_coloroffset_fnv1a(copy, size);
	failure = "none";
	goto report;
fail:
	vgl_free(copy);
report:
	sceClibPrintf("KAGE VITA PLAIN FP16: prog=%u ready=%u base=%u/%08x half=%u/%08x fail=%s\n",
		(uint32_t)(p - progs) + 1u, (unsigned)(s->fp16_id != NULL),
		s->gxp_size, isaac_coloroffset_fnv1a(s->gxp, s->gxp_size), size, hash, failure);
}

static SceGxmFragmentProgram *isaac_coloroffset_plain_fp16_select(program *p) {
	isaac_coloroffset_plain_state *s = p->isaac_coloroffset_plain;
	isaac_coloroffset_plain_entry *entry;
	++isaac_coloroffset_plain_fp16_window[0];
	if (!isaac_coloroffset_plain_link_matches(p, s) || !s->fp16_id || is_fbo_float)
		return NULL;
	for (uint32_t i = 0; i < s->fp16_count; ++i) {
		entry = &s->fp16_entries[i];
		if (entry->blend_raw == blend_info.raw &&
				entry->multisample_mode == (uint8_t)msaa_mode)
			goto selected;
	}
	if (s->fp16_count >= ISAAC_COLOROFFSET_PLAIN_CACHE_CAPACITY)
		return NULL;
	entry = &s->fp16_entries[s->fp16_count];
	ISAAC_GXM_SHADER_STATE_INVALIDATE();
	if (sceGxmShaderPatcherCreateFragmentProgram(gxm_shader_patcher, s->fp16_id,
			SCE_GXM_OUTPUT_REGISTER_FORMAT_UCHAR4, msaa_mode, &blend_info.info,
			p->vshader->prog, &entry->fragment) || !entry->fragment)
		return NULL;
	entry->blend_raw = blend_info.raw;
	entry->multisample_mode = (uint8_t)msaa_mode;
	++s->fp16_count;
selected:
	++isaac_coloroffset_plain_fp16_window[1];
	return entry->fragment;
}
#endif
