#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define SAMPLE_COUNT 8u
#define STAGE_ADD_RESULT 1u
#define STAGE_DISPLAY_CALLBACK 2u
#define BEGIN_MISMATCH 0x00000001u
#define END_MISMATCH   0x00000002u
#define CAPTURE_CAPACITY 4u
#define NOT_CALLED ((int32_t)0x80000000u)
#define SENTINEL_X 16u
#define SENTINEL_Y 16u
#define SENTINEL_SIZE 16u
#define SENTINEL_MAGENTA 0xffff00ffu
#define SENTINEL_GREEN   0xff00ff00u
#define DISPLAY_SURFACE_COUNT 3u
#define KNOWN_PRESENT_INDEX 2u
#define KNOWN_GREEN 0xff00ff00u
#define KNOWN_MAGENTA 0xffff00ffu
#define KNOWN_FORMAT 0x00060000u
#define KNOWN_X 64u
#define KNOWN_Y 64u
#define KNOWN_WIDTH 512u
#define KNOWN_HEIGHT 256u
#define KNOWN_GATE_TAG        0x00000001u
#define KNOWN_GATE_NON_SYSTEM 0x00000002u
#define KNOWN_GATE_COUNT      0x00000004u
#define KNOWN_GATE_BACK       0x00000008u
#define KNOWN_GATE_ADDRESS    0x00000010u
#define KNOWN_GATE_DEDICATED  0x00000020u
#define KNOWN_GATE_BEGIN_ONE  0x00000040u
#define KNOWN_GATE_END_ONE    0x00000080u
#define KNOWN_GATE_BEGIN_OK   0x00000100u
#define KNOWN_GATE_END_OK     0x00000200u
#define KNOWN_GATE_CONTEXT    0x00000400u
#define KNOWN_GATE_DATA       0x00000800u
#define KNOWN_GATE_SYNC       0x00001000u
#define KNOWN_GATE_MISMATCH   0x00002000u
#define KNOWN_GATE_ALL        0x00003fffu

enum surface_failure_stage {
    SURFACE_STAGE_NONE = 0,
    SURFACE_STAGE_ALLOC,
    SURFACE_STAGE_GET_BASE,
    SURFACE_STAGE_MAP
};

enum resize_event {
    RESIZE_QUEUE_FINISH = 1,
    RESIZE_GXM_FINISH,
    RESIZE_DETACH,
    RESIZE_WAIT_DETACH,
    RESIZE_DESTROY_TARGET,
    RESIZE_RELEASE_0,
    RESIZE_RELEASE_1,
    RESIZE_RELEASE_2,
    RESIZE_REBUILD
};

typedef struct ProbeEvent {
    uint32_t size;
    uint32_t stage;
    uint32_t sequence;
    uint32_t front_index;
    uint32_t back_index;
    uint32_t address;
    union {
        struct {
            int32_t result;
            uint32_t old_sync;
            uint32_t new_sync;
            uint32_t begin_context;
            uint32_t begin_render_target;
            uint32_t begin_fragment_sync;
            uint32_t begin_color_surface;
            uint32_t begin_color_data;
            int32_t begin_result;
            uint32_t begin_count;
            uint32_t end_context;
            int32_t end_result;
            uint32_t end_count;
            uint32_t mismatch_mask;
            int32_t back_memblock_uid;
            int32_t back_get_base_result;
            int32_t back_map_result;
            uint32_t back_dedicated;
        } add;
        struct {
            int32_t result;
            uint32_t size;
            uint32_t base;
            uint32_t pitch;
            uint32_t pixel_format;
            uint32_t width;
            uint32_t height;
            uint32_t sync;
            uint32_t sample_count;
            uint32_t sparse_hash;
            uint32_t rgba[SAMPLE_COUNT];
        } display;
    } detail;
} ProbeEvent;

typedef struct PendingLineage {
    uint32_t begin_context;
    uint32_t begin_render_target;
    uint32_t begin_fragment_sync;
    uint32_t begin_color_surface;
    uint32_t begin_color_data;
    int32_t begin_result;
    uint32_t begin_count;
    uint32_t end_context;
    int32_t end_result;
    uint32_t end_count;
    uint32_t mismatch_mask;
} PendingLineage;

typedef struct KnownColorEvent {
    uint32_t size;
    uint32_t present_index;
    uint32_t sequence;
    uint32_t front_index;
    uint32_t back_index;
    uint32_t address;
    uint32_t dedicated;
    uint32_t color;
    uint32_t format;
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
    uint32_t stride_bytes;
    uint32_t sync_object;
    uint32_t sync_flags;
    uint32_t gate_mask;
    uint32_t context_finish_called;
    int32_t fill_result;
    uint32_t transfer_finish_called;
    int32_t transfer_finish_result;
    int32_t queue_result;
} KnownColorEvent;

typedef struct CallbackData {
    uint32_t *address;
    uint32_t sequence;
    uint32_t front_index;
    uint32_t back_index;
} CallbackData;

typedef struct FrameBuf {
    uint32_t size;
    void *base;
    uint32_t pitch;
    uint32_t pixel_format;
    uint32_t width;
    uint32_t height;
} FrameBuf;

_Static_assert(sizeof(ProbeEvent) == 96u, "lineage event ABI drifted");
_Static_assert(sizeof(KnownColorEvent) == 88u,
               "known-color event ABI drifted");

static const uint16_t sample_xy[SAMPLE_COUNT][2] = {
    { 0, 0 }, { 959, 0 }, { 0, 543 }, { 959, 543 },
    { 480, 271 }, { 4, 4 }, { 100, 100 }, { 480, 365 }
};
static ProbeEvent captured[CAPTURE_CAPACITY];
static PendingLineage pending;
static uint32_t captured_count;
static uint32_t dropped_count;
static uint32_t failures;
static uint32_t pixels[544][960];

