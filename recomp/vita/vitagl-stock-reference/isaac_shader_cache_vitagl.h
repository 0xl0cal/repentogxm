#ifndef ISAAC_VITAGL_SHADER_CACHE_INTEGRATION_H
#define ISAAC_VITAGL_SHADER_CACHE_INTEGRATION_H

#ifdef HAVE_SHADER_CACHE
#error "Isaac hardened shader cache must not be combined with HAVE_SHADER_CACHE"
#endif

#include "isaac_shader_cache_policy.h"
#include "isaac_shader_cache_block_list.h"

_Static_assert(UBOS_NUM == ISAAC_SHADER_CACHE_BLOCK_MAX,
	"Isaac shader-cache UBO bound drifted from vitaGL");
_Static_assert(sizeof(((block_uniform *)0)->name) ==
		ISAAC_SHADER_CACHE_BLOCK_NAME_SIZE,
	"Isaac shader-cache block name size drifted from vitaGL");
_Static_assert(sizeof(((block_uniform *)0)->idx) == 1u,
	"Isaac shader-cache block index size drifted from vitaGL");

#define ISAAC_SHADER_CACHE_BLOCK_SCAN_MAX 64u

typedef struct isaac_shader_source_identity {
	uint32_t size;
	uint8_t sha256[32];
	GLboolean valid;
} isaac_shader_source_identity;

static isaac_shader_source_identity
	isaac_shader_sources[MAX_CUSTOM_SHADERS];

static int32_t isaac_shader_cache_index(const shader *s) {
	uintptr_t address = (uintptr_t)s;
	uintptr_t base = (uintptr_t)&shaders[0];
	uintptr_t limit = (uintptr_t)&shaders[MAX_CUSTOM_SHADERS];
	uintptr_t offset;
	if (address < base || address >= limit)
		return -1;
	offset = address - base;
	if (offset % sizeof(shader))
		return -1;
	return (int32_t)(offset / sizeof(shader));
}

static void isaac_shader_cache_forget_source(shader *s) {
	int32_t index = isaac_shader_cache_index(s);
	if (index >= 0)
		memset(&isaac_shader_sources[index], 0,
			sizeof isaac_shader_sources[index]);
}

static void isaac_shader_cache_capture_source(shader *s) {
	int32_t index = isaac_shader_cache_index(s);
	isaac_shader_source_identity *identity;
	if (index < 0)
		return;
	identity = &isaac_shader_sources[index];
	memset(identity, 0, sizeof *identity);
	if (!s->source || !s->size)
		return;
	identity->size = s->size;
	isaac_shader_cache_hash_timed(
		s->source, s->size, identity->sha256);
	identity->valid = GL_TRUE;
}

static const isaac_shader_source_identity *
isaac_shader_cache_source(const shader *s) {
	int32_t index = isaac_shader_cache_index(s);
	if (index < 0 || !isaac_shader_sources[index].valid)
		return NULL;
	return &isaac_shader_sources[index];
}

static uint32_t isaac_shader_cache_compiler_flags(void) {
	return ((uint32_t)compiler_opts & 0xffu) |
		(compiler_fastmath ? 0x00000100u : 0u) |
		(compiler_fastprecision ? 0x00000200u : 0u) |
		(compiler_fastint ? 0x00000400u : 0u);
}

static uint32_t isaac_shader_cache_type(const shader *s) {
	return s->type == GL_VERTEX_SHADER
		? ISAAC_SHADER_CACHE_VERTEX : ISAAC_SHADER_CACHE_FRAGMENT;
}

static int isaac_shader_cache_key_for(
		isaac_shader_cache_key *key, const shader *s,
		const shader *peer, uint32_t translator_mode,
		GLboolean save_bindings) {
	const isaac_shader_source_identity *source =
		isaac_shader_cache_source(s);
	const isaac_shader_source_identity *peer_source = peer
		? isaac_shader_cache_source(peer) : NULL;
	uint32_t flags = save_bindings
		? ISAAC_SHADER_CACHE_FLAG_BINDINGS : 0u;
	if (s->is_glsl)
		flags |= ISAAC_SHADER_CACHE_FLAG_GLSL;
	if (!source || (peer && !peer_source))
		return 0;
	if (peer) {
		flags |= ISAAC_SHADER_CACHE_FLAG_PAIR;
		if (peer->is_glsl)
			flags |= ISAAC_SHADER_CACHE_FLAG_PEER_GLSL;
	}
	return isaac_shader_cache_make_key(
		key, isaac_shader_cache_type(s), translator_mode,
		isaac_shader_cache_compiler_flags(), flags,
		source->size, source->sha256,
		peer_source ? peer_source->size : 0u,
		peer_source ? peer_source->sha256 : NULL);
}

