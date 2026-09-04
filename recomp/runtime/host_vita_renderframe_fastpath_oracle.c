#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host_vita_renderframe_fastpath.h"

static const uint32_t EXPECTED_VFTABLE = 0x98608360U;
static const uint32_t EXPECTED_OBSERVER = 0x9856d8c0U;
static const uint32_t IMAGE = 0x8a123400U;
static const uint32_t COUNTER = 0x8a567800U;

static void require(int condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "RenderFrame fast-path oracle: %s\n", message);
        exit(1);
    }
}

static isaac_vita_renderframe_snapshot valid_snapshot(void)
{
    isaac_vita_renderframe_snapshot snapshot = {0};
    snapshot.image = IMAGE;
    snapshot.counter = COUNTER;
    snapshot.counter_vftable = EXPECTED_VFTABLE;
    snapshot.counter_owned_image = IMAGE;
    snapshot.strong_count = 2U;
    snapshot.release_observer = EXPECTED_OBSERVER;
    snapshot.desired_min_filter = 1U;
    snapshot.actual_min_filter = 1U;
    snapshot.desired_mag_filter = 2U;
    snapshot.actual_mag_filter = 2U;
    snapshot.desired_wrap_s = 3U;
    snapshot.actual_wrap_s = 3U;
    snapshot.desired_wrap_t = 4U;
    snapshot.actual_wrap_t = 4U;
    return snapshot;
}

static void expect_accept(
    const isaac_vita_renderframe_snapshot *snapshot,
    uint32_t expected_image,
    const char *message)
{
    isaac_vita_renderframe_snapshot before = *snapshot;
    uint32_t image = 0xdeadbeefU;
    require(isaac_vita_renderframe_snapshot_is_borrowable(
                snapshot, EXPECTED_VFTABLE, EXPECTED_OBSERVER, &image) == 1,
            message);
    require(image == expected_image, "accepted image changed");
    require(memcmp(&before, snapshot, sizeof(before)) == 0,
            "policy mutated its input");
}

static void expect_reject(
    const isaac_vita_renderframe_snapshot *snapshot,
    uint32_t expected_vftable,
    uint32_t expected_observer,
    const char *message)
{
    isaac_vita_renderframe_snapshot before = *snapshot;
    uint32_t image = 0xdeadbeefU;
    require(isaac_vita_renderframe_snapshot_is_borrowable(
                snapshot, expected_vftable, expected_observer, &image) == 0,
            message);
    require(image == 0U, "rejection leaked a borrowed image");
    require(memcmp(&before, snapshot, sizeof(before)) == 0,
            "rejection mutated its input");
}

int main(void)
{
    isaac_vita_renderframe_snapshot snapshot = valid_snapshot();

    expect_accept(&snapshot, IMAGE, "valid owned image rejected");
    snapshot.strong_count = UINT16_MAX - 1U;
    expect_accept(&snapshot, IMAGE, "largest non-edge strong count rejected");
    snapshot = valid_snapshot();
    snapshot.release_observer = 0U;
    snapshot.strong_count = 1U;
    expect_accept(&snapshot, IMAGE, "disabled observer edge rejected");

    snapshot = valid_snapshot();
    snapshot.image = 0U;
    snapshot.counter = 0U;
    expect_accept(&snapshot, 0U, "exact empty SmartPointer rejected");
    snapshot.counter = COUNTER;
    expect_reject(&snapshot, EXPECTED_VFTABLE, EXPECTED_OBSERVER,
                  "image-less live counter accepted");

#define REJECT_MUTATION(field, value, message) do { \
        snapshot = valid_snapshot(); \
        snapshot.field = (value); \
        expect_reject(&snapshot, EXPECTED_VFTABLE, EXPECTED_OBSERVER, \
                      (message)); \
    } while (0)
    REJECT_MUTATION(counter, 0U, "NULL counter accepted");
    REJECT_MUTATION(counter_vftable, EXPECTED_VFTABLE + 4U,
                    "foreign counter vftable accepted");
    REJECT_MUTATION(counter_owned_image, IMAGE + 4U,
                    "counter for another image accepted");
    REJECT_MUTATION(strong_count, 0U, "zero strong count accepted");
    REJECT_MUTATION(strong_count, UINT16_MAX,
                    "overflowing strong count accepted");
    REJECT_MUTATION(strong_count, 1U,
                    "observable KAGE release edge accepted");
    REJECT_MUTATION(release_observer, EXPECTED_OBSERVER + 4U,
                    "foreign SmartPointer release observer accepted");
    REJECT_MUTATION(actual_min_filter, 9U,
                    "stale min filter accepted");
    REJECT_MUTATION(actual_mag_filter, 9U,
                    "stale mag filter accepted");
    REJECT_MUTATION(actual_wrap_s, 9U, "stale S wrap accepted");
    REJECT_MUTATION(actual_wrap_t, 9U, "stale T wrap accepted");
#undef REJECT_MUTATION

    snapshot = valid_snapshot();
    expect_reject(&snapshot, 0U, EXPECTED_OBSERVER,
                  "zero expected vftable accepted");
    expect_reject(&snapshot, EXPECTED_VFTABLE, 0U,
                  "nonzero observer accepted without a pinned callback");
    require(isaac_vita_renderframe_snapshot_is_borrowable(
                NULL, EXPECTED_VFTABLE, EXPECTED_OBSERVER, NULL) == 0,
            "NULL arguments accepted");

    puts("RenderFrame fast-path hostile snapshot oracle: PASS");
    return 0;
}
