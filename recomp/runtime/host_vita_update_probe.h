/* Inclusive wall-clock probe over Game::Update's translated subtrees
 * (ISAAC_VITA_UPDATE_PROBE, profiling builds only; ph120.ut / ph120.uz). */
#ifndef HOST_VITA_UPDATE_PROBE_H
#define HOST_VITA_UPDATE_PROBE_H

#include <stdint.h>

/* One --wrap seam per entry (CMake reads this list).  Names: ZHL signature,
 * vtable slot 3 (Update) of the RTTI vtable, or "by caller" = the function
 * Game::Update / Room-level update calls there (frozen PE, 2026-10-16). */
#define ISAAC_VITA_UPDATE_PROBE_LIST(X)                                     \
    X(002cdcf0) /* Game::Update (gen_all FULLSPEED pin)                   */ \
    X(00263d10) /* by caller: Game::Update                                 */ \
    X(005027e0) X(002c40e0) X(00484e40) X(0053bce0) X(004b3680)            \
    X(004f0010) X(003f5950) X(0048a3a0) X(003ebfd0) X(004e55f0)            \
    X(0050d5b0) X(003f1de0) X(00313840) X(005178e0) X(005163a0)            \
    X(004894c0) X(003e9650) X(002d0aa0) X(003bf630) X(0051a7e0)            \
    X(00013480) /* by caller 0048a3a0: entity list pass, runs 0x14f80      */ \
    X(00311e60) X(00313a20) X(002ccfa0) X(00488de0)                        \
    X(0034b8d0) /* Entity_Player vtable slot 3 (Update)                    */ \
    X(0029b140) /* Entity_NPC                                              */ \
    X(001bdf00) /* Entity_Effect::Update (ZHL, slot 3)                     */ \
    X(001fe830) /* Entity_Familiar                                         */ \
    X(0022f6f0) /* Entity_Projectile                                       */ \
    X(002b9ed0) /* Entity_Pickup                                           */ \
    X(0027ae90) /* Entity_Bomb                                             */ \
    X(00242990) /* Entity_Tear                                             */ \
    X(004d0aa0) /* Entity_Laser                                            */ \
    X(004ba640) /* Entity_Knife                                            */ \
    X(00239870) /* Entity_Slot                                             */ \
    X(002860a0) /* Entity (base) slot 3                                    */ \
    X(00016f10) /* collision broad phase, called by 0x14f80                */ \
    X(00003b00) /* AnimationState::AdvancePosition (ZHL)                   */ \
    X(00003740) /* AnimationState::Reset (ZHL)                             */ \
    X(0040eef0) /* Lua callback invoker: lua_rawgeti + pcall(id, param)    */ \
    X(005637e0) /* path hash                                               */ \
    X(00011fb0) X(00289cf0) X(00289980)                                    \
    X(001b79c0) /* Entity_Effect::Init (vtable slot 1; calls Entity::Init)  */ \
    /* v2 (2026-10-17): inside Effect::Init and the two Lua callbacks.     */ \
    X(001b7ec7) /* Effect::Init continuation (variant switch, own unit)    */ \
    X(002819e0) /* Entity::Init (ZHL)                                      */ \
    X(00007160) /* ANM2::Load (ZHL)                                        */ \
    X(000073a0) /* ANM2::LoadGraphics (ZHL)                                */ \
    X(00008be0) /* ANM2 data by path: hash, cache, parse 0x9b40 on a miss  */ \
    X(00007de0) X(00007c80) /* ANM2 helpers called by Effect::Init         */ \
    X(000054f0) /* ANM2::Play (ZHL)                                        */ \
    X(004b3340) /* Manager::LoadImage (port notes)                         */ \
    X(00470460) /* ModManager::TryRedirectPath (ZHL)                       */ \
    X(00563120) /* GetMountedFilePath (ZHL)                                */ \
    X(003fccf0) /* LuaEngine: MC_POST_UPDATE (id 1), called by Game::Update */ \
    X(003ffe10) /* LuaEngine::PostRender (ZHL): MC_POST_RENDER (id 2)      */ \
    X(001eeac0) /* called first by Effect::Init on this+0x3d8              */

#define ISAAC_VITA_UPDATE_PROBE_COUNT_ONE(r) +1
enum { ISAAC_VITA_UPDATE_PROBES = 0 ISAAC_VITA_UPDATE_PROBE_LIST(ISAAC_VITA_UPDATE_PROBE_COUNT_ONE) };
#define ISAAC_VITA_UPDATE_PROBE_PER_LINE 4