typedef struct SurfaceTransaction {
    int32_t uid[DISPLAY_SURFACE_COUNT];
    int32_t get_base[DISPLAY_SURFACE_COUNT];
    int32_t map[DISPLAY_SURFACE_COUNT];
    int32_t unmap_result[DISPLAY_SURFACE_COUNT];
    int32_t free_result[DISPLAY_SURFACE_COUNT];
    uint32_t live[DISPLAY_SURFACE_COUNT];
    uint32_t mapped[DISPLAY_SURFACE_COUNT];
    uint32_t dedicated[DISPLAY_SURFACE_COUNT];
    uint32_t retired[DISPLAY_SURFACE_COUNT];
    uint32_t unmap_calls[DISPLAY_SURFACE_COUNT];
    uint32_t free_calls[DISPLAY_SURFACE_COUNT];
    uint32_t pool_allocations;
    uint32_t unmaps;
    uint32_t frees;
    uint32_t failure_stage;
    uint32_t failure_index;
    const char *rollback_failure_site[DISPLAY_SURFACE_COUNT];
    int32_t rollback_failure_result[DISPLAY_SURFACE_COUNT];
    uint32_t rollback_failure_count;
} SurfaceTransaction;

static uint32_t resize_events[16];
static uint32_t resize_event_count;
static const char *resize_failure_site;
static int32_t resize_failure_result;
static uint32_t resize_release_failures;

enum known_order_event {
    KNOWN_ORDER_CLEAR = 1,
    KNOWN_ORDER_END,
    KNOWN_ORDER_CONTEXT_FINISH,
    KNOWN_ORDER_FILL,
    KNOWN_ORDER_TRANSFER_FINISH,
    KNOWN_ORDER_QUEUE,
    KNOWN_ORDER_SHARED_FB_END,
    KNOWN_ORDER_EVENT
};

typedef struct KnownColorInput {
    uint32_t present_index;
    uint32_t sequence;
    uint32_t system_app_mode;
    uint32_t buffer_count;
    uint32_t front_index;
    uint32_t back_index;
    uint32_t dedicated;
    uint32_t context;
    uint32_t back_sync;
    uint32_t *address;
    PendingLineage lineage;
    int32_t fill_result;
    int32_t transfer_finish_result;
    int32_t queue_result;
} KnownColorInput;

static uint32_t known_order[8];
static uint32_t known_order_count;

static void check(int condition, const char *name)
{
    if (!condition) {
        fprintf(stderr, "display-lineage oracle failed: %s\n", name);
        failures++;
    }
}

static void fill_surface(uint32_t color)
{
    uint32_t x;
    uint32_t y;

    for (y = 0; y < 544u; ++y)
        for (x = 0; x < 960u; ++x)
            pixels[y][x] = color;
}

static void fill_known_rect(uint32_t color)
{
    uint32_t x;
    uint32_t y;

    for (y = KNOWN_Y; y < KNOWN_Y + KNOWN_HEIGHT; ++y)
        for (x = KNOWN_X; x < KNOWN_X + KNOWN_WIDTH; ++x)
            pixels[y][x] = color;
}

static uint32_t sparse_hash(void)
{
    uint32_t hash = 2166136261u;
    uint32_t i;

    for (i = 0; i < SAMPLE_COUNT; ++i) {
        hash ^= pixels[sample_xy[i][1]][sample_xy[i][0]];
        hash *= 16777619u;
    }
    return hash;
}

static void known_note_order(uint32_t event)
{
    known_order[known_order_count++] = event;
}

/* Exact host model of the p2 decision.  The presentation tag, not the vitaGL
 * queue sequence, selects the probe; every safety bit must pass before GXM is
 * touched.  The ordinary non-system queue or system SharedFB path always runs
 * before the one bounded event is published. */
static int model_known_color(const KnownColorInput *input,
                             KnownColorEvent *event)
{
    uint32_t count_valid;
    uint32_t back_valid;

    memset(event, 0, sizeof(*event));
    memset(known_order, 0, sizeof(known_order));
    known_order_count = 0u;
    if (input->present_index == KNOWN_PRESENT_INDEX) {
        fill_surface(KNOWN_GREEN);
        known_note_order(KNOWN_ORDER_CLEAR);
    }
    known_note_order(KNOWN_ORDER_END);
    if (input->present_index != KNOWN_PRESENT_INDEX) {
        known_note_order(input->system_app_mode
            ? KNOWN_ORDER_SHARED_FB_END : KNOWN_ORDER_QUEUE);
        return 0;
    }

    event->size = sizeof(*event);
    event->present_index = input->present_index;
    event->color = KNOWN_MAGENTA;
    event->format = KNOWN_FORMAT;
    event->x = KNOWN_X;
    event->y = KNOWN_Y;
    event->width = KNOWN_WIDTH;
    event->height = KNOWN_HEIGHT;
    event->stride_bytes = 960u * 4u;
    event->fill_result = NOT_CALLED;
    event->transfer_finish_result = NOT_CALLED;
    event->queue_result = NOT_CALLED;
    event->gate_mask = KNOWN_GATE_TAG;

    if (input->system_app_mode) {
        known_note_order(KNOWN_ORDER_SHARED_FB_END);
        known_note_order(KNOWN_ORDER_EVENT);
        return 1;
    }

    event->sequence = input->sequence;
    event->front_index = input->front_index;
    event->back_index = input->back_index;
    event->address = (uint32_t)(uintptr_t)input->address;
    event->dedicated = input->dedicated;

    count_valid = input->buffer_count > 0u &&
        input->buffer_count <= DISPLAY_SURFACE_COUNT;
    back_valid = count_valid && input->back_index < input->buffer_count &&
        input->back_index < DISPLAY_SURFACE_COUNT;
    event->gate_mask |= KNOWN_GATE_NON_SYSTEM;
    if (count_valid)
        event->gate_mask |= KNOWN_GATE_COUNT;
    if (back_valid)
        event->gate_mask |= KNOWN_GATE_BACK;
    if (back_valid && input->address)
        event->gate_mask |= KNOWN_GATE_ADDRESS;
    if (back_valid && input->dedicated)
        event->gate_mask |= KNOWN_GATE_DEDICATED;
    if (input->lineage.begin_count == 1u)
        event->gate_mask |= KNOWN_GATE_BEGIN_ONE;
    if (input->lineage.end_count == 1u)
        event->gate_mask |= KNOWN_GATE_END_ONE;
    if (input->lineage.begin_result == 0)
        event->gate_mask |= KNOWN_GATE_BEGIN_OK;
    if (input->lineage.end_result == 0)
        event->gate_mask |= KNOWN_GATE_END_OK;
    if (input->lineage.begin_context == input->context &&
            input->lineage.end_context == input->context)
        event->gate_mask |= KNOWN_GATE_CONTEXT;
    if (input->lineage.begin_color_data ==
            (uint32_t)(uintptr_t)input->address)
        event->gate_mask |= KNOWN_GATE_DATA;
    if (back_valid && input->lineage.begin_fragment_sync == input->back_sync)
        event->gate_mask |= KNOWN_GATE_SYNC;
    if (input->lineage.mismatch_mask == 0u)
        event->gate_mask |= KNOWN_GATE_MISMATCH;

    if (event->gate_mask == KNOWN_GATE_ALL) {
        known_note_order(KNOWN_ORDER_CONTEXT_FINISH);
        event->context_finish_called = 1u;
        known_note_order(KNOWN_ORDER_FILL);
        event->fill_result = input->fill_result;
        if (event->fill_result == 0)
            fill_known_rect(KNOWN_MAGENTA);
        known_note_order(KNOWN_ORDER_TRANSFER_FINISH);
        event->transfer_finish_called = 1u;
        event->transfer_finish_result = input->transfer_finish_result;
    }
    known_note_order(KNOWN_ORDER_QUEUE);
    event->queue_result = input->queue_result;
    known_note_order(KNOWN_ORDER_EVENT);
    return 1;
}

