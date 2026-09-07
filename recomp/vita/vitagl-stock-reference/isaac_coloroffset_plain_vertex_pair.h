/* Optional 0015. Private fixed-layout VS paired with the existing P2 GXP.
 * The caller retains the original all-vertex PLAIN and metadata proof. */
#ifndef ISAAC_COLOROFFSET_PLAIN_VERTEX_PAIR_H
#define ISAAC_COLOROFFSET_PLAIN_VERTEX_PAIR_H

static const char isaac_plain_vertex_source[] =
	"attribute vec3 Position;\nattribute vec4 Color;\nattribute vec2 TexCoord;\n"
	"varying vec4 Color0;\nvarying vec2 TexCoord0;\nuniform mat4 Transform;\n"
	"void main(void) {\nColor0 = Color;\n"
	"gl_Position = Transform * vec4(Position.xyz, 1.0);\n"
	"TexCoord0 = TexCoord;\n}\n";

struct isaac_coloroffset_plain_vertex_pair {
	const isaac_coloroffset_plain_state *plain;
	SceGxmShaderPatcherId vertex_id;
	const SceGxmProgram *vertex_gxp;
	SceGxmVertexProgram *vertex;
	uint32_t count;
	isaac_coloroffset_plain_entry entries[ISAAC_COLOROFFSET_PLAIN_CACHE_CAPACITY];
};

/* Optional private ABI 1, exactly three uint32_t words. q = proven PLAIN
 * draw requests reaching pair selection; b = both stages selected for the
 * final binds; f = requests returning to P2/original VS, hence q == b+f.
 * This counts selection, not success of the native context-bind APIs.
 * Take-and-zero is single-render-thread; invalid arguments do not consume. */
static uint32_t isaac_plain_vertex_pair_window[3];
uint32_t vglTakeIsaacColorOffsetPlainVertexPairStats(uint32_t *out, uint32_t words) {
	if (!out || words != 3u)
		return 0u;
	vgl_fast_memcpy(out, isaac_plain_vertex_pair_window, sizeof isaac_plain_vertex_pair_window);
	vgl_memset(isaac_plain_vertex_pair_window, 0, sizeof isaac_plain_vertex_pair_window);
	return 1u;
}

/* SceGxmProgram.varyings_offset is at 0x2c, relative to that field;
 * SceGxmProgramVertexVaryings has output1/output2/pack at +16/+20/+24.
 * Copied from Vita3K gxm/types.h and gxp.cpp:get_vertex_outputs. Each
 * TEXCOORD's output width occupies three bits. Keep the original first two
 * TEXCOORD widths/precision exactly and reject every other VS output. */
static int isaac_plain_vertex_outputs(const SceGxmProgram *gxp, uint32_t size,
		uint32_t out[3]) {
	const uint8_t *bytes = (const uint8_t *)gxp;
	uint32_t relative;
	if (!gxp || size < 0x9cu)
		return 0;
	relative = isaac_coloroffset_load_u32(bytes + 0x2cu);
	if (relative > size - 0x2cu - 32u)
		return 0;
	bytes += 0x2cu + relative;
	for (uint32_t i = 0; i < 3u; ++i)
		out[i] = isaac_coloroffset_load_u32(bytes + 16u + 4u * i);
	return 1;
}

/* Hardware capture (064366b): authenticated stock VS
 * outputs=18001000,000c97cf,0; private VS=0a001000,0000000f,0.
 * output1[31:24] is the output-register count, not semantic flags:
 * psp2spvc 1c296ab builder.cpp registerVertexVaryings emits count << 24.
 * Support ONLY this observed F32 layout: Position4 + Color0[4] + UV[2]
 * = 10 registers, replacing the authenticated stock count of 24. Keep
 * every low24 bit equal, including undocumented bits; do not mask them.
 * TC codes 7/1 and zero packing keep the count proof independent of any
 * general half/packed-output interpretation. Other layouts still fail. */
