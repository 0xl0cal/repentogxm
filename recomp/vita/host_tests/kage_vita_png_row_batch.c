/* Included after the existing actual-wrapper clock fixture and freshly
 * regenerated ImagePng row loop. Allocation/stream boundaries are mocked;
 * wrapper, middle-row delivery, scratch checks and guest loop are production. */
#include "../../runtime/host_vita_texel_scratch.c"
static uint8_t batch_pixels[131072], batch_raw[131072];
static uint32_t batch_table[2048];
static unsigned deny_batch_lease, batch_lease_live, fail_batch_release;
static unsigned batch_calls, batch_taken;
unsigned char *g_guest_coverage_functions, *g_guest_coverage_cases;

uint32_t isaac_vita_guest_heap_lease_exact_range(const void *base,
    const void *range, size_t n)
{
    errno = ERANGE;
    if (deny_batch_lease || base != batch_table || range != base ||
        n != s_ses.height * 4u)
        return 0u;
    CHECK(!batch_lease_live);
    batch_lease_live = 1u;
    return 19u;
}
int isaac_vita_guest_heap_lease_release(uint32_t token)
{
    CHECK(token == 19u && batch_lease_live);
    batch_lease_live = 0u;
    errno = ERANGE;
    return !fail_batch_release;
}
static int batch_checked(CPU *c)
{
    int result;
    ++batch_calls;
    result = isaac_vita_native_png_middle_rows_try(c);
    if (result > 0) ++batch_taken;
    return result;
}
void sub_005b1500(CPU *c) { __wrap_sub_005b1500(c); }
/* FRESH_BATCH_LOOPS */

