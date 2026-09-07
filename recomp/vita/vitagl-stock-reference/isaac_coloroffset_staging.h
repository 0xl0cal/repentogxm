#ifndef ISAAC_COLOROFFSET_STAGING_H
#define ISAAC_COLOROFFSET_STAGING_H

#include "isaac_coloroffset_gpu_policy.h"
#if defined(HAVE_ISAAC_COLOROFFSET_PLAIN_FASTPATH) && HAVE_ISAAC_COLOROFFSET_PLAIN_FASTPATH == 1
#include "isaac_coloroffset_plain_policy.h"
#endif

/* Only the copy boundary may produce this draw-local proof.  UNKNOWN means
 * nothing was copied: the caller must use its original memcpy/fallback. */
enum isaac_coloroffset_staging_result {
	ISAAC_COLOROFFSET_STAGING_UNKNOWN = 0,
	ISAAC_COLOROFFSET_STAGING_REJECTED,
	ISAAC_COLOROFFSET_STAGING_NEUTRAL,
	ISAAC_COLOROFFSET_STAGING_PLAIN
};

#if defined(HAVE_ISAAC_COLOROFFSET_STAGING_LIMITS_NEON) && HAVE_ISAAC_COLOROFFSET_STAGING_LIMITS_NEON
#if !defined(HAVE_ISAAC_COLOROFFSET_STAGING_FINITE_NEON) || !HAVE_ISAAC_COLOROFFSET_STAGING_FINITE_NEON
#error "Staging limits NEON requires staging finite NEON"
#endif
#endif

#if defined(HAVE_ISAAC_COLOROFFSET_STAGING_FINITE_NEON) && HAVE_ISAAC_COLOROFFSET_STAGING_FINITE_NEON
#if !defined(HAVE_ISAAC_COLOROFFSET_STAGING_PLAIN_FUSION) || !HAVE_ISAAC_COLOROFFSET_STAGING_PLAIN_FUSION
#error "Staging finite NEON requires staging PLAIN fusion"
#endif
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#if ISAAC_COLOROFFSET_VERTEX_STRIDE != 88u
#error "Staging finite NEON requires the exact 88-byte record"
#endif
#define ISAAC_COLOROFFSET_STAGING_FINITE_NEON_ACTIVE
/* Private owned-record check, never a client/GPU pointer predicate. All 88
 * bytes have already been copied into the cached chunk. Compare each IEEE
 * exponent BEFORE OR reduction: ORing finite exponents can invent 0xff.
 * Byte loads keep arbitrary alignment/aliasing valid; the final load is
 * eight bytes, not a 96-byte read past the final complete record. */
static inline int isaac_coloroffset_staging_all_finite(
		const uint8_t *vertex, size_t available) {
	uint32x4_t mask = vdupq_n_u32(0x7f800000u);
	uint32x4_t bad = vdupq_n_u32(0u);
	uint32x2_t reduced, tail;
	uint32_t offset;
	if (!vertex || available < ISAAC_COLOROFFSET_VERTEX_STRIDE)
		return 0;
	for (offset = 0u; offset < 80u; offset += 16u) {
		uint32x4_t words = vreinterpretq_u32_u8(vld1q_u8(vertex + offset));
		bad = vorrq_u32(bad, vceqq_u32(vandq_u32(words, mask), mask));
	}
	tail = vreinterpret_u32_u8(vld1_u8(vertex + 80u));
	reduced = vorr_u32(vget_low_u32(bad), vget_high_u32(bad));
	reduced = vorr_u32(reduced,
		vceq_u32(vand_u32(tail, vget_low_u32(mask)), vget_low_u32(mask)));
	return (vget_lane_u32(reduced, 0) | vget_lane_u32(reduced, 1)) == 0u;
}

#if defined(HAVE_ISAAC_COLOROFFSET_STAGING_LIMITS_NEON) && HAVE_ISAAC_COLOROFFSET_STAGING_LIMITS_NEON
#define ISAAC_COLOROFFSET_STAGING_LIMITS_NEON_ACTIVE
/* Whole private predicate on one owned, initialized record. F=largest finite
 * magnitude, C=inclusive 16, Z=signed zero. Exact lane table by byte offset:
 * 0: F/F/F/C; 16: C/C/C/F; 32: F/C/C/C; 48: C/C/C/C;
 * 64: F/F/Z/Z; 80 (only 8 bytes): Z/F. Individual unsigned comparisons
 * precede Boolean reduction; raw84 also retains the old sign/zero rule.
 * plain/nonwhite are constant modes at the inlined production call sites.
 * nonwhite, when present, is a separate owner-local accumulator, not an alias
 * of the record. Partial WHITE state never publishes after rejection. */