static void reset_capture(void)
{
    memset(captured, 0, sizeof(captured));
    memset(&pending, 0, sizeof(pending));
    captured_count = 0;
    dropped_count = 0;
}

static void collect(const ProbeEvent *event)
{
    if (captured_count < CAPTURE_CAPACITY)
        captured[captured_count++] = *event;
    else
        dropped_count++;
}

static uint32_t next_sequence(uint32_t *state)
{
    uint32_t sequence = ++*state;

    if (!sequence)
        sequence = ++*state;
    return sequence;
}

static void note_begin(uint32_t context, uint32_t render_target,
                       uint32_t fragment_sync, uint32_t color_surface,
                       uint32_t color_data, int32_t result)
{
    if (!pending.begin_count) {
        pending.begin_context = context;
        pending.begin_render_target = render_target;
        pending.begin_fragment_sync = fragment_sync;
        pending.begin_color_surface = color_surface;
        pending.begin_color_data = color_data;
        pending.begin_result = result;
    } else if (pending.begin_context != context ||
               pending.begin_render_target != render_target ||
               pending.begin_fragment_sync != fragment_sync ||
               pending.begin_color_surface != color_surface ||
               pending.begin_color_data != color_data ||
               pending.begin_result != result) {
        pending.mismatch_mask |= BEGIN_MISMATCH;
    }
    if (pending.begin_count != UINT32_MAX)
        pending.begin_count++;
}

static void note_end(uint32_t context, int32_t result)
{
    if (!pending.end_count) {
        pending.end_context = context;
        pending.end_result = result;
    } else if (pending.end_context != context ||
               pending.end_result != result) {
        pending.mismatch_mask |= END_MISMATCH;
    }
    if (pending.end_count != UINT32_MAX)
        pending.end_count++;
}

static void init_event(ProbeEvent *event, const CallbackData *data,
                       uint32_t stage)
{
    memset(event, 0, sizeof(*event));
    event->size = (uint32_t)sizeof(*event);
    event->stage = stage;
    event->sequence = data->sequence;
    event->front_index = data->front_index;
    event->back_index = data->back_index;
    event->address = (uint32_t)(uintptr_t)data->address;
}

static void probe_add(const CallbackData *data, uint32_t old_sync,
                      uint32_t new_sync, int32_t result,
                      int32_t memblock_uid, int32_t get_base_result,
                      int32_t map_result, uint32_t dedicated)
{
    ProbeEvent event;

    init_event(&event, data, STAGE_ADD_RESULT);
    event.detail.add.result = result;
    event.detail.add.old_sync = old_sync;
    event.detail.add.new_sync = new_sync;
    event.detail.add.begin_context = pending.begin_context;
    event.detail.add.begin_render_target = pending.begin_render_target;
    event.detail.add.begin_fragment_sync = pending.begin_fragment_sync;
    event.detail.add.begin_color_surface = pending.begin_color_surface;
    event.detail.add.begin_color_data = pending.begin_color_data;
    event.detail.add.begin_result = pending.begin_result;
    event.detail.add.begin_count = pending.begin_count;
    event.detail.add.end_context = pending.end_context;
    event.detail.add.end_result = pending.end_result;
    event.detail.add.end_count = pending.end_count;
    event.detail.add.mismatch_mask = pending.mismatch_mask;
    event.detail.add.back_memblock_uid = memblock_uid;
    event.detail.add.back_get_base_result = get_base_result;
    event.detail.add.back_map_result = map_result;
    event.detail.add.back_dedicated = dedicated;
    collect(&event);
    memset(&pending, 0, sizeof(pending));
}