static void batch_setup(unsigned h, unsigned stride, unsigned channels)
{
    unsigned y, x;
    reset(NORMAL);
    CHECK(h <= 2048u && (uint64_t)h * stride < sizeof batch_pixels);
    CHECK((uint64_t)h * (channels * 3u + 1u) < sizeof batch_raw);
    memset(batch_pixels, 0xd3, sizeof batch_pixels);
    memset(batch_raw, 0xa6, sizeof batch_raw);
    memset(batch_table, 0x8d, sizeof batch_table);
    for (y = 0; y < h; ++y) {
        batch_table[y] = ptr(batch_pixels) + y * stride;
        batch_raw[y * (channels * 3u + 1u)] = (uint8_t)(y % 5u);
        for (x = 0; x < channels * 3u; ++x)
            batch_raw[y * (channels * 3u + 1u) + x + 1u] =
                (uint8_t)(x * 17u + y * 3u);
    }
    cpu.ebp = cpu.stack_ceiling - 256u;
    cpu.esp = cpu.ebp - 0x448u;
    cpu.stack_low_water = cpu.esp;
    cpu.edi = ptr(batch_table); cpu.ecx = h; cpu.eax = 1u;
    cpu.ebx = png_ptr();
    st32(cpu.ebp + 4u, 0x005a0c7eu);
    st32(cpu.ebp - 0x43cu, ptr(batch_table));
    st32(cpu.ebp - 0x438u, h);
    st32(cpu.ebp - 0x418u, png_ptr());
    st32(cpu.ebp - 0x410u, 1u);
    st32(png_ptr() + ISAAC_NP_PNG_HEIGHT, h);
    st32(png_ptr() + ISAAC_NP_PNG_NUM_ROWS, h);
    st32(png_ptr() + ISAAC_NP_PNG_ROWBYTES, channels * 3u);
    s_ses.active = 1u; s_ses.mode = NP_MODE_SERVE;
    s_ses.png = png_ptr(); s_ses.width = 3u; s_ses.height = h;
    s_ses.channels = channels; s_ses.rowbytes = channels * 3u;
    s_ses.color_type = channels == 4u ? 6u : channels == 3u ? 2u : channels == 2u ? 4u : 0u;
    s_ses.raw = batch_raw; s_ses.crc_final = 0x19283746u;
    s_ses.last_filter = (h - 1u) % 5u;
    memset(s_last_row, 0x68, sizeof s_last_row);
    memset(&s_state, 0, sizeof s_state);
    s_state.live = 1u; s_state.live_size = h * stride;
    s_state.owner_return_rva = KAGE_VITA_TEXEL_PNG_LOADER_RETURN;
    atomic_store(&s_published_base, ptr(batch_pixels));
    atomic_flag_clear(&s_state_lock);
    batch_lease_live = deny_batch_lease = fail_batch_release = 0u;
    batch_taken = batch_calls = 0u;
    g_guest_coverage_functions = g_guest_coverage_cases = NULL;
}
static void batch_pair(unsigned h, unsigned stride, unsigned channels, unsigned guard)
{
    CPU before;
    uint8_t memory[sizeof arena], pixels[sizeof batch_pixels];
    unsigned expected;
    batch_setup(h, stride, channels);
    batch_loop_original(&cpu);
    before = cpu;
    memcpy(memory, arena, sizeof arena);
    memcpy(pixels, batch_pixels, sizeof batch_pixels);
    CHECK(s_stats.native == 1u && s_stats.rows == h && !s_ses.active);
    batch_setup(h, stride, channels);
    if (guard == 1u) deny_batch_lease = 1u;
    if (guard == 2u) s_state.owner_return_rva ^= 1u;
    if (guard == 3u) atomic_flag_test_and_set(&s_state_lock);
    if (guard == 4u) s_state.live_size = 1u;
    if (guard == 5u) s_state.poisoned = 1u;
    if (guard == 6u) g_guest_coverage_functions = (unsigned char *)1;
    if (guard == 7u) g_guest_coverage_cases = (unsigned char *)1;
    errno = EDOM;
    batch_loop_candidate(&cpu);
    expected = h >= 3u && !guard;
    CHECK(batch_taken == expected && !batch_lease_live && errno == EDOM);
    CHECK(!memcmp(&cpu, &before, sizeof cpu));
    CHECK(!memcmp(arena, memory, sizeof arena));
    CHECK(!memcmp(batch_pixels, pixels, sizeof pixels));
    CHECK(s_stats.native == 1u && s_stats.rows == h && !s_ses.active);
}
static void batch_first_row(void)
{
    batch_setup(7u, 15u, 4u);
    cpu.esi = ptr(batch_table); cpu.edi = 7u;
    cpu.edx = batch_table[0]; cpu.ecx = png_ptr();
    gpush(&cpu, 7u); gpush(&cpu, ISAAC_NP_READ_ROW_SITE_RVA);
    __wrap_sub_005b1500(&cpu);
    CHECK(s_ses.next_row == 1u);
}
static void batch_rejections(void)
{
    unsigned i;
    for (i = 0; i < 27u; ++i) {
        CPU before;
        np_session session;
        uint8_t memory[sizeof arena], pixels[sizeof batch_pixels];
        batch_first_row();
        switch (i) {
        case 0: s_ses.active = 0u; break;
        case 1: s_ses.mode = NP_MODE_TRANSLATED; break;
        case 2: s_ses.mode = NP_MODE_VERIFY; break;
        case 3: s_ses.next_row = 2u; break;
        case 4: s_ses.height = 2u; break;
        case 5: cpu.ebx ^= 4u; break;
        case 6: cpu.edi = 6u; break;
        case 7: s_ses.raw = NULL; break;
        case 8: cpu.ebp = 0u; break;
        case 9: cpu.stack_owner = NULL; break;
        case 10: cpu.esp = 0u; break;
        case 11: st32(cpu.ebp - 0x43cu, 0u); break;
        case 12: st32(cpu.ebp - 0x438u, 0u); break;
        case 13: st32(cpu.ebp - 0x418u, 0u); break;
        case 14: st32(cpu.ebp - 0x410u, 0u); break;
        case 15: st32(cpu.ebp + 4u, 0u); break;
        case 16: st32(png_ptr() + ISAAC_NP_PNG_ROW_NUMBER, 0u); break;
        case 17: deny_batch_lease = 1u; break;
        case 18: batch_table[0] ^= 4u; break;
        case 19: batch_table[1] = batch_table[0] + 1u; break;
        case 20: batch_table[3] ^= 4u; break;
        case 21: s_state.live = 0u; break;
        case 22: s_state.poisoned = 1u; break;
        case 23: s_state.owner_return_rva ^= 1u; break;
        case 24: s_state.live_size = 1u; break;
        case 25: s_ses.raw = batch_pixels; break;
        case 26: atomic_flag_test_and_set(&s_state_lock); break;
        }
        before = cpu; session = s_ses;
        memcpy(memory, arena, sizeof arena);
        memcpy(pixels, batch_pixels, sizeof batch_pixels);
        errno = EDOM;
        CHECK(isaac_vita_native_png_middle_rows_try(&cpu) == 0);
        CHECK(errno == EDOM && !batch_lease_live);
        CHECK(!memcmp(&cpu, &before, sizeof cpu));
        CHECK(!memcmp(&s_ses, &session, sizeof session));
        CHECK(!memcmp(arena, memory, sizeof arena));
        CHECK(!memcmp(batch_pixels, pixels, sizeof pixels));
    }
    batch_first_row();
    fail_batch_release = 1u; errno = EDOM;
    if (!setjmp(escape)) {
        (void)isaac_vita_native_png_middle_rows_try(&cpu);
        CHECK(0);
    }
    CHECK(faulted == 1u && !batch_lease_live && errno == EDOM);
    CHECK(cpu.fault && s_ses.next_row == 6u);
}
int main(void)
{
    unsigned h, ch, pad, guard, cases = 0u;
    const unsigned heights[] = {1, 2, 3, 7, 32, 257, 1024};
    for (h = 0; h < sizeof heights / sizeof heights[0]; ++h)
        for (ch = 1; ch <= 4; ++ch)
            for (pad = 0; pad <= 3; pad += 3)
                for (guard = 0; guard < 8; ++guard) {
                    batch_pair(heights[h], ch * 3u + pad, ch, guard);
                    ++cases;
                }
    batch_rejections();
    printf("PNG row batch PASS %u fresh-loop pairs; full CPU/stack/png/pixels/padding; no timing claim\n", cases);
    puts("PNG row batch 27 clean rejections + terminal lease failure PASS");
    return 0;
}
