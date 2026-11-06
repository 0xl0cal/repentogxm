/* Host oracle for host_vita_render_scale.c
 * (ISAAC_VITA_RENDER_SCALE_DEFAULT=1 ISAAC_VITA_RENDER_SCALE_ORACLE=1).
 *
 * Part 1 drives the pure transform: stock LF and CRLF files, already-target
 * values, blanks and tabs around '=', a case-variant key, an absent key
 * appended at the end of [Options] (EOF with and without a final newline,
 * before a following section, [Options] not first), a last line without a
 * newline, an empty value, a look-alike key, duplicates, no section, empty,
 * a NUL byte, the size cap and a too-small output buffer, both targets.
 *
 * Part 2 drives isaac_vita_render_scale_apply() over a fake three-path
 * filesystem (options.ini, .rs-tmp, .rs-bak) with failure injection on
 * every seam: absent, rewritten, unchanged (no write), both modes, too
 * large, open/read errors, a refused O_EXCL create, a failing write, a
 * failing sync, each of the two renames failing (options.ini intact or put
 * back, no sidecar left), recovery of a stranded aside copy with and
 * without a live options.ini, a stale temp, and the receipt line golden of
 * every case.  I/O discipline: reads open only options.ini and the aside
 * copy, the create is only ever the temp, a rename never lands on an
 * existing path, every descriptor is closed, the device sync names ux0:. */
#include "host_vita_render_scale.h"
#include "platform.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define ORACLE_PATH                                                            \
    ISAAC_VITA_NATIVE_DATA_ROOT                                                \
    "/Documents/My Games/Binding of Isaac Repentance/options.ini"
#define ORACLE_TMP ORACLE_PATH ".rs-tmp"
#define ORACLE_BAK ORACLE_PATH ".rs-bak"
#define ORACLE_ENOENT ((int)0x80010002u)
#define ORACLE_EIO ((int)0x80010005u)
#define ORACLE_EEXIST ((int)0x80010011u)
#define ORACLE_EACCES ((int)0x80010013u)
#define ORACLE_ENOSPC ((int)0x8001001cu)

static unsigned s_checks;
static unsigned s_failures;

static void check(int condition, const char *what)
{
    ++s_checks;
    if (!condition) {
        ++s_failures;
        fprintf(stderr, "FAIL: %s\n", what);
    }
}

static void check_u(unsigned long got, unsigned long expected, const char *what)
{
    ++s_checks;
    if (got != expected) {
        ++s_failures;
        fprintf(stderr, "FAIL: %s: got %lu expected %lu\n", what, got,
                expected);
    }
}

static void check_str(const char *got, const char *expected, const char *what)
{
    ++s_checks;
    if (strcmp(got, expected) != 0) {
        ++s_failures;
        fprintf(stderr, "FAIL: %s:\n  got      \"%s\"\n  expected \"%s\"\n",
                what, got, expected);
    }
}

static void check_bytes(const char *got, size_t got_length,
                        const char *expected, const char *what)
{
    ++s_checks;
    if (got_length != strlen(expected) ||
        memcmp(got, expected, got_length) != 0) {
        ++s_failures;
        fprintf(stderr, "FAIL: %s:\n  got      \"%.*s\" (%u bytes)\n"
                "  expected \"%s\"\n", what, (int)got_length, got,
                (unsigned)got_length, expected);
    }
}

/* ---- fake filesystem ---------------------------------------------------- */

#define NODE_CAPACITY 17408u
enum { NODE_INI = 0, NODE_TMP, NODE_BAK, NODE_COUNT };
static const char *const k_paths[NODE_COUNT] = {
    ORACLE_PATH, ORACLE_TMP, ORACLE_BAK,
};

struct node {
    int exists;
    uint32_t length;
    char data[NODE_CAPACITY];
};

struct handle {
    int open;
    int node;
    int writable;
    uint32_t position;
};

#define HANDLE_COUNT 4
#define FD_BASE 0x40010000

static struct node s_nodes[NODE_COUNT];
static struct handle s_handles[HANDLE_COUNT];

static int s_fail_open_read_node = -1;
static int s_fail_open_read_error;
static int s_fail_open_create_error;
static int s_fail_read_error;
static int s_fail_write_error;
static int s_fail_sync_fd_error;
static int s_fail_rename_index = -1;
static int s_fail_rename_error;