static void probe_callback(const CallbackData *data,
                           const FrameBuf *framebuffer,
                           uint32_t sync, int32_t result)
{
    ProbeEvent event;
    uint32_t hash = 2166136261u;
    uint32_t i;

    if (data->sequence) {
        init_event(&event, data, STAGE_DISPLAY_CALLBACK);
        if (data->address && framebuffer->width >= 960u &&
                framebuffer->height >= 544u && framebuffer->pitch >= 960u) {
            for (i = 0; i < SAMPLE_COUNT; ++i) {
                uint32_t sample = data->address[
                    sample_xy[i][1] * framebuffer->pitch + sample_xy[i][0]];
                event.detail.display.rgba[i] = sample;
                hash ^= sample;
                hash *= 16777619u;
            }
            event.detail.display.sample_count = SAMPLE_COUNT;
            event.detail.display.sparse_hash = hash;
        }
        event.detail.display.result = result;
        event.detail.display.size = framebuffer->size;
        event.detail.display.base = (uint32_t)(uintptr_t)framebuffer->base;
        event.detail.display.pitch = framebuffer->pitch;
        event.detail.display.pixel_format = framebuffer->pixel_format;
        event.detail.display.width = framebuffer->width;
        event.detail.display.height = framebuffer->height;
        event.detail.display.sync = sync;
    }
    if (data->address && framebuffer->width >= SENTINEL_X + SENTINEL_SIZE &&
            framebuffer->height >= SENTINEL_Y + SENTINEL_SIZE &&
            framebuffer->pitch >= SENTINEL_X + SENTINEL_SIZE) {
        uint32_t x;
        uint32_t y;

        for (y = SENTINEL_Y; y < SENTINEL_Y + SENTINEL_SIZE; ++y)
            for (x = SENTINEL_X; x < SENTINEL_X + SENTINEL_SIZE; ++x)
                data->address[y * framebuffer->pitch + x] =
                    (((x >> 2u) ^ (y >> 2u)) & 1u) != 0u
                        ? SENTINEL_MAGENTA : SENTINEL_GREEN;
    }
    if (data->sequence)
        collect(&event);
}

/* Host model of the production transaction.  The mock failures prove that
 * alloc/GetBase/Map never leave a mixed dedicated/pool set behind. */
static void model_surface_transaction(
    SurfaceTransaction *state, uint32_t fail_stage, uint32_t fail_index,
    uint32_t unmap_failure_index, uint32_t free_failure_index)
{
    uint32_t i;
    int failed = 0;

    memset(state, 0, sizeof(*state));
    state->failure_index = UINT32_MAX;
    for (i = 0; i < DISPLAY_SURFACE_COUNT; ++i) {
        state->uid[i] = NOT_CALLED;
        state->get_base[i] = NOT_CALLED;
        state->map[i] = NOT_CALLED;
        state->unmap_result[i] = NOT_CALLED;
        state->free_result[i] = NOT_CALLED;
    }
    for (i = 0; i < DISPLAY_SURFACE_COUNT; ++i) {
        state->uid[i] = fail_stage == SURFACE_STAGE_ALLOC && i == fail_index
            ? (int32_t)0x8002000cu : (int32_t)(0x7000u + i);
        if (state->uid[i] < 0) {
            state->failure_stage = SURFACE_STAGE_ALLOC;
            state->failure_index = i;
            failed = 1;
            break;
        }
        state->live[i] = 1u;
        state->get_base[i] =
            fail_stage == SURFACE_STAGE_GET_BASE && i == fail_index
                ? (int32_t)0x80020005u : 0;
        if (state->get_base[i]) {
            state->failure_stage = SURFACE_STAGE_GET_BASE;
            state->failure_index = i;
            failed = 1;
            break;
        }
        state->map[i] = fail_stage == SURFACE_STAGE_MAP && i == fail_index
            ? (int32_t)0x805b0003u : 0;
        if (state->map[i]) {
            state->failure_stage = SURFACE_STAGE_MAP;
            state->failure_index = i;
            failed = 1;
            break;
        }
        state->mapped[i] = 1u;
    }
    if (failed) {
        for (i = 0; i < DISPLAY_SURFACE_COUNT; ++i) {
            if (state->mapped[i]) {
                state->unmap_calls[i]++;
                state->unmaps++;
                state->unmap_result[i] = i == unmap_failure_index
                    ? (int32_t)0x805b0003u : 0;
                if (state->unmap_result[i]) {
                    uint32_t failure = state->rollback_failure_count++;

                    state->rollback_failure_site[failure] =
                        "init-rollback:unmap-scanout";
                    state->rollback_failure_result[failure] =
                        state->unmap_result[i];
                } else {
                    state->mapped[i] = 0u;
                }
            }
            if (state->live[i] && !state->mapped[i]) {
                state->free_calls[i]++;
                state->frees++;
                state->free_result[i] = i == free_failure_index
                    ? (int32_t)0x80020005u : 0;
                if (state->free_result[i]) {
                    uint32_t failure = state->rollback_failure_count++;

                    state->rollback_failure_site[failure] =
                        "init-rollback:free-scanout";
                    state->rollback_failure_result[failure] =
                        state->free_result[i];
                } else {
                    state->live[i] = 0u;
                }
            }
            state->dedicated[i] = 0u;
            state->retired[i] = 1u;
            state->pool_allocations++;
        }
        return;
    }
    for (i = 0; i < DISPLAY_SURFACE_COUNT; ++i)
        state->dedicated[i] = 1u;
}

static void note_resize_event(uint32_t event)
{
    resize_events[resize_event_count++] = event;
}

/* Mirrors the production free gate: current scanout is detached and observed
 * before any render-target destruction, unmap, or memblock free. */
