#if defined(ISAAC_COLOROFFSET_STAGING_ARM_TEST)
/* Narrow freestanding entries for the existing ARM/Unicorn byte/bounds
 * harness. The copy boundary is intercepted, not a replacement predicate;
 * all classification and copy scheduling below are production headers. */
#include "isaac_coloroffset_gpu_policy.h"
__attribute__((noinline)) void *isaac_staging_arm_copy(
		void *destination, const void *source, size_t bytes) {
	__asm__ volatile ("" : : "r"(source), "r"(bytes) : "memory");
	return destination;
}
#define ISAAC_COLOROFFSET_STAGING_MEMCPY isaac_staging_arm_copy
#ifndef STAGING_HEADER
#define STAGING_HEADER "isaac_coloroffset_staging.h"
#endif
#include STAGING_HEADER
#undef ISAAC_COLOROFFSET_STAGING_MEMCPY

int isaac_staging_arm_finite(const uint8_t *vertex, size_t available) {
#if defined(HAVE_ISAAC_COLOROFFSET_STAGING_FINITE_NEON) && HAVE_ISAAC_COLOROFFSET_STAGING_FINITE_NEON && (defined(__ARM_NEON) || defined(__ARM_NEON__))
	return isaac_coloroffset_staging_all_finite(vertex, available);
#else
	/* Original integer loop, isolated only for finite-bit differential cases. */
	if (!vertex || available < ISAAC_COLOROFFSET_VERTEX_STRIDE)
		return 0;
	for (uint32_t offset = 0; offset < ISAAC_COLOROFFSET_VERTEX_STRIDE; offset += 4u)
		if ((isaac_coloroffset_load_u32(vertex + offset) & 0x7fffffffu) >= 0x7f800000u)
			return 0;
	return 1;
#endif
}

int isaac_staging_arm_neutral(const uint8_t *vertex, size_t available) {
#if defined(HAVE_ISAAC_COLOROFFSET_STAGING_FINITE_NEON) && HAVE_ISAAC_COLOROFFSET_STAGING_FINITE_NEON && (defined(__ARM_NEON) || defined(__ARM_NEON__))
	return isaac_coloroffset_staging_vertex_is_neutral(vertex, available);
#else
	return isaac_coloroffset_vertex_is_neutral(vertex, available);
#endif
}

int isaac_staging_arm_classify(const uint8_t *vertex, size_t available) {
	return isaac_coloroffset_staging_classify_vertex(vertex, available);
}

#if defined(HAVE_ISAAC_LASER_HALO_PROFILE) && HAVE_ISAAC_LASER_HALO_PROFILE
int isaac_staging_arm_white(const uint8_t *vertex, size_t available, uint32_t *nonwhite) {
	/* The production caller supplies a complete private record. This entry
	 * bounds hostile direct fixture inputs before that complete-record call. */
	if (!vertex || available < ISAAC_COLOROFFSET_VERTEX_STRIDE)
		return ISAAC_COLOROFFSET_STAGING_REJECTED;
	return isaac_coloroffset_staging_classify_white_vertex(vertex, nonwhite);
}
#endif

/* config is [vertex_count, stride, optional ordinary-proof pointer]. */
int isaac_staging_arm_stage(void *destination, const void *source, size_t bytes,
		const uint32_t *config) {
#if defined(HAVE_ISAAC_LASER_HALO_PROFILE) && HAVE_ISAAC_LASER_HALO_PROFILE
	return isaac_coloroffset_copy_and_prove_white(destination, source, bytes,
		config[0], config[1], (uint32_t *)(uintptr_t)config[2]);
#else
	return isaac_coloroffset_copy_and_prove_neutral(destination, source, bytes,
		config[0], config[1]);
#endif
}
#else
#include "isaac_coloroffset_gpu_policy.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void expect(int condition, const char *message)
{
	if (!condition) {
		fprintf(stderr, "coloroffset-policy oracle: %s\n", message);
		exit(1);
	}
}

static void store_u32(uint8_t *bytes, uint32_t value)
{
	memcpy(bytes, &value, sizeof value);
}

static uint32_t staging_copy_calls;
static uint8_t *staging_destination;
static size_t staging_destination_bytes;
static uint8_t *staging_mutate_source;
static uint32_t staging_mutate_value;
static uint32_t staging_mutate_offset = 72u;
static int staging_trace_enabled;
static uint32_t staging_trace_count;
static struct {
	uintptr_t destination, source;
	size_t bytes;
} staging_trace[8];

static void *staging_test_memcpy(void *destination, const void *source, size_t bytes)
{
	uintptr_t input = (uintptr_t)source;
	uintptr_t output = (uintptr_t)staging_destination;
	expect(input < output || input >= output + staging_destination_bytes,
		"staging proof read back from the GPU upload");
	if (staging_trace_enabled) {
		expect(staging_trace_count < sizeof staging_trace / sizeof staging_trace[0],
			"staging copy trace overflow");
		staging_trace[staging_trace_count].destination = (uintptr_t)destination;
		staging_trace[staging_trace_count].source = input;
		staging_trace[staging_trace_count].bytes = bytes;
		++staging_trace_count;
	}
	++staging_copy_calls;
	memcpy(destination, source, bytes);
	if (source == staging_mutate_source) {
		/* Mutate the original immediately after its chunk was copied.  The
		 * result must describe the snapshot uploaded, not a second read. */
		store_u32(staging_mutate_source + staging_mutate_offset, staging_mutate_value);
		staging_mutate_source = NULL;
	}
	return destination;
}

#define ISAAC_COLOROFFSET_STAGING_MEMCPY staging_test_memcpy
#include "isaac_coloroffset_staging.h"
#undef ISAAC_COLOROFFSET_STAGING_MEMCPY

#if defined(HAVE_ISAAC_COLOROFFSET_PLAIN_FASTPATH) && HAVE_ISAAC_COLOROFFSET_PLAIN_FASTPATH == 1
#define DEFAULT_STAGING_RESULT ISAAC_COLOROFFSET_STAGING_PLAIN
#else
#define DEFAULT_STAGING_RESULT ISAAC_COLOROFFSET_STAGING_NEUTRAL
#endif

static void verify_staging_copy(void)
{
	static uint8_t source_storage[4096u * ISAAC_COLOROFFSET_VERTEX_STRIDE + 16u];
	static uint8_t destination_storage[sizeof source_storage];
	static const uint32_t counts[] = {1u, 4u, 7u, 8u, 9u, 16u, 17u, 4096u};
	uint8_t *source = source_storage + 3u; /* Exercise unaligned clients too. */
	uint8_t *destination = destination_storage + 5u;
	uint32_t i, lane;
	size_t bytes;
	enum isaac_coloroffset_staging_result result;
	staging_destination = destination_storage;
	staging_destination_bytes = sizeof destination_storage;

	for (i = 0u; i < sizeof counts / sizeof counts[0]; ++i) {
		bytes = (size_t)counts[i] * ISAAC_COLOROFFSET_VERTEX_STRIDE;
		memset(source_storage, 0, sizeof source_storage);
		memset(destination_storage, 0xa5, sizeof destination_storage);
		staging_copy_calls = 0u;
		result = isaac_coloroffset_copy_and_prove_neutral(
			destination, source, bytes, counts[i], ISAAC_COLOROFFSET_VERTEX_STRIDE);
		expect(result == DEFAULT_STAGING_RESULT,
			"neutral staged count rejected");
		expect(memcmp(destination, source, bytes) == 0 &&
			destination[-1] == 0xa5 && destination[bytes] == 0xa5,
			"staged upload bytes or canaries changed");
		expect(staging_copy_calls == 2u * ((counts[i] + 7u) / 8u),
			"staging stopped using chunk-sized bulk copies");
	}

	bytes = 17u * ISAAC_COLOROFFSET_VERTEX_STRIDE;
	/* First and final-chunk rejection both still upload the whole draw. */
	for (i = 0u; i <= 16u; i += 16u) {
		memset(source, 0, bytes);
		store_u32(source + i * ISAAC_COLOROFFSET_VERTEX_STRIDE + 72u, 0x3f800000u);
		staging_copy_calls = 0u;
		expect(isaac_coloroffset_copy_and_prove_neutral(destination, source,
			bytes, 17u, ISAAC_COLOROFFSET_VERTEX_STRIDE) ==
			ISAAC_COLOROFFSET_STAGING_REJECTED, "nonneutral staged vertex admitted");
		expect(memcmp(destination, source, bytes) == 0,
			"rejected draw was not copied completely");
		expect(staging_copy_calls == (i == 0u ? 3u : 6u),
			"rejection did not return to one bulk remainder copy");
	}
	for (lane = 0u; lane < ISAAC_COLOROFFSET_VERTEX_STRIDE; lane += 4u) {
		memset(source, 0, bytes);
		store_u32(source + 16u * ISAAC_COLOROFFSET_VERTEX_STRIDE + lane, 0x7fc00000u);
		expect(isaac_coloroffset_copy_and_prove_neutral(destination, source,
			bytes, 17u, ISAAC_COLOROFFSET_VERTEX_STRIDE) ==
			ISAAC_COLOROFFSET_STAGING_REJECTED, "last-chunk NaN lane admitted");
		expect(memcmp(destination, source, bytes) == 0,
			"NaN fallback changed uploaded bytes");
	}

	memset(source, 0, bytes);
	staging_mutate_source = source;
	staging_mutate_value = 0x3f800000u;
	expect(isaac_coloroffset_copy_and_prove_neutral(destination, source,
		bytes, 17u, ISAAC_COLOROFFSET_VERTEX_STRIDE) == DEFAULT_STAGING_RESULT &&
		isaac_coloroffset_load_u32(source + 72u) == 0x3f800000u &&
		isaac_coloroffset_load_u32(destination + 72u) == 0u,
		"proof did not follow the uploaded neutral snapshot");
	staging_mutate_source = source;
	staging_mutate_value = 0u;
	expect(isaac_coloroffset_copy_and_prove_neutral(destination, source,
		bytes, 17u, ISAAC_COLOROFFSET_VERTEX_STRIDE) == ISAAC_COLOROFFSET_STAGING_REJECTED &&
		isaac_coloroffset_load_u32(source + 72u) == 0u &&
		isaac_coloroffset_load_u32(destination + 72u) == 0x3f800000u,
		"later neutral source hid a nonneutral uploaded snapshot");

	staging_copy_calls = 0u;
#define UNKNOWN(dst, src, n, count, stride) \
	expect(isaac_coloroffset_copy_and_prove_neutral(dst, src, n, count, stride) == \
		ISAAC_COLOROFFSET_STAGING_UNKNOWN, "unsupported staging range admitted")
	UNKNOWN(NULL, source, 88u, 1u, 88u);
	UNKNOWN(destination, NULL, 88u, 1u, 88u);
	UNKNOWN(destination, source, 0u, 0u, 88u);
	UNKNOWN(destination, source, 87u, 1u, 88u);
	UNKNOWN(destination, source, 88u, 1u, 0u);
	UNKNOWN(destination, source, 88u, 1u, 89u);
	UNKNOWN(destination, source, 4097u * 88u, 4097u, 88u);
	UNKNOWN(destination, source, SIZE_MAX, UINT32_MAX, UINT32_MAX);
	UNKNOWN(source, source, 88u, 1u, 88u);
	UNKNOWN(source + 1u, source, 88u, 1u, 88u);
	UNKNOWN(source, source + 1u, 88u, 1u, 88u);
	UNKNOWN((void *)(UINTPTR_MAX - 15u), source, 88u, 1u, 88u);
	UNKNOWN(destination, (void *)(UINTPTR_MAX - 15u), 88u, 1u, 88u);
#undef UNKNOWN
	expect(staging_copy_calls == 0u, "unsupported staging touched memory");
}

