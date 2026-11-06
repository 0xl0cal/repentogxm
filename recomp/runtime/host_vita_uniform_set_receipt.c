/* ISAAC_VITA_UNIFORM_SET_RECEIPT: census of the KAGE Shader uniform path.
 *
 * Why it exists.  the-browsing-of-isaac (a recompilation of Repentance+
 * 1.9.7.17) found its renderer calling SetShaderUniform("ChampionColor",
 * type, value) once per animation layer, with a linear strcmp scan of a
 * stride-0x18 uniform table and a malloc/memcpy/free round trip on every
 * value change (guest_malloc 1.8 % + guest_free 0.9 % of a loaded frame).
 * Our frozen PE (Repentance 1.7.9b) does not have that path:
 *
 *   - The champion tint is a VERTEX ATTRIBUTE.  The shader descriptor table
 *     the static initializer sub_0025a2c0 hands to the Shader at 0xbc5044
 *     ("shaders/coloroffset_champion", table 0x7aa558) lists (name, format)
 *     pairs Position/5, Color/6, TexCoord/7, ColorizeIn/4, ColorOffsetIn/3,
 *     TextureSize/2, PixelationAmount/1, ClipPlane/3, ChampionColorIn/4;
 *     Shader::Init (sub_00566f30) copies it to [this+8]/[this+0xc] and
 *     Shader::EnableAttribs (sub_005672d0) binds every entry with
 *     glGetAttribLocation/glVertexAttribPointer.  The per-layer draw
 *     sub_00003e50 only swaps the current-shader pointer (0x420c: `mov eax,
 *     [0xa08938]; mov [0xbc7a44], eax`, index 10 of the Shader* table at
 *     RVA 0x608910) and restores index 0 afterwards (0x4478); the colour
 *     rides in the quad Image::PushQuad writes.
 *   - The only uniform setters are Shader::SetUniform (sub_00567490,
 *     vtable slot 8: glGetUniformLocation + one glUniformNfv or
 *     glUniformMatrixNfv chosen by a 21-entry type switch, no scan, no
 *     allocation) and
 *     Shader::SetUniformInt (sub_00567430, slot 7: sampler unit).  Both are
 *     reached only through the vtable from Shader::Bind (sub_005671d0, slot
 *     4) and Shader::ApplyUniforms (sub_00567b50), which loop over an index
 *     list into the Shader's uniform-record vector [this+0x10..0x14) (stride
 *     0x18: name, count, type, values vector) and then set "Texture0",
 *     optionally "Texture1", and the "Transform" matrix (sub_00565240).
 *   - Nothing in .text appends to that record vector: the only functions
 *     walking it are the destructor (sub_005676d0), Clear (sub_005678c0),
 *     the per-frame collapse (sub_005679e0) and the vector dtor
 *     (sub_00567bf0); recomp/test_uniform_set_sites.py pins that census.
 *     So the uniform loop in Bind is expected to run zero iterations and the
 *     per-Bind cost is the fixed 2-3 location queries (already served by the
 *     shim location cache) plus glUniform1i/glUniformMatrix4fv (already
 *     filtered by the redundancy cache).
 *
 * What this file does.  GNU ld --wrap on the five roots counts entries and
 * reads the record count, the index-list length and the name literal at
 * entry (and, at Shader::Collapse, the values the body is about to free),
 * then runs the untouched translated body.  The window counters are
 * taken by kage_vita_phase_profile.c (ph120.us, ISAAC_VITA_PHASE_PROFILE);
 * without the profiler a cumulative "KAGE VITA UNIFORM SET:" line goes to the
 * file log at binds 1,2,4,...,4096 and then every 4096 binds.  Nothing here
 * changes guest state.  Compiled only with the knob ON: the OFF build has no
 * object, no link option and byte-identical units. */
#include <stdint.h>
#include <string.h>

#include "guest.h"
#include "host_vita_uniform_set_receipt.h"

#if !defined(ISAAC_VITA_UNIFORM_SET_RECEIPT) || !ISAAC_VITA_UNIFORM_SET_RECEIPT
#error "host_vita_uniform_set_receipt.c is compiled only with ISAAC_VITA_UNIFORM_SET_RECEIPT=ON"
#endif

/* recomp/vita/platform.h; declared here so a host oracle can stub it. */
void isaac_vita_log(const char *format, ...);

void __real_sub_005671d0(CPU *__restrict c);   /* Shader::Bind(list) */
void __real_sub_00567b50(CPU *__restrict c);   /* Shader::ApplyUniforms(list) */
void __real_sub_00567490(CPU *__restrict c);   /* Shader::SetUniform(name, type, value, count) */
void __real_sub_00567430(CPU *__restrict c);   /* Shader::SetUniformInt(name, unit, texture) */
void __real_sub_005679e0(CPU *__restrict c);   /* Shader::Collapse() (vtable slot 3) */

