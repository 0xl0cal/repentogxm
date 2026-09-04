/* Executable contract oracle for the pinned mode-1 render-target recovery.
 * The production source is separately compiled and source-censused by
 * test.sh; this model makes the allowed state transitions explicit. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define OUT_OF_RENDER_TARGETS ((int32_t)0x805b0027u)
#define OUT_OF_MEMORY        ((int32_t)0x805b0002u)
#define INVALID_POINTER       ((int32_t)0x805b0004u)
#define BUCKETS 4u
#define BUCKET_CAPACITY 3u
#define SLOT_COUNT 2u
#define MAX_REFS 8u
#define SCENES_PER_RT 8u
#define RESERVED_WIDTH 1024u
#define RESERVED_HEIGHT 1024u

typedef struct Target { uint32_t id; } Target;
typedef struct Slot {
    Target *target;
    uint32_t refs;
    uint32_t width;
    uint32_t height;
    uint32_t max_refs;
} Slot;
typedef struct Event {
    int32_t first_result, retry_result, destroy_result;
    Target *first_target, *retry_target;
    uint32_t finish_called, retried, pool_full, duplicate, invariant;
    uint32_t pending[BUCKETS], drained, recovered;
} Event;
typedef struct Driver {
    int32_t create_result[4];
    Target *create_target[4];
    uint32_t create_scenes[4];
    uint32_t create_index;
    int32_t destroy_result[BUCKETS * BUCKET_CAPACITY];
    uint32_t destroy_index;
    char calls[32];
    uint32_t call_count;
} Driver;
typedef struct State {
    Slot slots[SLOT_COUNT];
    Target *pending[BUCKETS][BUCKET_CAPACITY];
    uint32_t current_bucket;
    uint32_t current_count;
    uint32_t begin_calls;
    uint32_t needs_end_scene;
    uint32_t needs_scene_reset;
    uint32_t dirty_framebuffer;
    Target *reserved;
    uint32_t reserve_attempted;
    Driver driver;
} State;

#define CHECK(c) do { if (!(c)) { \
    fprintf(stderr, "RT recovery oracle failed at %d: %s\n", __LINE__, #c); \
    return 1; \
} } while (0)

static void record(Driver *driver, char call)
{
    driver->calls[driver->call_count++] = call;
    driver->calls[driver->call_count] = '\0';
}

static int32_t create_target(
    Driver *driver, Target **target, uint32_t scenes_per_frame)
{
    uint32_t index = driver->create_index++;
    record(driver, 'C');
    driver->create_scenes[index] = scenes_per_frame;
    *target = driver->create_target[index];
    return driver->create_result[index];
}

static void reserve_target_once(State *state, Event *event)
{
    Target *target = NULL;

    memset(event, 0, sizeof *event);
    if (state->reserve_attempted)
        return;
    state->reserve_attempted = 1u;
    event->first_result = create_target(
        &state->driver, &target, SCENES_PER_RT);
    event->first_target = target;
    event->recovered = 1u; /* A failed reserve retains the late path. */
    if (!event->first_result && target)
        state->reserved = target;
}

static uint32_t count_pending(State *state, Event *event)
{
    uint32_t bucket, index, total = 0u;
    for (bucket = 0u; bucket < BUCKETS; ++bucket) {
        for (index = 0u; index < BUCKET_CAPACITY; ++index) {
            if (state->pending[bucket][index]) {
                ++event->pending[bucket];
                ++total;
            }
        }
    }
    return total;
}

static int has_duplicate(State *state)
{
    uint32_t a, b, c, d;
    for (a = 0u; a < BUCKETS; ++a)
        for (b = 0u; b < BUCKET_CAPACITY; ++b)
            if (state->pending[a][b])
                for (c = a; c < BUCKETS; ++c)
                    for (d = c == a ? b + 1u : 0u;
                         d < BUCKET_CAPACITY; ++d)
                        if (state->pending[c][d] == state->pending[a][b])
                            return 1;
    return 0;
}

static void drain(State *state, Event *event)
{
    uint32_t bucket, read_index;
    Driver *driver = &state->driver;

    record(driver, 'F');
    event->finish_called = 1u;
    for (bucket = 0u; bucket < BUCKETS; ++bucket) {
        uint32_t write_index = 0u;
        for (read_index = 0u; read_index < BUCKET_CAPACITY; ++read_index) {
            Target *target = state->pending[bucket][read_index];
            int32_t result;
            if (!target)
                continue;
            record(driver, 'D');
            result = driver->destroy_result[driver->destroy_index++];
            if (result) {
                if (!event->destroy_result)
                    event->destroy_result = result;
                state->pending[bucket][write_index++] = target;
            } else {
                ++event->drained;
            }
        }
        while (write_index < BUCKET_CAPACITY)
            state->pending[bucket][write_index++] = NULL;
        if (bucket == state->current_bucket) {
            uint32_t count = 0u;
            while (count < BUCKET_CAPACITY && state->pending[bucket][count])
                ++count;
            state->current_count = count;
        }
    }
}