static int model_resolution_change(
    int32_t queue_result, int32_t detach_result, int32_t wait_result,
    int32_t destroy_result, uint32_t free_failure_index)
{
    uint32_t i;

    memset(resize_events, 0, sizeof(resize_events));
    resize_event_count = 0u;
    resize_failure_site = NULL;
    resize_failure_result = 0;
    resize_release_failures = 0u;
    note_resize_event(RESIZE_QUEUE_FINISH);
    if (queue_result) {
        resize_failure_site = "resize:queue-finish";
        resize_failure_result = queue_result;
        return 0;
    }
    note_resize_event(RESIZE_GXM_FINISH);
    note_resize_event(RESIZE_DETACH);
    if (detach_result) {
        resize_failure_site = "resize:detach-scanout";
        resize_failure_result = detach_result;
        return 0;
    }
    note_resize_event(RESIZE_WAIT_DETACH);
    if (wait_result) {
        resize_failure_site = "resize:wait-detach";
        resize_failure_result = wait_result;
        return 0;
    }
    note_resize_event(RESIZE_DESTROY_TARGET);
    if (destroy_result) {
        resize_failure_site = "resize:destroy-target";
        resize_failure_result = destroy_result;
        return 0;
    }
    for (i = 0; i < DISPLAY_SURFACE_COUNT; ++i) {
        note_resize_event(RESIZE_RELEASE_0 + i);
        if (i == free_failure_index) {
            resize_failure_site = "resize:free-scanout";
            resize_failure_result = (int32_t)0x80020005u;
            resize_release_failures++;
        }
    }
    note_resize_event(RESIZE_REBUILD);
    return 1;
}

static void test_success_and_exact_lineage(void)
{
    static const uint32_t words[SAMPLE_COUNT] = {
        0x01020304u, 0x11121314u, 0x21222324u, 0x31323334u,
        0x41424344u, 0x51525354u, 0x61626364u, 0x71727374u
    };
    CallbackData data = { &pixels[0][0], 37u, 2u, 1u };
    FrameBuf framebuffer = {
        24u, &pixels[0][0], 960u, 0u, 960u, 544u
    };
    uint32_t i;

    memset(pixels, 0, sizeof(pixels));
    for (i = 0; i < SAMPLE_COUNT; ++i)
        pixels[sample_xy[i][1]][sample_xy[i][0]] = words[i];
    reset_capture();
    note_begin(0x81001000u, 0x82002000u, 0x83003000u,
               0x84004000u, (uint32_t)(uintptr_t)data.address, 0);
    note_end(0x81001000u, 0);
    probe_add(&data, 0x83002000u, 0x83003000u, 0,
              0x7001, 0, 0, 1u);
    probe_callback(&data, &framebuffer, 1u, 0);

    check(captured_count == 2, "success event count");
    check(captured[0].size == 96u &&
          captured[0].stage == STAGE_ADD_RESULT, "success ADD first");
    check(captured[0].detail.add.result == 0 &&
          captured[0].detail.add.old_sync == 0x83002000u &&
          captured[0].detail.add.new_sync == 0x83003000u,
          "queue exact arguments and result");
    check(captured[0].detail.add.begin_count == 1u &&
          captured[0].detail.add.begin_context == 0x81001000u &&
          captured[0].detail.add.begin_render_target == 0x82002000u &&
          captured[0].detail.add.begin_fragment_sync == 0x83003000u &&
          captured[0].detail.add.begin_color_surface == 0x84004000u &&
          captured[0].detail.add.begin_color_data ==
              (uint32_t)(uintptr_t)data.address &&
          captured[0].detail.add.begin_result == 0,
          "begin exact arguments and result");
    check(captured[0].detail.add.end_count == 1u &&
          captured[0].detail.add.end_context == 0x81001000u &&
          captured[0].detail.add.end_result == 0 &&
          captured[0].detail.add.mismatch_mask == 0u,
          "end exact context and result");
    check(captured[0].detail.add.back_memblock_uid == 0x7001 &&
          captured[0].detail.add.back_get_base_result == 0 &&
          captured[0].detail.add.back_map_result == 0 &&
          captured[0].detail.add.back_dedicated == 1u,
          "dedicated display allocation lineage");
    check(captured[1].stage == STAGE_DISPLAY_CALLBACK &&
          captured[1].sequence == 37u &&
          captured[1].address == (uint32_t)(uintptr_t)data.address,
          "callback copied sequence and address");
    check(captured[1].detail.display.result == 0 &&
          captured[1].detail.display.size == 24u &&
          captured[1].detail.display.base ==
              (uint32_t)(uintptr_t)framebuffer.base &&
          captured[1].detail.display.pitch == 960u &&
          captured[1].detail.display.pixel_format == 0u &&
          captured[1].detail.display.width == 960u &&
          captured[1].detail.display.height == 544u &&
          captured[1].detail.display.sync == 1u,
          "SetFrameBuf exact arguments and result");
    check(captured[1].detail.display.sample_count == SAMPLE_COUNT &&
          captured[1].detail.display.sparse_hash == 0xcbf12dc5u,
          "callback bounded samples");
    for (i = 0; i < SAMPLE_COUNT; ++i)
        check(captured[1].detail.display.rgba[i] == words[i],
              "callback raw RGBA word");
    check(pixels[SENTINEL_Y][SENTINEL_X] == SENTINEL_GREEN &&
          pixels[SENTINEL_Y][SENTINEL_X + 4u] == SENTINEL_MAGENTA &&
          pixels[SENTINEL_Y + SENTINEL_SIZE][SENTINEL_X] == 0u,
          "post-sample pre-display sentinel is fixed and bounded");
}

static void test_mismatch_and_reset(void)
{
    CallbackData data = { &pixels[0][0], 41u, 0u, 1u };

    reset_capture();
    note_begin(1u, 2u, 3u, 4u, 5u, 0);
    note_begin(1u, 2u, 30u, 4u, 5u, -1);
    note_end(1u, 0);
    note_end(10u, -2);
    probe_add(&data, 8u, 3u, 0, 0x7000, 0, 0, 1u);
    probe_add(&data, 9u, 3u, 0, 0x7000, 0, 0, 1u);
    check(captured[0].detail.add.begin_count == 2u &&
          captured[0].detail.add.end_count == 2u &&
          captured[0].detail.add.mismatch_mask ==
              (BEGIN_MISMATCH | END_MISMATCH),
          "multiple display scenes marked non-exact");
    check(captured[1].detail.add.begin_count == 0u &&
          captured[1].detail.add.end_count == 0u &&
          captured[1].detail.add.mismatch_mask == 0u,
          "pending lineage reset only after queue publication");
}