static uint32_t isaac_plain_vertex_output_mismatch(const uint32_t old[3],
		const uint32_t candidate[3]) {
	uint32_t mask = 0u;
	if ((candidate[0] & 0x00ffffffu) != (old[0] & 0x00ffffffu)) mask |= 4u;
	if ((old[0] >> 24u) != 24u) mask |= 32u;
	if ((candidate[0] >> 24u) != (4u + 4u + 2u)) mask |= 64u;
	if ((old[1] & 0x3fu) != 0x0fu || candidate[1] != (old[1] & 0x3fu)) mask |= 8u;
	if ((old[2] & 3u) != 0u || candidate[2] != (old[2] & 3u)) mask |= 16u;
	return mask;
}

static int isaac_plain_vertex_abi(const SceGxmProgram *candidate, uint32_t size,
		const shader *stock, SceGxmVertexAttribute attrs[3]) {
	static const char *const names[3] = {"Position", "Color", "TexCoord"};
	static const uint16_t offsets[3] = {0u, 12u, 28u};
	static const uint8_t components[3] = {3u, 4u, 2u};
	const SceGxmProgramParameter *transform;
	uint32_t old_outputs[3], new_outputs[3];
	if (!candidate || size < 0x9cu || !stock || !stock->prog ||
			stock->unif_buf_size != 64u || sceGxmProgramCheck(candidate) != 0 ||
			sceGxmProgramGetType(candidate) != SCE_GXM_VERTEX_PROGRAM ||
			sceGxmProgramGetSize(candidate) < 0x9cu ||
			sceGxmProgramGetSize(candidate) > size ||
			sceGxmProgramGetParameterCount(candidate) != 4u ||
			sceGxmProgramGetDefaultUniformBufferSize(candidate) != 64u ||
			!isaac_plain_vertex_outputs(candidate, sceGxmProgramGetSize(candidate), new_outputs) ||
			!isaac_plain_vertex_outputs(stock->prog, sceGxmProgramGetSize(stock->prog), old_outputs) ||
			isaac_plain_vertex_output_mismatch(old_outputs, new_outputs))
		return 0;
	transform = sceGxmProgramFindParameterByName(candidate, "Transform");
	if (!transform || sceGxmProgramParameterGetCategory(transform) != SCE_GXM_PARAMETER_CATEGORY_UNIFORM ||
			sceGxmProgramParameterGetType(transform) != SCE_GXM_PARAMETER_TYPE_F32 ||
			sceGxmProgramParameterGetResourceIndex(transform) != 0u ||
			sceGxmProgramParameterGetComponentCount(transform) != 4u ||
			sceGxmProgramParameterGetArraySize(transform) != 4u ||
			sceGxmProgramParameterGetContainerIndex(transform) != 14u)
		return 0;
	vgl_memset(attrs, 0, sizeof(*attrs) * 3u);
	for (uint32_t i = 0; i < 3u; ++i) {
		const SceGxmProgramParameter *param = sceGxmProgramFindParameterByName(candidate, names[i]);
		if (!param || sceGxmProgramParameterGetCategory(param) != SCE_GXM_PARAMETER_CATEGORY_ATTRIBUTE ||
				sceGxmProgramParameterGetType(param) != SCE_GXM_PARAMETER_TYPE_F32 ||
				sceGxmProgramParameterGetArraySize(param) != 1u ||
				sceGxmProgramParameterGetComponentCount(param) != 4u ||
				sceGxmProgramParameterGetResourceIndex(param) != i * 4u)
			return 0;
		attrs[i].streamIndex = 0;
		attrs[i].offset = offsets[i];
		attrs[i].format = SCE_GXM_ATTRIBUTE_FORMAT_F32;
		attrs[i].componentCount = components[i];
		attrs[i].regIndex = (uint16_t)sceGxmProgramParameterGetResourceIndex(param);
	}
	return 1;
}

/* Failure-only, once per process. This reports native metadata before the
 * candidate copy is freed; it does not participate in ABI acceptance.
 * Each physical record is bounded well below the 512-byte debug formatter. */
