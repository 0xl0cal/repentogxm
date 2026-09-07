/* Exact no-op suppression at vitaGL's native Transform write boundary.
 * No guest-pointer cache, float comparison, reflection scan or GPU-pool read. */
#ifndef ISAAC_COLOROFFSET_TRANSFORM_UNIFORM_H
#define ISAAC_COLOROFFSET_TRANSFORM_UNIFORM_H

#if !defined(HAVE_ISAAC_COLOROFFSET_TRANSFORM_UNIFORM) || HAVE_ISAAC_COLOROFFSET_TRANSFORM_UNIFORM != 1
#error "Isaac Transform no-op suppression requires its opt-in flag"
#endif
#if !defined(HAVE_ISAAC_COLOROFFSET_PLAIN_VERTEX_PAIR) || HAVE_ISAAC_COLOROFFSET_PLAIN_VERTEX_PAIR != 1
#error "Isaac Transform no-op suppression requires the authenticated plain vertex pair"
#endif

/* Caller has already rejected dirty buffers, non-single matrices, transpose
 * and nonzero STRICT_UNIFORMS offsets. Do not move that cheap rejection here:
 * already-dirty updates must retain the existing write path without a scan.
 * The location has already been decoded by the native uniform API. */
static inline GLboolean isaac_coloroffset_transform_unchanged(
		const uniform *u, const GLfloat *value) {
	program *p;
	const isaac_coloroffset_plain_vertex_pair *pair;
	int32_t vertex_index;
#ifdef HAVE_FFP_SHADER_SUPPORT
	if (dirty_vert_unifs)
		return GL_FALSE;
#endif
	if (!cur_program || cur_program > MAX_CUSTOM_PROGRAMS || !u || !value)
		return GL_FALSE;
	p = &progs[cur_program - 1u];
	pair = p->isaac_plain_vertex_pair;
	/* Pointer equality authenticates the existing native uniform object; it
	 * does not infer bytes from a reused client/source address. Require the
	 * sole vertex uniform at the beginning of this program's 64-byte buffer. */
	if (!pair || !pair->vertex || p->vert_uniforms_num != 1u ||
			u != p->vert_uniforms || !p->unif_vbuffer ||
			u->type != UNIFORM_DATA || u->vptr != p->unif_vbuffer || u->fptr ||
			!u->ptr || pair->plain != p->isaac_coloroffset_plain ||
			!isaac_coloroffset_plain_link_matches(p, pair->plain) ||
			p->vshader->unif_buf_size != 64u)
		return GL_FALSE;
	/* Draw-time P5 selection normally inherits this live-VS-generation check
	 * from the shared draw proof. A uniform setter has no draw proof yet. */
	vertex_index = isaac_coloroffset_shader_index(p->vshader);
	if (vertex_index < 0 || !pair->plain->vertex_generation ||
			isaac_coloroffset_source_generation[vertex_index] !=
				pair->plain->vertex_generation ||
			sceGxmProgramParameterGetType(u->ptr) != SCE_GXM_PARAMETER_TYPE_F32)
		return GL_FALSE;
	/* Pinned glUniformMatrix4fv(count=1, transpose=false, offs=0) reaches
	 * vglSetUniformData with F32/count=4/components=4: precisely memcpy64.
	 * Compare those same bytes, preserving NaN payloads and signed zero.
	 * This is the CPU-side uniform shadow, not the submitted pool copy at
	 * vgl_def_vert_buf. */
	return memcmp(u->vptr, value, 64u) == 0 ? GL_TRUE : GL_FALSE;
}

#endif