static void test_callback_can_precede_return(void)
{
    CallbackData data = { &pixels[0][0], 43u, 0u, 1u };
    FrameBuf framebuffer = {
        24u, &pixels[0][0], 960u, 0u, 960u, 544u
    };

    reset_capture();
    probe_callback(&data, &framebuffer, 1u, (int32_t)0x80290002u);
    probe_add(&data, 7u, 8u, 0, 0x7000, 0, 0, 1u);
    check(captured_count == 2, "early callback event count");
    check(captured[0].stage == STAGE_DISPLAY_CALLBACK &&
          captured[1].stage == STAGE_ADD_RESULT,
          "early callback ordering retained");
    check(captured[0].sequence == captured[1].sequence &&
          captured[0].detail.display.result ==
              (int32_t)0x80290002u,
          "early callback exact INVALID_ADDR result and attribution");
}

static void test_error_rotation_bounds_and_capacity(void)
{
    CallbackData data = { &pixels[0][0], 53u, 1u, 2u };
    FrameBuf framebuffer = { 24u, &pixels[0][0], 960u, 0u, 959u, 544u };
    uint32_t state = UINT32_MAX;
    uint32_t front = data.front_index;
    uint32_t back = data.back_index;
    uint32_t i;

    reset_capture();
    probe_add(&data, 11u, 12u, (int32_t)0x805b0003u,
              -1, NOT_CALLED, NOT_CALLED, 0u);
    front = back;
    back = (back + 1u) % 3u;
    check(captured_count == 1 &&
          captured[0].detail.add.result == (int32_t)0x805b0003u &&
          captured[0].detail.add.back_memblock_uid == -1 &&
          captured[0].detail.add.back_get_base_result == NOT_CALLED &&
          captured[0].detail.add.back_map_result == NOT_CALLED &&
          captured[0].detail.add.back_dedicated == 0u,
          "queue error exact result");
    check(front == 2u && back == 0u, "error preserves index rotation");

    probe_callback(&data, &framebuffer, 1u, 0);
    check(captured[1].detail.display.sample_count == 0u &&
          captured[1].detail.display.sparse_hash == 0u,
          "short surface reads no pixels");
    data.address = NULL;
    framebuffer.base = NULL;
    framebuffer.width = 960u;
    probe_callback(&data, &framebuffer, 1u, 0);
    check(captured[2].detail.display.sample_count == 0u,
          "null surface reads no pixels");
    memset(pixels, 0, sizeof(pixels));
    data.address = &pixels[0][0];
    framebuffer.base = &pixels[0][0];
    framebuffer.pitch = 960u;
    data.sequence = 0u;
    probe_callback(&data, &framebuffer, 1u, 0);
    check(captured_count == 3u &&
          pixels[SENTINEL_Y][SENTINEL_X] == SENTINEL_GREEN &&
          pixels[SENTINEL_Y][SENTINEL_X + 4u] == SENTINEL_MAGENTA,
          "untagged callback emits no event but still writes p600 sentinel");
    check(next_sequence(&state) == 1u && state == 1u,
          "sequence wrap skips sentinel zero");

    reset_capture();
    data.sequence = 1u;
    for (i = 0; i < CAPTURE_CAPACITY + 3u; ++i)
        probe_add(&data, 1u, 2u, (int32_t)i,
                  0x7000, 0, 0, 1u);
    check(captured_count == CAPTURE_CAPACITY && dropped_count == 3u,
          "collector remains bounded");
}