/* Frozen PE facts, pinned by recomp/test_uniform_set_sites.py. */
#define VITA_SHADER_RECORDS_BEGIN_OFFSET   0x10U  /* std::vector<UniformRecord>::first */
#define VITA_SHADER_RECORDS_END_OFFSET     0x14U  /* std::vector<UniformRecord>::last */
#define VITA_SHADER_RECORD_STRIDE          0x18U
#define VITA_SHADER_DEFAULT_LIST_OFFSET    0x1cU  /* std::vector<int> used when the argument is NULL */
#define VITA_SHADER_RECORD_TYPE_OFFSET     0x08U  /* UniformRecord::type; >= 0x15 (signed) is a sampler */
#define VITA_SHADER_RECORD_VALUES_BEGIN    0x0cU  /* UniformRecord::values std::vector<void *>::first */
#define VITA_SHADER_RECORD_VALUES_END      0x10U  /* UniformRecord::values std::vector<void *>::last */
#define VITA_SHADER_SAMPLER_TYPE_BASE      0x15U  /* Collapse: `cmp [record+8], 0x15; jge` skips the frees */
#define VITA_UNIFORM_RECEIPT_MAX_RECORDS   64U    /* bound on the records read per Collapse entry */
#define VITA_UNIFORM_NAME_TRANSFORM_RVA    0x744ac0U
#define VITA_UNIFORM_NAME_TEXTURE0_RVA     0x75e098U
#define VITA_UNIFORM_NAME_TEXTURE1_RVA     0x75e03cU

static IsaacVitaUniformSetCounters s_window;
static IsaacVitaUniformSetCounters s_total;

static inline void vita_uniform_bump(uint32_t *window_field, uint32_t *total_field)
{
    if (*window_field != UINT32_MAX)
        ++*window_field;
    if (*total_field != UINT32_MAX)
        ++*total_field;
}

#define VITA_UNIFORM_BUMP(field) vita_uniform_bump(&s_window.field, &s_total.field)

static inline void vita_uniform_max(uint32_t *window_field, uint32_t *total_field,
                                    uint32_t value)
{
    if (value > *window_field)
        *window_field = value;
    if (value > *total_field)
        *total_field = value;
}

static inline void vita_uniform_add(uint32_t *window_field, uint32_t *total_field,
                                    uint32_t value)
{
    *window_field = (UINT32_MAX - *window_field < value) ? UINT32_MAX : *window_field + value;
    *total_field = (UINT32_MAX - *total_field < value) ? UINT32_MAX : *total_field + value;
}

void isaac_vita_uniform_set_receipt_take(IsaacVitaUniformSetCounters *out)
{
    if (out)
        *out = s_window;
    memset(&s_window, 0, sizeof s_window);
}

static void vita_uniform_periodic_log(void)
{
    uint32_t binds = s_total.binds;

    if (binds == 0U || (binds > 4096U && (binds & 4095U) != 0U) ||
            (binds <= 4096U && (binds & (binds - 1U)) != 0U))
        return;
    isaac_vita_log(
        "KAGE VITA UNIFORM SET: binds=%u applies=%u records_max=%u "
        "records_nonempty=%u it=%u it_max=%u set=%u set_int=%u "
        "names(t,x0,x1,o)=%u,%u,%u,%u collapses=%u values=%u frees=%u bad=%u",
        (unsigned)s_total.binds, (unsigned)s_total.applies,
        (unsigned)s_total.records_max, (unsigned)s_total.records_nonempty,
        (unsigned)s_total.iterations, (unsigned)s_total.iterations_max,
        (unsigned)s_total.sets, (unsigned)s_total.set_ints,
        (unsigned)s_total.name_transform, (unsigned)s_total.name_texture0,
        (unsigned)s_total.name_texture1, (unsigned)s_total.name_other,
        (unsigned)s_total.collapses, (unsigned)s_total.collapse_values,
        (unsigned)s_total.frees, (unsigned)s_total.bad);
}

/* Bind/ApplyUniforms entry: ECX = Shader this, [esp] = return RVA,
 * [esp+4] = std::vector<int> *list (NULL selects [this+0x1c]). */
static void vita_uniform_note_bind(CPU *__restrict c)
{
    uint32_t self = c->ecx;
    uint32_t list_address = guest_stack_address(c, c->esp + 4U, 4U, 0U);
    uint32_t list, records_begin, records_end, list_begin, list_end;
    uint32_t records, iterations;

    if (c->fault || !list_address || !self || (self & 3U) != 0U) {
        VITA_UNIFORM_BUMP(bad);
        return;
    }
    list = ld32(list_address);
    if (!list)
        list = self + VITA_SHADER_DEFAULT_LIST_OFFSET;
    if ((list & 3U) != 0U) {
        VITA_UNIFORM_BUMP(bad);
        return;
    }
    records_begin = ld32(self + VITA_SHADER_RECORDS_BEGIN_OFFSET);
    records_end = ld32(self + VITA_SHADER_RECORDS_END_OFFSET);
    list_begin = ld32(list);
    list_end = ld32(list + 4U);
    records = records_end >= records_begin
        ? (records_end - records_begin) / VITA_SHADER_RECORD_STRIDE : 0U;
    iterations = list_end >= list_begin ? (list_end - list_begin) / 4U : 0U;

    vita_uniform_max(&s_window.records_max, &s_total.records_max, records);
    if (records)
        VITA_UNIFORM_BUMP(records_nonempty);
    vita_uniform_add(&s_window.iterations, &s_total.iterations, iterations);
    vita_uniform_max(&s_window.iterations_max, &s_total.iterations_max, iterations);
}