static int isaac_shader_cache_program_check(void *context,
		const void *program) {
	(void)context;
	return sceGxmProgramCheck((const SceGxmProgram *)program);
}

static uint32_t isaac_shader_cache_program_size(void *context,
		const void *program) {
	(void)context;
	return sceGxmProgramGetSize((const SceGxmProgram *)program);
}

static uint32_t isaac_shader_cache_program_type(void *context,
		const void *program) {
	(void)context;
	return (uint32_t)sceGxmProgramGetType(
		(const SceGxmProgram *)program);
}

/* vitaShaRK returns ShaccCg's serialized programSize, which may include the
 * GXP container's zero-filled end padding.  SceGxmProgram::size deliberately
 * excludes those bytes.  Persist only the canonical embedded size so the
 * cache-hit validator sees the exact value returned by sceGxmProgramGetSize.
 * Accept no other compiler/container disagreement. */
static int isaac_shader_cache_canonical_gxp_size(
		const SceGxmProgram *program, uint32_t storage_size,
		uint32_t *canonical_size) {
	const uint8_t *bytes;
	uint32_t program_size;
	uint32_t padded_size;
	uint32_t index;
	if (!program || !canonical_size)
		return 0;
	program_size = sceGxmProgramGetSize(program);
	if (!program_size || program_size > ISAAC_SHADER_CACHE_GXP_MAX ||
			program_size > UINT32_MAX - 3u ||
			storage_size < program_size ||
			storage_size > ISAAC_SHADER_CACHE_GXP_MAX)
		return 0;
	padded_size = (program_size + 3u) & ~3u;
	if (storage_size != program_size && storage_size != padded_size)
		return 0;
	bytes = (const uint8_t *)program;
	for (index = program_size; index < storage_size; ++index) {
		if (bytes[index] != 0u)
			return 0;
	}
	*canonical_size = program_size;
	return 1;
}

static int isaac_shader_cache_program_register(void *context,
		const void *program, uintptr_t *registered_id) {
	SceGxmShaderPatcherId id = NULL;
	int result;
	(void)context;
	result = sceGxmShaderPatcherRegisterProgram(
		gxm_shader_patcher, (const SceGxmProgram *)program, &id);
	if (!result)
		*registered_id = (uintptr_t)id;
	return result;
}

static void isaac_shader_cache_free_matrices(matrix_uniform *matrices) {
	while (matrices) {
		matrix_uniform *next = (matrix_uniform *)matrices->chain;
		vgl_free(matrices);
		matrices = next;
	}
}

static int isaac_shader_cache_gxp_blocks_valid(
		const SceGxmProgram *program,
		const isaac_shader_cache_block_descriptor *blocks,
		uint32_t block_count) {
	uint32_t parameter_count;
	uint32_t parameter_index;
	uint32_t seen = 0u;
	uint32_t found_count = 0u;
	if (!program || !isaac_shader_cache_blocks_unique(blocks, block_count))
		return 0;
	parameter_count = sceGxmProgramGetParameterCount(program);
	for (parameter_index = 0u; parameter_index < parameter_count;
			++parameter_index) {
		const SceGxmProgramParameter *parameter =
			sceGxmProgramGetParameter(program, parameter_index);
		uint32_t block_index;
		uint32_t descriptor_index;
		const isaac_shader_cache_block_descriptor *descriptor = NULL;
		if (!parameter)
			return 0;
		if (sceGxmProgramParameterGetCategory(parameter) !=
				SCE_GXM_PARAMETER_CATEGORY_UNIFORM_BUFFER)
			continue;
		block_index = sceGxmProgramParameterGetResourceIndex(parameter);
		if (block_index >= ISAAC_SHADER_CACHE_BLOCK_MAX ||
				(seen & (1u << block_index)) != 0u)
			return 0;
		seen |= 1u << block_index;
		for (descriptor_index = 0u; descriptor_index < block_count;
				++descriptor_index) {
			if (blocks[descriptor_index].index == block_index) {
				descriptor = &blocks[descriptor_index];
				break;
			}
		}
		if (!descriptor ||
				sceGxmProgramFindParameterByName(
					program, descriptor->name) != parameter)
			return 0;
		++found_count;
	}
	return found_count == block_count;
}