static void test_surface_transaction_and_resize_lifecycle(void)
{
    SurfaceTransaction state;

    model_surface_transaction(&state, SURFACE_STAGE_NONE, UINT32_MAX,
                              UINT32_MAX, UINT32_MAX);
    check(state.failure_stage == SURFACE_STAGE_NONE &&
          state.failure_index == UINT32_MAX &&
          state.pool_allocations == 0u && state.unmaps == 0u &&
          state.frees == 0u && state.dedicated[0] == 1u &&
          state.dedicated[1] == 1u && state.dedicated[2] == 1u,
          "dedicated allocation succeeds as one three-buffer transaction");

    model_surface_transaction(&state, SURFACE_STAGE_ALLOC, 1u,
                              UINT32_MAX, UINT32_MAX);
    check(state.failure_stage == SURFACE_STAGE_ALLOC &&
          state.failure_index == 1u && state.unmaps == 1u &&
          state.frees == 1u && state.pool_allocations == 3u &&
          state.live[0] == 0u && state.live[1] == 0u &&
          state.dedicated[0] == 0u && state.dedicated[1] == 0u &&
          state.dedicated[2] == 0u,
          "alloc failure rolls back prior mapped block and falls back all-pool");

    model_surface_transaction(&state, SURFACE_STAGE_GET_BASE, 1u,
                              UINT32_MAX, UINT32_MAX);
    check(state.failure_stage == SURFACE_STAGE_GET_BASE &&
          state.failure_index == 1u && state.unmaps == 1u &&
          state.frees == 2u && state.pool_allocations == 3u &&
          state.live[0] == 0u && state.live[1] == 0u &&
          state.mapped[0] == 0u && state.mapped[1] == 0u,
          "GetBase failure frees failing UID and unmaps prior block");

    model_surface_transaction(&state, SURFACE_STAGE_MAP, 1u,
                              UINT32_MAX, UINT32_MAX);
    check(state.failure_stage == SURFACE_STAGE_MAP &&
          state.failure_index == 1u && state.unmaps == 1u &&
          state.frees == 2u && state.pool_allocations == 3u &&
          state.live[0] == 0u && state.live[1] == 0u &&
          state.mapped[0] == 0u && state.mapped[1] == 0u,
          "Map failure never unmaps failed map and rolls back prior block");

    model_surface_transaction(&state, SURFACE_STAGE_ALLOC, 2u, 0u,
                              UINT32_MAX);
    check(state.unmaps == 2u && state.frees == 1u &&
          state.unmap_calls[0] == 1u && state.free_calls[0] == 0u &&
          state.live[0] == 1u && state.mapped[0] == 1u &&
          state.retired[0] == 1u && state.dedicated[0] == 0u &&
          state.unmap_calls[1] == 1u && state.free_calls[1] == 1u &&
          state.live[1] == 0u && state.mapped[1] == 0u &&
          state.retired[1] == 1u && state.pool_allocations == 3u &&
          state.rollback_failure_count == 1u &&
          strcmp(state.rollback_failure_site[0],
                 "init-rollback:unmap-scanout") == 0 &&
          state.rollback_failure_result[0] == (int32_t)0x805b0003u,
          "failed rollback unmap is logged, never freed, retired, and later slot cleans");

    model_surface_transaction(&state, SURFACE_STAGE_ALLOC, 2u,
                              UINT32_MAX, 0u);
    check(state.unmaps == 2u && state.frees == 2u &&
          state.unmap_result[0] == 0 && state.free_calls[0] == 1u &&
          state.free_result[0] == (int32_t)0x80020005u &&
          state.live[0] == 1u && state.mapped[0] == 0u &&
          state.retired[0] == 1u && state.live[1] == 0u &&
          state.rollback_failure_count == 1u &&
          strcmp(state.rollback_failure_site[0],
                 "init-rollback:free-scanout") == 0 &&
          state.rollback_failure_result[0] == (int32_t)0x80020005u,
          "failed rollback free is captured and retired while later slot cleans");

    check(model_resolution_change(0, 0, 0, 0, UINT32_MAX) &&
          resize_event_count == 9u &&
          resize_events[0] == RESIZE_QUEUE_FINISH &&
          resize_events[1] == RESIZE_GXM_FINISH &&
          resize_events[2] == RESIZE_DETACH &&
          resize_events[3] == RESIZE_WAIT_DETACH &&
          resize_events[4] == RESIZE_DESTROY_TARGET &&
          resize_events[5] == RESIZE_RELEASE_0 &&
          resize_events[7] == RESIZE_RELEASE_2 &&
          resize_events[8] == RESIZE_REBUILD,
          "resize detaches and waits before destroy/release");
    check(!model_resolution_change(
              (int32_t)0x805b0003u, 0, 0, 0, UINT32_MAX) &&
          resize_event_count == 1u &&
          strcmp(resize_failure_site, "resize:queue-finish") == 0 &&
          resize_failure_result == (int32_t)0x805b0003u,
          "queue-finish failure retains every old surface");
    check(!model_resolution_change(
              0, (int32_t)0x80290002u, 0, 0, UINT32_MAX) &&
          resize_event_count == 3u &&
          strcmp(resize_failure_site, "resize:detach-scanout") == 0 &&
          resize_failure_result == (int32_t)0x80290002u,
          "detach failure retains every old surface");
    check(!model_resolution_change(
              0, 0, (int32_t)0x80290001u, 0, UINT32_MAX) &&
          resize_event_count == 4u &&
          strcmp(resize_failure_site, "resize:wait-detach") == 0 &&
          resize_failure_result == (int32_t)0x80290001u,
          "wait failure retains every old surface");
    check(!model_resolution_change(
              0, 0, 0, (int32_t)0x805b0003u, UINT32_MAX) &&
          resize_event_count == 5u &&
          strcmp(resize_failure_site, "resize:destroy-target") == 0,
          "destroy failure occurs before and prevents every release");
    check(model_resolution_change(0, 0, 0, 0, 1u) &&
          resize_release_failures == 1u &&
          resize_event_count == 9u &&
          resize_events[5] == RESIZE_RELEASE_0 &&
          resize_events[6] == RESIZE_RELEASE_1 &&
          resize_events[7] == RESIZE_RELEASE_2 &&
          resize_events[8] == RESIZE_REBUILD &&
          strcmp(resize_failure_site, "resize:free-scanout") == 0,
          "post-detach partial free failure leaks then releases rest and rebuilds");
}

