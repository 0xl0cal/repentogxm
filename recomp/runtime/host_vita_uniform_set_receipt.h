#ifndef ISAAC_HOST_VITA_UNIFORM_SET_RECEIPT_H
#define ISAAC_HOST_VITA_UNIFORM_SET_RECEIPT_H

#include <stdint.h>

/* ISAAC_VITA_UNIFORM_SET_RECEIPT (2026-09-10): per-window
 * census of the KAGE Shader uniform path (host_vita_uniform_set_receipt.c).
 *
 * b = Shader::Bind (sub_005671d0) and Shader::ApplyUniforms (sub_00567b50)
 * entries; records = the Shader's uniform-record vector [this+0x10..0x14)
 * (stride 0x18) as seen at entry; iterations = the index-list length the
 * body loops over ([list+4]-[list])/4, i.e. the number of per-record
 * SetUniform/SetUniformInt calls the body will make.  s = Shader::SetUniform
 * (sub_00567490, name/type/value) and Shader::SetUniformInt (sub_00567430,
 * sampler) entries, with the name literal classified by its address
 * ("Transform" 0x744ac0, "Texture0" 0x75e098, "Texture1" 0x75e03c; every
 * other pointer is `other`).  c = Shader::Collapse (sub_005679e0, vtable
 * slot 3) entries, the values held by all of the Shader's records at entry
 * and the non-sampler values the body is about to free (values - 1 per
 * record: the "free" half of a malloc/free churn).  bad = wrapper entries
 * whose CPU state was not readable (faulted stack window, misaligned
 * this/list/records, more than 64 records); those are counted and otherwise
 * ignored, the translated body still runs. */
typedef struct IsaacVitaUniformSetCounters {
    uint32_t binds;             /* Shader::Bind entries */
    uint32_t applies;           /* Shader::ApplyUniforms entries */
    uint32_t records_max;       /* max uniform-record count seen */
    uint32_t records_nonempty;  /* binds+applies with records > 0 */
    uint32_t iterations;        /* sum of index-list lengths */
    uint32_t iterations_max;    /* max index-list length */
    uint32_t sets;              /* Shader::SetUniform entries */
    uint32_t set_ints;          /* Shader::SetUniformInt entries */
    uint32_t name_transform;    /* name == "Transform" literal */
    uint32_t name_texture0;     /* name == "Texture0" literal */
    uint32_t name_texture1;     /* name == "Texture1" literal */
    uint32_t name_other;        /* any other name pointer */
    uint32_t collapses;         /* Shader::Collapse entries */
    uint32_t collapse_values;   /* values held by all records at Collapse entry */
    uint32_t frees;             /* non-sampler values Collapse is about to free */
    uint32_t bad;               /* unreadable entry state */
} IsaacVitaUniformSetCounters;

/* Copies the window counters into *out (NULL discards) and zeroes them.  The
 * cumulative counters behind the periodic KAGE VITA UNIFORM SET log line are
 * untouched. */
void isaac_vita_uniform_set_receipt_take(IsaacVitaUniformSetCounters *out);

#endif