static Slot *acquire(State *state, uint32_t width, uint32_t height, Event *event)
{
    uint32_t compatible = SLOT_COUNT, index;

    memset(event, 0, sizeof *event);

    /* A complete first pass validates the pool and finds reusable live state;
     * an earlier empty slot must never hide a later compatible target. */
    for (index = 0u; index < SLOT_COUNT; ++index) {
        Slot *slot = &state->slots[index];
        if (slot->target) {
            if (!slot->refs || !slot->max_refs ||
                slot->refs > slot->max_refs) {
                event->invariant = 1u;
                return NULL;
            }
            if (compatible == SLOT_COUNT && slot->width == width &&
                slot->height == height && slot->refs < slot->max_refs)
                compatible = index;
        } else if (slot->refs || slot->width || slot->height ||
                   slot->max_refs) {
            event->invariant = 1u;
            return NULL;
        }
    }
    if (compatible != SLOT_COUNT) {
        ++state->slots[compatible].refs;
        return &state->slots[compatible];
    }

    for (index = 0u; index < SLOT_COUNT; ++index) {
        Slot *slot = &state->slots[index];
        Target *target = NULL;
        uint32_t max_refs = width > 256u ? 1u : MAX_REFS;
        if (slot->target)
            continue;
        if (state->reserved && width == RESERVED_WIDTH &&
            height == RESERVED_HEIGHT) {
            target = state->reserved;
            slot->target = target;
            slot->refs = 1u;
            slot->width = width;
            slot->height = height;
            slot->max_refs = max_refs;
            state->reserved = NULL;
            event->first_target = target;
            event->recovered = 1u;
            return slot;
        }
        event->first_result = create_target(
            &state->driver, &target, SCENES_PER_RT);
        event->first_target = target;
        if (!event->first_result && target) {
            slot->target = target;
            slot->refs = 1u;
            slot->width = width;
            slot->height = height;
            slot->max_refs = max_refs;
            return slot;
        }
        if (event->first_result != OUT_OF_RENDER_TARGETS)
            return NULL;
        if (!count_pending(state, event))
            return NULL;
        if (has_duplicate(state)) {
            event->duplicate = 1u;
            return NULL;
        }
        drain(state, event);
        if (event->destroy_result)
            return NULL;
        target = NULL;
        event->retried = 1u;
        event->retry_result = create_target(
            &state->driver, &target, SCENES_PER_RT);
        event->retry_target = target;
        if (event->retry_result || !target)
            return NULL;
        slot->target = target;
        slot->refs = 1u;
        slot->width = width;
        slot->height = height;
        slot->max_refs = max_refs;
        event->recovered = 1u;
        return slot;
    }
    event->pool_full = 1u;
    return NULL;
}

static int scene_attempt(State *state, Event *event)
{
    Slot *slot = acquire(state, 128u, 128u, event);
    if (!slot || !slot->target) {
        state->needs_end_scene = 0u;
        state->needs_scene_reset = 1u;
        state->dirty_framebuffer = 1u;
        return 0;
    }
    ++state->begin_calls;
    return 1;
}