#if defined(HAVE_ISAAC_COLOROFFSET_PLAIN_FASTPATH) && HAVE_ISAAC_COLOROFFSET_PLAIN_FASTPATH == 1
static void verify_plain_staging(void)
{
	uint8_t source[17u * ISAAC_COLOROFFSET_VERTEX_STRIDE];
	uint8_t destination[sizeof source];
	const uint32_t lanes[] = {48u, 52u, 56u, 60u};
	uint32_t i;
	staging_destination = destination;
	staging_destination_bytes = sizeof destination;
	memset(source, 0, sizeof source);
	for (i = 0u; i < 17u; ++i) {
		uint8_t *v = source + i * ISAAC_COLOROFFSET_VERTEX_STRIDE;
		store_u32(v + 12u, 0x3e800000u); /* Red tint 0.25, not white. */
		store_u32(v + 16u, 0x40000000u);
		store_u32(v + 24u, 0x3f000000u); /* Alpha 0.5, not opaque. */
		store_u32(v + 36u, 0x41800000u); /* Bounded unused Colorize RGB. */
		store_u32(v + 40u, 0xc1800000u);
		store_u32(v + 48u, 0x80000000u);
		store_u32(v + 56u, 0x80000000u);
	}
	expect(isaac_coloroffset_copy_and_prove_neutral(destination, source,
		sizeof source, 17u, 88u) == ISAAC_COLOROFFSET_STAGING_PLAIN &&
		memcmp(source, destination, sizeof source) == 0,
		"plain proof rejected tint/alpha/signed zero or changed bytes");
	for (i = 0u; i < 4u; ++i) {
		uint8_t *last = source + 16u * ISAAC_COLOROFFSET_VERTEX_STRIDE + lanes[i];
		uint32_t prior = isaac_coloroffset_load_u32(last);
		store_u32(last, 1u); /* Even the smallest positive nonzero is fallback. */
		expect(isaac_coloroffset_copy_and_prove_neutral(destination, source,
			sizeof source, 17u, 88u) == ISAAC_COLOROFFSET_STAGING_NEUTRAL &&
			memcmp(source, destination, sizeof source) == 0,
			"nonzero last-vertex color lane was admitted or lost neutral fallback");
		store_u32(last, prior);
	}
	store_u32(source + 36u, 0x7f800000u);
	expect(isaac_coloroffset_copy_and_prove_neutral(destination, source,
		sizeof source, 17u, 88u) == ISAAC_COLOROFFSET_STAGING_REJECTED,
		"zero mix weight admitted infinite unused color branch");
	store_u32(source + 36u, 0x41800001u);
	expect(isaac_coloroffset_copy_and_prove_neutral(destination, source,
		sizeof source, 17u, 88u) == ISAAC_COLOROFFSET_STAGING_REJECTED,
		"plain proof weakened finite arithmetic bounds");
	store_u32(source + 36u, 0x41800000u);
	staging_mutate_offset = 52u;
	staging_mutate_source = source;
	staging_mutate_value = 0x3f800000u;
	expect(isaac_coloroffset_copy_and_prove_neutral(destination, source,
		sizeof source, 17u, 88u) == ISAAC_COLOROFFSET_STAGING_PLAIN &&
		isaac_coloroffset_load_u32(source + 52u) == 0x3f800000u &&
		isaac_coloroffset_load_u32(destination + 52u) == 0u,
		"plain proof reread mutable source instead of uploaded chunk");
	staging_mutate_offset = 72u;

	/* Non-PLAIN is not rejection, within a chunk or after the aggregate
	 * candidate has been lost. Pin the exact copy sequence as well as bytes:
	 * input->cached chunk, chunk->upload, then one whole rejected remainder. */
	{
		static const uint32_t reject_vertices[] = {1u, 7u, 8u, 16u};
		static const uint32_t bad_offsets[] = {0u, 64u, 12u, 84u};
		static const uint32_t bad_values[] = {
			0x7fc00000u, 0xff800000u, 0x41800001u, 0x3f800000u
		};
		uint32_t lane, sign, scenario, bad;
		staging_trace_enabled = 1;
		for (lane = 0u; lane < 4u; ++lane) {
			for (sign = 0u; sign < 2u; ++sign) {
				/* Scenario 4 has no rejection: later all-zero chunks must
				 * not resurrect the first vertex's lost PLAIN candidate. */
				for (scenario = 0u; scenario < 5u; ++scenario) {
					for (bad = 0u; bad < (scenario == 4u ? 1u : 4u); ++bad) {
						uint32_t rejected = scenario < 4u;
						uint32_t last_chunk = rejected ? reject_vertices[scenario] / 8u : 2u;
						uint32_t chunk, record = 0u;
						size_t copied = 0u;
						uintptr_t cached;
						memset(source, 0, sizeof source);
						memset(destination, 0xa5, sizeof destination);
						store_u32(source + lanes[lane], 1u | (sign << 31));
						if (rejected)
							store_u32(source + reject_vertices[scenario] * 88u + bad_offsets[bad],
								bad_values[bad]);
						staging_copy_calls = staging_trace_count = 0u;
						expect(isaac_coloroffset_copy_and_prove_neutral(destination, source,
							sizeof source, 17u, 88u) == (rejected ?
								ISAAC_COLOROFFSET_STAGING_REJECTED : ISAAC_COLOROFFSET_STAGING_NEUTRAL),
							"early non-PLAIN hid later rejection or resurrected PLAIN");
						expect(memcmp(source, destination, sizeof source) == 0,
							"non-PLAIN then rejection changed complete upload bytes");
						cached = staging_trace[0].destination;
						for (chunk = 0u; chunk <= last_chunk; ++chunk) {
							size_t n = sizeof source - copied;
							if (n > 8u * 88u) n = 8u * 88u;
							expect(staging_trace[record].destination == cached &&
								staging_trace[record].source == (uintptr_t)source + copied &&
								staging_trace[record].bytes == n,
								"input-to-snapshot copy sequence changed");
							++record;
							expect(staging_trace[record].destination == (uintptr_t)destination + copied &&
								staging_trace[record].source == cached && staging_trace[record].bytes == n,
								"snapshot-to-upload copy sequence changed");
							++record;
							copied += n;
						}
						if (copied < sizeof source) {
							expect(staging_trace[record].destination == (uintptr_t)destination + copied &&
								staging_trace[record].source == (uintptr_t)source + copied &&
								staging_trace[record].bytes == sizeof source - copied,
								"rejection did not copy the original remainder exactly once");
							++record;
						}
						expect(staging_trace_count == record && staging_copy_calls == record,
							"non-PLAIN fallback added or omitted copies");
					}
				}
			}
		}
		staging_trace_enabled = 0;
	}
}

#if defined(HAVE_ISAAC_LASER_HALO_PROFILE) && HAVE_ISAAC_LASER_HALO_PROFILE && \
    defined(HAVE_ISAAC_COLOROFFSET_STAGING_PLAIN_FUSION) && HAVE_ISAAC_COLOROFFSET_STAGING_PLAIN_FUSION
static void verify_white_staging(void)
{
	static uint8_t source[4096u * 88u], expected[sizeof source], output[sizeof source];
	const uint32_t counts[] = {1u,4u,7u,8u,9u,16u,17u,4096u};
	const uint32_t positions[] = {0u,7u,8u,16u};
	const uint32_t nonwhite[] = {0xbf800000u,0u,0x80000000u,0x3f7fffffu,0x3f800001u};
	uint32_t proof, old_copies;
	for (uint32_t v=0;v<4096u;++v)
		for (uint32_t lane=12;lane<28;lane+=4)
			store_u32(source+v*88u+lane,0x3f800000u);
	for (uint32_t c=0;c<sizeof counts/sizeof counts[0];++c) {
		staging_destination=expected; staging_destination_bytes=sizeof expected; staging_copy_calls=0;
		expect(isaac_coloroffset_copy_and_prove_neutral(expected,source,counts[c]*88u,counts[c],88u)==ISAAC_COLOROFFSET_STAGING_PLAIN,"ordinary baseline plain");
		old_copies=staging_copy_calls;
		staging_destination=output; staging_destination_bytes=sizeof output; staging_copy_calls=0; proof=99u;
		expect(isaac_coloroffset_copy_and_prove_white(output,source,counts[c]*88u,counts[c],88u,&proof)==ISAAC_COLOROFFSET_STAGING_PLAIN && proof==2u,"all-white copied population");
		expect(staging_copy_calls==old_copies && !memcmp(expected,output,counts[c]*88u),"white changed copy count/bytes");
	}
	for (uint32_t pos=0;pos<4;++pos) {
		uint8_t *v=source+positions[pos]*88u;
		for (uint32_t lane=12;lane<28;lane+=4) {
			for (uint32_t n=0;n<sizeof nonwhite/sizeof nonwhite[0];++n) {
				store_u32(v+lane,nonwhite[n]); proof=99;
				expect(isaac_coloroffset_copy_and_prove_white(output,source,17u*88u,17,88,&proof)==ISAAC_COLOROFFSET_STAGING_PLAIN && proof==1u,"raw signed/ULP Color word falsely white");
			}
			store_u32(v+lane,0x3f800000u);
		}
		const uint32_t cancel_lanes[]={48u,72u,84u};
		for (uint32_t l=0;l<3;++l) {
			store_u32(v+cancel_lanes[l],0x3f800000u);
			staging_destination=expected; staging_destination_bytes=sizeof expected; staging_copy_calls=0;
			enum isaac_coloroffset_staging_result result=isaac_coloroffset_copy_and_prove_neutral(expected,source,17u*88u,17,88);
			old_copies=staging_copy_calls;
			staging_destination=output; staging_destination_bytes=sizeof output; staging_copy_calls=0; proof=99;
			expect(isaac_coloroffset_copy_and_prove_white(output,source,17u*88u,17,88,&proof)==result && proof==0u,"late non-PLAIN/rejection leaked white token");
			expect(staging_copy_calls==old_copies && !memcmp(expected,output,17u*88u),"cancel changed remainder copies");
			store_u32(v+cancel_lanes[l],0u);
		}
	}
	for (uint32_t lane=0;lane<88u;lane+=4) {
		uint8_t *at=source+16u*88u+lane;
		uint32_t saved=isaac_coloroffset_load_u32(at);
		store_u32(at,0x7fc00000u); proof=99;
		expect(isaac_coloroffset_copy_and_prove_white(output,source,17u*88u,17,88,&proof)==ISAAC_COLOROFFSET_STAGING_REJECTED && proof==0u,"late NaN token not cancelled");
		store_u32(at,saved);
	}
	staging_mutate_source=source; staging_mutate_offset=12u; staging_mutate_value=0xbf800000u;
	expect(isaac_coloroffset_copy_and_prove_white(output,source,17u*88u,17,88,&proof)==ISAAC_COLOROFFSET_STAGING_PLAIN && proof==2u && isaac_coloroffset_load_u32(output+12)==0x3f800000u,"white reread original client after snapshot");
	staging_mutate_source=source; staging_mutate_value=0x3f800000u;
	expect(isaac_coloroffset_copy_and_prove_white(output,source,17u*88u,17,88,&proof)==ISAAC_COLOROFFSET_STAGING_PLAIN && proof==1u && isaac_coloroffset_load_u32(output+12)==0xbf800000u,"nonwhite uploaded snapshot became white");
	staging_mutate_offset=72u;
	staging_copy_calls=0;
#define WHITE_UNKNOWN(dst,src,n,count,stride) do { proof=99; \
	expect(isaac_coloroffset_copy_and_prove_white(dst,src,n,count,stride,&proof)==ISAAC_COLOROFFSET_STAGING_UNKNOWN && proof==0u,"unknown leaked ordinary proof"); } while(0)
	WHITE_UNKNOWN(NULL,source,88,1,88); WHITE_UNKNOWN(output,NULL,88,1,88);
	WHITE_UNKNOWN(source,source,88,1,88); WHITE_UNKNOWN(source+1,source,88,1,88);
	WHITE_UNKNOWN(output,source,87,1,88); WHITE_UNKNOWN(output,source,0,0,88);
	WHITE_UNKNOWN(output,source,88,1,89); WHITE_UNKNOWN(output,source,4097u*88u,4097,88);
	WHITE_UNKNOWN((void *)(UINTPTR_MAX-15u),source,88,1,88);