static inline enum isaac_coloroffset_staging_result
isaac_coloroffset_staging_limits_vertex(const uint8_t *vertex, size_t available,
		int plain, uint32_t *nonwhite) {
	const uint32x4_t sign = vdupq_n_u32(0x7fffffffu);
	const uint32x4_t limit0 = {0x7f7fffffu, 0x7f7fffffu, 0x7f7fffffu, 0x41800000u};
	const uint32x4_t limit16 = {0x41800000u, 0x41800000u, 0x41800000u, 0x7f7fffffu};
	const uint32x4_t limit32 = {0x7f7fffffu, 0x41800000u, 0x41800000u, 0x41800000u};
	const uint32x4_t limit48 = vdupq_n_u32(0x41800000u);
	const uint32x4_t limit64 = {0x7f7fffffu, 0x7f7fffffu, 0u, 0u};
	const uint32x2_t limit80 = {0u, 0x7f7fffffu};
	uint32x4_t raw0, raw16, raw32, raw48, raw64, bad;
	uint32x2_t tail, reduced;
	uint32_t raw84, nonzero;
	if (!vertex || available < ISAAC_COLOROFFSET_VERTEX_STRIDE)
		return ISAAC_COLOROFFSET_STAGING_REJECTED;
	raw0 = vreinterpretq_u32_u8(vld1q_u8(vertex));
	bad = vcgtq_u32(vandq_u32(raw0, sign), limit0);
	raw16 = vreinterpretq_u32_u8(vld1q_u8(vertex + 16u));
	bad = vorrq_u32(bad, vcgtq_u32(vandq_u32(raw16, sign), limit16));
	raw32 = vreinterpretq_u32_u8(vld1q_u8(vertex + 32u));
	bad = vorrq_u32(bad, vcgtq_u32(vandq_u32(raw32, sign), limit32));
	raw48 = vreinterpretq_u32_u8(vld1q_u8(vertex + 48u));
	bad = vorrq_u32(bad, vcgtq_u32(vandq_u32(raw48, sign), limit48));
	raw64 = vreinterpretq_u32_u8(vld1q_u8(vertex + 64u));
	bad = vorrq_u32(bad, vcgtq_u32(vandq_u32(raw64, sign), limit64));
	tail = vreinterpret_u32_u8(vld1_u8(vertex + 80u));
	reduced = vorr_u32(vget_low_u32(bad), vget_high_u32(bad));
	reduced = vorr_u32(reduced, vcgt_u32(vand_u32(tail, vget_low_u32(sign)), limit80));
	raw84 = vget_lane_u32(tail, 1);
	if ((vget_lane_u32(reduced, 0) | vget_lane_u32(reduced, 1)) != 0u ||
			((raw84 & 0x80000000u) == 0u && raw84 != 0u))
		return ISAAC_COLOROFFSET_STAGING_REJECTED;
	if (!plain)
		return ISAAC_COLOROFFSET_STAGING_NEUTRAL;
	/* Reuse loaded words, never reread the snapshot for PLAIN or WHITE. */
	raw48 = vandq_u32(raw48, sign);
	reduced = vorr_u32(vget_low_u32(raw48), vget_high_u32(raw48));
	nonzero = vget_lane_u32(reduced, 0) | vget_lane_u32(reduced, 1);
#if defined(HAVE_ISAAC_LASER_HALO_PROFILE) && HAVE_ISAAC_LASER_HALO_PROFILE
	if (nonwhite) {
		/* Exactly raw Color 12/16/20/24: exclude UV28 and preserve -1. */
		uint32x4_t white = veorq_u32(vextq_u32(raw0, raw16, 3), vdupq_n_u32(0x3f800000u));
		reduced = vorr_u32(vget_low_u32(white), vget_high_u32(white));
		*nonwhite |= vget_lane_u32(reduced, 0) | vget_lane_u32(reduced, 1);
	}
#else
	(void)nonwhite; /* Observer OFF has no WHITE arithmetic or accumulator access. */
#endif
	return nonzero ? ISAAC_COLOROFFSET_STAGING_NEUTRAL : ISAAC_COLOROFFSET_STAGING_PLAIN;
}
#endif