static unsigned s_open_read_calls;
static unsigned s_open_create_calls;
static unsigned s_read_calls;
static unsigned s_write_calls;
static unsigned s_close_calls;
static unsigned s_sync_fd_calls;
static unsigned s_remove_calls;
static unsigned s_rename_calls;
static unsigned s_sync_device_calls;
static unsigned s_bad_calls; /* unknown path, bad descriptor, protocol slip */
static int s_renames[8][2];

static void fs_reset(void)
{
    memset(s_nodes, 0, sizeof s_nodes);
    memset(s_handles, 0, sizeof s_handles);
    memset(s_renames, -1, sizeof s_renames);
    s_fail_open_read_node = -1;
    s_fail_open_read_error = 0;
    s_fail_open_create_error = 0;
    s_fail_read_error = 0;
    s_fail_write_error = 0;
    s_fail_sync_fd_error = 0;
    s_fail_rename_index = -1;
    s_fail_rename_error = 0;
    s_open_read_calls = s_open_create_calls = s_read_calls = s_write_calls = 0u;
    s_close_calls = s_sync_fd_calls = s_remove_calls = s_rename_calls = 0u;
    s_sync_device_calls = s_bad_calls = 0u;
}

static void fs_set(int node, const char *text, uint32_t length)
{
    s_nodes[node].exists = 1;
    s_nodes[node].length = length;
    memcpy(s_nodes[node].data, text, length);
}

static void fs_set_text(int node, const char *text)
{
    fs_set(node, text, (uint32_t)strlen(text));
}

static int node_of(const char *path)
{
    int index;
    for (index = 0; index < NODE_COUNT; ++index) {
        if (strcmp(path, k_paths[index]) == 0)
            return index;
    }
    ++s_bad_calls;
    return -1;
}

static int handle_alloc(int node, int writable)
{
    int index;
    for (index = 0; index < HANDLE_COUNT; ++index) {
        if (!s_handles[index].open) {
            s_handles[index].open = 1;
            s_handles[index].node = node;
            s_handles[index].writable = writable;
            s_handles[index].position = 0u;
            return FD_BASE + index;
        }
    }
    ++s_bad_calls;
    return ORACLE_EIO;
}

static struct handle *handle_of(int descriptor)
{
    int index = descriptor - FD_BASE;
    if (index < 0 || index >= HANDLE_COUNT || !s_handles[index].open) {
        ++s_bad_calls;
        return NULL;
    }
    return &s_handles[index];
}

static unsigned handles_open(void)
{
    unsigned count = 0u;
    int index;
    for (index = 0; index < HANDLE_COUNT; ++index)
        count += s_handles[index].open ? 1u : 0u;
    return count;
}

int isaac_vita_render_scale_oracle_open_read(const char *path)
{
    int node = node_of(path);
    ++s_open_read_calls;
    if (node < 0)
        return ORACLE_EIO;
    if (node == NODE_TMP)
        ++s_bad_calls; /* the temp is never read back */
    if (node == s_fail_open_read_node)
        return s_fail_open_read_error;
    if (!s_nodes[node].exists)
        return ORACLE_ENOENT;
    return handle_alloc(node, 0);
}

int isaac_vita_render_scale_oracle_open_create(const char *path)
{
    int node = node_of(path);
    ++s_open_create_calls;
    if (node < 0)
        return ORACLE_EIO;
    if (node != NODE_TMP)
        ++s_bad_calls; /* only the temp is ever created */
    if (s_fail_open_create_error != 0)
        return s_fail_open_create_error;
    if (s_nodes[node].exists)
        return ORACLE_EEXIST;
    s_nodes[node].exists = 1;
    s_nodes[node].length = 0u;
    return handle_alloc(node, 1);
}

int isaac_vita_render_scale_oracle_read(int descriptor, void *buffer,
                                        uint32_t size)
{
    struct handle *handle = handle_of(descriptor);
    uint32_t remaining;
    ++s_read_calls;
    if (handle == NULL || handle->writable) {
        ++s_bad_calls;
        return ORACLE_EIO;
    }
    if (s_fail_read_error != 0) {
        int error = s_fail_read_error;
        s_fail_read_error = 0;
        return error;
    }
    remaining = s_nodes[handle->node].length - handle->position;
    if (size > remaining)
        size = remaining;
    memcpy(buffer, s_nodes[handle->node].data + handle->position, size);
    handle->position += size;
    return (int)size;
}