static int isaac_shader_cache_collect_gxp_blocks(
		const shader *s,
		isaac_shader_cache_block_descriptor blocks[
			ISAAC_SHADER_CACHE_BLOCK_MAX],
		uint32_t *block_count) {
	const block_uniform *by_index[ISAAC_SHADER_CACHE_BLOCK_MAX];
	const SceGxmProgramParameter *gxp_by_index[
		ISAAC_SHADER_CACHE_BLOCK_MAX];
	const block_uniform *block;
	uint32_t scans = 0u;
	uint32_t parameter_count;
	uint32_t parameter_index;
	uint32_t count = 0u;
	uint32_t needed = 0u;
	if (!s || !s->prog || !blocks || !block_count)
		return 0;
	memset(by_index, 0, sizeof by_index);
	memset(gxp_by_index, 0, sizeof gxp_by_index);
	parameter_count = sceGxmProgramGetParameterCount(s->prog);
	for (parameter_index = 0u; parameter_index < parameter_count;
			++parameter_index) {
		const SceGxmProgramParameter *parameter =
			sceGxmProgramGetParameter(s->prog, parameter_index);
		uint32_t index;
		if (!parameter)
			return 0;
		if (sceGxmProgramParameterGetCategory(parameter) !=
				SCE_GXM_PARAMETER_CATEGORY_UNIFORM_BUFFER)
			continue;
		index = sceGxmProgramParameterGetResourceIndex(parameter);
		if (index >= ISAAC_SHADER_CACHE_BLOCK_MAX ||
				(needed & (1u << index)) != 0u)
			return 0;
		needed |= 1u << index;
		gxp_by_index[index] = parameter;
	}
	/* vitaGL consumes unif_blk only while linking parameters in the GXP
	 * UNIFORM_BUFFER category.  SceShaccCg may still leave compiler-only
	 * block nodes behind; they are not runtime metadata and must not be
	 * serialized merely because the list is non-NULL. */
	if (needed == 0u) {
		*block_count = 0u;
		return 1;
	}
	for (block = s->unif_blk; block && scans <
			ISAAC_SHADER_CACHE_BLOCK_SCAN_MAX;
			block = (const block_uniform *)block->chain, ++scans) {
		if (block->idx < ISAAC_SHADER_CACHE_BLOCK_MAX &&
				(needed & (1u << block->idx)) != 0u) {
			if (by_index[block->idx] ||
					!isaac_shader_cache_block_name_valid(
						block->name, NULL))
				return 0;
			by_index[block->idx] = block;
		}
	}
	if (block)
		return 0;
	for (parameter_index = 0u; parameter_index < parameter_count;
			++parameter_index) {
		const SceGxmProgramParameter *parameter =
			sceGxmProgramGetParameter(s->prog, parameter_index);
		const block_uniform *source;
		uint32_t index;
		size_t name_length;
		if (!parameter)
			return 0;
		if (sceGxmProgramParameterGetCategory(parameter) !=
				SCE_GXM_PARAMETER_CATEGORY_UNIFORM_BUFFER)
			continue;
		index = sceGxmProgramParameterGetResourceIndex(parameter);
		source = by_index[index];
		if (!source ||
				!isaac_shader_cache_block_name_valid(
					source->name, &name_length) ||
				gxp_by_index[index] != parameter ||
				sceGxmProgramFindParameterByName(
					s->prog, source->name) != parameter)
			return 0;
		memset(&blocks[count], 0, sizeof blocks[count]);
		blocks[count].index = index;
		memcpy(blocks[count].name, source->name, name_length + 1u);
		++count;
	}
	if (!isaac_shader_cache_gxp_blocks_valid(s->prog, blocks, count))
		return 0;
	*block_count = count;
	return 1;
}