/* SetUniform/SetUniformInt entry: [esp+4] = const char *name. */
static void vita_uniform_note_name(CPU *__restrict c)
{
    uint32_t name_address = guest_stack_address(c, c->esp + 4U, 4U, 0U);
    uint32_t name;

    if (c->fault || !name_address) {
        VITA_UNIFORM_BUMP(bad);
        return;
    }
    name = ld32(name_address);
    if (name == (uint32_t)GUEST_IMAGE_BASE + VITA_UNIFORM_NAME_TRANSFORM_RVA)
        VITA_UNIFORM_BUMP(name_transform);
    else if (name == (uint32_t)GUEST_IMAGE_BASE + VITA_UNIFORM_NAME_TEXTURE0_RVA)
        VITA_UNIFORM_BUMP(name_texture0);
    else if (name == (uint32_t)GUEST_IMAGE_BASE + VITA_UNIFORM_NAME_TEXTURE1_RVA)
        VITA_UNIFORM_BUMP(name_texture1);
    else
        VITA_UNIFORM_BUMP(name_other);
}

/* Collapse entry: ECX = Shader this, no stack arguments (`ret`).  The body
 * keeps the last value of every record and frees the others of every
 * non-sampler record (CRT free at 0x567a4f behind `cmp [record+8], 0x15;
 * jge`), so what it is about to free is sum(values - 1) over records with
 * a signed type < 0x15 and at least one value: the "free" half of the
 * reference project's malloc/free pair, counted directly rather than
 * inferred from r=0.  The body reads the same words right after us. */
static void vita_uniform_note_collapse(CPU *__restrict c)
{
    uint32_t self = c->ecx;
    uint32_t records_begin, records_end, records, i;
    uint32_t values_total = 0U, frees = 0U;

    if (c->fault || !self || (self & 3U) != 0U) {
        VITA_UNIFORM_BUMP(bad);
        return;
    }
    records_begin = ld32(self + VITA_SHADER_RECORDS_BEGIN_OFFSET);
    records_end = ld32(self + VITA_SHADER_RECORDS_END_OFFSET);
    if ((records_begin & 3U) != 0U || (records_end & 3U) != 0U) {
        VITA_UNIFORM_BUMP(bad);
        return;
    }
    records = records_end >= records_begin
        ? (records_end - records_begin) / VITA_SHADER_RECORD_STRIDE : 0U;
    vita_uniform_max(&s_window.records_max, &s_total.records_max, records);
    if (records > VITA_UNIFORM_RECEIPT_MAX_RECORDS) {
        VITA_UNIFORM_BUMP(bad);
        records = VITA_UNIFORM_RECEIPT_MAX_RECORDS;
    }
    for (i = 0U; i < records; ++i) {
        uint32_t record = records_begin + i * VITA_SHADER_RECORD_STRIDE;
        uint32_t type = ld32(record + VITA_SHADER_RECORD_TYPE_OFFSET);
        uint32_t values_begin = ld32(record + VITA_SHADER_RECORD_VALUES_BEGIN);
        uint32_t values_end = ld32(record + VITA_SHADER_RECORD_VALUES_END);
        uint32_t values = values_end >= values_begin
            ? (values_end - values_begin) / 4U : 0U;

        values_total += values;
        if ((int32_t)type < (int32_t)VITA_SHADER_SAMPLER_TYPE_BASE && values > 1U)
            frees += values - 1U;
    }
    vita_uniform_add(&s_window.collapse_values, &s_total.collapse_values, values_total);
    vita_uniform_add(&s_window.frees, &s_total.frees, frees);
}

void __wrap_sub_005671d0(CPU *__restrict c)
{
    VITA_UNIFORM_BUMP(binds);
    vita_uniform_note_bind(c);
    vita_uniform_periodic_log();
    __real_sub_005671d0(c);
}

void __wrap_sub_00567b50(CPU *__restrict c)
{
    VITA_UNIFORM_BUMP(applies);
    vita_uniform_note_bind(c);
    __real_sub_00567b50(c);
}

void __wrap_sub_00567490(CPU *__restrict c)
{
    VITA_UNIFORM_BUMP(sets);
    vita_uniform_note_name(c);
    __real_sub_00567490(c);
}

void __wrap_sub_00567430(CPU *__restrict c)
{
    VITA_UNIFORM_BUMP(set_ints);
    vita_uniform_note_name(c);
    __real_sub_00567430(c);
}

void __wrap_sub_005679e0(CPU *__restrict c)
{
    VITA_UNIFORM_BUMP(collapses);
    vita_uniform_note_collapse(c);
    __real_sub_005679e0(c);
}
