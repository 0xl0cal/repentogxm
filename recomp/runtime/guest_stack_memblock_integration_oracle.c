/* Host integration oracle for ISAAC_VITA_GUEST_STACK_MEMBLOCK: the stack
 * owners of guest.c (guest_stack_init / guest_stack_free), compiled ON with
 * -D__vita__ against stub psp2 headers, run on the host against this private
 * fake sysmem.  The fake owns one 0x401000-byte slot mapped below 4 GiB
 * (guest.c rejects a stack base above UINT32_MAX on a 64-bit host).  Built,
 * linked with --gc-sections and run by recomp/vita/test_guest_stack_memblock.sh;
 * not part of the Vita build.
 *
 * Pinned sequence: acquire -> p = base -> floor/ceiling margins -> exactly
 * GUEST_STACK_SIZE zeroed (padding page untouched) -> bind; release + status
 * reset at both init failure sites; free -> unbinding published (floor,
 * ceiling, esp zero: the cross-core reader guard is dead) -> release, uid -1,
 * status none; a second init or free makes no sysmem call; alloc and
 * get-base failures are fail-closed (rc 1, kernel rc kept in the snapshot,
 * no newlib fallback). */
#define _GNU_SOURCE
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

#include <psp2/kernel/sysmem.h>

#include "guest.h"
#include "guest_stack_memblock.h"
#if defined(ISAAC_VITA_GUEST_SAMPLER)
/* Case 9 links the production sampler (kage_vita_guest_sampler.c in oracle
 * mode: SceKernel thread and start routine compiled out, the two memory
 * accessors resolve here) against the real memblock-backed stack: bound, it
 * scans the live slot; stopped the way the entry tail stops it before
 * guest_stack_free, it reads nothing; after the free it reads nothing and no
 * load ever touched the dead slot. */
#include "kage_vita_guest_sampler.h"

void kage_vita_guest_sampler_oracle_bind(
    CPU *c, const uint32_t *functions, uint32_t function_count,
    uint32_t text_present);
void kage_vita_guest_sampler_oracle_sample(void);
uint32_t kage_vita_guest_sampler_oracle_scan_words(void);
#endif

#define SLOT_BYTES GUEST_STACK_MEMBLOCK_REQUEST_BYTES
#define MARGIN_BYTES 4096U
#define DIRTY_BYTE 0xa5U
#define FAKE_UID ((SceUID)0x4242)
#define FAKE_ALLOC_RC (-0x111)
#define FAKE_GET_RC (-0x211)

static unsigned char *s_slot;
static int s_live;
static int s_fail_alloc;   /* next alloc fails with FAKE_ALLOC_RC */
static int s_fail_get;     /* next get-base fails with FAKE_GET_RC */
static int s_high_base;    /* next get-base returns a base above UINT32_MAX */
static unsigned s_allocs, s_gets, s_frees;
static SceSize s_last_size;
static char s_last_name[32];
static SceKernelMemBlockType s_last_type;
static int s_last_option_null;
static const CPU *s_reader_cpu;          /* the CPU a cross-core reader samples */
static unsigned s_frees_under_live_guard; /* unmaps while that guard still
                                          * admitted the stack */

/* The guard of the two cross-core stack readers (kage_vita_guest_sampler.c
 * kvgs_sample, host_vita_lua.c vita_lua_beat_sample_stack), same shape: a
 * reader that re-checks it after the unmap must be refused, so
 * guest_stack_free has to publish the unbinding before the release. */
static int reader_guard_admits(const CPU *c)
{
    uint32_t esp, floor, ceiling;

    if (!c)
        return 0;
    esp = c->esp;
    floor = c->stack_floor;
    ceiling = c->stack_ceiling;
    return floor && floor < ceiling && esp >= floor && esp <= ceiling &&
           (esp & 3U) == 0U;
}

#if defined(ISAAC_VITA_GUEST_SAMPLER)
/* guest.c owns this word in a sampler build; the guest.c object linked here
 * is compiled without the sampler macro, so the oracle provides it. */