static int isaac_shader_cache_semantics_valid(const binds_map *semantics) {
	uint32_t index;
	for (index = 0u; index < MAX_CG_TEXCOORD_ID; ++index) {
		if (semantics->texcoord_used[index] != GL_FALSE &&
				semantics->texcoord_used[index] != GL_TRUE)
			return 0;
		if (semantics->texcoord_used[index] &&
				!memchr(semantics->texcoord_names[index], '\0',
					sizeof semantics->texcoord_names[index]))
			return 0;
	}
	for (index = 0u; index < MAX_CG_COLOR_ID; ++index) {
		if (semantics->color_used[index] != GL_FALSE &&
				semantics->color_used[index] != GL_TRUE)
			return 0;
		if (semantics->color_used[index] &&
				!memchr(semantics->color_names[index], '\0',
					sizeof semantics->color_names[index]))
			return 0;
	}
	return 1;
}

static int isaac_shader_cache_load_shader(
		shader *s, const isaac_shader_cache_key *key,
		GLboolean load_bindings) {
	static const isaac_shader_cache_program_ops program_ops = {
		isaac_shader_cache_program_check,
		isaac_shader_cache_program_size,
		isaac_shader_cache_program_type,
		isaac_shader_cache_program_register,
	};
	isaac_shader_cache_record record;
	binds_map semantics;
	matrix_uniform *matrices = NULL;
	block_uniform *blocks = NULL;
	isaac_shader_cache_block_descriptor block_descriptors[
		ISAAC_SHADER_CACHE_BLOCK_MAX];
	const uint8_t *metadata;
	uint32_t matrix_offset;
	uint32_t block_offset;
	uint32_t bindings_offset;
	uint32_t block_count;
	uint32_t parameter_count;
	uint32_t index;
	uint32_t expected_type;
	uintptr_t registered_id = 0;
	uint64_t register_start;
	enum isaac_shader_cache_program_result program_result;

	if (!s || !key || s->prog || s->mat || s->unif_blk)
		return 0;
	if (isaac_shader_cache_load(key, &record) !=
			ISAAC_SHADER_CACHE_LOAD_CANDIDATE)
		return 0;
	expected_type = s->type == GL_VERTEX_SHADER
		? (uint32_t)SCE_GXM_VERTEX_PROGRAM
		: (uint32_t)SCE_GXM_FRAGMENT_PROGRAM;
	/* A cache file is untrusted input.  Do not ask GXM to walk its parameter
	 * table until the complete program has passed Check/GetSize/GetType. */
	program_result = isaac_shader_cache_check_program(
		&record, expected_type, &program_ops, NULL);
	if (program_result != ISAAC_SHADER_CACHE_PROGRAM_ACCEPT) {
		isaac_shader_cache_reject_corrupt(key, &record);
		return 0;
	}
	if (((record.flags & ISAAC_SHADER_CACHE_FLAG_BINDINGS) != 0u) !=
			(load_bindings != GL_FALSE)) {
		isaac_shader_cache_reject_corrupt(key, &record);
		return 0;
	}
	metadata = record.body + record.gxp_len;
	if (!isaac_shader_cache_metadata_decode_header(
			metadata, record.metadata_len, record.matrix_count,
			load_bindings ? (uint32_t)sizeof(binds_map) : 0u,
			&block_count, &matrix_offset, &block_offset,
			&bindings_offset)) {
		isaac_shader_cache_reject_corrupt(key, &record);
		return 0;
	}
	for (index = 0u; index < block_count; ++index) {
		if (!isaac_shader_cache_decode_block(
				metadata + block_offset +
					index * ISAAC_SHADER_CACHE_BLOCK_RECORD_SIZE,
				&block_descriptors[index])) {
			isaac_shader_cache_reject_corrupt(key, &record);
			return 0;
		}
	}
	if (!isaac_shader_cache_blocks_unique(
			block_descriptors, block_count) ||
			!isaac_shader_cache_gxp_blocks_valid(
				(const SceGxmProgram *)record.body,
				block_descriptors, block_count)) {
		isaac_shader_cache_reject_corrupt(key, &record);
		return 0;
	}
	parameter_count = sceGxmProgramGetParameterCount(
		(const SceGxmProgram *)record.body);
	if (record.matrix_count > parameter_count) {
		isaac_shader_cache_reject_corrupt(key, &record);
		return 0;
	}
	for (index = 0; index < record.matrix_count; ++index) {
		uint32_t parameter_index =
			isaac_shader_cache_get_u32(
				metadata + matrix_offset + index * sizeof(uint32_t));
		matrix_uniform *matrix;
		if (parameter_index >= parameter_count) {
			isaac_shader_cache_free_matrices(matrices);
			isaac_shader_cache_reject_corrupt(key, &record);
			return 0;
		}
		matrix = (matrix_uniform *)vglMalloc(sizeof(matrix_uniform));
		if (!matrix) {
			isaac_shader_cache_free_matrices(matrices);
			isaac_shader_cache_dispose(&record);
			return 0;
		}
		matrix->ptr = sceGxmProgramGetParameter(
			(const SceGxmProgram *)record.body, parameter_index);
		if (!matrix->ptr) {
			vgl_free(matrix);
			isaac_shader_cache_free_matrices(matrices);
			isaac_shader_cache_reject_corrupt(key, &record);
			return 0;
		}
		matrix->chain = matrices;
		matrices = matrix;
	}
	if (!isaac_shader_cache_rebuild_blocks(
			block_descriptors, block_count, &blocks)) {
		isaac_shader_cache_free_matrices(matrices);
		isaac_shader_cache_dispose(&record);
		return 0;
	}
	if (load_bindings) {
		memcpy(&semantics, metadata + bindings_offset, sizeof semantics);
		if (!isaac_shader_cache_semantics_valid(&semantics)) {
			isaac_shader_cache_free_matrices(matrices);
			isaac_shader_cache_free_blocks(blocks);
			isaac_shader_cache_reject_corrupt(key, &record);
			return 0;
		}
	} else {
		memset(&semantics, 0, sizeof semantics);
	}
	register_start = isaac_shader_cache_now_us();
	program_result = isaac_shader_cache_register_program(
		&record, &program_ops, NULL, &registered_id);
	isaac_shader_cache_note_register(
		isaac_shader_cache_now_us() - register_start,
		program_result == ISAAC_SHADER_CACHE_PROGRAM_REGISTER_FAILED);
	if (program_result != ISAAC_SHADER_CACHE_PROGRAM_ACCEPT) {
		isaac_shader_cache_free_matrices(matrices);
		isaac_shader_cache_free_blocks(blocks);
		/* A transient patcher/register failure says nothing about the bytes.
		 * Keep the authenticated record and fall back to native compilation. */
		isaac_shader_cache_dispose(&record);
		return 0;
	}

	/* Commit only after every byte/metadata/GXM/Register gate passed.  The
	 * record body begins with the aligned GXP, so it becomes the shader's
	 * ordinary allocation and release_shader can free it unchanged. */
	s->prog = (const SceGxmProgram *)record.body;
	s->size = record.gxp_len;
	s->id = (SceGxmShaderPatcherId)registered_id;
	s->unif_buf_size = sceGxmProgramGetDefaultUniformBufferSize(s->prog);
	s->mat = matrices;
	s->unif_blk = blocks;
	if (load_bindings)
		s->semantics = semantics;
	s->is_glsl = GL_FALSE;
	record.body = NULL;
	isaac_shader_cache_dispose(&record);
	if (s->source) {
		vgl_free(s->source);
		s->source = NULL;
	}
#ifdef HAVE_ISAAC_COLOROFFSET_GPU_OPTIMIZATIONS
	isaac_coloroffset_note_compile(s);
#endif
	isaac_shader_cache_mark_hit();
	return 1;
}