/* Finite-only mode copies the shared bounds/flags in their original order;
 * limits mode substitutes its complete private predicate. Shared standalone
 * and halo paths stay unchanged; neutral-only never accumulates PLAIN/WHITE. */
static inline int isaac_coloroffset_staging_vertex_is_neutral(
		const uint8_t *vertex, size_t available) {
#ifdef ISAAC_COLOROFFSET_STAGING_LIMITS_NEON_ACTIVE
	return isaac_coloroffset_staging_limits_vertex(vertex, available, 0, NULL) ==
		ISAAC_COLOROFFSET_STAGING_NEUTRAL;
#else
	uint32_t offset, bits;
	if (!isaac_coloroffset_staging_all_finite(vertex, available))
		return 0;
	for (offset = 12u; offset < 64u; offset += 4u) {
		if (offset == 28u || offset == 32u)
			continue;
		if ((isaac_coloroffset_load_u32(vertex + offset) & 0x7fffffffu) >
				0x41800000u)
			return 0;
	}
	if ((isaac_coloroffset_load_u32(vertex + 72u) & 0x7fffffffu) != 0u ||
			(isaac_coloroffset_load_u32(vertex + 76u) & 0x7fffffffu) != 0u ||
			(isaac_coloroffset_load_u32(vertex + 80u) & 0x7fffffffu) != 0u)
		return 0;
	bits = isaac_coloroffset_load_u32(vertex + 84u);
	return (bits & 0x80000000u) != 0u || bits == 0u;
#endif
}

static inline int isaac_coloroffset_staging_contiguous_neutral(
		const void *vertices, size_t vertex_bytes, uint32_t vertex_count,
		uint32_t stride) {
	const uint8_t *bytes = (const uint8_t *)vertices;
	uint32_t index;
	if (!bytes || stride != ISAAC_COLOROFFSET_VERTEX_STRIDE ||
			vertex_count == 0u || vertex_count > 4096u ||
			vertex_count > SIZE_MAX / stride ||
			vertex_bytes != (size_t)vertex_count * stride)
		return 0;
	for (index = 0u; index < vertex_count; ++index) {
		size_t offset = (size_t)index * stride;
		if (!isaac_coloroffset_staging_vertex_is_neutral(
				bytes + offset, vertex_bytes - offset))
			return 0;
	}
	return 1;
}
#endif
#endif

#ifdef ISAAC_COLOROFFSET_STAGING_FINITE_NEON_ACTIVE
#define ISAAC_STAGING_VERTEX_NEUTRAL isaac_coloroffset_staging_vertex_is_neutral
#define ISAAC_STAGING_CONTIGUOUS_NEUTRAL isaac_coloroffset_staging_contiguous_neutral
#else
#define ISAAC_STAGING_VERTEX_NEUTRAL isaac_coloroffset_vertex_is_neutral
#define ISAAC_STAGING_CONTIGUOUS_NEUTRAL isaac_coloroffset_contiguous_vertices_are_neutral
#endif

#if defined(HAVE_ISAAC_COLOROFFSET_STAGING_PLAIN_FUSION) && HAVE_ISAAC_COLOROFFSET_STAGING_PLAIN_FUSION
#if !defined(HAVE_ISAAC_COLOROFFSET_PLAIN_FASTPATH) || HAVE_ISAAC_COLOROFFSET_PLAIN_FASTPATH != 1
#error "Staging PLAIN fusion requires the PLAIN fastpath"
#endif
/* Private to the ordinary staging snapshot. Keep the check order copied
 * from isaac_coloroffset_vertex_is_neutral: all finite lanes, color bounds,
 * then flags. Only accumulate the PLAIN words already loaded for bounds;
 * never publish PLAIN/NEUTRAL before every neutral check has passed. */