int isaac_vita_render_scale_oracle_write(int descriptor, const void *buffer,
                                         uint32_t size)
{
    struct handle *handle = handle_of(descriptor);
    struct node *node;
    ++s_write_calls;
    if (handle == NULL || !handle->writable) {
        ++s_bad_calls;
        return ORACLE_EIO;
    }
    if (s_fail_write_error != 0)
        return s_fail_write_error;
    node = &s_nodes[handle->node];
    if (node->length + size > NODE_CAPACITY) {
        ++s_bad_calls;
        return ORACLE_ENOSPC;
    }
    memcpy(node->data + node->length, buffer, size);
    node->length += size;
    return (int)size;
}

int isaac_vita_render_scale_oracle_close(int descriptor)
{
    struct handle *handle = handle_of(descriptor);
    ++s_close_calls;
    if (handle == NULL)
        return ORACLE_EIO;
    handle->open = 0;
    return 0;
}

int isaac_vita_render_scale_oracle_sync_fd(int descriptor)
{
    struct handle *handle = handle_of(descriptor);
    ++s_sync_fd_calls;
    if (handle == NULL || !handle->writable) {
        ++s_bad_calls;
        return ORACLE_EIO;
    }
    return s_fail_sync_fd_error;
}

int isaac_vita_render_scale_oracle_remove(const char *path)
{
    int node = node_of(path);
    ++s_remove_calls;
    if (node < 0)
        return ORACLE_EIO;
    if (!s_nodes[node].exists)
        return ORACLE_ENOENT;
    s_nodes[node].exists = 0;
    s_nodes[node].length = 0u;
    return 0;
}

int isaac_vita_render_scale_oracle_rename(const char *source,
                                          const char *destination)
{
    int from = node_of(source);
    int to = node_of(destination);
    unsigned index = s_rename_calls++;
    if (from < 0 || to < 0)
        return ORACLE_EIO;
    if (index < 8u) {
        s_renames[index][0] = from;
        s_renames[index][1] = to;
    }
    if ((int)index == s_fail_rename_index)
        return s_fail_rename_error;
    if (!s_nodes[from].exists)
        return ORACLE_ENOENT;
    if (s_nodes[to].exists) {
        ++s_bad_calls; /* the protocol never renames onto an existing path */
        return ORACLE_EEXIST;
    }
    s_nodes[to] = s_nodes[from];
    s_nodes[from].exists = 0;
    s_nodes[from].length = 0u;
    return 0;
}

int isaac_vita_render_scale_oracle_sync_device(const char *device)
{
    ++s_sync_device_calls;
    if (strcmp(device, "ux0:") != 0)
        ++s_bad_calls;
    return 0;
}

/* ---- log capture -------------------------------------------------------- */

static char s_log[4][320];
static unsigned s_log_count;

void isaac_vita_log(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    if (s_log_count < 4u)
        (void)vsnprintf(s_log[s_log_count], sizeof s_log[0], format, args);
    va_end(args);
    ++s_log_count;
}

/* ---- part 1: the transform ---------------------------------------------- */

static char s_out[ISAAC_VITA_RENDER_SCALE_OUT_BYTES];

static void expect_transform(const char *what, const char *in,
                             const char *target,
                             enum isaac_vita_render_scale_status status,
                             const char *out, const char *old)
{
    struct isaac_vita_render_scale_edit edit;
    char label[160];

    memset(s_out, 0x7f, sizeof s_out);
    isaac_vita_render_scale_transform(in, (uint32_t)strlen(in), target, s_out,
                                      (uint32_t)sizeof s_out, &edit);
    (void)snprintf(label, sizeof label, "%s: status", what);
    check_str(isaac_vita_render_scale_status_name(
                  (enum isaac_vita_render_scale_status)edit.status),
              isaac_vita_render_scale_status_name(status), label);
    (void)snprintf(label, sizeof label, "%s: old value", what);
    check_str(edit.old_value, old, label);
    (void)snprintf(label, sizeof label, "%s: output", what);
    if (out != NULL) {
        check_bytes(s_out, edit.out_length, out, label);
    } else {
        check_u(edit.out_length, 0u, label);
    }
}