static int isaac_shader_cache_publish_shader(
		const shader *s, const isaac_shader_cache_key *key,
		GLboolean save_bindings, GLboolean metadata_complete) {
	uint8_t *metadata = NULL;
	uint32_t metadata_size;
	uint32_t matrix_offset;
	uint32_t block_offset;
	uint32_t bindings_offset;
	uint32_t matrix_count = 0;
	uint32_t block_count = 0;
	uint32_t parameter_count;
	uint32_t index = 0;
	uint32_t canonical_gxp_size;
	isaac_shader_cache_block_descriptor blocks[
		ISAAC_SHADER_CACHE_BLOCK_MAX];
	matrix_uniform *matrix;
	int result;

	isaac_shader_cache_note_publish_attempt(s && s->unif_blk);
	if (!s || !key || !s->prog ||
			sceGxmProgramCheck(s->prog) != 0 ||
			!isaac_shader_cache_canonical_gxp_size(
				s->prog, s->size, &canonical_gxp_size) ||
			(sceGxmProgramGetType(s->prog) == SCE_GXM_VERTEX_PROGRAM) !=
				(s->type == GL_VERTEX_SHADER)) {
		isaac_shader_cache_note_publish_reject(
			ISAAC_SHADER_CACHE_PUBLISH_REJECT_SHAPE);
		return 0;
	}
	if (!metadata_complete) {
		isaac_shader_cache_note_publish_reject(
			ISAAC_SHADER_CACHE_PUBLISH_REJECT_INCOMPLETE);
		return 0;
	}
	if (save_bindings && !isaac_shader_cache_semantics_valid(&s->semantics)) {
		isaac_shader_cache_note_publish_reject(
			ISAAC_SHADER_CACHE_PUBLISH_REJECT_SEMANTICS);
		return 0;
	}
	if (!isaac_shader_cache_collect_gxp_blocks(s, blocks, &block_count)) {
		isaac_shader_cache_note_publish_reject(
			ISAAC_SHADER_CACHE_PUBLISH_REJECT_BLOCKS);
		return 0;
	}
	parameter_count = sceGxmProgramGetParameterCount(s->prog);
	for (matrix = s->mat; matrix;
			matrix = (matrix_uniform *)matrix->chain) {
		if (matrix_count == ISAAC_SHADER_CACHE_MATRIX_MAX) {
			isaac_shader_cache_note_publish_reject(
				ISAAC_SHADER_CACHE_PUBLISH_REJECT_MATRICES);
			return 0;
		}
		++matrix_count;
	}
	if (matrix_count > parameter_count ||
			!isaac_shader_cache_metadata_layout(
				matrix_count, block_count,
				save_bindings ? (uint32_t)sizeof(binds_map) : 0u,
				&metadata_size, &matrix_offset, &block_offset,
				&bindings_offset)) {
		isaac_shader_cache_note_publish_reject(
			ISAAC_SHADER_CACHE_PUBLISH_REJECT_MATRICES);
		return 0;
	}
	metadata = (uint8_t *)vglMalloc(metadata_size);
	if (!metadata) {
		isaac_shader_cache_note_publish_reject(
			ISAAC_SHADER_CACHE_PUBLISH_REJECT_ALLOC);
		return 0;
	}
	memset(metadata, 0, metadata_size);
	isaac_shader_cache_metadata_encode_header(
		metadata, matrix_count, block_count);
	for (matrix = s->mat; matrix;
			matrix = (matrix_uniform *)matrix->chain) {
		if (!matrix->ptr) {
			vgl_free(metadata);
			isaac_shader_cache_note_publish_reject(
				ISAAC_SHADER_CACHE_PUBLISH_REJECT_MATRICES);
			return 0;
		}
		uint32_t parameter_index =
			sceGxmProgramParameterGetIndex(s->prog, matrix->ptr);
		if (parameter_index >= parameter_count) {
			vgl_free(metadata);
			isaac_shader_cache_note_publish_reject(
				ISAAC_SHADER_CACHE_PUBLISH_REJECT_MATRICES);
			return 0;
		}
		isaac_shader_cache_put_u32(
			metadata + matrix_offset + index * sizeof(uint32_t),
			parameter_index);
		++index;
	}
	for (index = 0u; index < block_count; ++index) {
		if (!isaac_shader_cache_encode_block(
				metadata + block_offset +
					index * ISAAC_SHADER_CACHE_BLOCK_RECORD_SIZE,
				&blocks[index])) {
			vgl_free(metadata);
			isaac_shader_cache_note_publish_reject(
				ISAAC_SHADER_CACHE_PUBLISH_REJECT_BLOCKS);
			return 0;
		}
	}
	if (save_bindings)
		memcpy(metadata + bindings_offset, &s->semantics,
			sizeof(binds_map));
	isaac_shader_cache_note_publish_ready(block_count);
	result = isaac_shader_cache_publish(
		key, s->prog, canonical_gxp_size,
		metadata, metadata_size, matrix_count);
	vgl_free(metadata);
	return result;
}

void vglGetIsaacShaderCacheStats(vglIsaacShaderCacheStats *stats) {
	isaac_shader_cache_stats internal;
	if (!stats)
		return;
	isaac_shader_cache_get_stats(&internal);
	_Static_assert(sizeof internal == sizeof *stats,
		"Isaac shader-cache public statistics ABI drifted");
	memcpy(stats, &internal, sizeof *stats);
}

#endif