volatile uint32_t g_kage_guest_last_indirect_target;
static uint32_t s_sampler_reads;        /* sampler loads of the live slot */
static uint32_t s_sampler_reads_dead;   /* sampler loads anywhere else */
static char s_sampler_log[256];

uint32_t kvgs_oracle_stack_word(uint32_t address)
{
    uintptr_t slot = (uintptr_t)s_slot;

    if (!s_live || address < slot || address > slot + SLOT_BYTES - 4U ||
        (address & 3U) != 0U) {
        ++s_sampler_reads_dead;
        return 0U;
    }
    ++s_sampler_reads;
    return *(volatile const uint32_t *)(uintptr_t)address;
}

unsigned kvgs_oracle_image_byte(uint32_t rva)
{
    (void)rva;   /* zero-filled stack: the marker validator never runs */
    return 0U;
}

int sceClibPrintf(const char *format, ...)
{
    (void)format;   /* the window reporter is not driven here */
    return 0;
}

int sceClibSnprintf(char *buffer, size_t size, const char *format, ...)
{
    va_list arguments;
    int result;

    va_start(arguments, format);
    result = vsnprintf(buffer, size, format, arguments);
    va_end(arguments);
    return result;
}

void isaac_vita_log(const char *format, ...)
{
    va_list arguments;

    va_start(arguments, format);
    (void)vsnprintf(s_sampler_log, sizeof s_sampler_log, format, arguments);
    va_end(arguments);
    printf("ON log: %s\n", s_sampler_log);
}
#endif

SceUID sceKernelAllocMemBlock(const char *name, SceKernelMemBlockType type,
                              SceSize size, SceKernelAllocMemBlockOpt *option)
{
    ++s_allocs;
    s_last_size = size;
    s_last_type = type;
    s_last_option_null = option == NULL;
    s_last_name[0] = '\0';
    if (name) {
        strncpy(s_last_name, name, sizeof s_last_name - 1U);
        s_last_name[sizeof s_last_name - 1U] = '\0';
    }
    if (s_fail_alloc) {
        s_fail_alloc = 0;
        return FAKE_ALLOC_RC;
    }
    if (s_live || size != SLOT_BYTES)
        return -0x112;
    s_live = 1;
    memset(s_slot, DIRTY_BYTE, SLOT_BYTES);
    return FAKE_UID;
}

int sceKernelGetMemBlockBase(SceUID uid, void **base)
{
    ++s_gets;
    if (uid != FAKE_UID || !s_live || !base)
        return -0x212;
    if (s_fail_get) {
        s_fail_get = 0;
        return FAKE_GET_RC;
    }
    if (s_high_base) {
        /* Page-aligned, never dereferenced: guest.c's own base check must
         * release the block before touching it. */
        s_high_base = 0;
        *base = (void *)(uintptr_t)((uint64_t)UINT32_MAX + 1U);
        return 0;
    }
    *base = s_slot;
    return 0;
}

int sceKernelFreeMemBlock(SceUID uid)
{
    ++s_frees;
    if (reader_guard_admits(s_reader_cpu))
        ++s_frees_under_live_guard;
    if (uid != FAKE_UID || !s_live)
        return -0x311;
    s_live = 0;
    return 0;
}

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); \
        return 1; \
    } \
} while (0)