static uint8_t isaac_plain_vertex_abi_reported;
static uint32_t isaac_plain_vertex_abi_param_diag(uint32_t prog,
		const SceGxmProgram *candidate, const char *name, uint32_t index) {
	const SceGxmProgramParameter *param = sceGxmProgramFindParameterByName(candidate, name);
	uint32_t values[6] = {0u, 0u, 0u, 0u, 0u, 0u};
	uint32_t want_category = index == 3u ? SCE_GXM_PARAMETER_CATEGORY_UNIFORM : SCE_GXM_PARAMETER_CATEGORY_ATTRIBUTE;
	uint32_t want_resource = index == 3u ? 0u : index * 4u;
	uint32_t want_array = index == 3u ? 4u : 1u;
	uint32_t mask = param ? 0u : 64u;
	if (param) {
		values[0] = sceGxmProgramParameterGetCategory(param);
		values[1] = sceGxmProgramParameterGetType(param);
		values[2] = sceGxmProgramParameterGetResourceIndex(param);
		values[3] = sceGxmProgramParameterGetComponentCount(param);
		values[4] = sceGxmProgramParameterGetArraySize(param);
		values[5] = sceGxmProgramParameterGetContainerIndex(param);
		if (values[0] != want_category) mask |= 1u;
		if (values[1] != SCE_GXM_PARAMETER_TYPE_F32) mask |= 2u;
		if (values[2] != want_resource) mask |= 4u;
		if (values[3] != 4u) mask |= 8u;
		if (values[4] != want_array) mask |= 16u;
		if (index == 3u && values[5] != 14u) mask |= 32u;
	}
	sceClibPrintf("KAGE VITA PLAIN VERTEX PAIR ABI param: prog=%u name=%s mask=%02x got(c,t,r,n,a,b)=%u,%u,%u,%u,%u,%u want=%u,%u,%u,4,%u,%s\n",
		prog, name, mask, values[0], values[1], values[2], values[3], values[4], values[5],
		want_category, (unsigned)SCE_GXM_PARAMETER_TYPE_F32, want_resource, want_array,
		index == 3u ? "14" : "any");
	return mask;
}
static void isaac_plain_vertex_abi_diag(program *p,
		const SceGxmProgram *candidate, uint32_t available) {
	const shader *stock = p->vshader;
	uint32_t prog = (uint32_t)(p - progs) + 1u;
	uint32_t mask = 0u, native_size = 0u, type = 0u, count = 0u, ub = 0u;
	uint32_t old_outputs[3] = {0u, 0u, 0u}, new_outputs[3] = {0u, 0u, 0u};
	uint32_t old_size = 0u, old_valid = 0u, new_valid = 0u, out_mask = 0u;
	int check = -1;
	if (isaac_plain_vertex_abi_reported)
		return;
	isaac_plain_vertex_abi_reported = 1u;
	if (!candidate || available < 0x9cu) mask |= 1u;
	if (!stock || !stock->prog) mask |= 2u;
	if (stock && stock->unif_buf_size != 64u) mask |= 4u;
	if (!(mask & 1u)) {
		check = sceGxmProgramCheck(candidate);
		if (check) mask |= 8u;
		else {
			native_size = sceGxmProgramGetSize(candidate);
			type = sceGxmProgramGetType(candidate);
			count = sceGxmProgramGetParameterCount(candidate);
			ub = sceGxmProgramGetDefaultUniformBufferSize(candidate);
			if (type != SCE_GXM_VERTEX_PROGRAM) mask |= 16u;
			if (native_size < 0x9cu || native_size > available) mask |= 32u;
			if (count != 4u) mask |= 64u;
			if (ub != 64u) mask |= 128u;
		}
	}
	sceClibPrintf("KAGE VITA PLAIN VERTEX PAIR ABI header: prog=%u mask=%02x available=%u size=%u check=%08x type=%u params=%u ub(candidate,stock)=%u,%u\n",
		prog, mask, available, native_size, (unsigned)check, type, count, ub,
		stock ? stock->unif_buf_size : 0u);
	/* Follow the original guard's short-circuit boundaries. Do not inspect
	 * parameter names if its header/output checks already explain refusal. */
	if (mask)
		return;
	old_size = sceGxmProgramGetSize(stock->prog);
	old_valid = isaac_plain_vertex_outputs(stock->prog, old_size, old_outputs);
	new_valid = isaac_plain_vertex_outputs(candidate, native_size, new_outputs);
	if (!new_valid) out_mask |= 1u;
	if (!old_valid) out_mask |= 2u;
	if (new_valid && old_valid)
		out_mask |= isaac_plain_vertex_output_mismatch(old_outputs, new_outputs);
	sceClibPrintf("KAGE VITA PLAIN VERTEX PAIR ABI outputs: prog=%u mask=%02x valid(new,old)=%u,%u stock-size=%u old=%08x,%08x,%08x new=%08x,%08x,%08x want=%08x,%08x,%08x\n",
		prog, out_mask, new_valid, old_valid, old_size,
		old_outputs[0], old_outputs[1], old_outputs[2],
		new_outputs[0], new_outputs[1], new_outputs[2],
		((4u + 4u + 2u) << 24u) | (old_outputs[0] & 0x00ffffffu),
		old_outputs[1] & 0x3fu, old_outputs[2] & 3u);
	if (out_mask)
		return;
	/* Same order as acceptance: stop at Transform or the first bad attribute.
	 * Each named parameter below was already visited by the failing guard. */
	if (isaac_plain_vertex_abi_param_diag(prog, candidate, "Transform", 3u)) return;
	if (isaac_plain_vertex_abi_param_diag(prog, candidate, "Position", 0u)) return;
	if (isaac_plain_vertex_abi_param_diag(prog, candidate, "Color", 1u)) return;
	(void)isaac_plain_vertex_abi_param_diag(prog, candidate, "TexCoord", 2u);
}