static inline enum isaac_coloroffset_staging_result
isaac_coloroffset_staging_classify_vertex(const uint8_t *vertex, size_t available) {
#ifdef ISAAC_COLOROFFSET_STAGING_LIMITS_NEON_ACTIVE
	return isaac_coloroffset_staging_limits_vertex(vertex, available, 1, NULL);
#else
	uint32_t offset, bits, nonzero = 0u;
	if (!vertex || available < ISAAC_COLOROFFSET_VERTEX_STRIDE)
		return ISAAC_COLOROFFSET_STAGING_REJECTED;
#ifdef ISAAC_COLOROFFSET_STAGING_FINITE_NEON_ACTIVE
	if (!isaac_coloroffset_staging_all_finite(vertex, available))
		return ISAAC_COLOROFFSET_STAGING_REJECTED;
#else
	for (offset = 0u; offset < ISAAC_COLOROFFSET_VERTEX_STRIDE; offset += 4u) {
		bits = isaac_coloroffset_load_u32(vertex + offset) & 0x7fffffffu;
		if (bits >= 0x7f800000u)
			return ISAAC_COLOROFFSET_STAGING_REJECTED;
	}
#endif
	for (offset = 12u; offset < 64u; offset += 4u) {
		if (offset == 28u || offset == 32u)
			continue;
		bits = isaac_coloroffset_load_u32(vertex + offset) & 0x7fffffffu;
		if (bits > 0x41800000u)
			return ISAAC_COLOROFFSET_STAGING_REJECTED;
		if (offset >= 48u)
			nonzero |= bits;
	}
	if ((isaac_coloroffset_load_u32(vertex + 72u) & 0x7fffffffu) != 0u ||
			(isaac_coloroffset_load_u32(vertex + 76u) & 0x7fffffffu) != 0u ||
			(isaac_coloroffset_load_u32(vertex + 80u) & 0x7fffffffu) != 0u)
		return ISAAC_COLOROFFSET_STAGING_REJECTED;
	bits = isaac_coloroffset_load_u32(vertex + 84u);
	if ((bits & 0x80000000u) == 0u && bits != 0u)
		return ISAAC_COLOROFFSET_STAGING_REJECTED;
	return nonzero ? ISAAC_COLOROFFSET_STAGING_NEUTRAL : ISAAC_COLOROFFSET_STAGING_PLAIN;
#endif
}

static inline enum isaac_coloroffset_staging_result
isaac_coloroffset_staging_classify_chunk(
		const uint8_t *bytes, size_t vertex_bytes, uint32_t vertex_count, uint32_t stride) {
	uint32_t index;
	if (!bytes || stride != ISAAC_COLOROFFSET_VERTEX_STRIDE ||
			vertex_count == 0u || vertex_count > 4096u ||
			vertex_count > SIZE_MAX / stride ||
			vertex_bytes != (size_t)vertex_count * stride)
		return ISAAC_COLOROFFSET_STAGING_REJECTED;
	for (index = 0u; index < vertex_count; ++index) {
		size_t offset = (size_t)index * stride;
		enum isaac_coloroffset_staging_result result =
			isaac_coloroffset_staging_classify_vertex(bytes + offset, vertex_bytes - offset);
		if (result == ISAAC_COLOROFFSET_STAGING_REJECTED)
			return result;
		if (result == ISAAC_COLOROFFSET_STAGING_NEUTRAL) {
			/* PLAIN is lost, not neutral admission. The rest of this chunk
			 * uses the original neutral-only predicate, with no accumulator
			 * or per-vertex PLAIN branch. Later chunks take the old path too. */
			for (++index; index < vertex_count; ++index) {
				offset = (size_t)index * stride;
				if (!ISAAC_STAGING_VERTEX_NEUTRAL(bytes + offset, vertex_bytes - offset))
					return ISAAC_COLOROFFSET_STAGING_REJECTED;
			}
			return ISAAC_COLOROFFSET_STAGING_NEUTRAL;
		}
	}
	return ISAAC_COLOROFFSET_STAGING_PLAIN;
}
#endif

#ifndef ISAAC_COLOROFFSET_STAGING_MEMCPY
#define ISAAC_COLOROFFSET_STAGING_MEMCPY memcpy
#define ISAAC_COLOROFFSET_STAGING_DEFAULT_MEMCPY
#endif

/* Keep the complete copy/proof frame off draw paths that do not stage.
 * Only these two whole-copy entries change linkage decoration; all inner
 * predicates and the copy/publication bodies stay unchanged. */
