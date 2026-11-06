/* Host oracle for the _access import with and without
 * ISAAC_VITA_ACCESS_NEGATIVE_CACHE.  Includes the production filesystem TU
 * (fake psp2 headers come from the test script) and drives _access through a
 * guest cdecl frame against a fake sceIoGetstat.  Every call prints one
 * transcript line (path, mode, EAX, guest errno); the script requires the
 * OFF and ON transcripts to be identical and the ON build to issue fewer
 * stats. */
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "guest.h"

#include "host_vita_filesystem.c"

#define ROOT "ux0:/data/isaacr001"
#define MODS ROOT "/mods/"

int32_t g_isaac_vita_crt_errno;
static const char *s_mapped_path;
static unsigned s_stat_calls;
_Alignas(16) static uint32_t s_guest_stack[64];

static const char *const s_dirs[] = {
    ROOT, ROOT "/mods", MODS "eid", MODS "eid/resources",
    MODS "eid/resources/gfx", MODS "repro_a", NULL
};
static const char *const s_files[] = {
    MODS "eid/metadata.xml", MODS "eid/resources/gfx/eid_icons.png",
    MODS "eid/resources/stages.xml", MODS "repro_a/main.lua", NULL
};
/* Read-only file: _access(W) answers EACCES. */
static const char s_readonly[] = MODS "eid/resources/gfx/readonly.png";
static const char *const s_created_dirs[] = {
    MODS "repro_a/resources-dlc3", MODS "repro_a/resources-dlc3/gfx", NULL
};
static const char s_created_file[] = MODS "repro_a/resources-dlc3/gfx/new.png";
static int s_created;

void guest_fault(CPU *__restrict cpu, uint32_t address, const char *what)
{
    if (!cpu->fault) {
        cpu->fault = what;
        cpu->fault_addr = address;
    }
}

void isaac_vita_log(const char *format, ...)
{
    (void)format;
}

int sceClibPrintf(const char *format, ...)
{
    (void)format;
    return 0;
}

int isaac_vita_startup_map_path(CPU *__restrict cpu, uint32_t guest_path,
                                char *native_path, uint32_t capacity)
{
    size_t length = strlen(s_mapped_path);

    (void)cpu;
    (void)guest_path;
    if (capacity <= length)
        return 0;
    memcpy(native_path, s_mapped_path, length + 1U);
    return 1;
}

static int listed(const char *const *list, const char *path)
{
    for (; *list; ++list)
        if (strcmp(*list, path) == 0)
            return 1;
    return 0;
}

int sceIoGetstat(const char *file, SceIoStat *status)
{
    size_t length;
    const char *const *entry;

    ++s_stat_calls;
    if (strchr(file, '\\'))
        return (int)VITA_FILESYSTEM_SCE_EINVAL;
    if (strstr(file, "/locked"))
        return (int)VITA_FILESYSTEM_SCE_EACCES;
    if (listed(s_dirs, file) || (s_created && listed(s_created_dirs, file))) {
        status->st_mode = SCE_S_IFDIR | SCE_S_IWUSR;
        return 0;
    }
    if (listed(s_files, file) || (s_created && !strcmp(file, s_created_file))) {
        status->st_mode = SCE_S_IFREG | SCE_S_IWUSR;
        return 0;
    }
    if (!strcmp(file, s_readonly)) {
        status->st_mode = SCE_S_IFREG;
        return 0;
    }
    /* Below an existing file: ENOTDIR. */
    for (entry = s_files; *entry; ++entry) {
        length = strlen(*entry);
        if (!strncmp(file, *entry, length) && file[length] == '/')
            return (int)VITA_FILESYSTEM_SCE_ENOTDIR;
    }
    return (int)VITA_FILESYSTEM_SCE_ENOENT;
}

static int access_call(const char *path, uint32_t mode)
{
    CPU cpu;
    uint32_t esp = (uint32_t)(uintptr_t)&s_guest_stack[24];

    memset(&cpu, 0, sizeof cpu);
    memset(s_guest_stack, 0, sizeof s_guest_stack);
    st32(esp, UINT32_C(0xaabbccdd));
    st32(esp + 4U, 1U);
    st32(esp + 8U, mode);
    cpu.esp = esp;
    cpu.stack_owner = &cpu;
    cpu.stack_floor = (uint32_t)(uintptr_t)&s_guest_stack[0];
    cpu.stack_ceiling = (uint32_t)(uintptr_t)&s_guest_stack[64];
    cpu.stack_low_water = cpu.stack_ceiling;
    s_mapped_path = path;
    g_isaac_vita_crt_errno = 0;
    vita_filesystem_access(&cpu);
    if (cpu.fault || cpu.esp != esp + 4U) {
        fprintf(stderr, "frame damage for %s\n", path);
        return -1;
    }
    printf("access %s mode=%u eax=%d errno=%d\n", path, (unsigned)mode,
           (int)cpu.eax, (int)g_isaac_vita_crt_errno);
    return 0;
}