static void test_known_color_p2_matrix(void)
{
    static const uint32_t expected_samples[SAMPLE_COUNT] = {
        KNOWN_GREEN, KNOWN_GREEN, KNOWN_GREEN, KNOWN_GREEN,
        KNOWN_MAGENTA, KNOWN_GREEN, KNOWN_MAGENTA, KNOWN_GREEN
    };
    KnownColorInput input;
    KnownColorEvent event;
    CallbackData callback = { &pixels[0][0], 0xdeadbeefu, 1u, 2u };
    FrameBuf framebuffer = {
        24u, &pixels[0][0], 960u, 0u, 960u, 544u
    };
    uint32_t i;

    memset(&input, 0, sizeof(input));
    input.present_index = KNOWN_PRESENT_INDEX;
    input.sequence = callback.sequence;
    input.buffer_count = DISPLAY_SURFACE_COUNT;
    input.front_index = callback.front_index;
    input.back_index = callback.back_index;
    input.dedicated = 1u;
    input.context = 0x81001000u;
    input.back_sync = 0x83003000u;
    input.address = callback.address;
    input.lineage.begin_context = input.context;
    input.lineage.begin_fragment_sync = input.back_sync;
    input.lineage.begin_color_data =
        (uint32_t)(uintptr_t)input.address;
    input.lineage.begin_count = 1u;
    input.lineage.end_context = input.context;
    input.lineage.end_count = 1u;

    fill_surface(0u);
    check(model_known_color(&input, &event),
          "p2 tag emits one known-color event independent of queue sequence");
    check(event.size == 88u && event.present_index == KNOWN_PRESENT_INDEX &&
          event.sequence == 0xdeadbeefu && event.front_index == 1u &&
          event.back_index == 2u && event.address ==
              (uint32_t)(uintptr_t)input.address &&
          event.dedicated == 1u && event.color == KNOWN_MAGENTA &&
          event.format == KNOWN_FORMAT && event.x == KNOWN_X &&
          event.y == KNOWN_Y && event.width == KNOWN_WIDTH &&
          event.height == KNOWN_HEIGHT && event.stride_bytes == 960u * 4u &&
          event.sync_object == 0u && event.sync_flags == 0u &&
          event.gate_mask == KNOWN_GATE_ALL,
          "p2 event preserves exact RAW fill arguments and full safety gate");
    check(event.context_finish_called == 1u && event.fill_result == 0 &&
          event.transfer_finish_called == 1u &&
          event.transfer_finish_result == 0 && event.queue_result == 0,
          "p2 successful call results are exact");
    check(known_order_count == 7u &&
          known_order[0] == KNOWN_ORDER_CLEAR &&
          known_order[1] == KNOWN_ORDER_END &&
          known_order[2] == KNOWN_ORDER_CONTEXT_FINISH &&
          known_order[3] == KNOWN_ORDER_FILL &&
          known_order[4] == KNOWN_ORDER_TRANSFER_FINISH &&
          known_order[5] == KNOWN_ORDER_QUEUE &&
          known_order[6] == KNOWN_ORDER_EVENT,
          "p2 order is clear -> scene end -> finish -> fill -> transfer finish -> queue -> event");
    check(input.lineage.begin_count == 1u &&
          input.lineage.end_count == 1u,
          "p2 clear adds no scene and preserves one Begin plus one End");
    check(sparse_hash() == 0xd1e9c80du,
          "green plus magenta exact sparse hash");

    reset_capture();
    probe_callback(&callback, &framebuffer, 1u, 0);
    check(captured_count == 1u &&
          captured[0].detail.display.sparse_hash == 0xd1e9c80du &&
          captured[0].detail.display.sample_count == SAMPLE_COUNT,
          "callback sees known-color surface before CPU marker");
    for (i = 0; i < SAMPLE_COUNT; ++i)
        check(captured[0].detail.display.rgba[i] == expected_samples[i],
              "p2 sample vector is G,G,G,G,M,G,M,G");

    fill_surface(0u);
    check(sparse_hash() == 0x9be17165u, "all-black oracle hash");
    fill_known_rect(KNOWN_MAGENTA);
    check(sparse_hash() == 0x8c06c40du,
          "black plus magenta oracle hash");
    fill_surface(KNOWN_GREEN);
    check(sparse_hash() == 0xc53cf965u, "all-green oracle hash");

    input.system_app_mode = 1u;
    check(model_known_color(&input, &event) &&
          event.size == 88u && event.present_index == KNOWN_PRESENT_INDEX &&
          event.sequence == 0u && event.front_index == 0u &&
          event.back_index == 0u && event.address == 0u &&
          event.dedicated == 0u &&
          event.gate_mask == KNOWN_GATE_TAG &&
          (event.gate_mask & KNOWN_GATE_NON_SYSTEM) == 0u &&
          event.context_finish_called == 0u &&
          event.fill_result == NOT_CALLED &&
          event.transfer_finish_called == 0u &&
          event.transfer_finish_result == NOT_CALLED &&
          event.queue_result == NOT_CALLED &&
          known_order_count == 4u &&
          known_order[0] == KNOWN_ORDER_CLEAR &&
          known_order[1] == KNOWN_ORDER_END &&
          known_order[2] == KNOWN_ORDER_SHARED_FB_END &&
          known_order[3] == KNOWN_ORDER_EVENT,
          "system p2 publishes one explicit skip after unchanged SharedFB path");
    input.system_app_mode = 0u;

    input.lineage.begin_count = 2u;
    fill_surface(0x11223344u);
    check(model_known_color(&input, &event) &&
          event.gate_mask != KNOWN_GATE_ALL &&
          event.context_finish_called == 0u &&
          event.fill_result == NOT_CALLED &&
          event.transfer_finish_called == 0u &&
          event.transfer_finish_result == NOT_CALLED &&
          event.queue_result == 0 &&
          known_order_count == 4u &&
          known_order[0] == KNOWN_ORDER_CLEAR &&
          known_order[1] == KNOWN_ORDER_END &&
          known_order[2] == KNOWN_ORDER_QUEUE &&
          known_order[3] == KNOWN_ORDER_EVENT,
          "invalid p2 lineage is explicit and leaves ordinary queue untouched");

    input.lineage.begin_count = 1u;
    input.fill_result = (int32_t)0x805b0003u;
    input.transfer_finish_result = (int32_t)0x805b0004u;
    check(model_known_color(&input, &event) &&
          event.fill_result == (int32_t)0x805b0003u &&
          event.transfer_finish_called == 1u &&
          event.transfer_finish_result == (int32_t)0x805b0004u &&
          known_order_count == 7u &&
          known_order[4] == KNOWN_ORDER_TRANSFER_FINISH &&
          known_order[5] == KNOWN_ORDER_QUEUE &&
          known_order[6] == KNOWN_ORDER_EVENT,
          "fill failure still has exactly one transfer finish and one queue");

    input.present_index = 3u;
    input.sequence = KNOWN_PRESENT_INDEX;
    fill_surface(0x55667788u);
    check(!model_known_color(&input, &event) && event.size == 0u &&
          sparse_hash() == 0xa5fdd865u && known_order_count == 2u &&
          known_order[0] == KNOWN_ORDER_END &&
          known_order[1] == KNOWN_ORDER_QUEUE,
          "p3 is untouched even when its queue sequence equals two");
    input.present_index = 4u;
    check(!model_known_color(&input, &event) && event.size == 0u &&
          sparse_hash() == 0xa5fdd865u,
          "p4 is untouched");
}

int main(void)
{
    test_success_and_exact_lineage();
    test_mismatch_and_reset();
    test_callback_can_precede_return();
    test_error_rotation_bounds_and_capacity();
    test_surface_transaction_and_resize_lifecycle();
    test_known_color_p2_matrix();
    if (failures)
        return 1;
    puts("display-lineage oracle: PASS");
    return 0;
}