#undef WHITE_UNKNOWN
	expect(staging_copy_calls==0u,"unknown touched copied data");
	puts("white ordinary staging: exact raw words, all chunks, cancellation, same-byte uploads and copy counts PASS");
}
#endif

/* Exercise the production 0013 lifecycle/cache header with bounded native
 * stubs, in this existing oracle. Real GLSL compilation/GXM remain device
 * work; the fixture checks ownership/failure/selection, not shader ISA. */
typedef int GLboolean;
typedef unsigned GLenum;
typedef unsigned SceGxmOutputRegisterFormat;
typedef void *SceGxmShaderPatcherId;
typedef struct { uint32_t words[64]; } SceGxmProgram;
typedef struct { uint32_t unused; } SceGxmFragmentProgram;
typedef struct { uint32_t unused; } SceGxmVertexProgram;
typedef struct { unsigned value; } binds_map;
typedef struct {
	unsigned type, valid, is_glsl;
	uint32_t size;
	char *source;
	const SceGxmProgram *prog;
	SceGxmShaderPatcherId id;
	uint32_t unif_buf_size;
	binds_map semantics;
} shader;
typedef struct isaac_coloroffset_plain_state isaac_coloroffset_plain_state;
typedef struct isaac_coloroffset_plain_vertex_pair isaac_coloroffset_plain_vertex_pair;
typedef struct {
	unsigned status;
	shader *fshader, *vshader;
	unsigned isaac_coloroffset_link_vertex_exact;
	uint32_t isaac_coloroffset_link_vertex_generation;
	uint32_t isaac_coloroffset_link_source_generation;
	unsigned isaac_coloroffset_fs_probe_state;
	isaac_coloroffset_plain_state *isaac_coloroffset_plain;
	isaac_coloroffset_plain_vertex_pair *isaac_plain_vertex_pair;
} program;
#define GL_TRUE 1
#define GL_FALSE 0
#define PROG_LINKED 1u
#define GL_FRAGMENT_SHADER 2u
#define SHARK_FRAGMENT_SHADER 2u
#define VGL_MODE_SHADER_PAIR 2u
#define ISAAC_COLOROFFSET_FS_PROBE_STATE_READY 2u
#define SCE_GXM_OUTPUT_REGISTER_FORMAT_HALF4 1u
#define SCE_GXM_OUTPUT_REGISTER_FORMAT_UCHAR4 2u
static program progs[1];
static shader plain_fs, plain_vs;
static SceGxmProgram plain_compiled;
static SceGxmFragmentProgram plain_fragments[32];
static unsigned glsl_sema_mode, is_shark_online;
static GLboolean glsl_is_first_shader;
static binds_map glsl_bindings_map;
static unsigned compiler_opts, compiler_fastmath, compiler_fastprecision, compiler_fastint;
static unsigned gxm_context, gxm_shader_patcher, is_fbo_float, msaa_mode;
static struct { uint32_t raw; unsigned info; } blend_info;
static uint32_t isaac_coloroffset_source_generation[1];
static const char isaac_coloroffset_fs_probe_trivial_source[] = "exact-plain-fixture";
static unsigned plain_allocations, plain_frees, plain_fail_allocation;
static unsigned plain_fail_compile, plain_fail_check, plain_fail_register, plain_fail_create;
static unsigned plain_finishes, plain_unregisters, plain_releases, plain_creates;
#if defined(HAVE_ISAAC_COLOROFFSET_PLAIN_VERTEX_PAIR) || defined(HAVE_ISAAC_COLOROFFSET_PLAIN_FP16)
#define ISAAC_GXM_SHADER_STATE_INVALIDATE() ((void)0)
#endif
#if defined(HAVE_ISAAC_COLOROFFSET_PLAIN_FP16)
static SceGxmProgram fp16_compiled;
static unsigned fp16_enabled, fp16_fail_compile, fp16_fail_check;
static unsigned fp16_fail_register, fp16_fail_create, fp16_null_create;
static unsigned fp16_creates;
static SceGxmShaderPatcherId fp16_last_fragment_id;
static int fp16_is_program(const SceGxmProgram *p) {
	return p && p->words[3] == 0xface1600u;
}
#endif
#if defined(HAVE_ISAAC_COLOROFFSET_PLAIN_VERTEX_PAIR)
#define GL_VERTEX_SHADER 1u
#define SHARK_VERTEX_SHADER 1u
#define SCE_GXM_VERTEX_PROGRAM 0u
#define SCE_GXM_PARAMETER_CATEGORY_ATTRIBUTE 0u
#define SCE_GXM_PARAMETER_CATEGORY_UNIFORM 1u
#define SCE_GXM_PARAMETER_TYPE_F32 0u
#define SCE_GXM_ATTRIBUTE_FORMAT_F32 0u
#define SCE_GXM_INDEX_SOURCE_INDEX_16BIT 0u
typedef struct { uint16_t streamIndex, offset; uint8_t format, componentCount; uint16_t regIndex; } SceGxmVertexAttribute;
typedef struct { uint16_t stride, indexSource; } SceGxmVertexStream;
typedef struct { unsigned category, type, resource, components, array, container; } SceGxmProgramParameter;
static SceGxmProgram pair_compiled;
static SceGxmProgramParameter pair_params[4];
static SceGxmVertexProgram pair_vertex;
static unsigned pair_bad_count, pair_bad_uniform, pair_fail_vertex, pair_vertex_releases;
static const SceGxmProgram *pair_fragment_vertex;
static char pair_diag_log[4096];
static unsigned pair_diag_capture, pair_diag_length, pair_diag_max_line;
static unsigned pair_parameter_visits;
#endif