static void test_transform(void)
{
    static const char stock_lf[] =
        "[Options]\nMusicVolume=1\nMaxRenderScale=99\nWindowWidth=960\n";
    static const char stock_lf_1[] =
        "[Options]\nMusicVolume=1\nMaxRenderScale=1\nWindowWidth=960\n";
    static const char stock_crlf[] =
        "[Options]\r\nMusicVolume=1\r\nMaxRenderScale=99\r\nWindowWidth=960\r\n";
    static const char stock_crlf_1[] =
        "[Options]\r\nMusicVolume=1\r\nMaxRenderScale=1\r\nWindowWidth=960\r\n";
    static char big[ISAAC_VITA_RENDER_SCALE_FILE_MAX + 2u];
    struct isaac_vita_render_scale_edit edit;
    size_t index;

    expect_transform("stock LF 99 -> 1", stock_lf, "1",
                     ISAAC_VITA_RENDER_SCALE_REWRITTEN, stock_lf_1, "99");
    expect_transform("stock CRLF 99 -> 1", stock_crlf, "1",
                     ISAAC_VITA_RENDER_SCALE_REWRITTEN, stock_crlf_1, "99");
    expect_transform("stock LF 1 -> 99", stock_lf_1, "99",
                     ISAAC_VITA_RENDER_SCALE_REWRITTEN, stock_lf, "1");
    expect_transform("already 1", stock_lf_1, "1",
                     ISAAC_VITA_RENDER_SCALE_UNCHANGED, NULL, "1");
    expect_transform("already 99", stock_crlf, "99",
                     ISAAC_VITA_RENDER_SCALE_UNCHANGED, NULL, "99");
    expect_transform("blanks around = kept",
                     "[Options]\r\nMaxRenderScale = 99 \r\nA=1\r\n", "1",
                     ISAAC_VITA_RENDER_SCALE_REWRITTEN,
                     "[Options]\r\nMaxRenderScale = 1 \r\nA=1\r\n", "99");
    expect_transform("tabs kept",
                     "[Options]\n\tMaxRenderScale\t=\t99\t\nA=1\n", "1",
                     ISAAC_VITA_RENDER_SCALE_REWRITTEN,
                     "[Options]\n\tMaxRenderScale\t=\t1\t\nA=1\n", "99");
    expect_transform("case-variant key kept",
                     "[Options]\nmaxrenderscale=2\n", "1",
                     ISAAC_VITA_RENDER_SCALE_REWRITTEN,
                     "[Options]\nmaxrenderscale=1\n", "2");
    expect_transform("empty value",
                     "[Options]\nMaxRenderScale=\nA=1\n", "1",
                     ISAAC_VITA_RENDER_SCALE_REWRITTEN,
                     "[Options]\nMaxRenderScale=1\nA=1\n", "");
    expect_transform("key on the last line without newline",
                     "[Options]\nMaxRenderScale=99", "1",
                     ISAAC_VITA_RENDER_SCALE_REWRITTEN,
                     "[Options]\nMaxRenderScale=1", "99");
    expect_transform("key outside any section still rewritten",
                     "MaxRenderScale=99\n", "1",
                     ISAAC_VITA_RENDER_SCALE_REWRITTEN,
                     "MaxRenderScale=1\n", "99");
    expect_transform("long value truncated in the receipt",
                     "[Options]\nMaxRenderScale=0123456789abcdefXYZ\n", "1",
                     ISAAC_VITA_RENDER_SCALE_REWRITTEN,
                     "[Options]\nMaxRenderScale=1\n", "0123456789abcde");
    /* Absent key. */
    expect_transform("absent key appended at EOF (LF)",
                     "[Options]\nA=1\n", "1",
                     ISAAC_VITA_RENDER_SCALE_APPENDED,
                     "[Options]\nA=1\nMaxRenderScale=1\n", "-");
    expect_transform("absent key appended, final newline completed (CRLF)",
                     "[Options]\r\nA=1", "99",
                     ISAAC_VITA_RENDER_SCALE_APPENDED,
                     "[Options]\r\nA=1\r\nMaxRenderScale=99\r\n", "-");
    expect_transform("absent key inserted before the next section",
                     "[Options]\nA=1\n[Other]\nB=2\n", "1",
                     ISAAC_VITA_RENDER_SCALE_APPENDED,
                     "[Options]\nA=1\nMaxRenderScale=1\n[Other]\nB=2\n", "-");
    expect_transform("[Options] not first: appended at EOF",
                     "[Other]\nB=2\n[Options]\nA=1\n", "1",
                     ISAAC_VITA_RENDER_SCALE_APPENDED,
                     "[Other]\nB=2\n[Options]\nA=1\nMaxRenderScale=1\n", "-");
    expect_transform("section header with blanks and case",
                     "  [options]  \r\nA=1\r\n", "1",
                     ISAAC_VITA_RENDER_SCALE_APPENDED,
                     "  [options]  \r\nA=1\r\nMaxRenderScale=1\r\n", "-");
    expect_transform("look-alike key is not the key",
                     "[Options]\nMaxRenderScaleX=99\n", "1",
                     ISAAC_VITA_RENDER_SCALE_APPENDED,
                     "[Options]\nMaxRenderScaleX=99\nMaxRenderScale=1\n", "-");
    expect_transform("header only, no newline",
                     "[Options]", "1",
                     ISAAC_VITA_RENDER_SCALE_APPENDED,
                     "[Options]\nMaxRenderScale=1\n", "-");
    /* Left alone. */
    expect_transform("duplicate key",
                     "[Options]\nMaxRenderScale=99\nMaxRenderScale=1\n", "1",
                     ISAAC_VITA_RENDER_SCALE_DUPLICATE, NULL, "99");
    expect_transform("no section and no key", "A=1\nB=2\n", "1",
                     ISAAC_VITA_RENDER_SCALE_NO_SECTION, NULL, "-");
    expect_transform("empty file", "", "1",
                     ISAAC_VITA_RENDER_SCALE_EMPTY, NULL, "-");
    {
        static const char nul[] = "[Options]\nMaxRenderScale=99\n";
        char copy[sizeof nul];
        memcpy(copy, nul, sizeof nul);
        copy[3] = '\0';
        isaac_vita_render_scale_transform(copy, (uint32_t)(sizeof nul - 1u),
                                          "1", s_out, (uint32_t)sizeof s_out,
                                          &edit);
        check_u(edit.status, ISAAC_VITA_RENDER_SCALE_BINARY, "NUL byte: binary");
        check_u(edit.out_length, 0u, "NUL byte: no output");
    }
    memset(big, 'a', sizeof big);
    memcpy(big, "[Options]\n", 10u);
    big[sizeof big - 2u] = '\n';
    big[sizeof big - 1u] = '\0';
    isaac_vita_render_scale_transform(big, ISAAC_VITA_RENDER_SCALE_FILE_MAX + 1u,
                                      "1", s_out, (uint32_t)sizeof s_out, &edit);
    check_u(edit.status, ISAAC_VITA_RENDER_SCALE_TOO_LARGE, "one byte past the cap");
    /* Exactly the cap: still processed (the file has no key: appended). */
    isaac_vita_render_scale_transform(big, ISAAC_VITA_RENDER_SCALE_FILE_MAX, "1",
                                      s_out, (uint32_t)sizeof s_out, &edit);
    check_u(edit.status, ISAAC_VITA_RENDER_SCALE_APPENDED, "exactly the cap");
    check_u(edit.out_length,
            ISAAC_VITA_RENDER_SCALE_FILE_MAX + 1u + 14u + 1u + 1u + 1u,
            "cap file grows by ending + line");
    check(memcmp(s_out + ISAAC_VITA_RENDER_SCALE_FILE_MAX,
                 "\nMaxRenderScale=1\n", 18u) == 0,
          "cap file: line appended after the completed newline");
    isaac_vita_render_scale_transform(big, ISAAC_VITA_RENDER_SCALE_FILE_MAX, "1",
                                      s_out, ISAAC_VITA_RENDER_SCALE_FILE_MAX,
                                      &edit);
    check_u(edit.status, ISAAC_VITA_RENDER_SCALE_TOO_LARGE,
            "output buffer too small");
    for (index = 0u; index < 3u; ++index) {
        const char *dash = isaac_vita_render_scale_status_name(
            (enum isaac_vita_render_scale_status)
                (ISAAC_VITA_RENDER_SCALE_STATUS_COUNT + index));
        check_str(dash, "?", "status name out of range");
    }
    check_str(isaac_vita_render_scale_target(1u), "1", "target of 1");
    check_str(isaac_vita_render_scale_target(2u), "99", "target of 2");
    check_str(isaac_vita_render_scale_target(0u), "99", "target of anything else");
    check_str(isaac_vita_render_scale_status_name(ISAAC_VITA_RENDER_SCALE_RENAME_ERROR),
              "rename-error", "last status name");
}