#if defined(HAVE_ISAAC_COLOROFFSET_STAGING_OUTLINE) && HAVE_ISAAC_COLOROFFSET_STAGING_OUTLINE
#if !defined(HAVE_ISAAC_COLOROFFSET_STAGING_LIMITS_NEON) || !HAVE_ISAAC_COLOROFFSET_STAGING_LIMITS_NEON
#error "Staging outline requires staging limits NEON"
#endif
#if defined(_MSC_VER)
#define ISAAC_STAGING_COPY_DECL static __declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
#define ISAAC_STAGING_COPY_DECL static __attribute__((noinline))
#else
#error "Staging outline requires a supported noinline compiler"
#endif
#else
#define ISAAC_STAGING_COPY_DECL static inline
#endif

/* Eight complete records are 704 bytes, a multiple of 32.  Keep
 * the CPU proof on this small cached stack buffer, never read back the GPU
 * upload.  Production supplies vgl_fast_memcpy for BOTH bulk copies; no
 * scalar upload loop or allocation is introduced.  The bytes classified
 * are exactly the bytes uploaded, even if the client source later changes. */
ISAAC_STAGING_COPY_DECL enum isaac_coloroffset_staging_result
isaac_coloroffset_copy_and_prove_neutral(
		void *destination, const void *source, size_t bytes,
		uint32_t vertex_count, uint32_t stride) {
	uint64_t chunk_words[8u * ISAAC_COLOROFFSET_VERTEX_STRIDE / sizeof(uint64_t)];
	uint8_t *chunk = (uint8_t *)chunk_words;
	uint8_t *out = (uint8_t *)destination;
	const uint8_t *in = (const uint8_t *)source;
	uintptr_t dst = (uintptr_t)destination, src = (uintptr_t)source;
	size_t remaining = bytes;
	enum isaac_coloroffset_staging_result result =
		ISAAC_COLOROFFSET_STAGING_NEUTRAL;
#if defined(HAVE_ISAAC_COLOROFFSET_PLAIN_FASTPATH) && HAVE_ISAAC_COLOROFFSET_PLAIN_FASTPATH == 1
	int plain = 1;
#endif

	if (!destination || !source || stride != ISAAC_COLOROFFSET_VERTEX_STRIDE ||
			vertex_count == 0u || vertex_count > 4096u ||
			vertex_count > SIZE_MAX / stride ||
			bytes != (size_t)vertex_count * stride ||
			bytes > UINTPTR_MAX - dst || bytes > UINTPTR_MAX - src ||
			(dst < src + bytes && src < dst + bytes))
		return ISAAC_COLOROFFSET_STAGING_UNKNOWN;

	while (remaining) {
		size_t count = remaining < sizeof(chunk_words) ?
			remaining : sizeof(chunk_words);
		ISAAC_COLOROFFSET_STAGING_MEMCPY(chunk, in, count);
#if defined(HAVE_ISAAC_COLOROFFSET_STAGING_PLAIN_FUSION) && HAVE_ISAAC_COLOROFFSET_STAGING_PLAIN_FUSION
		if (plain) {
			enum isaac_coloroffset_staging_result chunk_result =
				isaac_coloroffset_staging_classify_chunk(chunk, count, (uint32_t)(count / stride), stride);
			if (chunk_result == ISAAC_COLOROFFSET_STAGING_REJECTED)
				result = ISAAC_COLOROFFSET_STAGING_REJECTED;
			else if (chunk_result == ISAAC_COLOROFFSET_STAGING_NEUTRAL)
				plain = 0;
		} else
#endif
		if (!ISAAC_STAGING_CONTIGUOUS_NEUTRAL(
				chunk, count, (uint32_t)(count / stride), stride))
			result = ISAAC_COLOROFFSET_STAGING_REJECTED;
#if defined(HAVE_ISAAC_COLOROFFSET_PLAIN_FASTPATH) && HAVE_ISAAC_COLOROFFSET_PLAIN_FASTPATH == 1
#if !defined(HAVE_ISAAC_COLOROFFSET_STAGING_PLAIN_FUSION) || !HAVE_ISAAC_COLOROFFSET_STAGING_PLAIN_FUSION
		else if (plain && !isaac_coloroffset_neutral_vertices_have_plain_colors(
				chunk, (uint32_t)(count / stride)))
			plain = 0;
#endif
#endif
		ISAAC_COLOROFFSET_STAGING_MEMCPY(out, chunk, count);
		remaining -= count;
		in += count;
		out += count;
		if (result == ISAAC_COLOROFFSET_STAGING_REJECTED) {
			/* Rejection changes only shader choice, never draw bytes.  No
			 * more proof work is needed, but upload the entire remainder. */
			if (remaining)
				ISAAC_COLOROFFSET_STAGING_MEMCPY(out, in, remaining);
			break;
		}
	}
#if defined(HAVE_ISAAC_COLOROFFSET_PLAIN_FASTPATH) && HAVE_ISAAC_COLOROFFSET_PLAIN_FASTPATH == 1
	if (result == ISAAC_COLOROFFSET_STAGING_NEUTRAL && plain)
		return ISAAC_COLOROFFSET_STAGING_PLAIN;
#endif
	return result;
}