/* The caller has finished the context. Release fragments before either of
 * their registered parent programs. No general p->isaac_vertex_cache entry
 * owns this private vertex program. */
static void isaac_plain_vertex_pair_release(program *p) {
	isaac_coloroffset_plain_vertex_pair *s = p->isaac_plain_vertex_pair;
	if (!s)
		return;
	ISAAC_GXM_SHADER_STATE_INVALIDATE();
	for (uint32_t i = 0; i < s->count; ++i)
		sceGxmShaderPatcherReleaseFragmentProgram(gxm_shader_patcher, s->entries[i].fragment);
	if (s->vertex)
		sceGxmShaderPatcherReleaseVertexProgram(gxm_shader_patcher, s->vertex);
	if (s->vertex_id)
		sceGxmShaderPatcherUnregisterProgram(gxm_shader_patcher, s->vertex_id);
	vgl_free((void *)s->vertex_gxp);
	vgl_free(s);
	p->isaac_plain_vertex_pair = NULL;
}

static void isaac_plain_vertex_pair_before_link(program *p) {
	if (p->isaac_plain_vertex_pair &&
			!isaac_coloroffset_plain_link_matches(p, p->isaac_coloroffset_plain)) {
		sceGxmFinish(gxm_context);
		isaac_plain_vertex_pair_release(p);
	}
}