/* ---- part 2: apply over the fake filesystem ------------------------------ */

static void apply_expect(const char *what, uint8_t mode, const char *status,
                         const char *old, unsigned bytes, unsigned rc,
                         unsigned recovered)
{
    char expected[200];
    char label[160];

    s_log_count = 0u;
    isaac_vita_render_scale_apply(mode);
    (void)snprintf(expected, sizeof expected,
                   "ISAAC VITA RENDER SCALE: mode=%u target=%s status=%s "
                   "old=%s bytes=%u rc=0x%08x recovered=%u",
                   (unsigned)mode, mode == 1u ? "1" : "99", status, old,
                   bytes, rc, recovered);
    (void)snprintf(label, sizeof label, "%s: one receipt line", what);
    check_u(s_log_count, 1u, label);
    (void)snprintf(label, sizeof label, "%s: receipt", what);
    check_str(s_log[0], expected, label);
    (void)snprintf(label, sizeof label, "%s: every descriptor closed", what);
    check_u(handles_open(), 0u, label);
    (void)snprintf(label, sizeof label, "%s: no protocol slip", what);
    check_u(s_bad_calls, 0u, label);
}

static void expect_node(const char *what, int node, const char *text)
{
    char label[160];
    (void)snprintf(label, sizeof label, "%s: %s", what,
                   node == NODE_INI ? "options.ini" :
                   node == NODE_TMP ? "temp" : "aside copy");
    if (text == NULL) {
        check(!s_nodes[node].exists, label);
    } else {
        check(s_nodes[node].exists, label);
        check_bytes(s_nodes[node].data, s_nodes[node].length, text, label);
    }
}