int main(void)
{
    Target a = { 1u }, b = { 2u }, c = { 3u };
    State state;
    Event event;

    /* Early failure is explicitly nonfatal and the exact late request still
     * performs its own create. */
    memset(&state, 0, sizeof state);
    state.driver.create_result[0] = OUT_OF_MEMORY;
    reserve_target_once(&state, &event);
    CHECK(state.reserve_attempted && !state.reserved && event.recovered);
    CHECK(event.first_result == OUT_OF_MEMORY &&
          strcmp(state.driver.calls, "C") == 0);
    CHECK(state.driver.create_scenes[0] == SCENES_PER_RT);
    state.driver.create_result[1] = 0;
    state.driver.create_target[1] = &b;
    CHECK(acquire(&state, RESERVED_WIDTH, RESERVED_HEIGHT, &event));
    CHECK(state.slots[0].target == &b &&
          strcmp(state.driver.calls, "CC") == 0);
    CHECK(state.slots[0].max_refs == 1u &&
          state.driver.create_scenes[1] == SCENES_PER_RT);

    /* A successful reserve is consumed exactly once without a second driver
     * call.  The consumed pointer is null before control returns. */
    memset(&state, 0, sizeof state);
    state.driver.create_target[0] = &a;
    reserve_target_once(&state, &event);
    reserve_target_once(&state, &event);
    CHECK(state.reserved == &a && state.driver.create_index == 1u);
    CHECK(state.driver.create_scenes[0] == SCENES_PER_RT);
    CHECK(acquire(&state, RESERVED_WIDTH, RESERVED_HEIGHT, &event));
    CHECK(state.slots[0].target == &a && !state.reserved && event.recovered);
    CHECK(event.first_target == &a && state.driver.create_index == 1u &&
          strcmp(state.driver.calls, "C") == 0);

    /* A mismatched request cannot steal the reserve.  If it remains unused,
     * it is deliberately process-owned just like vitaGL's display target. */
    memset(&state, 0, sizeof state);
    state.driver.create_target[0] = &a;
    state.driver.create_target[1] = &b;
    reserve_target_once(&state, &event);
    CHECK(acquire(&state, 128u, 128u, &event));
    CHECK(state.slots[0].target == &b && state.reserved == &a &&
          state.driver.create_index == 2u);
    CHECK(acquire(&state, RESERVED_WIDTH, RESERVED_HEIGHT, &event));
    CHECK(state.slots[1].target == &a && !state.reserved &&
          state.driver.create_index == 2u);

    memset(&state, 0, sizeof state);
    state.needs_end_scene = 1u;
    state.driver.create_result[0] = INVALID_POINTER;
    state.driver.create_target[0] = &a;
    CHECK(!scene_attempt(&state, &event));
    CHECK(!state.slots[0].target && !state.slots[0].refs);
    CHECK(!state.begin_calls && strcmp(state.driver.calls, "C") == 0);
    CHECK(!state.needs_end_scene && state.needs_scene_reset &&
          state.dirty_framebuffer);
    CHECK(event.first_result == INVALID_POINTER && event.first_target == &a);

    memset(&state, 0, sizeof state);
    state.driver.create_result[0] = OUT_OF_RENDER_TARGETS;
    CHECK(!scene_attempt(&state, &event));
    CHECK(strcmp(state.driver.calls, "C") == 0 && !event.retried);

    memset(&state, 0, sizeof state);
    state.driver.create_result[0] = OUT_OF_RENDER_TARGETS;
    state.driver.create_result[1] = 0;
    state.driver.create_target[1] = &c;
    state.pending[0][0] = &a;
    state.pending[2][0] = &b;
    CHECK(scene_attempt(&state, &event));
    CHECK(strcmp(state.driver.calls, "CFDDC") == 0);
    CHECK(event.pending[0] == 1u && event.pending[2] == 1u);
    CHECK(event.finish_called && event.drained == 2u &&
          event.retried && event.recovered);
    CHECK(event.first_result == OUT_OF_RENDER_TARGETS &&
          event.retry_result == 0 && event.retry_target == &c);
    CHECK(state.slots[0].target == &c && state.slots[0].refs == 1u);
    CHECK(state.begin_calls == 1u);

    memset(&state, 0, sizeof state);
    state.driver.create_result[0] = OUT_OF_RENDER_TARGETS;
    state.driver.destroy_result[0] = INVALID_POINTER;
    state.pending[0][0] = &a;
    CHECK(!scene_attempt(&state, &event));
    CHECK(strcmp(state.driver.calls, "CFD") == 0 && !event.retried);
    CHECK(event.destroy_result == INVALID_POINTER &&
          state.pending[0][0] == &a && state.current_count == 1u);

    memset(&state, 0, sizeof state);
    state.driver.create_result[0] = OUT_OF_RENDER_TARGETS;
    state.driver.create_result[1] = INVALID_POINTER;
    state.driver.create_target[1] = &c;
    state.pending[1][0] = &a;
    CHECK(!scene_attempt(&state, &event));
    CHECK(strcmp(state.driver.calls, "CFDC") == 0 && event.retried);
    CHECK(event.retry_result == INVALID_POINTER && event.retry_target == &c);
    CHECK(!state.slots[0].target && !state.begin_calls);

    memset(&state, 0, sizeof state);
    state.driver.create_result[0] = OUT_OF_RENDER_TARGETS;
    state.pending[0][0] = &a;
    state.pending[3][0] = &a;
    CHECK(!scene_attempt(&state, &event));
    CHECK(event.duplicate && strcmp(state.driver.calls, "C") == 0);

    /* The OUTRT response is armed but must never be consumed: the complete
     * reuse pass finds slot 1 even though canonical slot 0 is empty. */
    memset(&state, 0, sizeof state);
    state.driver.create_result[0] = OUT_OF_RENDER_TARGETS;
    state.slots[1].target = &b;
    state.slots[1].refs = 1u;
    state.slots[1].width = 128u;
    state.slots[1].height = 128u;
    state.slots[1].max_refs = MAX_REFS;
    CHECK(scene_attempt(&state, &event));
    CHECK(state.driver.call_count == 0u && state.driver.create_index == 0u);
    CHECK(state.slots[1].refs == 2u && state.begin_calls == 1u);

    memset(&state, 0, sizeof state);
    state.slots[0].refs = 1u;
    CHECK(!scene_attempt(&state, &event));
    CHECK(event.invariant && state.driver.call_count == 0u &&
          !state.begin_calls);

    memset(&state, 0, sizeof state);
    state.slots[0].target = &a;
    state.slots[0].refs = 1u;
    state.slots[0].max_refs = 1u;
    state.slots[1].target = &b;
    state.slots[1].refs = 1u;
    state.slots[1].max_refs = 1u;
    CHECK(!scene_attempt(&state, &event));
    CHECK(event.pool_full && state.driver.call_count == 0u);
    CHECK(!state.begin_calls && state.needs_scene_reset &&
          state.dirty_framebuffer);

    puts("vitaGL mode-1 render-target recovery contract oracle: PASS");
    return 0;
}
