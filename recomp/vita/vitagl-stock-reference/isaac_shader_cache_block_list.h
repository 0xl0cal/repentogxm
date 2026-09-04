#ifndef ISAAC_VITAGL_SHADER_CACHE_BLOCK_LIST_H
#define ISAAC_VITAGL_SHADER_CACHE_BLOCK_LIST_H

/* This small adapter deliberately contains the complete cached block_uniform
 * reconstruction transaction.  Production and the hostile host oracle both
 * compile this exact code.  The including translation unit supplies vitaGL's
 * block_uniform, vglMalloc and vgl_free definitions. */

static void isaac_shader_cache_free_blocks(block_uniform *blocks)
{
	while (blocks) {
		block_uniform *next = (block_uniform *)blocks->chain;
		vgl_free(blocks);
		blocks = next;
	}
}

static int isaac_shader_cache_rebuild_blocks(
		const isaac_shader_cache_block_descriptor *descriptors,
		uint32_t block_count, block_uniform **result)
{
	block_uniform *blocks = NULL;
	block_uniform *tail = NULL;
	uint32_t index;

	if (!result)
		return 0;
	*result = NULL;
	if (!isaac_shader_cache_blocks_unique(descriptors, block_count))
		return 0;
	for (index = 0u; index < block_count; ++index) {
		block_uniform *block =
			(block_uniform *)vglMalloc(sizeof(block_uniform));
		if (!block) {
			isaac_shader_cache_free_blocks(blocks);
			return 0;
		}
		memset(block, 0, sizeof *block);
		memcpy(block->name, descriptors[index].name,
			sizeof block->name);
		block->idx = (uint8_t)descriptors[index].index;
		block->chain = NULL;
		if (tail)
			tail->chain = block;
		else
			blocks = block;
		tail = block;
	}
	*result = blocks;
	return 1;
}

#endif