/* Keyed probes: every entry is timed and also tabled by a key read from the
 * guest at entry (ph120.uk, top ISAAC_VITA_UPDATE_PROBE_TOP_KEYS by us).
 *   0x0040eef0: [ecx + 4] = the callback id pushed to Lua (1 POST_UPDATE,
 *               2 POST_RENDER, ...: 003fcd21 / 003ffe41 store it);
 *   0x001b79c0: variant << 16 | (subtype & 0xffff), from [esp + 8] and
 *               [esp + 0xc] (001b7a0d / 001b7a12 pass [ebp + 0xc], [ebp + 0x10]
 *               on to Entity::Init(type, variant, subtype, seed)). */
#define ISAAC_VITA_UPDATE_PROBE_KEYED 2
#define ISAAC_VITA_UPDATE_PROBE_KEY_SLOTS 64
#define ISAAC_VITA_UPDATE_PROBE_TOP_KEYS 8

/* Composition probes (v3): during each timed span of these seams, the
 * import calls the span made, from the dispatcher's per-import census
 * (g_guest_phase_profile_import_calls; zero when the build has no census).
 * Import IDs = index of the IAT slot in the frozen PE's imports sorted by
 * slot RVA (gen_all import_rows): 305 free, 306 calloc, 307 malloc,
 * 309 realloc, 303 _access, 368 fopen, 359 fread, 361 fseek,
 * 61 EnterCriticalSection, 383 strncpy_s, 385 strcat_s, 281 memcpy,
 * 284 memmove (test_vita_update_probe.py re-derives them from the PE). */
/* Y, not X: CMake and the test collect the seams by matching X(rva). */
#define ISAAC_VITA_UPDATE_PROBE_COMPOSED_LIST(Y)                            \
    Y(00563120) Y(00008be0) Y(004b3340) Y(000073a0) Y(00007160)            \
    Y(001b79c0) Y(001bdf00) Y(002860a0)
#define ISAAC_VITA_UPDATE_PROBE_COMPOSED 8
enum {
    ISAAC_VITA_UPDATE_PROBE_GROUP_HEAP = 0,  /* h: malloc+free+calloc+realloc */
    ISAAC_VITA_UPDATE_PROBE_GROUP_ACCESS,    /* a: _access */
    ISAAC_VITA_UPDATE_PROBE_GROUP_FILE,      /* f: fopen+fread+fseek */
    ISAAC_VITA_UPDATE_PROBE_GROUP_LOCK,      /* c: EnterCriticalSection */
    ISAAC_VITA_UPDATE_PROBE_GROUP_STRING,    /* s: strncpy_s+strcat_s+memcpy+memmove */
    ISAAC_VITA_UPDATE_PROBE_GROUPS
};

typedef struct IsaacVitaUpdateProbeComposed {
    uint32_t rva, t, us;    /* timed spans and their sum (as in ph120.ut) */
    uint32_t calls[ISAAC_VITA_UPDATE_PROBE_GROUPS];
} IsaacVitaUpdateProbeComposed;

typedef struct IsaacVitaUpdateProbeKey {
    uint32_t key, n, us;   /* n entries (all timed), us their inclusive sum */
} IsaacVitaUpdateProbeKey;

typedef struct IsaacVitaUpdateProbeKeyed {
    uint32_t rva;
    uint32_t distinct, overflow;   /* keys seen; entries that found no slot */
    IsaacVitaUpdateProbeKey top[ISAAC_VITA_UPDATE_PROBE_TOP_KEYS];
} IsaacVitaUpdateProbeKeyed;

typedef struct IsaacVitaUpdateProbeRow {
    uint32_t rva;
    uint32_t n;     /* entries through the seam (other units, guest_table) */
    uint32_t t;     /* timed spans: the first 64 entries of a window, then 1
                     * in 64; keyed probes time every entry */
    uint32_t us;    /* sum of timed spans, inclusive of callees */
    uint32_t in;    /* timed probe spans completed inside them (each adds ~z) */
    uint32_t mx;    /* longest timed span */
} IsaacVitaUpdateProbeRow;

typedef struct IsaacVitaUpdateProbe {
    IsaacVitaUpdateProbeRow row[ISAAC_VITA_UPDATE_PROBES];
    uint32_t z_n, z_us;   /* empty clock brackets */
    IsaacVitaUpdateProbeKeyed keyed[ISAAC_VITA_UPDATE_PROBE_KEYED];
    IsaacVitaUpdateProbeComposed composed[ISAAC_VITA_UPDATE_PROBE_COMPOSED];
} IsaacVitaUpdateProbe;

/* Copy the window's counters and zero them (out may be NULL: discard). */
void isaac_vita_update_probe_take(IsaacVitaUpdateProbe *out);

#endif