static void expect_clean(const char *what, const char *ini)
{
    expect_node(what, NODE_INI, ini);
    expect_node(what, NODE_TMP, NULL);
    expect_node(what, NODE_BAK, NULL);
}

static const char k_stock_99[] =
    "[Options]\r\nMusicVolume=1\r\nSFXVolume=1\r\nMaxRenderScale=99\r\n"
    "WindowWidth=960\r\nWindowHeight=540\r\n";
static const char k_stock_1[] =
    "[Options]\r\nMusicVolume=1\r\nSFXVolume=1\r\nMaxRenderScale=1\r\n"
    "WindowWidth=960\r\nWindowHeight=540\r\n";
#define STOCK_BYTES ((unsigned)(sizeof k_stock_99 - 1u))
#define STOCK_1_BYTES ((unsigned)(sizeof k_stock_1 - 1u))

static void test_apply_paths(void)
{
    /* Absent: nothing created, the stale-temp removal is the only write. */
    fs_reset();
    apply_expect("absent", 1u, "absent", "-", 0u, 0u, 0u);
    expect_clean("absent", NULL);
    check_u(s_open_create_calls, 0u, "absent: no create");
    check_u(s_rename_calls, 0u, "absent: no rename");
    check_u(s_sync_device_calls, 0u, "absent: no device sync");
    check_u(s_remove_calls, 1u, "absent: only the stale temp removal");

    /* Rewritten: temp -> aside -> promote -> aside removed -> device sync. */
    fs_reset();
    fs_set_text(NODE_INI, k_stock_99);
    apply_expect("rewritten", 1u, "rewritten", "99", STOCK_BYTES, 0u, 0u);
    expect_clean("rewritten", k_stock_1);
    check_u(s_open_create_calls, 1u, "rewritten: one create");
    check_u(s_sync_fd_calls, 1u, "rewritten: one file sync");
    check_u(s_rename_calls, 2u, "rewritten: two renames");
    check(s_renames[0][0] == NODE_INI && s_renames[0][1] == NODE_BAK &&
              s_renames[1][0] == NODE_TMP && s_renames[1][1] == NODE_INI,
          "rewritten: aside then promote");
    check_u(s_sync_device_calls, 1u, "rewritten: one device sync");

    /* Unchanged: read only. */
    fs_reset();
    fs_set_text(NODE_INI, k_stock_1);
    apply_expect("unchanged", 1u, "unchanged", "1", STOCK_1_BYTES, 0u, 0u);
    expect_clean("unchanged", k_stock_1);
    check_u(s_open_create_calls, 0u, "unchanged: no create");
    check_u(s_write_calls, 0u, "unchanged: no write");
    check_u(s_rename_calls, 0u, "unchanged: no rename");
    check_u(s_sync_device_calls, 0u, "unchanged: no device sync");

    /* Mode 2: the stock value is the target; a 1 goes back to 99. */
    fs_reset();
    fs_set_text(NODE_INI, k_stock_99);
    apply_expect("mode 2 unchanged", 2u, "unchanged", "99", STOCK_BYTES, 0u, 0u);
    expect_clean("mode 2 unchanged", k_stock_99);
    fs_reset();
    fs_set_text(NODE_INI, k_stock_1);
    apply_expect("mode 2 rewritten", 2u, "rewritten", "1", STOCK_1_BYTES, 0u, 0u);
    expect_clean("mode 2 rewritten", k_stock_99);

    /* Appended and published. */
    fs_reset();
    fs_set_text(NODE_INI, "[Options]\r\nMusicVolume=1\r\n");
    apply_expect("appended", 1u, "appended", "-", 26u, 0u, 0u);
    expect_clean("appended", "[Options]\r\nMusicVolume=1\r\nMaxRenderScale=1\r\n");

    /* Left alone: duplicate, no section, empty, too large. */
    fs_reset();
    fs_set_text(NODE_INI, "[Options]\nMaxRenderScale=99\nMaxRenderScale=1\n");
    apply_expect("duplicate", 1u, "duplicate", "99", 45u, 0u, 0u);
    expect_clean("duplicate", "[Options]\nMaxRenderScale=99\nMaxRenderScale=1\n");
    check_u(s_open_create_calls, 0u, "duplicate: no create");
    fs_reset();
    fs_set_text(NODE_INI, "MusicVolume=1\n");
    apply_expect("no section", 1u, "no-section", "-", 14u, 0u, 0u);
    expect_clean("no section", "MusicVolume=1\n");
    fs_reset();
    fs_set(NODE_INI, "", 0u);
    apply_expect("empty", 1u, "empty", "-", 0u, 0u, 0u);
    expect_clean("empty", "");
    {
        static char big[ISAAC_VITA_RENDER_SCALE_FILE_MAX + 1u];
        memset(big, 'a', sizeof big);
        memcpy(big, "[Options]\n", 10u);
        fs_reset();
        fs_set(NODE_INI, big, (uint32_t)sizeof big);
        apply_expect("too large", 1u, "too-large", "-",
                     ISAAC_VITA_RENDER_SCALE_FILE_MAX + 1u, 0u, 0u);
        check(s_nodes[NODE_INI].exists &&
                  s_nodes[NODE_INI].length == sizeof big,
              "too large: file untouched");
        expect_node("too large", NODE_TMP, NULL);
        check_u(s_open_create_calls, 0u, "too large: no create");
        /* Exactly the cap is processed (appended here). */
        fs_reset();
        fs_set(NODE_INI, big, ISAAC_VITA_RENDER_SCALE_FILE_MAX);
        apply_expect("exactly the cap", 1u, "appended", "-",
                     ISAAC_VITA_RENDER_SCALE_FILE_MAX, 0u, 0u);
        check_u(s_nodes[NODE_INI].length,
                ISAAC_VITA_RENDER_SCALE_FILE_MAX + 18u,
                "exactly the cap: line appended");
    }

    /* Read failures. */
    fs_reset();
    fs_set_text(NODE_INI, k_stock_99);
    s_fail_open_read_node = NODE_INI;
    s_fail_open_read_error = ORACLE_EACCES;
    apply_expect("open error", 1u, "read-error", "-", 0u,
                 (unsigned)ORACLE_EACCES, 0u);
    expect_clean("open error", k_stock_99);
    fs_reset();
    fs_set_text(NODE_INI, k_stock_99);
    s_fail_read_error = ORACLE_EIO;
    apply_expect("read error", 1u, "read-error", "-", 0u,
                 (unsigned)ORACLE_EIO, 0u);
    expect_clean("read error", k_stock_99);
    check_u(s_open_create_calls, 0u, "read error: no create");
}