#if defined(HAVE_ISAAC_LASER_HALO_PROFILE) && HAVE_ISAAC_LASER_HALO_PROFILE && \
    defined(HAVE_ISAAC_COLOROFFSET_STAGING_PLAIN_FUSION) && HAVE_ISAAC_COLOROFFSET_STAGING_PLAIN_FUSION
/* Observer-only copy entry. Keep the nontracking entry and standalone
 * predicates above literally unchanged. 0 = no ordinary PLAIN proof,
 * 1 = ordinary PLAIN/nonwhite, 2 = ordinary PLAIN/all-white. This token is
 * caller-owned, never retained globally or shared with the halo shortcut. */
static inline enum isaac_coloroffset_staging_result
isaac_coloroffset_staging_classify_white_vertex(
		const uint8_t *vertex, uint32_t *nonwhite) {
#ifdef ISAAC_COLOROFFSET_STAGING_LIMITS_NEON_ACTIVE
	return isaac_coloroffset_staging_limits_vertex(vertex, ISAAC_COLOROFFSET_VERTEX_STRIDE, 1, nonwhite);
#else
	uint32_t offset, bits, nonzero = 0u;
	/* Same finite/bounds/flags order as the fused standalone predicate.
	 * Only called for a complete vertex in the private cached chunk. */
#ifdef ISAAC_COLOROFFSET_STAGING_FINITE_NEON_ACTIVE
	if (!isaac_coloroffset_staging_all_finite(vertex, ISAAC_COLOROFFSET_VERTEX_STRIDE))
		return ISAAC_COLOROFFSET_STAGING_REJECTED;
#else
	for (offset = 0u; offset < ISAAC_COLOROFFSET_VERTEX_STRIDE; offset += 4u) {
		bits = isaac_coloroffset_load_u32(vertex + offset) & 0x7fffffffu;
		if (bits >= 0x7f800000u)
			return ISAAC_COLOROFFSET_STAGING_REJECTED;
	}
#endif
	for (offset = 12u; offset < 64u; offset += 4u) {
		if (offset == 28u || offset == 32u)
			continue;
		bits = isaac_coloroffset_load_u32(vertex + offset);
		/* Use the original word BEFORE sign masking: -1 is not white. */
		if (offset < 28u)
			*nonwhite |= bits ^ 0x3f800000u;
		bits &= 0x7fffffffu;
		if (bits > 0x41800000u)
			return ISAAC_COLOROFFSET_STAGING_REJECTED;
		if (offset >= 48u)
			nonzero |= bits;
	}
	if ((isaac_coloroffset_load_u32(vertex + 72u) & 0x7fffffffu) != 0u ||
			(isaac_coloroffset_load_u32(vertex + 76u) & 0x7fffffffu) != 0u ||
			(isaac_coloroffset_load_u32(vertex + 80u) & 0x7fffffffu) != 0u)
		return ISAAC_COLOROFFSET_STAGING_REJECTED;
	bits = isaac_coloroffset_load_u32(vertex + 84u);
	if ((bits & 0x80000000u) == 0u && bits != 0u)
		return ISAAC_COLOROFFSET_STAGING_REJECTED;
	return nonzero ? ISAAC_COLOROFFSET_STAGING_NEUTRAL : ISAAC_COLOROFFSET_STAGING_PLAIN;
#endif
}