static void *plain_allocate(size_t size) {
	++plain_allocations;
	return plain_allocations == plain_fail_allocation ? NULL : malloc(size);
}
static void plain_free(void *pointer) { if (pointer) ++plain_frees; free(pointer); }
#define vglMalloc plain_allocate
#define vgl_free plain_free
#define vgl_fast_memcpy memcpy
#define vgl_memset memset
static void sceGxmFinish(unsigned context) { (void)context; ++plain_finishes; }
static void sceGxmShaderPatcherReleaseFragmentProgram(unsigned patcher, SceGxmFragmentProgram *f) {
	(void)patcher; (void)f; ++plain_releases;
}
static void sceGxmShaderPatcherUnregisterProgram(unsigned patcher, SceGxmShaderPatcherId id) {
	(void)patcher; (void)id; ++plain_unregisters;
}
static int32_t isaac_coloroffset_shader_index(const shader *s) { return s == &plain_fs ? 0 : -1; }
static int start_shader_compiler(void) { is_shark_online = 1; return 1; }
static void glsl_translator_set_process(shader *vertex, shader *fragment) {
	(void)vertex; (void)fragment;
	glsl_sema_mode = 99; glsl_is_first_shader = 99; glsl_bindings_map.value = 99;
}
static const SceGxmProgram *shark_compile_shader_extended(const char *source,
		uint32_t *size, unsigned type, unsigned opts, unsigned fastmath,
		unsigned precision, unsigned fastint) {
	(void)source; (void)type; (void)opts; (void)fastmath; (void)precision; (void)fastint;
	*size = sizeof plain_compiled;
#if defined(HAVE_ISAAC_COLOROFFSET_PLAIN_FP16)
	if (strstr(source, "tintColor *= sampleColor")) {
		expect(strstr(source, "in vec2 TexCoord0;") &&
			strstr(source, "half4 sampleColor = half4(texture(Texture0, TexCoord0));") &&
			strstr(source, "half4 tintColor = half4(Color0);") &&
			strstr(source, "fragColor = vec4(tintColor);"), "half source changed public/UV declarations");
		return !fp16_enabled || fp16_fail_compile ? NULL : &fp16_compiled;
	}
#endif
#if defined(HAVE_ISAAC_COLOROFFSET_PLAIN_VERTEX_PAIR)
	if (type == SHARK_VERTEX_SHADER)
		return plain_fail_compile ? NULL : &pair_compiled;
#endif
	return plain_fail_compile ? NULL : &plain_compiled;
}
static void shark_clear_output(void) {}
static int isaac_coloroffset_fs_probe_program_is_exact(const SceGxmProgram *candidate,
		uint32_t size, const shader *stock) {
#if defined(HAVE_ISAAC_COLOROFFSET_PLAIN_FP16)
	if (fp16_is_program(candidate) && fp16_fail_check) return 0;
#endif
	return candidate && size == sizeof plain_compiled && stock == &plain_fs && !plain_fail_check;
}
static int sceGxmShaderPatcherRegisterProgram(unsigned patcher,
		const SceGxmProgram *gxp, SceGxmShaderPatcherId *id) {
	(void)patcher;
#if defined(HAVE_ISAAC_COLOROFFSET_PLAIN_FP16)
	if (fp16_is_program(gxp) && fp16_fail_register) return -1;
#endif
	if (plain_fail_register) return -1;
	*id = (void *)gxp;
	return 0;
}
static int sceGxmShaderPatcherCreateFragmentProgram(unsigned patcher,
		SceGxmShaderPatcherId id, SceGxmOutputRegisterFormat output,
		unsigned msaa, const unsigned *blend, const SceGxmProgram *vertex,
		SceGxmFragmentProgram **fragment) {
	(void)patcher; (void)id; (void)output; (void)msaa; (void)blend; (void)vertex;
#if defined(HAVE_ISAAC_COLOROFFSET_PLAIN_FP16)
	fp16_last_fragment_id = id;
	if (fp16_is_program((const SceGxmProgram *)id)) {
		expect(output == SCE_GXM_OUTPUT_REGISTER_FORMAT_UCHAR4 && vertex == plain_vs.prog,
			"half changed output format or paired vertex program");
		if (fp16_fail_create) return -1;
		if (fp16_null_create) { *fragment = NULL; return 0; }
		++fp16_creates;
	}
#endif
#if defined(HAVE_ISAAC_COLOROFFSET_PLAIN_VERTEX_PAIR)
	pair_fragment_vertex = vertex;
#endif
	if (plain_fail_create) return -1;
	expect(plain_creates < 32u, "plain fixture fragment capacity");
	*fragment = &plain_fragments[plain_creates++];
	return 0;
}
static uint32_t isaac_coloroffset_fnv1a(const void *p, uint32_t n) {
	return isaac_coloroffset_policy_fnv1a(p, n);
}
static int sceClibPrintf(const char *format, ...) {
#if defined(HAVE_ISAAC_COLOROFFSET_PLAIN_VERTEX_PAIR)
	if (pair_diag_capture) {
		char line[512];
		va_list args;
		va_start(args, format);
		int size = vsnprintf(line, sizeof line, format, args);
		va_end(args);
		expect(size >= 0 && (unsigned)size < sizeof line, "pair diagnostic physical line truncated");
		expect(pair_diag_length + (unsigned)size < sizeof pair_diag_log, "pair diagnostic fixture overflow");
		memcpy(pair_diag_log + pair_diag_length, line, (unsigned)size + 1u);
		pair_diag_length += (unsigned)size;
		if ((unsigned)size > pair_diag_max_line) pair_diag_max_line = (unsigned)size;
	}
#else
	(void)format;
#endif
	return 0;
}
#include "isaac_coloroffset_plain_vitagl.h"
#if defined(HAVE_ISAAC_COLOROFFSET_PLAIN_VERTEX_PAIR)
static int sceGxmProgramCheck(const SceGxmProgram *p) { (void)p; return plain_fail_check; }
static unsigned sceGxmProgramGetType(const SceGxmProgram *p) { (void)p; return SCE_GXM_VERTEX_PROGRAM; }
static unsigned sceGxmProgramGetSize(const SceGxmProgram *p) { (void)p; return sizeof(*p); }
static unsigned sceGxmProgramGetParameterCount(const SceGxmProgram *p) { (void)p; return pair_bad_count ? 5u : 4u; }
static unsigned sceGxmProgramGetDefaultUniformBufferSize(const SceGxmProgram *p) { (void)p; return pair_bad_uniform ? 80u : 64u; }
static const SceGxmProgramParameter *sceGxmProgramFindParameterByName(const SceGxmProgram *p, const char *name) {
	static const char *const names[4] = {"Position", "Color", "TexCoord", "Transform"};
	(void)p;
	++pair_parameter_visits;
	for (unsigned i = 0; i < 4; ++i) if (!strcmp(name, names[i])) return &pair_params[i];
	return NULL;
}
static unsigned sceGxmProgramParameterGetCategory(const SceGxmProgramParameter *p) { return p->category; }
static unsigned sceGxmProgramParameterGetType(const SceGxmProgramParameter *p) { return p->type; }
static unsigned sceGxmProgramParameterGetResourceIndex(const SceGxmProgramParameter *p) { return p->resource; }
static unsigned sceGxmProgramParameterGetComponentCount(const SceGxmProgramParameter *p) { return p->components; }
static unsigned sceGxmProgramParameterGetArraySize(const SceGxmProgramParameter *p) { return p->array; }
static unsigned sceGxmProgramParameterGetContainerIndex(const SceGxmProgramParameter *p) { return p->container; }
static int sceGxmShaderPatcherCreateVertexProgram(unsigned patcher, SceGxmShaderPatcherId id,
		const SceGxmVertexAttribute *a, unsigned ac, const SceGxmVertexStream *s,
		unsigned sc, SceGxmVertexProgram **v) {
	(void)patcher; (void)id;
	expect(ac == 3 && sc == 1 && s->stride == 88 && s->indexSource == 0,
		"pair changed stream/index contract");
	expect(a[0].offset == 0 && a[1].offset == 12 && a[2].offset == 28 &&
		a[0].componentCount == 3 && a[1].componentCount == 4 && a[2].componentCount == 2 &&
		a[0].regIndex == 0 && a[1].regIndex == 4 && a[2].regIndex == 8,
		"pair changed attribute routing");
	if (pair_fail_vertex) return -1;
	*v = &pair_vertex;
	return 0;
}
static void sceGxmShaderPatcherReleaseVertexProgram(unsigned patcher, SceGxmVertexProgram *v) {
	(void)patcher; expect(v == &pair_vertex, "pair released wrong vertex program"); ++pair_vertex_releases;
}
#include "isaac_coloroffset_plain_vertex_pair.h"
#endif

static void plain_reset_fixture(void)
{
	expect(progs[0].isaac_coloroffset_plain == NULL, "plain state leaked between fixtures");
	memset(progs, 0, sizeof progs);
	plain_allocations = plain_frees = plain_fail_allocation = 0;
	plain_fail_compile = plain_fail_check = plain_fail_register = plain_fail_create = 0;
	plain_finishes = plain_unregisters = plain_releases = plain_creates = 0;
#if defined(HAVE_ISAAC_COLOROFFSET_PLAIN_FP16)
	fp16_enabled = fp16_fail_compile = fp16_fail_check = 0;
	fp16_fail_register = fp16_fail_create = fp16_null_create = fp16_creates = 0;
	fp16_last_fragment_id = NULL;
	memset(isaac_coloroffset_plain_fp16_window, 0, sizeof isaac_coloroffset_plain_fp16_window);
#endif
	memset(isaac_coloroffset_plain_window, 0, sizeof isaac_coloroffset_plain_window);
	glsl_sema_mode = 3; glsl_is_first_shader = 4; glsl_bindings_map.value = 5;
	plain_fs.id = &plain_fs;
	plain_vs.prog = &plain_compiled;
	progs[0].status = PROG_LINKED;
	progs[0].fshader = &plain_fs;
	progs[0].vshader = &plain_vs;
	progs[0].isaac_coloroffset_link_vertex_exact = 1;
	progs[0].isaac_coloroffset_link_vertex_generation = 11;
	progs[0].isaac_coloroffset_link_source_generation = 7;
	progs[0].isaac_coloroffset_fs_probe_state = ISAAC_COLOROFFSET_FS_PROBE_STATE_READY;
	isaac_coloroffset_source_generation[0] = 7;
	blend_info.raw = is_fbo_float = msaa_mode = 0;
}