static void test_apply_failures(void)
{
    /* Refused O_EXCL create: nothing else touched. */
    fs_reset();
    fs_set_text(NODE_INI, k_stock_99);
    s_fail_open_create_error = ORACLE_EEXIST;
    apply_expect("create refused", 1u, "write-error", "99", STOCK_BYTES,
                 (unsigned)ORACLE_EEXIST, 0u);
    expect_clean("create refused", k_stock_99);
    check_u(s_rename_calls, 0u, "create refused: no rename");

    /* Failing write: temp removed, options.ini intact. */
    fs_reset();
    fs_set_text(NODE_INI, k_stock_99);
    s_fail_write_error = ORACLE_ENOSPC;
    apply_expect("write failed", 1u, "write-error", "99", STOCK_BYTES,
                 (unsigned)ORACLE_ENOSPC, 0u);
    expect_clean("write failed", k_stock_99);
    check_u(s_rename_calls, 0u, "write failed: no rename");

    /* Failing sync: same. */
    fs_reset();
    fs_set_text(NODE_INI, k_stock_99);
    s_fail_sync_fd_error = ORACLE_EIO;
    apply_expect("sync failed", 1u, "write-error", "99", STOCK_BYTES,
                 (unsigned)ORACLE_EIO, 0u);
    expect_clean("sync failed", k_stock_99);

    /* First rename (aside) fails: temp removed, options.ini intact. */
    fs_reset();
    fs_set_text(NODE_INI, k_stock_99);
    s_fail_rename_index = 0;
    s_fail_rename_error = ORACLE_EACCES;
    apply_expect("aside failed", 1u, "rename-error", "99", STOCK_BYTES,
                 (unsigned)ORACLE_EACCES, 0u);
    expect_clean("aside failed", k_stock_99);
    check_u(s_rename_calls, 1u, "aside failed: one rename");
    check_u(s_sync_device_calls, 0u, "aside failed: no device sync");

    /* Second rename (promote) fails: aside copy moved back, temp removed. */
    fs_reset();
    fs_set_text(NODE_INI, k_stock_99);
    s_fail_rename_index = 1;
    s_fail_rename_error = ORACLE_EIO;
    apply_expect("promote failed", 1u, "rename-error", "99", STOCK_BYTES,
                 (unsigned)ORACLE_EIO, 0u);
    expect_clean("promote failed", k_stock_99);
    check_u(s_rename_calls, 3u, "promote failed: aside, promote, restore");
    check(s_renames[2][0] == NODE_BAK && s_renames[2][1] == NODE_INI,
          "promote failed: the restore rename");
    check_u(s_sync_device_calls, 0u, "promote failed: no device sync");
}