ISAAC_STAGING_COPY_DECL enum isaac_coloroffset_staging_result
isaac_coloroffset_copy_and_prove_white(
		void *destination, const void *source, size_t bytes,
		uint32_t vertex_count, uint32_t stride, uint32_t *ordinary_proof) {
	uint64_t chunk_words[8u * ISAAC_COLOROFFSET_VERTEX_STRIDE / sizeof(uint64_t)];
	uint8_t *chunk = (uint8_t *)chunk_words, *out = (uint8_t *)destination;
	const uint8_t *in = (const uint8_t *)source;
	uintptr_t dst = (uintptr_t)destination, src = (uintptr_t)source;
	size_t remaining = bytes;
	uint32_t nonwhite = 0u;
	int plain = 1;
	enum isaac_coloroffset_staging_result result = ISAAC_COLOROFFSET_STAGING_NEUTRAL;
	if (!ordinary_proof)
		return isaac_coloroffset_copy_and_prove_neutral(destination, source, bytes, vertex_count, stride);
	*ordinary_proof = 0u;
	if (!destination || !source || stride != ISAAC_COLOROFFSET_VERTEX_STRIDE ||
			vertex_count == 0u || vertex_count > 4096u ||
			vertex_count > SIZE_MAX / stride || bytes != (size_t)vertex_count * stride ||
			bytes > UINTPTR_MAX - dst || bytes > UINTPTR_MAX - src ||
			(dst < src + bytes && src < dst + bytes))
		return ISAAC_COLOROFFSET_STAGING_UNKNOWN;
	while (remaining) {
		size_t count = remaining < sizeof(chunk_words) ? remaining : sizeof(chunk_words);
		ISAAC_COLOROFFSET_STAGING_MEMCPY(chunk, in, count);
		if (plain) {
			for (size_t offset = 0u; offset < count; offset += stride) {
				enum isaac_coloroffset_staging_result vertex_result =
					isaac_coloroffset_staging_classify_white_vertex(chunk + offset, &nonwhite);
				if (vertex_result == ISAAC_COLOROFFSET_STAGING_REJECTED) {
					result = vertex_result;
					break;
				}
				if (vertex_result == ISAAC_COLOROFFSET_STAGING_NEUTRAL) {
					plain = 0;
					/* Match fusion: neutral-only remainder, then neutral-only
					 * later chunks. No additional white work after PLAIN loss. */
					for (offset += stride; offset < count; offset += stride)
						if (!ISAAC_STAGING_VERTEX_NEUTRAL(chunk + offset, count - offset)) {
							result = ISAAC_COLOROFFSET_STAGING_REJECTED;
							break;
						}
					break;
				}
			}
		} else if (!ISAAC_STAGING_CONTIGUOUS_NEUTRAL(
				chunk, count, (uint32_t)(count / stride), stride))
			result = ISAAC_COLOROFFSET_STAGING_REJECTED;
		ISAAC_COLOROFFSET_STAGING_MEMCPY(out, chunk, count);
		remaining -= count;
		in += count;
		out += count;
		if (result == ISAAC_COLOROFFSET_STAGING_REJECTED) {
			if (remaining)
				ISAAC_COLOROFFSET_STAGING_MEMCPY(out, in, remaining);
			break;
		}
	}
	/* Commit only after every classified byte was uploaded. Partial white
	 * chunks never escape after a later non-PLAIN or rejection. */
	if (result == ISAAC_COLOROFFSET_STAGING_NEUTRAL && plain) {
		*ordinary_proof = nonwhite ? 1u : 2u;
		return ISAAC_COLOROFFSET_STAGING_PLAIN;
	}
	return result;
}
#endif

#ifdef ISAAC_COLOROFFSET_STAGING_DEFAULT_MEMCPY
#undef ISAAC_COLOROFFSET_STAGING_DEFAULT_MEMCPY
#undef ISAAC_COLOROFFSET_STAGING_MEMCPY
#endif

#undef ISAAC_STAGING_VERTEX_NEUTRAL
#undef ISAAC_STAGING_CONTIGUOUS_NEUTRAL
#undef ISAAC_COLOROFFSET_STAGING_FINITE_NEON_ACTIVE
#undef ISAAC_COLOROFFSET_STAGING_LIMITS_NEON_ACTIVE
#undef ISAAC_STAGING_COPY_DECL

#endif