static void isaac_plain_vertex_pair_link(program *p) {
	isaac_coloroffset_plain_vertex_pair *s;
	shader probe, partner;
	const SceGxmProgram *compiled;
	SceGxmProgram *copy = NULL;
	SceGxmVertexAttribute attrs[3];
	SceGxmVertexStream stream = {88u, SCE_GXM_INDEX_SOURCE_INDEX_16BIT};
	GLenum saved_sema;
	GLboolean saved_first;
	binds_map saved_bindings;
	const char *failure = "allocation";
	uint32_t size = 0u, hash = 0u;
	if (p->isaac_plain_vertex_pair ||
			!isaac_coloroffset_plain_link_matches(p, p->isaac_coloroffset_plain))
		return;
	s = vglMalloc(sizeof(*s));
	if (!s)
		goto report;
	vgl_memset(s, 0, sizeof(*s));
	if (!is_shark_online && !start_shader_compiler()) {
		failure = "compiler";
		goto fail;
	}
	vgl_memset(&probe, 0, sizeof probe);
	probe.type = GL_VERTEX_SHADER;
	probe.valid = GL_TRUE;
	probe.is_glsl = GL_TRUE;
	probe.size = sizeof isaac_plain_vertex_source - 1u;
	probe.source = vglMalloc(probe.size + 1u);
	if (!probe.source)
		goto fail;
	vgl_fast_memcpy(probe.source, isaac_plain_vertex_source, probe.size + 1u);
	/* P2 compiled its FS against this exact original semantic map. Seed the
	 * new VS from that same map instead of renumbering Color0/UV by name. */
	vgl_memset(&partner, 0, sizeof partner);
	partner.prog = p->isaac_coloroffset_plain->gxp;
	partner.semantics = p->vshader->semantics;
	saved_sema = glsl_sema_mode;
	saved_first = glsl_is_first_shader;
	saved_bindings = glsl_bindings_map;
	glsl_sema_mode = VGL_MODE_SHADER_PAIR;
	glsl_translator_set_process(&probe, &partner);
	glsl_sema_mode = saved_sema;
	glsl_is_first_shader = saved_first;
	glsl_bindings_map = saved_bindings;
	compiled = shark_compile_shader_extended((const char *)probe.source,
		&probe.size, SHARK_VERTEX_SHADER, compiler_opts, compiler_fastmath,
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
	/* Preserve actual compiler artefact identity even if ABI/native linking
	 * refuses it. ready still means the complete pair was published. */
	size = probe.size;
	hash = isaac_coloroffset_fnv1a(copy, size);
	failure = "abi";
	if (!isaac_plain_vertex_abi(copy, probe.size, p->vshader, attrs)) {
		isaac_plain_vertex_abi_diag(p, copy, probe.size);
		goto fail;
	}
	failure = "register";
	if (sceGxmShaderPatcherRegisterProgram(gxm_shader_patcher, copy, &s->vertex_id))
		goto fail;
	failure = "vertex";
	ISAAC_GXM_SHADER_STATE_INVALIDATE();
	if (sceGxmShaderPatcherCreateVertexProgram(gxm_shader_patcher,
			s->vertex_id, attrs, 3u, &stream, 1u, &s->vertex) || !s->vertex) {
		sceGxmShaderPatcherUnregisterProgram(gxm_shader_patcher, s->vertex_id);
		goto fail;
	}
	s->vertex_gxp = copy;
	s->plain = p->isaac_coloroffset_plain;
	p->isaac_plain_vertex_pair = s;
	size = probe.size;
	hash = isaac_coloroffset_fnv1a(copy, size);
	failure = "none";
	goto report;
fail:
	vgl_free(copy);
	vgl_free(s);
report:
	sceClibPrintf("KAGE VITA PLAIN VERTEX PAIR: prog=%u ready=%u gxp=%u/%08x fail=%s\n",
		(uint32_t)(p - progs) + 1u, (unsigned)(p->isaac_plain_vertex_pair != NULL), size, hash, failure);
}

/* Success produces both stages. Failure leaves *vertex NULL, so the caller
 * must keep P2 with its original VS. No native context bind happens here. */
static SceGxmFragmentProgram *isaac_plain_vertex_pair_select(program *p,
		SceGxmVertexProgram **vertex) {
	isaac_coloroffset_plain_vertex_pair *s = p->isaac_plain_vertex_pair;
	isaac_coloroffset_plain_entry *entry;
	*vertex = NULL;
	++isaac_plain_vertex_pair_window[0];
	if (!s || s->plain != p->isaac_coloroffset_plain ||
			!isaac_coloroffset_plain_link_matches(p, p->isaac_coloroffset_plain) ||
			is_fbo_float)
		goto fallback;
	for (uint32_t i = 0; i < s->count; ++i) {
		entry = &s->entries[i];
		if (entry->blend_raw == blend_info.raw &&
				entry->multisample_mode == (uint8_t)msaa_mode)
			goto selected;
	}
	if (s->count >= ISAAC_COLOROFFSET_PLAIN_CACHE_CAPACITY)
		goto fallback;
	entry = &s->entries[s->count];
	ISAAC_GXM_SHADER_STATE_INVALIDATE();
	if (sceGxmShaderPatcherCreateFragmentProgram(gxm_shader_patcher,
			s->plain->id, SCE_GXM_OUTPUT_REGISTER_FORMAT_UCHAR4, msaa_mode,
			&blend_info.info, s->vertex_gxp, &entry->fragment) || !entry->fragment)
		goto fallback;
	entry->blend_raw = blend_info.raw;
	entry->multisample_mode = (uint8_t)msaa_mode;
	++s->count;
selected:
	*vertex = s->vertex;
	++isaac_plain_vertex_pair_window[1];
	/* P2's existing plain q/b still counts the whole plain family once. */
	++isaac_coloroffset_plain_window[0];
	++isaac_coloroffset_plain_window[1];
	return entry->fragment;
fallback:
	++isaac_plain_vertex_pair_window[2];
	return NULL;
}

#endif