static void test_recovery(void)
{
    /* Stranded aside copy, options.ini missing (power loss between the two
     * renames): moved back, then normalized as usual. */
    fs_reset();
    fs_set_text(NODE_BAK, k_stock_99);
    fs_set_text(NODE_TMP, "garbage");
    apply_expect("recovered", 1u, "rewritten", "99", STOCK_BYTES, 0u, 1u);
    expect_clean("recovered", k_stock_1);
    check(s_renames[0][0] == NODE_BAK && s_renames[0][1] == NODE_INI,
          "recovered: the first rename is the recovery");
    check_u(s_rename_calls, 3u, "recovered: recovery plus the two publish renames");

    /* Aside copy left behind after a completed promotion: removed. */
    fs_reset();
    fs_set_text(NODE_INI, k_stock_1);
    fs_set_text(NODE_BAK, k_stock_99);
    apply_expect("stale aside copy", 1u, "unchanged", "1", STOCK_1_BYTES, 0u, 0u);
    expect_clean("stale aside copy", k_stock_1);
    check_u(s_rename_calls, 0u, "stale aside copy: no rename");

    /* Stale temp alone: removed, nothing else. */
    fs_reset();
    fs_set_text(NODE_INI, k_stock_1);
    fs_set_text(NODE_TMP, "garbage");
    apply_expect("stale temp", 1u, "unchanged", "1", STOCK_1_BYTES, 0u, 0u);
    expect_clean("stale temp", k_stock_1);

    /* Recovery rename fails: reported as absent, the aside copy is kept. */
    fs_reset();
    fs_set_text(NODE_BAK, k_stock_99);
    s_fail_rename_index = 0;
    s_fail_rename_error = ORACLE_EIO;
    apply_expect("recovery failed", 1u, "absent", "-", 0u, 0u, 0u);
    expect_node("recovery failed", NODE_INI, NULL);
    expect_node("recovery failed", NODE_BAK, k_stock_99);
    check_u(s_open_create_calls, 0u, "recovery failed: nothing created");

    /* options.ini probe fails with a non-ENOENT error while an aside copy
     * exists: the copy is kept, the read reports the error. */
    fs_reset();
    fs_set_text(NODE_INI, k_stock_99);
    fs_set_text(NODE_BAK, k_stock_1);
    s_fail_open_read_node = NODE_INI;
    s_fail_open_read_error = ORACLE_EACCES;
    apply_expect("probe error", 1u, "read-error", "-", 0u,
                 (unsigned)ORACLE_EACCES, 0u);
    expect_node("probe error", NODE_INI, k_stock_99);
    expect_node("probe error", NODE_BAK, k_stock_1);
}

int main(void)
{
    test_transform();
    test_apply_paths();
    test_apply_failures();
    test_recovery();
    if (s_failures != 0u) {
        fprintf(stderr, "host_vita_render_scale_oracle: FAIL (%u of %u checks)\n",
                s_failures, s_checks);
        return 1;
    }
    printf("host_vita_render_scale_oracle: PASS (%u checks; transform LF/CRLF, "
           "append, duplicates, cap; publish protocol, every failure seam, "
           "recovery, receipt goldens)\n", s_checks);
    return 0;
}
