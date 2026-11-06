/* Backdrop watergfx guard helpers (ISAAC_VITA_BACKDROP_WATERGFX_GUARD).
 *
 * GitHub issue 5 items 5.4/5.17: entering a backdrop whose `watergfx` image
 * did not load (a modded gfx/backdrop/water_blood.png in the rc3 dump) left
 * Backdrop+0x2c NULL, and Backdrop::LoadGraphics faulted at 00498ce1 on the
 * virtual wrap-mode call.  gen_all.py emits the skip in the owner; these
 * helpers only name what was skipped:
 *
 *   KAGE VITA BACKDROP WATERGFX: id=%u name=%s NULL -> skipped
 *   image load: resolve failed for "%s"
 *
 * The second line comes from the loader's resolver-NULL branch (0056c834),
 * which the frozen game leaves silent, so the reporter's log names the file.
 *
 * Only reads: every address below was already read by the guest code right
 * before the seam (00498c8b..00498cb7 read the same id and string, the
 * resolver was handed the same path), so no new guest access can fault.
 * Guest address == host address (guest.h).  The static state is updated
 * without locks: a race can only duplicate or drop one log line, and every
 * table index comes from a bounded local copy.
 */
#include "kage_vita_backdrop_watergfx.h"

#include <stddef.h>
#include <stdint.h>

#if !defined(ISAAC_VITA_BACKDROP_WATERGFX_GUARD)
#error "backdrop watergfx helpers must only be compiled for the guard"
#endif

#ifndef KAGE_VITA_BACKDROP_WATERGFX_LOG
void isaac_vita_log(const char *format, ...);
#define KAGE_VITA_BACKDROP_WATERGFX_LOG isaac_vita_log
#endif

/* Backdrop layout, copied from the frozen owner 00498c8b..00498cae:
 * `mov ecx,[esi+0x150]; imul eax,ecx,0x13c; cmp [eax+esi+0x180],0;
 *  add eax,0x170; add eax,esi; cmp [eax+0x14],0x10; jb; mov eax,[eax]`. */
#define BACKDROP_ID_OFFSET            0x150u
#define BACKDROP_ENTRY_STRIDE         0x13cu
#define BACKDROP_WATER_STRING_OFFSET  0x170u
/* MSVC std::string: inline buffer or heap pointer at +0, size at +0x10,
 * capacity at +0x14; the buffer is inline while capacity < 0x10. */
#define MSVC_STRING_SIZE              0x10u
#define MSVC_STRING_CAPACITY          0x14u
#define MSVC_STRING_INLINE_LIMIT      0x10u

#define WATERGFX_NAME_BYTES           255u
#define WATERGFX_TRACKED_IDS          256u
#define WATERGFX_TRACKED_PATHS        64u

static uint32_t s_logged_ids[WATERGFX_TRACKED_IDS / 32u];
static uint32_t s_logged_untracked_id;
static uint32_t s_path_hashes[WATERGFX_TRACKED_PATHS];
static uint32_t s_path_count;
static uint32_t s_untracked_failures;

static uint32_t watergfx_read32(uint32_t address)
{
    return *(const uint32_t *)(uintptr_t)address;
}

static uint8_t watergfx_read8(uint32_t address)
{
    return *(const uint8_t *)(uintptr_t)address;
}

/* Copies at most `limit` bytes, stopping early at a NUL; always terminates. */
static void watergfx_copy(char *out, uint32_t source, uint32_t limit)
{
    uint32_t i;

    for (i = 0u; i < limit; ++i) {
        uint8_t ch = watergfx_read8(source + i);
        if (ch == 0u)
            break;
        out[i] = (char)ch;
    }
    out[i] = '\0';
}

static uint32_t watergfx_fnv1a(const char *text)
{
    uint32_t hash = 0x811c9dc5u;

    while (*text != '\0') {
        hash ^= (uint8_t)*text++;
        hash *= 0x01000193u;
    }
    return hash;
}

void isaac_vita_backdrop_watergfx_skipped(uint32_t backdrop)
{
    char name[WATERGFX_NAME_BYTES + 1u];
    uint32_t id = watergfx_read32(backdrop + BACKDROP_ID_OFFSET);
    uint32_t string;
    uint32_t size;
    uint32_t capacity;
    uint32_t data;

    if (id < WATERGFX_TRACKED_IDS) {
        uint32_t bit = 1u << (id & 31u);
        if ((s_logged_ids[id >> 5] & bit) != 0u)
            return;
        s_logged_ids[id >> 5] |= bit;
    } else {
        /* Not a Repentance id; still report it, once. */
        if (s_logged_untracked_id != 0u)
            return;
        s_logged_untracked_id = 1u;
    }

    string = backdrop + BACKDROP_WATER_STRING_OFFSET +
             id * BACKDROP_ENTRY_STRIDE;
    size = watergfx_read32(string + MSVC_STRING_SIZE);
    capacity = watergfx_read32(string + MSVC_STRING_CAPACITY);
    data = capacity < MSVC_STRING_INLINE_LIMIT ?
           string : watergfx_read32(string);
    if (size > capacity || data == 0u) {
        name[0] = '?';
        name[1] = '\0';
    } else {
        watergfx_copy(name, data,
                      size < WATERGFX_NAME_BYTES ? size : WATERGFX_NAME_BYTES);
    }
    KAGE_VITA_BACKDROP_WATERGFX_LOG(
        "KAGE VITA BACKDROP WATERGFX: id=%u name=%s NULL -> skipped",
        (unsigned)id, name);
}

void isaac_vita_backdrop_watergfx_resolve_failed(uint32_t path)
{
    char name[WATERGFX_NAME_BYTES + 1u];
    uint32_t hash;
    uint32_t count;
    uint32_t i;

    if (path == 0u) {
        name[0] = '?';
        name[1] = '\0';
    } else {
        watergfx_copy(name, path, WATERGFX_NAME_BYTES);
    }
    hash = watergfx_fnv1a(name);
    count = s_path_count;
    if (count > WATERGFX_TRACKED_PATHS)
        count = WATERGFX_TRACKED_PATHS;
    for (i = 0u; i < count; ++i) {
        if (s_path_hashes[i] == hash)
            return;
    }
    if (count < WATERGFX_TRACKED_PATHS) {
        s_path_hashes[count] = hash;
        s_path_count = count + 1u;
        KAGE_VITA_BACKDROP_WATERGFX_LOG(
            "image load: resolve failed for \"%s\"", name);
        return;
    }
    /* Table full: keep the log bounded (1st, 2nd, 4th, 8th, ... failure). */
    ++s_untracked_failures;
    if ((s_untracked_failures & (s_untracked_failures - 1u)) == 0u)
        KAGE_VITA_BACKDROP_WATERGFX_LOG(
            "image load: resolve failed for \"%s\" (untracked=%u after %u "
            "distinct paths)",
            name, (unsigned)s_untracked_failures,
            (unsigned)WATERGFX_TRACKED_PATHS);
}