static int map_slot_below_4gib(void)
{
    void *p;
#ifdef MAP_32BIT
    p = mmap(NULL, SLOT_BYTES, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
    if (p != MAP_FAILED && (uintptr_t)p <= UINT32_MAX - SLOT_BYTES) {
        s_slot = p;
        return 1;
    }
    if (p != MAP_FAILED)
        munmap(p, SLOT_BYTES);
#endif
    /* Hosts without MAP_32BIT: hint the kernel page by page through the low
     * 4 GiB and keep the first mapping that lands there. */
    {
        uintptr_t hint;

        for (hint = 0x10000000U; hint <= 0xf0000000U; hint += 0x10000000U) {
            p = mmap((void *)hint, SLOT_BYTES, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            if (p == MAP_FAILED)
                continue;
            if ((uintptr_t)p <= UINT32_MAX - SLOT_BYTES) {
                s_slot = p;
                return 1;
            }
            munmap(p, SLOT_BYTES);
        }
    }
    return 0;
}

static int slot_bytes_are(unsigned value, size_t begin, size_t end)
{
    size_t i;

    for (i = begin; i < end; ++i)
        if (s_slot[i] != (unsigned char)value)
            return 0;
    return 1;
}

static int counts_are(unsigned allocs, unsigned gets, unsigned frees)
{
    return s_allocs == allocs && s_gets == gets && s_frees == frees;
}

static void print_failure(const char *what,
                          const guest_stack_memblock_snapshot *snap)
{
    printf("ON %s: status=%s uid=0x%08x get=0x%08x base=0x%08x "
           "rollback=0x%08x\n",
           what, guest_stack_memblock_status_text(snap->status),
           (unsigned)snap->uid, (unsigned)snap->get_result,
           (unsigned)snap->base, (unsigned)snap->rollback_result);
}

int main(void)
{
    static CPU cpu;
    guest_stack_memblock_snapshot snap;
    uint32_t slot_base;

    CHECK(map_slot_below_4gib());
    CHECK(((uintptr_t)s_slot & (MARGIN_BYTES - 1U)) == 0U);
    slot_base = (uint32_t)(uintptr_t)s_slot;
    memset(&cpu, 0, sizeof cpu);
    s_reader_cpu = &cpu;

    /* 1. READY transaction: request, name, type, no option; the stack lives
     *    inside the block with the unchanged 4 KiB margins; exactly
     *    GUEST_STACK_SIZE bytes are zeroed and the padding page stays dirty. */
    CHECK(guest_stack_init(&cpu) == 0);
    CHECK(counts_are(1U, 1U, 0U) && s_live);
    CHECK(s_last_size == SLOT_BYTES);
    CHECK(strcmp(s_last_name, GUEST_STACK_MEMBLOCK_NAME) == 0);
    CHECK(s_last_type == SCE_KERNEL_MEMBLOCK_TYPE_USER_RW && s_last_option_null);
    CHECK(guest_stack_memblock_snapshot_get(&snap) == 1);
    CHECK(snap.status == GUEST_STACK_MEMBLOCK_READY && snap.uid == FAKE_UID);
    CHECK(snap.base == (uintptr_t)s_slot);
    CHECK(snap.request_bytes == GUEST_STACK_MEMBLOCK_REQUEST_BYTES);
    CHECK(snap.get_result == 0 && snap.rollback_result == 0);
    CHECK(cpu.stack_owner == &cpu);
    CHECK(cpu.stack_floor == slot_base + MARGIN_BYTES);
    CHECK(cpu.stack_ceiling == slot_base + GUEST_STACK_SIZE - MARGIN_BYTES);
    CHECK(guest_stack_capacity(&cpu) == GUEST_STACK_SIZE - 2U * MARGIN_BYTES);
    CHECK(cpu.esp == cpu.stack_ceiling && cpu.ebp == cpu.stack_ceiling);
    CHECK(cpu.stack_low_water == cpu.stack_ceiling);
    CHECK(slot_bytes_are(0U, 0U, GUEST_STACK_SIZE));
    CHECK(slot_bytes_are(DIRTY_BYTE, GUEST_STACK_SIZE, SLOT_BYTES));
    printf("ON init: floor=%08x ceiling=%08x capacity=%u uid=0x%08x "
           "base=0x%08x request=%u\n",
           (unsigned)cpu.stack_floor, (unsigned)cpu.stack_ceiling,
           (unsigned)guest_stack_capacity(&cpu), (unsigned)snap.uid,
           (unsigned)snap.base, (unsigned)snap.request_bytes);

    /* 2. A second init while the stack is live is refused before sysmem. */
    CHECK(guest_stack_init(&cpu) == 1);
    CHECK(counts_are(1U, 1U, 0U) && s_live);
    CHECK(cpu.stack_owner == &cpu && cpu.stack_floor == slot_base + MARGIN_BYTES);

    /* 3. free: one release, snapshot reset to uid -1 / none, CPU unbound; a
     *    second free makes no sysmem call.  The unbinding (floor, ceiling,
     *    esp) is published before the unmap: the reader guard was already
     *    dead when the fake sysmem saw the free. */
    CHECK(reader_guard_admits(&cpu));
    guest_stack_free(&cpu);
    CHECK(counts_are(1U, 1U, 1U) && !s_live);
    CHECK(s_frees_under_live_guard == 0U);
    CHECK(guest_stack_memblock_snapshot_get(&snap) == 1);
    CHECK(snap.uid == -1 && snap.status == GUEST_STACK_MEMBLOCK_NONE);
    CHECK(strcmp(guest_stack_memblock_status_text(snap.status), "none") == 0);
    CHECK(cpu.stack_owner == NULL && cpu.stack_floor == 0U &&
          cpu.stack_ceiling == 0U && cpu.stack_low_water == 0U &&
          cpu.esp == 0U && cpu.ebp == 0U);
    puts("ON free: unbinding published before sceKernelFreeMemBlock "
         "(reader guard dead at the unmap)");
    guest_stack_free(&cpu);
    CHECK(counts_are(1U, 1U, 1U));
    CHECK(guest_stack_memblock_snapshot_get(&snap) == 1);
    CHECK(snap.uid == -1 && snap.status == GUEST_STACK_MEMBLOCK_NONE);

    /* 4. Fail-closed alloc failure: rc 1, the kernel rc in the snapshot, no
     *    get or free, nothing bound, no newlib fallback. */
    s_fail_alloc = 1;
    CHECK(guest_stack_init(&cpu) == 1);
    CHECK(counts_are(2U, 1U, 1U) && !s_live);
    CHECK(guest_stack_memblock_snapshot_get(&snap) == 1);
    CHECK(snap.status == GUEST_STACK_MEMBLOCK_ALLOC_FAILED);
    CHECK(snap.uid == FAKE_ALLOC_RC && snap.base == 0U);
    CHECK(snap.get_result == 0 && snap.rollback_result == 0);
    CHECK(strcmp(guest_stack_memblock_status_text(snap.status),
                 "alloc-failed") == 0);
    CHECK(cpu.stack_owner == NULL && cpu.stack_floor == 0U);
    print_failure("alloc failure", &snap);

    /* 5. get-base failure: the unit rolls the block back itself (one free),
     *    init returns 1 and the receipt fields survive for entry_vita.c. */
    s_fail_get = 1;
    CHECK(guest_stack_init(&cpu) == 1);
    CHECK(counts_are(3U, 2U, 2U) && !s_live);
    CHECK(guest_stack_memblock_snapshot_get(&snap) == 1);
    CHECK(snap.status == GUEST_STACK_MEMBLOCK_GET_FAILED);
    CHECK(snap.uid == FAKE_UID && snap.get_result == FAKE_GET_RC);
    CHECK(snap.base == 0U && snap.rollback_result == 0);
    CHECK(strcmp(guest_stack_memblock_status_text(snap.status),
                 "get-base-failed") == 0);
    CHECK(cpu.stack_owner == NULL && cpu.stack_floor == 0U);
    print_failure("get failure", &snap);

#if UINTPTR_MAX > UINT32_MAX
    /* 6. First release site in guest.c: a READY block whose base does not fit
     *    the 32-bit guest is released by guest_stack_init and the status is
     *    reset; nothing was written to it. */
    s_high_base = 1;
    CHECK(guest_stack_init(&cpu) == 1);
    CHECK(counts_are(4U, 3U, 3U) && !s_live);
    CHECK(guest_stack_memblock_snapshot_get(&snap) == 1);
    CHECK(snap.status == GUEST_STACK_MEMBLOCK_NONE && snap.uid == FAKE_UID);
    CHECK(cpu.stack_owner == NULL && cpu.stack_floor == 0U);
    CHECK(slot_bytes_are(DIRTY_BYTE, 0U, SLOT_BYTES));
    puts("ON high base: released by guest_stack_init, status=none");
#else
    s_allocs = 4U;
    s_gets = 3U;
    s_frees = 3U;
#endif

    /* 7. Second release site: guest_stack_bind refuses a CPU that already
     *    carries a stack field; the block was zeroed (memset precedes bind),
     *    then released, status reset. */
    cpu.stack_low_water = 1U;
    CHECK(guest_stack_init(&cpu) == 1);
    CHECK(counts_are(5U, 4U, 4U) && !s_live);
    CHECK(guest_stack_memblock_snapshot_get(&snap) == 1);
    CHECK(snap.status == GUEST_STACK_MEMBLOCK_NONE && snap.uid == FAKE_UID);
    CHECK(cpu.stack_owner == NULL && cpu.stack_floor == 0U &&
          cpu.stack_low_water == 1U);
    CHECK(slot_bytes_are(0U, 0U, GUEST_STACK_SIZE));
    CHECK(slot_bytes_are(DIRTY_BYTE, GUEST_STACK_SIZE, SLOT_BYTES));
    cpu.stack_low_water = 0U;
    puts("ON bind failure: released by guest_stack_init, status=none");

    /* 8. After every failure the owner is reusable: READY again, then a
     *    clean free. */
    CHECK(guest_stack_init(&cpu) == 0);
    CHECK(counts_are(6U, 5U, 4U) && s_live);
    CHECK(guest_stack_memblock_snapshot_get(&snap) == 1);
    CHECK(snap.status == GUEST_STACK_MEMBLOCK_READY && snap.uid == FAKE_UID);
    CHECK(cpu.stack_floor == slot_base + MARGIN_BYTES);
    guest_stack_free(&cpu);
    CHECK(counts_are(6U, 5U, 5U) && !s_live);
    CHECK(s_frees_under_live_guard == 0U);
    CHECK(guest_stack_memblock_snapshot_get(&snap) == 1);
    CHECK(snap.uid == -1 && snap.status == GUEST_STACK_MEMBLOCK_NONE);

#if defined(ISAAC_VITA_GUEST_SAMPLER)
    /* 9. The entry tail's teardown order against the production sampler:
     *    bound to this CPU it scans every word of one 256-word window of the
     *    live slot; kage_vita_guest_sampler_stop (the tail's call before
     *    guest_stack_free) leaves a tick with no read and logs the stop
     *    receipt; guest_stack_free then unmaps with the reader guard already
     *    dead, and a later tick still reads nothing.  No load ever touched a
     *    dead slot. */
    CHECK(guest_stack_init(&cpu) == 0);
    CHECK(counts_are(7U, 6U, 5U) && s_live);
    kage_vita_guest_sampler_oracle_bind(&cpu, NULL, 0U, 0U);
    cpu.esp = cpu.stack_ceiling -
              kage_vita_guest_sampler_oracle_scan_words() * 4U;
    kage_vita_guest_sampler_oracle_sample();
    CHECK(s_sampler_reads == kage_vita_guest_sampler_oracle_scan_words());
    CHECK(s_sampler_reads_dead == 0U);
    kage_vita_guest_sampler_stop();
    CHECK(strcmp(s_sampler_log,
                 "KAGE VITA GUEST SAMPLER: stopped ticks=1 busy_spins=0") == 0);
    kage_vita_guest_sampler_oracle_sample();
    CHECK(s_sampler_reads == kage_vita_guest_sampler_oracle_scan_words());
    CHECK(reader_guard_admits(&cpu));
    guest_stack_free(&cpu);
    CHECK(counts_are(7U, 6U, 6U) && !s_live);
    CHECK(s_frees_under_live_guard == 0U);
    kage_vita_guest_sampler_oracle_sample();
    CHECK(s_sampler_reads == kage_vita_guest_sampler_oracle_scan_words());
    CHECK(s_sampler_reads_dead == 0U);
    printf("ON sampler: %u words read while bound, 0 after "
           "kage_vita_guest_sampler_stop, 0 after guest_stack_free\n",
           (unsigned)s_sampler_reads);
#endif

    CHECK(munmap(s_slot, SLOT_BYTES) == 0);
    puts("guest stack memblock integration oracle: PASS");
    return 0;
}