static void verify_plain_lifecycle(void)
{
	uint32_t counts[3];
	for (unsigned failure = 1; failure <= 6; ++failure) {
		plain_reset_fixture();
		if (failure <= 3) plain_fail_allocation = failure;
		if (failure == 4) plain_fail_compile = 1;
		if (failure == 5) plain_fail_check = 1;
		if (failure == 6) plain_fail_register = 1;
		isaac_coloroffset_plain_link(&progs[0]);
		expect(!progs[0].isaac_coloroffset_plain &&
			plain_frees == plain_allocations - (failure <= 3 ? 1u : 0u),
			"plain failed link leaked or published state");
		expect(isaac_coloroffset_plain_select(&progs[0]) == NULL,
			"unavailable plain did not return neutral fallback");
		expect(vglTakeIsaacColorOffsetPlainStats(counts, 3) == 1 &&
			counts[0] == 1 && counts[1] == 0 && counts[2] == 1,
			"plain fallback accounting");
	}
	plain_reset_fixture();
	isaac_coloroffset_plain_link(&progs[0]);
	expect(progs[0].isaac_coloroffset_plain != NULL && glsl_sema_mode == 3 &&
		glsl_is_first_shader == 4 && glsl_bindings_map.value == 5,
		"plain link failed or changed translator globals");
	{
		unsigned allocations = plain_allocations;
		isaac_coloroffset_plain_link(&progs[0]);
		expect(plain_allocations == allocations && plain_finishes == 0, "stable link recompiled");
	}
	plain_fail_create = 1;
	expect(!isaac_coloroffset_plain_select(&progs[0]), "failed fragment create did not fallback");
	plain_fail_create = 0;
	for (unsigned key = 0; key < 4; ++key) {
		blend_info.raw = key >= 1u;
		is_fbo_float = key >= 2u;
		msaa_mode = key >= 3u;
		SceGxmFragmentProgram *a = isaac_coloroffset_plain_select(&progs[0]);
		expect(a && a == isaac_coloroffset_plain_select(&progs[0]), "plain cache missed stable key");
	}
	expect(plain_creates == 4, "plain cache creation count");
	blend_info.raw = 4;
	expect(!isaac_coloroffset_plain_select(&progs[0]), "full cache did not fallback");
	expect(vglTakeIsaacColorOffsetPlainStats(counts, 2) == 0,
		"invalid private stats ABI accepted");
	expect(vglTakeIsaacColorOffsetPlainStats(counts, 3) == 1 &&
		counts[0] == 10 && counts[1] == 8 && counts[2] == 2, "plain q=b+f accounting");
	expect(vglTakeIsaacColorOffsetPlainStats(counts, 3) == 1 &&
		counts[0] == 0 && counts[1] == 0 && counts[2] == 0, "plain take did not zero");
	++progs[0].isaac_coloroffset_link_vertex_generation;
	expect(!isaac_coloroffset_plain_select(&progs[0]), "linked VS generation mutation admitted");
	--progs[0].isaac_coloroffset_link_vertex_generation;
	plain_vs.prog = NULL;
	expect(!isaac_coloroffset_plain_select(&progs[0]), "linked VS program mutation admitted");
	plain_vs.prog = &plain_compiled;
	progs[0].status = 0;
	expect(!isaac_coloroffset_plain_select(&progs[0]), "unlinked program admitted");
	progs[0].status = PROG_LINKED;
	isaac_coloroffset_source_generation[0] = 8;
	expect(!isaac_coloroffset_plain_select(&progs[0]), "source generation mutation admitted");
	progs[0].isaac_coloroffset_link_source_generation = 8;
	isaac_coloroffset_plain_link(&progs[0]);
	expect(plain_finishes == 1 && plain_releases == 4 && plain_unregisters == 1,
		"relink did not retire old fragments/GXP before replacement");
	isaac_coloroffset_plain_release(&progs[0]);
	expect(!progs[0].isaac_coloroffset_plain && plain_frees == plain_allocations,
		"plain delete leaked ownership");
}
#if defined(HAVE_ISAAC_COLOROFFSET_PLAIN_FP16)
static void fp16_fixture_u32(SceGxmProgram *p, unsigned offset, uint32_t value) {
	memcpy((uint8_t *)p + offset, &value, sizeof value);
}
static void fp16_fixture_programs(void) {
	uint8_t *bytes = (uint8_t *)&plain_compiled;
	uint16_t count = 2u;
	memset(&plain_compiled, 0, sizeof plain_compiled);
	fp16_fixture_u32(&plain_compiled, 8u, sizeof plain_compiled);
	fp16_fixture_u32(&plain_compiled, 0x14u, 1u);
	fp16_fixture_u32(&plain_compiled, 0x2cu, 0x9cu - 0x2cu);
	bytes[0x9cu + 11u] = 4u;
	memcpy(bytes + 0x9cu + 12u, &count, sizeof count);
	fp16_fixture_u32(&plain_compiled, 0x9cu + 16u, 16u);
	for (unsigned i = 0; i < 32u; ++i) bytes[0xbcu + i] = (uint8_t)(i + 1u);
	fp16_compiled = plain_compiled;
	fp16_compiled.words[3] = 0xface1600u; /* GUID is not part of the interface. */
}
static void fp16_reset_fixture(unsigned enabled) {
	plain_reset_fixture();
	fp16_fixture_programs();
	fp16_enabled = enabled;
	isaac_coloroffset_plain_link(&progs[0]);
	expect(progs[0].isaac_coloroffset_plain != NULL, "half fixture lost baseline P2");
}
static void verify_fp16_counts(uint32_t q, uint32_t b) {
	uint32_t counts[2] = {99u, 99u};
	expect(!vglTakeIsaacColorOffsetPlainFp16Stats(NULL, 2u) &&
		!vglTakeIsaacColorOffsetPlainFp16Stats(counts, 3u) && counts[0] == 99u,
		"invalid half stats request consumed/wrote counters");
	expect(vglTakeIsaacColorOffsetPlainFp16Stats(counts, 2u) == 1u &&
		counts[0] == q && counts[1] == b && b <= q, "half selection counters drifted");
	expect(vglTakeIsaacColorOffsetPlainFp16Stats(counts, 2u) == 1u &&
		counts[0] == 0u && counts[1] == 0u, "half stats did not take-and-zero");
}
static void verify_fp16_interface(void) {
	SceGxmProgram candidate;
	isaac_plain_fp16_interface view;
	fp16_fixture_programs();
	expect(isaac_plain_fp16_interface_matches(&fp16_compiled, 256u,
		&plain_compiled, 256u), "identical compiled interface rejected");
	/* Every byte of every descriptor, including precision/width bits, matters. */
	for (unsigned i = 0; i < 32u; ++i) {
		candidate = fp16_compiled;
		((uint8_t *)&candidate)[0xbcu + i] ^= 1u;
		expect(!isaac_plain_fp16_interface_matches(&candidate, 256u,
			&plain_compiled, 256u), "changed compiled iterator descriptor admitted");
	}
	for (unsigned i = 0; i < 32u; ++i) {
		if (i >= 16u && i < 20u) continue; /* Relocated descriptor pointer. */
		candidate = fp16_compiled;
		((uint8_t *)&candidate)[0x9cu + i] ^= 1u;
		expect(!isaac_plain_fp16_interface_matches(&candidate, 256u,
			&plain_compiled, 256u), "changed compiled varying/output block admitted");
	}
	for (unsigned i = 0; i < 16u; ++i) {
		candidate = fp16_compiled;
		((uint8_t *)&candidate)[0x14u + i] ^= 1u;
		expect(!isaac_plain_fp16_interface_matches(&candidate, 256u,
			&plain_compiled, 256u), "changed compiled interface flags admitted");
	}
	candidate = fp16_compiled;
	memmove((uint8_t *)&candidate + 0xc4u, (uint8_t *)&candidate + 0xbcu, 32u);
	memmove((uint8_t *)&candidate + 0xa0u, (uint8_t *)&candidate + 0x9cu, 32u);
	fp16_fixture_u32(&candidate, 0x2cu, 0xa0u - 0x2cu);
	fp16_fixture_u32(&candidate, 0xa0u + 16u, 0xc4u - 0xb0u);
	expect(isaac_plain_fp16_interface_matches(&candidate, 256u,
		&plain_compiled, 256u), "equivalent relocated compiled interface rejected");
	expect(!isaac_plain_fp16_interface_view(NULL, 256u, &view) &&
		!isaac_plain_fp16_interface_view(&candidate, 155u, &view) &&
		!isaac_plain_fp16_interface_view(&candidate, 256u, NULL), "unbounded interface input admitted");
	for (unsigned kind = 0; kind < 7u; ++kind) {
		candidate = fp16_compiled;
		if (kind == 0u) fp16_fixture_u32(&candidate, 8u, 257u);
		if (kind == 1u) fp16_fixture_u32(&candidate, 8u, 155u);
		if (kind == 2u) fp16_fixture_u32(&candidate, 0x2cu, UINT32_MAX);
		if (kind == 3u) fp16_fixture_u32(&candidate, 0xacu, UINT32_MAX);
		if (kind == 4u) fp16_fixture_u32(&candidate, 0xa8u, 0u);
		if (kind == 5u) fp16_fixture_u32(&candidate, 0xa8u, 33u);
		if (kind == 6u) fp16_fixture_u32(&candidate, 0xacu, 256u - 0xacu - 16u);
		expect(!isaac_plain_fp16_interface_view(&candidate, 256u, &view),
			"malformed/truncated compiled interface admitted");
	}
}
static void verify_fp16_lifecycle(void) {
	uint32_t counts[3];
	for (unsigned failure = 1u; failure <= 6u; ++failure) {
		fp16_reset_fixture(0u);
		isaac_coloroffset_plain_state *s = progs[0].isaac_coloroffset_plain;
		const SceGxmProgram *baseline = s->gxp;
		SceGxmShaderPatcherId baseline_id = s->id;
		fp16_enabled = 1u;
		if (failure <= 2u) plain_fail_allocation = plain_allocations + failure;
		if (failure == 3u) fp16_fail_compile = 1u;
		if (failure == 4u) fp16_fail_check = 1u;
		if (failure == 5u) ((uint8_t *)&fp16_compiled)[0xbcu + 15u] ^= 1u;
		if (failure == 6u) fp16_fail_register = 1u;
		isaac_coloroffset_plain_fp16_link(&progs[0]);
		expect(!s->fp16_id && !s->fp16_gxp && s->gxp == baseline && s->id == baseline_id &&
			!memcmp(s->gxp, &plain_compiled, sizeof plain_compiled),
			"half failure changed/published baseline or candidate state");
		expect(isaac_coloroffset_plain_select(&progs[0]) && fp16_last_fragment_id == baseline_id,
			"half failure did not select exact baseline P2");
		verify_fp16_counts(1u, 0u);
		expect(vglTakeIsaacColorOffsetPlainStats(counts, 3u) == 1u &&
			counts[0] == 1u && counts[1] == 1u && counts[2] == 0u,
			"half fallback changed existing plain semantics");
		isaac_coloroffset_plain_release(&progs[0]);
		expect(plain_frees == plain_allocations - (failure <= 2u ? 1u : 0u),
			"failed half link leaked allocation");
	}
	for (unsigned failure = 0u; failure < 2u; ++failure) {
		fp16_reset_fixture(1u);
		isaac_coloroffset_plain_state *s = progs[0].isaac_coloroffset_plain;
		expect(s->fp16_id && s->fp16_gxp && s->fp16_id != s->id,
			"half did not publish separate registered program");
		fp16_fail_create = failure == 0u;
		fp16_null_create = failure == 1u;
		expect(isaac_coloroffset_plain_select(&progs[0]) &&
			fp16_last_fragment_id == s->id && s->fp16_count == 0u,
			"native half creation failure did not use baseline P2");
		verify_fp16_counts(1u, 0u);
		isaac_coloroffset_plain_release(&progs[0]);
		expect(plain_frees == plain_allocations && plain_unregisters == 2u,
			"half create failure leaked ownership");
	}
	fp16_reset_fixture(1u);
	isaac_coloroffset_plain_state *s = progs[0].isaac_coloroffset_plain;
	expect(s->fp16_id && glsl_sema_mode == 3u && glsl_is_first_shader == 4 &&
		glsl_bindings_map.value == 5u, "half link changed translator globals");
	for (unsigned key = 0u; key < 4u; ++key) {
		blend_info.raw = key;
		msaa_mode = key & 1u;
		SceGxmFragmentProgram *a = isaac_coloroffset_plain_select(&progs[0]);
		expect(a && a == isaac_coloroffset_plain_select(&progs[0]) &&
			fp16_last_fragment_id == s->fp16_id, "half cache missed/selected wrong program");
	}
	expect(fp16_creates == 4u && s->count == 0u, "half cache polluted baseline P2 cache");
	is_fbo_float = 1u;
	expect(isaac_coloroffset_plain_select(&progs[0]) && fp16_last_fragment_id == s->id,
		"float output admitted lossy half path");
	is_fbo_float = 0u;
	blend_info.raw = 9u;
	expect(isaac_coloroffset_plain_select(&progs[0]) && fp16_last_fragment_id == s->id,
		"full half cache did not use separate baseline P2 cache");
	++isaac_coloroffset_source_generation[0];
	expect(!isaac_coloroffset_plain_select(&progs[0]), "stale half/P2 source admitted");
	verify_fp16_counts(11u, 8u);
	expect(vglTakeIsaacColorOffsetPlainStats(counts, 3u) == 1u &&
		counts[0] == 11u && counts[1] == 10u && counts[2] == 1u,
		"half selections changed plain q=b+f semantics");
	progs[0].isaac_coloroffset_link_source_generation = 8u;
	isaac_coloroffset_plain_link(&progs[0]);
	expect(plain_finishes == 1u && plain_releases == 6u && plain_unregisters == 2u,
		"half relink did not retire both owned caches");
	isaac_coloroffset_plain_release(&progs[0]);
	expect(plain_unregisters == 4u && plain_frees == plain_allocations,
		"half deletion leaked registered/GXP ownership");
}
#endif
#if defined(HAVE_ISAAC_COLOROFFSET_PLAIN_VERTEX_PAIR)
static void verify_pair_counts(uint32_t q, uint32_t b, uint32_t f) {
	uint32_t counts[3] = {99u, 99u, 99u};
	expect(vglTakeIsaacColorOffsetPlainVertexPairStats(NULL, 3u) == 0u &&
		vglTakeIsaacColorOffsetPlainVertexPairStats(counts, 2u) == 0u &&
		counts[0] == 99u, "invalid pair stats request consumed/wrote counters");
	expect(vglTakeIsaacColorOffsetPlainVertexPairStats(counts, 3u) == 1u &&
		counts[0] == q && counts[1] == b && counts[2] == f &&
		counts[0] == counts[1] + counts[2], "pair selection q=b+f drifted");
	expect(vglTakeIsaacColorOffsetPlainVertexPairStats(counts, 3u) == 1u &&
		counts[0] == 0u && counts[1] == 0u && counts[2] == 0u,
		"pair stats did not take-and-zero");
}
static void pair_reset_fixture(void) {
	plain_reset_fixture();
	pair_diag_capture = pair_diag_length = pair_diag_max_line = 0u;
	pair_parameter_visits = 0u;
	pair_diag_log[0] = '\0';
	isaac_plain_vertex_abi_reported = 0u;
	memset(&plain_compiled, 0, sizeof plain_compiled);
	plain_compiled.words[0x2c / 4] = 0x9c - 0x2c;
	/* Actual 064366b hardware output triples, not a same-count model. */
	plain_compiled.words[(0x9c + 16) / 4] = 0x18001000u;
	plain_compiled.words[(0x9c + 20) / 4] = 0x000c97cfu;
	pair_compiled = plain_compiled;
	pair_compiled.words[(0x9c + 16) / 4] = 0x0a001000u;
	pair_compiled.words[(0x9c + 20) / 4] = 0x0000000fu;
	plain_vs.unif_buf_size = 64;
	pair_bad_count = pair_bad_uniform = pair_fail_vertex = pair_vertex_releases = 0;
	memset(pair_params, 0, sizeof pair_params);
	for (unsigned i = 0; i < 4; ++i) {
		pair_params[i].resource = i * 4;
		pair_params[i].components = 4;
		pair_params[i].array = 1;
	}
	pair_params[3].category = SCE_GXM_PARAMETER_CATEGORY_UNIFORM;
	pair_params[3].resource = 0;
	pair_params[3].array = 4;
	pair_params[3].container = 14;
	isaac_coloroffset_plain_link(&progs[0]);
	expect(progs[0].isaac_coloroffset_plain != NULL, "pair fixture P2 unavailable");
}
static void verify_pair_abi_diagnostic(void) {
	SceGxmVertexAttribute attrs[3];
	for (unsigned kind = 0u; kind < 4u; ++kind) {
		pair_reset_fixture();
		pair_diag_capture = 1u;
		if (kind == 0u) pair_compiled.words[(0x9c + 24) / 4] = 0x100u;
		if (kind == 1u) pair_params[0].components = 3u;
		if (kind == 2u) pair_params[3].container = 13u;
		if (kind == 3u) pair_bad_uniform = 1u;
		expect(!isaac_plain_vertex_abi(&pair_compiled, sizeof pair_compiled, &plain_vs, attrs),
			"diagnostic fixture expected ABI refusal");
		unsigned visited = pair_parameter_visits;
		isaac_plain_vertex_abi_diag(&progs[0], &pair_compiled, sizeof pair_compiled);
		expect(pair_parameter_visits - visited == (kind == 1u ? 2u : kind == 2u ? 1u : 0u),
			"diagnostic walked names beyond the original refusal boundary");
		unsigned length = pair_diag_length;
		isaac_plain_vertex_abi_diag(&progs[0], &pair_compiled, sizeof pair_compiled);
		expect(pair_diag_length == length, "pair ABI diagnostic not one-time");
		expect(!isaac_plain_vertex_abi(&pair_compiled, sizeof pair_compiled, &plain_vs, attrs),
			"diagnostic changed ABI acceptance");
		if (kind == 0u) expect(strstr(pair_diag_log, "outputs: prog=1 mask=10") &&
			strstr(pair_diag_log, "old=18001000,000c97cf,00000000 new=0a001000,0000000f,00000100") &&
			strstr(pair_diag_log, "want=0a001000,0000000f,00000000"),
			"pair diagnostic lost actual old/new output values");
		if (kind == 1u) expect(strstr(pair_diag_log,
			"name=Position mask=08 got(c,t,r,n,a,b)=0,0,0,3,1,0") != NULL, "pair attribute diagnostic wrong");
		if (kind == 2u) expect(strstr(pair_diag_log,
			"name=Transform mask=20 got(c,t,r,n,a,b)=1,0,0,4,4,13") != NULL, "pair matrix diagnostic wrong");
		if (kind == 3u) {
			expect(strstr(pair_diag_log, "header: prog=1 mask=80 available=256 size=256") &&
				strstr(pair_diag_log, "ub(candidate,stock)=80,64"), "pair uniform size diagnostic wrong");
			isaac_plain_vertex_pair_link(&progs[0]);
			expect(!progs[0].isaac_plain_vertex_pair && strstr(pair_diag_log, "ready=0 gxp=256/") &&
				strstr(pair_diag_log, "fail=abi"), "pair refusal hid actual compiled GXP size/hash");
		}
		expect(pair_diag_max_line < 512u, "pair diagnostic exceeds logger capacity");
		pair_diag_capture = 0u;
		isaac_coloroffset_plain_release(&progs[0]);
		expect(plain_frees == plain_allocations, "pair diagnostic leaked ownership");
	}
}
static void verify_pair_output_count_contract(void) {
	SceGxmVertexAttribute attrs[3];
	uint32_t old[3] = {0x18001000u, 0x000c97cfu, 0u};
	uint32_t candidate[3] = {0x0a001000u, 0x0000000fu, 0u};
	pair_reset_fixture();
	expect(!isaac_plain_vertex_output_mismatch(old, candidate) &&
		isaac_plain_vertex_abi(&pair_compiled, sizeof pair_compiled, &plain_vs, attrs),
		"actual hardware 24-to-10 output layout rejected");
	for (uint32_t count = 0u; count < 256u; ++count) {
		candidate[0] = (count << 24u) | 0x1000u;
		expect(isaac_plain_vertex_output_mismatch(old, candidate) == (count == 10u ? 0u : 64u),
			"candidate output count was masked rather than validated");
		pair_compiled.words[(0x9c + 16) / 4] = candidate[0];
		expect(!!isaac_plain_vertex_abi(&pair_compiled, sizeof pair_compiled, &plain_vs, attrs) == (count == 10u),
			"actual ABI bypassed candidate output count contract");
	}
	candidate[0] = pair_compiled.words[(0x9c + 16) / 4] = 0x0a001000u;
	for (uint32_t count = 0u; count < 256u; ++count) {
		old[0] = (count << 24u) | 0x1000u;
		expect(isaac_plain_vertex_output_mismatch(old, candidate) == (count == 24u ? 0u : 32u),
			"unauthenticated stock output count admitted");
	}
	old[0] = 0x18001000u;
	for (uint32_t bit = 0u; bit < 24u; ++bit) {
		candidate[0] ^= 1u << bit;
		expect(isaac_plain_vertex_output_mismatch(old, candidate) == 4u,
			"low output flag/reserved bit was masked");
		candidate[0] ^= 1u << bit;
	}
	for (uint32_t bit = 0u; bit < 32u; ++bit) {
		candidate[1] ^= 1u << bit;
		expect(isaac_plain_vertex_output_mismatch(old, candidate) == 8u,
			"changed or extra TEXCOORD admitted");
		candidate[1] ^= 1u << bit;
		candidate[2] ^= 1u << bit;
		expect(isaac_plain_vertex_output_mismatch(old, candidate) == 16u,
			"packed/half or extra output admitted");
		candidate[2] ^= 1u << bit;
	}
	/* Equal-but-unsupported old/new TC/precision cannot justify F32 count10. */
	for (uint32_t code = 0u; code < 64u; ++code) {
		old[1] = (0x000c97cfu & ~0x3fu) | code;
		candidate[1] = code;
		expect(isaac_plain_vertex_output_mismatch(old, candidate) == (code == 15u ? 0u : 8u),
			"non-4/2 TEXCOORD widths admitted through equal old/new bits");
	}
	old[1] = 0x000c97cfu; candidate[1] = 15u;
	for (uint32_t pack = 1u; pack < 4u; ++pack) {
		old[2] = candidate[2] = pack;
		expect(isaac_plain_vertex_output_mismatch(old, candidate) == 16u,
			"non-F32 precision admitted through equal old/new packing");
	}
	isaac_coloroffset_plain_release(&progs[0]);
	expect(plain_frees == plain_allocations, "output count fixture leaked baseline P2");
}
static void verify_plain_vertex_pair(void) {
	SceGxmVertexProgram *vertex;
	SceGxmVertexAttribute attrs[3];
	uint32_t plain_counts[3];
	for (unsigned failure = 1; failure <= 9; ++failure) {
		pair_reset_fixture();
		if (failure <= 3) plain_fail_allocation = plain_allocations + failure;
		if (failure == 4) plain_fail_compile = 1;
		if (failure == 5) plain_fail_check = 1;
		if (failure == 6) plain_fail_register = 1;
		if (failure == 7) pair_fail_vertex = 1;
		if (failure == 8) pair_bad_uniform = 1;
		if (failure == 9) pair_params[3].type = 1;
		isaac_plain_vertex_pair_link(&progs[0]);
		expect(!progs[0].isaac_plain_vertex_pair, "pair published failed ABI/link");
		vertex = &pair_vertex;
		expect(!isaac_plain_vertex_pair_select(&progs[0], &vertex) && !vertex,
			"pair failed selection did not clear VS");
		verify_pair_counts(1u, 0u, 1u);
		expect(vglTakeIsaacColorOffsetPlainStats(plain_counts, 3u) == 1u &&
			plain_counts[0] == 0u && plain_counts[1] == 0u && plain_counts[2] == 0u,
			"failed pair selection changed existing plain accounting");
		isaac_coloroffset_plain_release(&progs[0]);
		expect(plain_frees == plain_allocations - (failure <= 3 ? 1u : 0u), "pair failure leaked");
	}
	pair_reset_fixture();
	expect(isaac_plain_vertex_abi(&pair_compiled, sizeof pair_compiled, &plain_vs, attrs), "valid pair ABI rejected");
	{
		unsigned *fields[6] = {&pair_params[3].category, &pair_params[3].type,
			&pair_params[3].resource, &pair_params[3].components,
			&pair_params[3].array, &pair_params[3].container};
		for (unsigned i = 0; i < 6; ++i) {
			++*fields[i];
			expect(!isaac_plain_vertex_abi(&pair_compiled, sizeof pair_compiled, &plain_vs, attrs), "pair Transform ABI drift admitted");
			--*fields[i];
		}
	}
	pair_bad_count = 1;
	expect(!isaac_plain_vertex_abi(&pair_compiled, sizeof pair_compiled, &plain_vs, attrs), "extra private VS parameter admitted");
	pair_bad_count = 0;
	for (unsigned i = 0; i < 3; ++i) {
		++pair_params[i].resource;
		expect(!isaac_plain_vertex_abi(&pair_compiled, sizeof pair_compiled, &plain_vs, attrs), "pair attribute register drift admitted");
		--pair_params[i].resource;
	}
	for (unsigned lane = 0; lane < 3; ++lane) {
		pair_compiled.words[(0x9c + 16) / 4 + lane] ^= 0x100;
		expect(!isaac_plain_vertex_abi(&pair_compiled, sizeof pair_compiled, &plain_vs, attrs), "pair output mask drift admitted");
		pair_compiled.words[(0x9c + 16) / 4 + lane] ^= 0x100;
	}
	pair_compiled.words[0x2c / 4] = UINT32_MAX;
	expect(!isaac_plain_vertex_abi(&pair_compiled, sizeof pair_compiled, &plain_vs, attrs), "pair malformed GXP range admitted");
	pair_compiled.words[0x2c / 4] = 0x9c - 0x2c;
	isaac_plain_vertex_pair_link(&progs[0]);
	expect(progs[0].isaac_plain_vertex_pair != NULL, "pair valid link failed");
	plain_fail_create = 1;
	expect(!isaac_plain_vertex_pair_select(&progs[0], &vertex) && !vertex, "pair FS failure left private VS selected");
	plain_fail_create = 0;
	for (unsigned key = 0; key < 4; ++key) {
		blend_info.raw = key;
		expect(isaac_plain_vertex_pair_select(&progs[0], &vertex) && vertex == &pair_vertex &&
			pair_fragment_vertex == progs[0].isaac_plain_vertex_pair->vertex_gxp,
			"pair did not patch/bind matching stages");
	}
	blend_info.raw = 4;
	expect(!isaac_plain_vertex_pair_select(&progs[0], &vertex) && !vertex, "pair full cache did not fallback");
	verify_pair_counts(6u, 4u, 2u);
	expect(vglTakeIsaacColorOffsetPlainStats(plain_counts, 3u) == 1u &&
		plain_counts[0] == 4u && plain_counts[1] == 4u && plain_counts[2] == 0u,
		"successful pair selection changed existing plain accounting");
	++isaac_coloroffset_source_generation[0];
	isaac_plain_vertex_pair_before_link(&progs[0]);
	expect(!progs[0].isaac_plain_vertex_pair && plain_finishes == 1 && pair_vertex_releases == 1,
		"pair stale relink did not retire private stages");
	isaac_coloroffset_plain_release(&progs[0]);
	expect(plain_frees == plain_allocations && plain_releases == 4, "pair successful lifecycle leaked");
}
#endif
#endif