int main(void)
{
    /* Resource paths the game asked for at startup and in Room 1.22 (the
     * [isaac-lua] access receipts, vita-debug-before.log 2026-09-06, plus
     * the hotprof fight's "not preloaded" sounds). */
    static const char *const resources[] = {
        "gfx/shadow.png", "gfx/grid/grid_pit_mask.png", "gfx/grid/lava.png",
        "gfx/grid/lava_cold.png", "gfx/backdrop/8bitclouds_64_thick.png",
        "gfx/ash_mask.png", "backdrops.xml", "stages.xml", "curses.xml",
        "gfx/ui/ScoreMenu.anm2", "gfx/ui/ScoreMenu.png",
        "gfx/ui/LeaderboardMenu.anm2", "gfx/ui/LeaderboardMenu.png",
        "gfx/effects/effect_002_bloodpool.anm2",
        "gfx/effects/effect_002_bloodpool.png",
        "gfx/monsters/rebirth/monster_000_bodies01.png",
        "gfx/items/collectibles/collectibles_001_thesadonion.png",
        "sfx/plop.wav", "sfx/boss1_explosions_0.wav", NULL
    };
    static const char *const mods[] = { "repro_a", "eid", NULL };
    static const char *const sequence[] = {
        MODS "eid/resources/gfx/grid/lava_cold.png",
        MODS "eid/content-dlc3/curses.xml",
        MODS "eid/content/curses.xml",
        MODS "eid/disable.it",
        MODS "eid/metadata.xml",
        MODS "eid/resources/gfx/eid_icons.png",
        MODS "eid/resources",
        MODS "eid/metadata.xml/x",
        MODS "eid/locked/a.png",
        MODS "eid/locked/a.png",
        MODS "eid//metadata.xml",
        MODS "eid\\bomb.png",
        ROOT "/resources/gfx/shadow.png",
        MODS "repro_a/resources-dlc3",
        NULL
    };
    const char *const *path;
    const char *const *resource;
    const char *const *mod;
    char probe[256];
    unsigned before;

    /* TryRedirectPath: resources-dlc3 then resources, per enabled mod. */
    for (resource = resources; *resource; ++resource) {
        for (mod = mods; *mod; ++mod) {
            (void)snprintf(probe, sizeof probe, MODS "%s/resources-dlc3/%s",
                           *mod, *resource);
            if (access_call(probe, 0U) != 0)
                return 1;
            (void)snprintf(probe, sizeof probe, MODS "%s/resources/%s",
                           *mod, *resource);
            if (access_call(probe, 0U) != 0)
                return 1;
        }
    }
    fprintf(stderr, "redirect probes=%u stats=%u\n",
            (unsigned)(2U * 2U * (sizeof resources / sizeof resources[0] - 1U)),
            s_stat_calls);
    for (path = sequence; *path; ++path)
        if (access_call(*path, 0U) != 0)
            return 1;
    if (access_call(s_readonly, 2U) != 0 ||
        access_call(MODS "repro_a/resources-dlc3/gfx/new.png", 2U) != 0)
        return 1;

    /* The directories and the file appear; the creators report them. */
    s_created = 1;
    before = s_stat_calls;
#if defined(ISAAC_VITA_ACCESS_NEGATIVE_CACHE)
    isaac_vita_access_cache_note_create(MODS "repro_a/resources-dlc3");
    isaac_vita_access_cache_note_create(MODS "repro_a/resources-dlc3/gfx");
    isaac_vita_access_cache_note_create(s_created_file);
#endif
    if (access_call(s_created_file, 0U) != 0 ||
        access_call(MODS "repro_a/resources-dlc3/gfx/shadow.png", 0U) != 0 ||
        access_call(MODS "repro_a/resources-dlc3", 0U) != 0)
        return 1;
    fprintf(stderr, "stats=%u after_create=%u\n", s_stat_calls,
            s_stat_calls - before);
    return 0;
}