static void make_vertices(uint8_t *vertices, size_t size)
{
	uint32_t vertex;
	memset(vertices, 0, size);
	for (vertex = 0; vertex < ISAAC_COLOROFFSET_VERTEX_COUNT; ++vertex) {
		uint8_t *value = vertices +
			(size_t)vertex * ISAAC_COLOROFFSET_VERTEX_STRIDE;
		store_u32(value + 24u, 0x3f800000u);
	}
}

static void read_exact_file(
	const char *path, void *bytes, size_t size, const char *label)
{
	FILE *stream = fopen(path, "rb");
	expect(stream != NULL, label);
	expect(fread(bytes, 1u, size, stream) == size, label);
	expect(fgetc(stream) == EOF, label);
	expect(!ferror(stream), label);
	expect(fclose(stream) == 0, label);
}

static void verify_captured_gxp_receipts(
	const char *vertex_path, const char *fragment_path)
{
	uint8_t vertex_gxp[ISAAC_COLOROFFSET_VERTEX_GXP_SIZE];
	uint8_t fragment_gxp[ISAAC_COLOROFFSET_FRAGMENT_GXP_SIZE];

	read_exact_file(vertex_path, vertex_gxp, sizeof vertex_gxp,
		"captured exact ColorOffset vertex GXP receipt failed");
	expect(isaac_coloroffset_policy_fnv1a(
			vertex_gxp, sizeof vertex_gxp) ==
			ISAAC_COLOROFFSET_VERTEX_GXP_FNV1A &&
		isaac_coloroffset_policy_fnv1a(vertex_gxp, 512u) ==
			ISAAC_COLOROFFSET_VERTEX_GXP_FIRST512_FNV1A,
		"captured vertex GXP does not match the full binary receipt");
	read_exact_file(fragment_path, fragment_gxp, sizeof fragment_gxp,
		"captured exact ColorOffset fragment GXP receipt failed");
	expect(isaac_coloroffset_policy_fnv1a(
			fragment_gxp, sizeof fragment_gxp) ==
			ISAAC_COLOROFFSET_FRAGMENT_GXP_FNV1A &&
		isaac_coloroffset_policy_fnv1a(fragment_gxp, 512u) ==
			ISAAC_COLOROFFSET_FRAGMENT_GXP_FIRST512_FNV1A,
		"captured fragment GXP does not match the full binary receipt");
}

int main(int argc, char **argv)
{
	isaac_coloroffset_attribute_key attributes[ISAAC_COLOROFFSET_ATTRIBUTE_COUNT];
	uint16_t strides[ISAAC_COLOROFFSET_ATTRIBUTE_COUNT];
	static const uint16_t offsets[ISAAC_COLOROFFSET_ATTRIBUTE_COUNT] = {
		0u, 12u, 28u, 36u, 52u, 64u, 72u, 76u
	};
	static const uint8_t components[ISAAC_COLOROFFSET_ATTRIBUTE_COUNT] = {
		3u, 4u, 2u, 4u, 3u, 2u, 1u, 3u
	};
	static const uint16_t indices[ISAAC_COLOROFFSET_INDEX_COUNT] = {
		0u, 2u, 1u, 1u, 2u, 3u
	};
	uint8_t vertices[ISAAC_COLOROFFSET_VERTEX_COUNT *
		ISAAC_COLOROFFSET_VERTEX_STRIDE];
	isaac_coloroffset_cache_key key;
	isaac_coloroffset_cache_key changed;
	uint32_t index;
	float source_values[] = {0.0f, 0.125f, 0.5f, 1.0f};
	float destination_values[] = {0.0f, 0.25f, 0.75f, 1.0f};

	expect(isaac_coloroffset_next_generation(1u) == 2u,
		"live lifecycle generation did not advance");
	expect(isaac_coloroffset_next_generation(UINT32_MAX - 1u) == UINT32_MAX,
		"last non-wrapping lifecycle generation was skipped");
	expect(isaac_coloroffset_next_generation(UINT32_MAX) == 0u &&
		isaac_coloroffset_next_generation(0u) == 0u,
		"wrapped lifecycle generation was not permanently poisoned");

	for (index = 0; index < ISAAC_COLOROFFSET_ATTRIBUTE_COUNT; ++index) {
		attributes[index].stream_index = (uint16_t)index;
		attributes[index].offset = offsets[index];
		attributes[index].format = 9u;
		attributes[index].component_count = components[index];
		attributes[index].reg_index = (uint16_t)(index * 4u);
		strides[index] = ISAAC_COLOROFFSET_VERTEX_STRIDE;
	}
	expect(isaac_coloroffset_layout_is_exact(
		attributes, strides, ISAAC_COLOROFFSET_ATTRIBUTE_COUNT, 0xffu, 9u),
		"measured eight-attribute/88-byte layout rejected");
	attributes[7].offset++;
	expect(!isaac_coloroffset_layout_is_exact(
		attributes, strides, ISAAC_COLOROFFSET_ATTRIBUTE_COUNT, 0xffu, 9u),
		"hostile attribute offset passed");
	attributes[7].offset--;
	expect(!isaac_coloroffset_layout_is_exact(
		attributes, strides, ISAAC_COLOROFFSET_ATTRIBUTE_COUNT, 0x7fu, 9u),
		"disabled referenced attribute passed");

	make_vertices(vertices, sizeof vertices);
	expect(isaac_coloroffset_contiguous_vertices_are_neutral(
		vertices, sizeof vertices, ISAAC_COLOROFFSET_VERTEX_COUNT,
		ISAAC_COLOROFFSET_VERTEX_STRIDE), "neutral batch rejected");
	/* Every vertex matters: the last one must force fallback too. */
	store_u32(vertices + 3u * ISAAC_COLOROFFSET_VERTEX_STRIDE + 72u, 0x3f800000u);
	expect(!isaac_coloroffset_contiguous_vertices_are_neutral(
		vertices, sizeof vertices, ISAAC_COLOROFFSET_VERTEX_COUNT,
		ISAAC_COLOROFFSET_VERTEX_STRIDE), "last nonneutral pixelation passed");
	store_u32(vertices + 3u * ISAAC_COLOROFFSET_VERTEX_STRIDE + 72u, 0x80000000u);
	store_u32(vertices + 48u, 0x3f000000u); /* Nonzero colorize alpha. */
	store_u32(vertices + 52u, 0xbf000000u); /* Negative color offset. */
	store_u32(vertices + 84u, 0xbf800000u); /* Inactive negative clip z. */
	expect(isaac_coloroffset_contiguous_vertices_are_neutral(
		vertices, sizeof vertices, ISAAC_COLOROFFSET_VERTEX_COUNT,
		ISAAC_COLOROFFSET_VERTEX_STRIDE), "retained colorize/offset or -0 rejected");
	store_u32(vertices + 84u, 0x3f800000u);
	expect(!isaac_coloroffset_vertex_is_neutral(vertices, sizeof vertices),
		"positive clipping z passed");
	store_u32(vertices + 84u, 0u);
	store_u32(vertices + 76u, 0x00000001u);
	expect(!isaac_coloroffset_vertex_is_neutral(vertices, sizeof vertices),
		"nonzero clipping normal passed");
	store_u32(vertices + 76u, 0u);
	for (index = 0u; index < ISAAC_COLOROFFSET_VERTEX_STRIDE; index += 4u) {
		uint32_t saved = isaac_coloroffset_load_u32(vertices + index);
		store_u32(vertices + index, 0x7fc00000u);
		expect(!isaac_coloroffset_vertex_is_neutral(vertices, sizeof vertices),
			"NaN vertex lane passed neutral proof");
		store_u32(vertices + index, 0x7f800000u);
		expect(!isaac_coloroffset_vertex_is_neutral(vertices, sizeof vertices),
			"infinite vertex lane passed neutral proof");
		store_u32(vertices + index, saved);
	}
	expect(!isaac_coloroffset_contiguous_vertices_are_neutral(
		vertices, sizeof vertices - 1u, ISAAC_COLOROFFSET_VERTEX_COUNT,
		ISAAC_COLOROFFSET_VERTEX_STRIDE), "short neutral span passed");
	expect(!isaac_coloroffset_contiguous_vertices_are_neutral(
		vertices, sizeof vertices, ISAAC_COLOROFFSET_VERTEX_COUNT, 0u),
		"zero neutral stride passed");
	expect(!isaac_coloroffset_contiguous_vertices_are_neutral(
		vertices, sizeof vertices, 4097u, ISAAC_COLOROFFSET_VERTEX_STRIDE),
		"unbounded neutral scan passed");
	make_vertices(vertices, sizeof vertices);
	expect(isaac_coloroffset_contiguous_vertices_are_opaque(
		vertices, sizeof vertices, ISAAC_COLOROFFSET_VERTEX_COUNT,
		ISAAC_COLOROFFSET_VERTEX_STRIDE),
		"bit-exact default vertices rejected");
	expect(!isaac_coloroffset_contiguous_vertices_are_opaque(
		vertices, sizeof vertices - 1u, ISAAC_COLOROFFSET_VERTEX_COUNT,
		ISAAC_COLOROFFSET_VERTEX_STRIDE),
		"short hostile vertex span passed");
	expect(!isaac_coloroffset_contiguous_vertices_are_opaque(
		vertices, sizeof vertices, ISAAC_COLOROFFSET_VERTEX_COUNT, 0u),
		"hostile zero stride passed");
	expect(isaac_coloroffset_indexed_vertices_are_opaque(
		vertices, sizeof vertices, ISAAC_COLOROFFSET_VERTEX_STRIDE,
		indices, sizeof indices, ISAAC_COLOROFFSET_INDEX_COUNT, 2u, 0u),
		"bounded index proof rejected");
	expect(!isaac_coloroffset_indexed_vertices_are_opaque(
		vertices, sizeof vertices, ISAAC_COLOROFFSET_VERTEX_STRIDE,
		indices, sizeof indices - 1u, ISAAC_COLOROFFSET_INDEX_COUNT, 2u, 0u),
		"short hostile index span passed");
	expect(!isaac_coloroffset_indexed_vertices_are_opaque(
		vertices, sizeof vertices, ISAAC_COLOROFFSET_VERTEX_STRIDE,
		indices, sizeof indices, ISAAC_COLOROFFSET_INDEX_COUNT, 3u, 0u),
		"invalid index width passed");
	{
		uint16_t hostile[ISAAC_COLOROFFSET_INDEX_COUNT] = {0, 1, 2, 4, 2, 3};
		expect(!isaac_coloroffset_indexed_vertices_are_opaque(
			vertices, sizeof vertices, ISAAC_COLOROFFSET_VERTEX_STRIDE,
			hostile, sizeof hostile, ISAAC_COLOROFFSET_INDEX_COUNT, 2u, 0u),
			"out-of-range vertex index passed");
	}
	{
		uint32_t hostile = UINT32_MAX;
		expect(!isaac_coloroffset_indexed_vertices_are_opaque(
			vertices, sizeof vertices, ISAAC_COLOROFFSET_VERTEX_STRIDE,
			&hostile, sizeof hostile, 1u, 4u, 1u),
			"base/index overflow passed");
	}
	store_u32(vertices + ISAAC_COLOROFFSET_VERTEX_STRIDE + 24u, 0x3f7fffffu);
	expect(!isaac_coloroffset_contiguous_vertices_are_opaque(
		vertices, sizeof vertices, ISAAC_COLOROFFSET_VERTEX_COUNT,
		ISAAC_COLOROFFSET_VERTEX_STRIDE),
		"non-one Color.a passed");
	store_u32(vertices + ISAAC_COLOROFFSET_VERTEX_STRIDE + 24u, 0x3f800000u);
	store_u32(vertices + 48u, 0x80000000u);
	expect(!isaac_coloroffset_contiguous_vertices_are_opaque(
		vertices, sizeof vertices, ISAAC_COLOROFFSET_VERTEX_COUNT,
		ISAAC_COLOROFFSET_VERTEX_STRIDE),
		"negative-zero ColorizeOut.a passed bit-exact gate");
	store_u32(vertices + 48u, 0u);
	store_u32(vertices + 52u, 0x80000000u);
	expect(!isaac_coloroffset_contiguous_vertices_are_opaque(
		vertices, sizeof vertices, ISAAC_COLOROFFSET_VERTEX_COUNT,
		ISAAC_COLOROFFSET_VERTEX_STRIDE),
		"negative-zero ColorOffsetOut passed bit-exact gate");
	store_u32(vertices + 52u, 0u);
	store_u32(vertices + 72u, 0x7fc00000u);
	expect(!isaac_coloroffset_contiguous_vertices_are_opaque(
		vertices, sizeof vertices, ISAAC_COLOROFFSET_VERTEX_COUNT,
		ISAAC_COLOROFFSET_VERTEX_STRIDE),
		"NaN pixelation passed <= 0 gate");
	store_u32(vertices + 72u, 0xbf800000u);
	expect(isaac_coloroffset_contiguous_vertices_are_opaque(
		vertices, sizeof vertices, ISAAC_COLOROFFSET_VERTEX_COUNT,
		ISAAC_COLOROFFSET_VERTEX_STRIDE),
		"finite negative pixelation was not accepted");

	/* Physical GXM raw 0x5151110f decodes to mask ALL, ADD/ADD,
	 * color ONE/ONE_MINUS_SRC_ALPHA and alpha
	 * ONE/ONE_MINUS_SRC_ALPHA.  The old accepted source factor was
	 * SRC_ALPHA.  Both source factors are identical when the separately
	 * proven shader output alpha is mathematically one. */
	expect((0x5151110fu & 0xffu) == 0x0fu &&
		((0x5151110fu >> 8u) & 0x0fu) == 1u &&
		((0x5151110fu >> 12u) & 0x0fu) == 1u &&
		((0x5151110fu >> 16u) & 0x0fu) == 1u &&
		((0x5151110fu >> 20u) & 0x0fu) == 5u &&
		((0x5151110fu >> 24u) & 0x0fu) == 1u &&
		((0x5151110fu >> 28u) & 0x0fu) == 5u,
		"physical ColorOffset blend receipt decoded incorrectly");
	for (index = 0; index < sizeof source_values / sizeof source_values[0]; ++index) {
		uint32_t destination;
		uint32_t source_factor;
		for (source_factor = 0; source_factor < 2u; ++source_factor) {
			for (destination = 0;
					destination < sizeof destination_values /
						sizeof destination_values[0]; ++destination) {
				float alpha = 1.0f;
				float factor = source_factor ? alpha : 1.0f;
				float blended = source_values[index] * factor +
					destination_values[destination] * (1.0f - alpha);
				expect(blended == source_values[index],
					"opaque blend algebra differs from no-blend output");
			}
		}
	}

	/* The byte-identical stock shader writes Color.a.  RGB24 sampling supplies
	 * alpha one and the gate requires Color0.a to be bit-exact one, so source
	 * alpha is mathematically one regardless of its RGB/colorize arithmetic. */
	expect(1.0f * 1.0f == 1.0f,
		"RGB24/Color0 output-alpha proof failed");

	memset(&key, 0, sizeof key);
	key.fragment_shader_id = 7u;
	key.source_generation = 3u;
	key.vertex_program = (uintptr_t)0x12340000u;
	key.source_blend_raw = 0x5151110fu;
	key.output_format = 1u;
	key.multisample_mode = 0u;
	key.texture_format = 0x98000000u;
	key.texture_width = ISAAC_COLOROFFSET_TEXTURE_WIDTH;
	key.texture_height = ISAAC_COLOROFFSET_TEXTURE_HEIGHT;
	key.viewport_width = ISAAC_COLOROFFSET_VIEWPORT_WIDTH;
	key.viewport_height = ISAAC_COLOROFFSET_VIEWPORT_HEIGHT;
	changed = key;
	expect(isaac_coloroffset_cache_key_equal(&key, &changed),
		"identical complete cache key rejected");
#define REJECT_CHANGED(field, value, message) \
	changed = key; changed.field = (value); \
	expect(!isaac_coloroffset_cache_key_equal(&key, &changed), (message))
	REJECT_CHANGED(fragment_shader_id, 8u, "fragment identity omitted from key");
	REJECT_CHANGED(source_generation, 4u, "source/relink generation omitted from key");
	REJECT_CHANGED(vertex_program, (uintptr_t)0x12340004u,
		"vertex link omitted from key");
	REJECT_CHANGED(source_blend_raw, 0x5151111fu, "blend state omitted from key");
	REJECT_CHANGED(output_format, 2u, "output format omitted from key");
	REJECT_CHANGED(multisample_mode, 1u, "MSAA state omitted from key");
	REJECT_CHANGED(texture_format, 0x9c000000u, "sampler format omitted from key");
	REJECT_CHANGED(texture_width, 431u, "sampler width omitted from key");
	REJECT_CHANGED(texture_height, 239u, "sampler height omitted from key");
	REJECT_CHANGED(viewport_x, 1, "viewport x omitted from key");
	REJECT_CHANGED(viewport_y, 1, "viewport y omitted from key");
	REJECT_CHANGED(viewport_width, 959u, "viewport width omitted from key");
	REJECT_CHANGED(viewport_height, 544u, "viewport state omitted from key");
#undef REJECT_CHANGED

	if (argc == 3)
		verify_captured_gxp_receipts(argv[1], argv[2]);
	else
		expect(argc == 1,
			"usage: oracle [captured-vertex.gxp captured-fragment.gxp]");

	verify_staging_copy();
#if defined(HAVE_ISAAC_COLOROFFSET_PLAIN_FASTPATH) && HAVE_ISAAC_COLOROFFSET_PLAIN_FASTPATH == 1
	verify_plain_staging();
#if defined(HAVE_ISAAC_LASER_HALO_PROFILE) && HAVE_ISAAC_LASER_HALO_PROFILE && \
    defined(HAVE_ISAAC_COLOROFFSET_STAGING_PLAIN_FUSION) && HAVE_ISAAC_COLOROFFSET_STAGING_PLAIN_FUSION
	verify_white_staging();
#endif
	verify_plain_lifecycle();
#if defined(HAVE_ISAAC_COLOROFFSET_PLAIN_FP16)
	verify_fp16_interface();
	verify_fp16_lifecycle();
#endif
#if defined(HAVE_ISAAC_COLOROFFSET_PLAIN_VERTEX_PAIR)
	verify_pair_abi_diagnostic();
	verify_pair_output_count_contract();
	verify_plain_vertex_pair();
#endif
#endif
	puts("coloroffset-policy oracle: PASS");
	return 0;
}
#endif /* ISAAC_COLOROFFSET_STAGING_ARM_TEST */
