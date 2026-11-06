/*
 * Host test for the manager settings core (manager_settings.c) and the
 * fenced text primitives of manager_storage.c, over the shared fake
 * filesystem of fake_fs.h (nodes enlarged so a 16 KiB log head fits).
 *
 * Cases:
 *   - read/head/find-free/newest primitives;
 *   - load: absent / ok / ok-with-ignored / schema / garbage / 4097 bytes /
 *     oversize / io-error / directory in the way;
 *   - save happy path: exact canonical bytes, previous version archived,
 *     no temp or rollback left; non-canonical originals archived verbatim;
 *   - refusals: FTP listening, a/b guard, unserializable pending, no free
 *     archive index, exclusive-open conflict, readback corruption;
 *   - fault matrix: every vtable operation occurrence along
 *     write_text_atomic in three filesystem shapes; after each fault
 *     port.ini is byte-identical to old or new, never both .rollback and
 *     .tmp exist, recover()+load() are consistent and idempotent;
 *   - recover_port repairs rollback-only, rollback + new port.ini, stale
 *     tmp, all three; folded into manager_storage_recover;
 *   - DEFAULTS archives + removes, survives two recover() calls, reports
 *     NO PORT.INI the second time;
 *   - UNDO restores the previous bytes and archives the current (twice =
 *     redo), hidden on equal hashes / no archive, offered but refused on a
 *     bad archive; a re-read failure after a verified SAVE/UNDO keeps the
 *     written values on the page;
 *   - fault matrices (every vtable operation, every occurrence) for DEFAULTS
 *     (archive_and_remove), UNDO (read half + write) and recover_port from
 *     the fully torn state: legal end states only, recover()+load() agree;
 *   - log-head goldens: receipt values, build line without receipt, no
 *     log, 16 KiB cap with the receipt beyond it or cut by it, CRLF;
 *   - every status string uses the narrow glyph set and never ':'.
 */
#define FAKE_DATA_CAPACITY 17408u
#include "fake_fs.h"

#include "manager_settings.h"

#include <stdio.h>
#include <string.h>

#define PORT_INI MANAGER_STORAGE_PORT_INI_PATH
#define PORT_TMP MANAGER_STORAGE_ROOT "/port.ini.tmp"
#define PORT_ROLLBACK MANAGER_STORAGE_ROOT "/port.ini.rollback"
#define ARCHIVE(index) MANAGER_STORAGE_BACKUP_ROOT "/port-" index ".ini"
#define RECOVERED(index) \
    MANAGER_STORAGE_BACKUP_ROOT "/port-recovered-" index ".ini"
#define LOG_PATH MANAGER_SETTINGS_LOG_PATH
/* Save-slot 2 rollback sidecar (manager_storage.c rollback_path): a
 * directory here makes recover_slot(2) report CONFLICT. */
#define SLOT2_ROLLBACK MANAGER_STORAGE_SAVE_ROOT "/.manager-rollback-slot2.dat"

/* Canonical serialization (table order of host_vita_settings_parse.h). */
#define CANON_REST                                                             \
    "button.cross=a\n"                                                         \
    "button.circle=b\n"                                                        \
    "button.square=x\n"                                                        \
    "button.triangle=y\n"                                                      \
    "button.l=lb\n"                                                            \
    "button.r=lt\n"                                                            \
    "button.select=rt\n"                                                       \
    "chord.start_l=rb\n"                                                       \
    "chord.start_r=back\n"                                                     \
    "chord.restart_hold=on\n"                                                  \
    "touch.top_left=lt\n"                                                      \
    "touch.top_right=back\n"                                                   \
    "touch.bottom_right=rb\n"                                                  \
    "stick.swap=off\n"
#define CANON_TAIL "display_raster=960\n" "render_scale=1\n" CANON_REST
#define CANON_60 "schema=1\nfps_lock=60\n" CANON_TAIL
#define CANON_45 "schema=1\nfps_lock=45\n" CANON_TAIL
#define CANON_30 "schema=1\nfps_lock=30\n" CANON_TAIL
#define HAND_45 "# written by hand\r\nschema=1\r\n\r\nfps_lock=45\r\n"

#define BUILD_LINE                                                             \
    "[0.123 thr 0x40010003] build 2026-09-10-abc; guest base=0x00400000; "    \
    "softfp ARMv7\n"
#define ROTATE_LINE                                                            \
    "[0.130 thr 0x40010003] KAGE VITA LOG ROTATE: rotate=ok "                 \
    "prev=ux0:/data/isaacr001/first-arm-fault.prev.log\n"
#define RECEIPT_LINE_45                                                        \
    "[0.201 thr 0x40010003] ISAAC VITA SETTINGS: "                            \
    "path=ux0:/data/isaacr001/port.ini status=ok bytes=32 lines=2 "          \
    "rejected=1 unknown=0 dup=0 fps_lock=45 "                                  \
    "raster=960x544(ignored:not-compiled) remap=default(guard) "              \
    "chords=rb/back restart_hold=1 stick_swap=0\n"
#define REJECTED_LINE                                                          \
    "[0.202 thr 0x40010003] ISAAC VITA SETTINGS REJECTED: lines=3,0,0,0 "     \
    "unknown=,,,\n"
#define RECEIPT_LINE_ABSENT                                                    \
    "[0.201 thr 0x40010003] ISAAC VITA SETTINGS: "                            \
    "path=ux0:/data/isaacr001/port.ini status=absent bytes=0 lines=0 "       \
    "rejected=0 unknown=0 dup=0 fps_lock=60(ignored:no-fullspeed-scheduler) " \
    "raster=960x544(ignored:not-compiled) remap=custom "                      \
    "chords=rb/back restart_hold=1 stick_swap=0\n"

/* One fake filesystem (about 1.4 MiB with the enlarged nodes) in static
 * storage; every case starts from fake_init(). */
static struct fake_fs g_fs;

static struct manager_storage_io settings_fs_init(struct fake_fs *fs,
                                                  int backup_root)
{
    fake_init(fs);
    if (backup_root)
        fake_add_dir(fs, MANAGER_STORAGE_BACKUP_ROOT);
    return fake_io(fs);
}

static int fake_exists(const struct fake_fs *fs, const char *path)
{
    return fake_find(fs, path) >= 0;
}

static void require_absent(const struct fake_fs *fs, const char *path,
                           const char *message)
{
    require(!fake_exists(fs, path), message);
}

static void require_no_inflight(const struct fake_fs *fs, const char *message)
{
    require(!fake_exists(fs, PORT_TMP), message);
    require(!fake_exists(fs, PORT_ROLLBACK), message);
}

static int fake_count_containing(const struct fake_fs *fs, const char *needle)
{
    size_t index;
    int count = 0;
    for (index = 0u; index < fs->node_count; ++index) {
        if (strstr(fs->nodes[index].path, needle) != NULL)
            ++count;
    }
    return count;
}

static void require_narrow(const char *text, const char *message)
{
    require(text[0] != '\0', message);
    require(manager_settings_text_is_narrow(text), message);
    require(strchr(text, ':') == NULL, message);
    require(strlen(text) < 58u, message);
}

static void require_result(const struct manager_settings_result *result,
                           enum manager_settings_action action,
                           enum manager_settings_code code, const char *text,
                           const char *message)
{
    char rendered[MANAGER_SETTINGS_TEXT_CAPACITY];
    require(result->action == (uint8_t)action, message);
    require(result->code == (uint8_t)code, message);
    manager_settings_result_text(result, rendered, sizeof(rendered));
    require(strcmp(rendered, text) == 0, message);
    require_narrow(rendered, message);
}

/* ---- primitives ---------------------------------------------------------- */

static void test_text_primitives(void)
{
    struct fake_fs *fs = &g_fs;
    struct manager_storage_io io = settings_fs_init(fs, 1);
    char buffer[8];
    char big[1200];
    char path[MANAGER_STORAGE_PATH_CAPACITY];
    size_t length = 99u;
    unsigned int index = 77u;
    int truncated = 5;

    require(manager_storage_read_text(&io, PORT_INI, buffer, sizeof(buffer),
                                      &length) == MANAGER_STORAGE_ABSENT,
            "read_text absent");
    require(length == 0u && buffer[0] == '\0', "read_text absent clears");
    fake_add_file(fs, PORT_INI, "1234567");
    require(manager_storage_read_text(&io, PORT_INI, buffer, sizeof(buffer),
                                      &length) == MANAGER_STORAGE_OK,
            "read_text exactly capacity-1 bytes");
    require(length == 7u && strcmp(buffer, "1234567") == 0,
            "read_text bytes and NUL");
    require(manager_storage_read_text(&io, PORT_INI, buffer, 7u, &length) ==
                MANAGER_STORAGE_TOO_LARGE,
            "read_text refuses capacity bytes");
    require(length == 0u, "read_text too-large keeps nothing");
    require(manager_storage_read_head(&io, PORT_INI, buffer, 5u, &length,
                                      &truncated) == MANAGER_STORAGE_OK,
            "read_head cuts");
    require(length == 4u && truncated == 1 && strcmp(buffer, "1234") == 0,
            "read_head reports truncation");
    require(manager_storage_read_head(&io, PORT_INI, buffer, sizeof(buffer),
                                      &length, &truncated) == MANAGER_STORAGE_OK,
            "read_head whole file");
    require(length == 7u && truncated == 0, "read_head whole file flag");
    require(manager_storage_read_text(&io, MANAGER_STORAGE_BACKUP_ROOT, buffer,
                                      sizeof(buffer), &length) ==
                MANAGER_STORAGE_CONFLICT,
            "read_text refuses a directory");
    fake_fail(fs, FAKE_OP_OPEN_READ, 1);
    require(manager_storage_read_text(&io, PORT_INI, buffer, sizeof(buffer),
                                      &length) == MANAGER_STORAGE_IO_ERROR,
            "read_text open failure");
    fake_fail(fs, FAKE_OP_READ, 1);
    require(manager_storage_read_text(&io, PORT_INI, buffer, sizeof(buffer),
                                      &length) == MANAGER_STORAGE_IO_ERROR,
            "read_text read failure");
    fake_fail(fs, FAKE_OP_STAT, 1);
    require(manager_storage_read_text(&io, PORT_INI, buffer, sizeof(buffer),
                                      &length) == MANAGER_STORAGE_IO_ERROR,
            "read_text stat failure is not absent");
    fake_no_fail(fs);
    /* A file larger than the stat says: the read is authoritative. */
    memset(big, 'x', sizeof(big));
    big[sizeof(big) - 1u] = '\0';
    fs->nodes[fake_find(fs, PORT_INI)].size = 3u;
    require(manager_storage_read_text(&io, PORT_INI, buffer, sizeof(buffer),
                                      &length) == MANAGER_STORAGE_OK,
            "read_text short stat");
    require(length == 3u, "read_text trusts the read");

    /* find_free_named / newest_named. */
    require(manager_storage_newest_named(&io, "port", ".ini", &index, path,
                                         sizeof(path)) == MANAGER_STORAGE_ABSENT,
            "newest_named none");
    require(index == 0u && path[0] == '\0', "newest_named none clears");
    require(manager_storage_find_free_named(&io, "port", ".ini", path,
                                            sizeof(path), &index) ==
                MANAGER_STORAGE_OK,
            "find_free_named first");
    require(index == 0u && strcmp(path, ARCHIVE("0000")) == 0,
            "find_free_named 0000");
    fake_add_file(fs, ARCHIVE("0000"), "A");
    fake_add_file(fs, ARCHIVE("0002"), "C");
    fake_add_file(fs, RECOVERED("0007"), "Q");
    fake_add_file(fs, MANAGER_STORAGE_BACKUP_ROOT "/port-00x2.ini", "bad");
    fake_add_file(fs, MANAGER_STORAGE_BACKUP_ROOT "/port-0003.txt", "bad");
    fake_add_file(fs, MANAGER_STORAGE_BACKUP_ROOT "/slot1-0004.dat", "save");
    require(manager_storage_newest_named(&io, "port", ".ini", &index, path,
                                         sizeof(path)) == MANAGER_STORAGE_OK,
            "newest_named finds the highest index");
    require(index == 2u && strcmp(path, ARCHIVE("0002")) == 0,
            "newest_named ignores quarantine, malformed and foreign names");
    require(manager_storage_find_free_named(&io, "port", ".ini", path,
                                            sizeof(path), &index) ==
                MANAGER_STORAGE_OK,
            "find_free_named above newest");
    require(index == 3u && strcmp(path, ARCHIVE("0003")) == 0,
            "find_free_named never reuses the 0001 gap");
    require(manager_storage_newest_named(&io, "port-recovered", ".ini", &index,
                                         path, sizeof(path)) ==
                MANAGER_STORAGE_OK,
            "newest_named quarantine prefix");
    require(index == 7u && strcmp(path, RECOVERED("0007")) == 0,
            "newest_named quarantine index");
    fs->pretend_exists_prefix = MANAGER_STORAGE_BACKUP_ROOT "/port-";
    require(manager_storage_find_free_named(&io, "port", ".ini", path,
                                            sizeof(path), &index) ==
                MANAGER_STORAGE_CAPACITY,
            "find_free_named capacity");
    fs->pretend_exists_prefix = NULL;
    require(manager_storage_find_free_named(&io, ".hidden", ".ini", path,
                                            sizeof(path), &index) ==
                MANAGER_STORAGE_INVALID,
            "find_free_named rejects hidden prefix");
    require(manager_storage_find_free_named(&io, "port", "ini", path,
                                            sizeof(path), &index) ==
                MANAGER_STORAGE_INVALID,
            "find_free_named requires a dotted extension");
    require(manager_storage_write_text_atomic(&io, ".rollback", "x", 1u) ==
                MANAGER_STORAGE_INVALID,
            "write_text_atomic rejects hidden names");
    require(manager_storage_write_text_atomic(&io, "noext", "x", 1u) ==
                MANAGER_STORAGE_INVALID,
            "write_text_atomic rejects names without extension");
    require(manager_storage_write_text_atomic(&io, "../port.ini", "x", 1u) ==
                MANAGER_STORAGE_INVALID,
            "write_text_atomic rejects path segments");
    require(manager_storage_write_text_atomic(&io, "port.ini", NULL, 1u) ==
                MANAGER_STORAGE_INVALID,
            "write_text_atomic rejects NULL bytes");
    require(manager_storage_write_text_atomic(
                &io, "port.ini", big, MANAGER_STORAGE_TEXT_MAX_BYTES + 1u) ==
                MANAGER_STORAGE_TOO_LARGE,
            "write_text_atomic caps the size");
}

/* ---- load ---------------------------------------------------------------- */

static void require_defaults(const struct isaac_vita_settings *settings,
                             const char *message)
{
    struct isaac_vita_settings defaults;
    isaac_vita_settings_defaults(&defaults);
    require(isaac_vita_settings_equal(settings, &defaults), message);
}

static void require_port_text(const struct manager_settings_state *state,
                              const char *expected, const char *message)
{
    char text[MANAGER_SETTINGS_TEXT_CAPACITY];
    manager_settings_port_ini_text(state, text, sizeof(text));
    require(strcmp(text, expected) == 0, message);
    require_narrow(text, message);
}

static void test_load(void)
{
    struct fake_fs *fs = &g_fs;
    struct manager_storage_io io = settings_fs_init(fs, 0);
    struct manager_settings_state state;
    static char big[ISAAC_VITA_SETTINGS_MAX_BYTES + 2000u];
    static const char garbage[] = "schema=1\nfps_lock=45\0\n";

    require(manager_settings_load(&io, &state) == MANAGER_STORAGE_ABSENT,
            "load absent result");
    require(state.status == MANAGER_SETTINGS_LOAD_ABSENT, "load absent status");
    require(state.receipt.status == ISAAC_VITA_SETTINGS_STATUS_ABSENT,
            "load absent receipt");
    require_defaults(&state.saved, "load absent saved defaults");
    require_defaults(&state.pending, "load absent pending defaults");
    require(manager_settings_pending_count(&state) == 0u,
            "load absent no pending");
    require(!state.undo_available, "load absent no undo");
    require_port_text(&state, "PORT.INI ABSENT", "absent text");

    fake_add_file(fs, PORT_INI, HAND_45);
    require(manager_settings_load(&io, &state) == MANAGER_STORAGE_OK,
            "load ok result");
    require(state.status == MANAGER_SETTINGS_LOAD_OK, "load ok status");
    require(state.saved.fps_lock == 45u && state.pending.fps_lock == 45u,
            "load ok fps");
    require(state.receipt.lines == 4u && state.ignored == 0u,
            "load ok receipt");
    require_port_text(&state, "PORT.INI OK", "ok text");

    fs->nodes[fake_find(fs, PORT_INI)].size = 0u;
    (void)fake_remove(fs, PORT_INI);
    fake_add_file(fs, PORT_INI,
                  "schema=1\nfps_lock=45\nbogus=1\nfps_lock=99\n"
                  "chord.start_l=rt\n");
    require(manager_settings_load(&io, &state) == MANAGER_STORAGE_OK,
            "load ignored result");
    require(state.status == MANAGER_SETTINGS_LOAD_OK_WITH_IGNORED,
            "load ignored status");
    require(state.saved.fps_lock == 45u, "load ignored keeps the good value");
    require(state.receipt.rejected == 2u && state.receipt.unknown == 1u &&
                state.ignored == 3u,
            "load ignored counts");
    require_port_text(&state, "PORT.INI OK - 3 LINES IGNORED", "ignored text");
    (void)fake_remove(fs, PORT_INI);
    fake_add_file(fs, PORT_INI, "schema=1\nbogus=1\n");
    require(manager_settings_load(&io, &state) == MANAGER_STORAGE_OK,
            "load one ignored");
    require_port_text(&state, "PORT.INI OK - 1 LINE IGNORED",
                      "one ignored text");

    /* Remap guard: the file stays OK, the remap section is shown as default. */
    (void)fake_remove(fs, PORT_INI);
    fake_add_file(fs, PORT_INI, "schema=1\nfps_lock=40\nbutton.cross=x\n");
    require(manager_settings_load(&io, &state) == MANAGER_STORAGE_OK,
            "load guard");
    require(state.status == MANAGER_SETTINGS_LOAD_OK && state.receipt.remap_guard,
            "load guard flagged");
    require(state.saved.fps_lock == 40u &&
                state.saved.button[ISAAC_VITA_SETTINGS_BUTTON_CROSS] ==
                    ISAAC_VITA_SETTINGS_ELEMENT_A,
            "load guard resets the remap only");

    (void)fake_remove(fs, PORT_INI);
    fake_add_file(fs, PORT_INI, "fps_lock=45\n");
    require(manager_settings_load(&io, &state) == MANAGER_STORAGE_OK,
            "load schema result");
    require(state.status == MANAGER_SETTINGS_LOAD_INVALID, "load schema status");
    require(state.receipt.status == ISAAC_VITA_SETTINGS_STATUS_SCHEMA,
            "load schema receipt");
    require_defaults(&state.saved, "load schema defaults");
    require_port_text(&state, "PORT.INI INVALID - DEFAULTS SHOWN",
                      "schema text");

    (void)fake_remove(fs, PORT_INI);
    (void)fake_add_node(fs, PORT_INI, MANAGER_STORAGE_NODE_FILE, garbage,
                        sizeof(garbage) - 1u);
    require(manager_settings_load(&io, &state) == MANAGER_STORAGE_OK,
            "load garbage result");
    require(state.status == MANAGER_SETTINGS_LOAD_INVALID &&
                state.receipt.status == ISAAC_VITA_SETTINGS_STATUS_GARBAGE,
            "load garbage");
    require_defaults(&state.saved, "load garbage defaults");

    memset(big, '#', sizeof(big));
    (void)fake_remove(fs, PORT_INI);
    (void)fake_add_node(fs, PORT_INI, MANAGER_STORAGE_NODE_FILE, big,
                        ISAAC_VITA_SETTINGS_MAX_BYTES + 1u);
    require(manager_settings_load(&io, &state) == MANAGER_STORAGE_OK,
            "load 4097 read");
    require(state.status == MANAGER_SETTINGS_LOAD_INVALID &&
                state.receipt.status == ISAAC_VITA_SETTINGS_STATUS_TOO_LARGE &&
                state.receipt.bytes == ISAAC_VITA_SETTINGS_MAX_BYTES + 1u,
            "load 4097 parser too-large");
    (void)fake_remove(fs, PORT_INI);
    (void)fake_add_node(fs, PORT_INI, MANAGER_STORAGE_NODE_FILE, big,
                        ISAAC_VITA_SETTINGS_MAX_BYTES + 1000u);
    require(manager_settings_load(&io, &state) == MANAGER_STORAGE_TOO_LARGE,
            "load oversize refused by storage");
    require(state.status == MANAGER_SETTINGS_LOAD_INVALID &&
                state.receipt.status == ISAAC_VITA_SETTINGS_STATUS_TOO_LARGE,
            "load oversize too-large");
    require_defaults(&state.saved, "load oversize defaults");
    (void)fake_remove(fs, PORT_INI);
    (void)fake_add_node(fs, PORT_INI, MANAGER_STORAGE_NODE_FILE, big,
                        ISAAC_VITA_SETTINGS_MAX_BYTES);
    require(manager_settings_load(&io, &state) == MANAGER_STORAGE_OK,
            "load 4096 read");
    require(state.status == MANAGER_SETTINGS_LOAD_INVALID &&
                state.receipt.status == ISAAC_VITA_SETTINGS_STATUS_SCHEMA,
            "load 4096 comment-only file is schema, not too-large");

    (void)fake_remove(fs, PORT_INI);
    fake_add_file(fs, PORT_INI, CANON_45);
    fake_fail(fs, FAKE_OP_STAT, 1);
    require(manager_settings_load(&io, &state) == MANAGER_STORAGE_IO_ERROR,
            "load stat failure");
    require(state.status == MANAGER_SETTINGS_LOAD_IO_ERROR &&
                state.receipt.status == ISAAC_VITA_SETTINGS_STATUS_IO_ERROR,
            "load io-error status");
    require_defaults(&state.saved, "load io-error defaults");
    require_port_text(&state, "PORT.INI IO ERROR", "io-error text");
    fake_fail(fs, FAKE_OP_OPEN_READ, 1);
    require(manager_settings_load(&io, &state) == MANAGER_STORAGE_IO_ERROR,
            "load open failure");
    fake_fail(fs, FAKE_OP_READ, 1);
    require(manager_settings_load(&io, &state) == MANAGER_STORAGE_IO_ERROR,
            "load read failure");
    fake_fail(fs, FAKE_OP_CLOSE, 1);
    require(manager_settings_load(&io, &state) == MANAGER_STORAGE_IO_ERROR,
            "load close failure");
    fake_no_fail(fs);
    (void)fake_remove(fs, PORT_INI);
    fake_add_dir(fs, PORT_INI);
    require(manager_settings_load(&io, &state) == MANAGER_STORAGE_CONFLICT,
            "load directory in the way");
    require(state.status == MANAGER_SETTINGS_LOAD_IO_ERROR,
            "load conflict is an io error for the page");
    require(manager_settings_load(NULL, &state) == MANAGER_STORAGE_INVALID,
            "load without io");
    require(state.status == MANAGER_SETTINGS_LOAD_IO_ERROR, "load NULL io");
}

/* ---- pending edits ------------------------------------------------------- */

static void test_pending(void)
{
    struct fake_fs *fs = &g_fs;
    struct manager_storage_io io = settings_fs_init(fs, 0);
    struct manager_settings_state state;

    (void)manager_settings_load(&io, &state);
    require(manager_settings_fps_lock_step(60u, 1) == 45u, "step 60 -> 45");
    require(manager_settings_fps_lock_step(45u, 1) == 40u, "step 45 -> 40");
    require(manager_settings_fps_lock_step(40u, 1) == 30u, "step 40 -> 30");
    require(manager_settings_fps_lock_step(30u, 1) == 30u, "step clamps low");
    require(manager_settings_fps_lock_step(30u, -1) == 40u, "step 30 -> 40");
    require(manager_settings_fps_lock_step(60u, -1) == 60u, "step clamps high");
    require(manager_settings_fps_lock_step(45u, 0) == 45u, "step zero");
    require(manager_settings_fps_lock_step(50u, 1) == 60u, "step out of set");
    require(manager_settings_set_fps_lock(&state, 45u) == 0, "set fps 45");
    require(state.pending.fps_lock == 45u && state.saved.fps_lock == 60u,
            "set fps pending only");
    require(manager_settings_pending_count(&state) == 1u, "one pending row");
    require(manager_settings_set_fps_lock(&state, 50u) == -1, "set fps 50");
    require(state.pending.fps_lock == 45u, "rejected value leaves pending");
    require(manager_settings_set_pending_text(
                &state, ISAAC_VITA_SETTINGS_KEY_STICK_SWAP, "on", 2u) == 0,
            "set text on");
    require(manager_settings_pending_count(&state) == 2u, "two pending rows");
    require(manager_settings_set_pending_text(
                &state, ISAAC_VITA_SETTINGS_KEY_CHORD_START_L, "rt", 2u) == -1,
            "rt refused on a chord key");
    require(manager_settings_pending_count(&state) == 2u,
            "refused edit leaves pending");
    /* render_scale (ROOM RENDER row): 1 <-> 2, clamped. */
    require(manager_settings_render_scale_step(1u, 1) == 2u, "scale 1 -> 2");
    require(manager_settings_render_scale_step(2u, 1) == 2u, "scale clamps high");
    require(manager_settings_render_scale_step(2u, -1) == 1u, "scale 2 -> 1");
    require(manager_settings_render_scale_step(1u, -1) == 1u, "scale clamps low");
    require(manager_settings_render_scale_step(2u, 0) == 2u, "scale step zero");
    require(manager_settings_render_scale_step(7u, 1) == 1u, "scale out of set");
    require(manager_settings_set_render_scale(&state, 2u) == 0, "set scale 2");
    require(state.pending.render_scale == 2u && state.saved.render_scale == 1u,
            "set scale pending only");
    require(manager_settings_pending_count(&state) == 3u, "three pending rows");
    require(manager_settings_set_render_scale(&state, 3u) == -1, "set scale 3");
    require(state.pending.render_scale == 2u, "rejected scale leaves pending");
    require(manager_settings_set_pending_text(
                &state, ISAAC_VITA_SETTINGS_KEY_RENDER_SCALE, "99", 2u) == -1,
            "99 refused on the render_scale key");
    manager_settings_discard(&state);
    require(manager_settings_pending_count(&state) == 0u, "discard");
    require(state.pending.fps_lock == 60u && state.pending.stick_swap == 0u &&
                state.pending.render_scale == 1u,
            "discard restores saved");
    require(manager_settings_set_render_scale(NULL, 2u) == -1, "NULL scale state");
    require(manager_settings_set_fps_lock(NULL, 45u) == -1, "NULL state");
    require(manager_settings_pending_count(NULL) == 0u, "NULL count");
    manager_settings_discard(NULL);
}

/* ---- save ---------------------------------------------------------------- */

/* render_scale=2 serializes right after display_raster and reads back. */
static void test_save_render_scale(void)
{
    struct fake_fs *fs = &g_fs;
    struct manager_storage_io io = settings_fs_init(fs, 0);
    struct manager_settings_state state;
    struct manager_settings_result result;

    (void)manager_settings_load(&io, &state);
    require(manager_settings_set_render_scale(&state, 2u) == 0, "pending scale 2");
    result = manager_settings_save(&io, &state, 0);
    require(result.code == MANAGER_SETTINGS_CODE_OK, "scale save ok");
    require_file(fs, PORT_INI,
                 "schema=1\nfps_lock=60\ndisplay_raster=960\nrender_scale=2\n"
                 CANON_REST,
                 "render_scale=2 after display_raster");
    require(state.saved.render_scale == 2u &&
                manager_settings_pending_count(&state) == 0u,
            "scale reloaded after save");
    require(manager_settings_load(&io, &state) == MANAGER_STORAGE_OK, "reload");
    require(state.saved.render_scale == 2u && state.pending.render_scale == 2u,
            "scale 2 read back");
}

static void test_save(void)
{
    struct fake_fs *fs = &g_fs;
    struct manager_storage_io io = settings_fs_init(fs, 0);
    struct manager_settings_state state;
    struct manager_settings_result result;

    /* Fresh install: no port.ini, no backup root. */
    (void)manager_settings_load(&io, &state);
    require(manager_settings_set_fps_lock(&state, 45u) == 0, "pending 45");
    result = manager_settings_save(&io, &state, 0);
    require_result(&result, MANAGER_SETTINGS_ACTION_SAVE,
                   MANAGER_SETTINGS_CODE_OK, "SAVE OK", "first save");
    require_file(fs, PORT_INI, CANON_45, "first save canonical bytes");
    require(fake_exists(fs, MANAGER_STORAGE_BACKUP_ROOT),
            "first save creates the backup root");
    require(fake_count_containing(fs, "/port-") == 0,
            "first save has nothing to archive");
    require_no_inflight(fs, "first save sidecars");
    require(state.status == MANAGER_SETTINGS_LOAD_OK &&
                state.saved.fps_lock == 45u &&
                manager_settings_pending_count(&state) == 0u,
            "first save reloads");
    require(!state.undo_available, "first save offers no undo");

    /* Second save archives the previous version under 0000. */
    require(manager_settings_set_fps_lock(&state, 30u) == 0, "pending 30");
    result = manager_settings_save(&io, &state, 0);
    require_result(&result, MANAGER_SETTINGS_ACTION_SAVE,
                   MANAGER_SETTINGS_CODE_OK, "SAVE OK", "second save");
    require_file(fs, PORT_INI, CANON_30, "second save bytes");
    require_file(fs, ARCHIVE("0000"), CANON_45, "second save archive");
    require_no_inflight(fs, "second save sidecars");
    require(state.undo_available && state.undo_index == 0u,
            "second save offers undo of 0000");

    /* A no-change save still writes and archives; undo then hides. */
    result = manager_settings_save(&io, &state, 0);
    require_result(&result, MANAGER_SETTINGS_ACTION_SAVE,
                   MANAGER_SETTINGS_CODE_OK, "SAVE OK", "no-change save");
    require_file(fs, ARCHIVE("0001"), CANON_30, "no-change save archive");
    require(!state.undo_available, "equal hashes hide undo");

    /* Hand-written original: archived verbatim, live file canonical. */
    io = settings_fs_init(fs, 1);
    fake_add_file(fs, PORT_INI, HAND_45);
    (void)manager_settings_load(&io, &state);
    require(state.saved.fps_lock == 45u, "hand file loads");
    require(manager_settings_set_fps_lock(&state, 40u) == 0, "pending 40");
    result = manager_settings_save(&io, &state, 0);
    require_result(&result, MANAGER_SETTINGS_ACTION_SAVE,
                   MANAGER_SETTINGS_CODE_OK, "SAVE OK", "hand save");
    require_file(fs, PORT_INI, "schema=1\nfps_lock=40\n" CANON_TAIL,
                 "hand save canonical");
    require_file(fs, ARCHIVE("0000"), HAND_45, "hand original archived verbatim");

    /* Rewrite of an INVALID file: SAVE is the repair. */
    io = settings_fs_init(fs, 1);
    fake_add_file(fs, PORT_INI, "no schema here\n");
    (void)manager_settings_load(&io, &state);
    require(state.status == MANAGER_SETTINGS_LOAD_INVALID, "invalid loads");
    result = manager_settings_save(&io, &state, 0);
    require_result(&result, MANAGER_SETTINGS_ACTION_SAVE,
                   MANAGER_SETTINGS_CODE_OK, "SAVE OK", "repair save");
    require_file(fs, PORT_INI, CANON_60, "repair save writes defaults");
    require_file(fs, ARCHIVE("0000"), "no schema here\n",
                 "repair save archives the bad file");
    require(state.status == MANAGER_SETTINGS_LOAD_OK, "repair save reloads OK");

    /* Post-archive sync failure is a warning; the new file stands. */
    io = settings_fs_init(fs, 1);
    fake_add_file(fs, PORT_INI, CANON_45);
    (void)manager_settings_load(&io, &state);
    require(manager_settings_set_fps_lock(&state, 30u) == 0, "pending 30 again");
    /* recover (1) + step 4 (2) + step 5 (3) + post-archive (4). */
    fake_fail(fs, FAKE_OP_SYNC_DEVICE, 4);
    result = manager_settings_save(&io, &state, 0);
    fake_no_fail(fs);
    require_result(&result, MANAGER_SETTINGS_ACTION_SAVE,
                   MANAGER_SETTINGS_CODE_SYNC_WARNING, "SAVE OK - SYNC WARNING",
                   "sync warning");
    require_file(fs, PORT_INI, CANON_30, "sync warning keeps the new file");
    require_file(fs, ARCHIVE("0000"), CANON_45, "sync warning archived");
}

static void test_save_refusals(void)
{
    struct fake_fs *fs = &g_fs;
    struct manager_storage_io io = settings_fs_init(fs, 1);
    struct manager_settings_state state;
    struct manager_settings_result result;
    size_t node_count;
    int occurrence;

    fake_add_file(fs, PORT_INI, CANON_45);
    (void)manager_settings_load(&io, &state);
    require(manager_settings_set_fps_lock(&state, 30u) == 0, "pending 30");
    node_count = fs->node_count;
    memset(fs->operation_counts, 0, sizeof(fs->operation_counts));

    result = manager_settings_save(&io, &state, 1);
    require_result(&result, MANAGER_SETTINGS_ACTION_SAVE,
                   MANAGER_SETTINGS_CODE_FTP_LISTENING,
                   "STOP FTP BEFORE LOCAL CHANGES", "ftp refusal");
    require(fs->node_count == node_count && fs->operation_counts[FAKE_OP_STAT] == 0 &&
                fs->operation_counts[FAKE_OP_OPEN_WRITE] == 0,
            "ftp refusal touches nothing");
    require(state.pending.fps_lock == 30u, "ftp refusal keeps pending");
    result = manager_settings_defaults(&io, &state, 1);
    require_result(&result, MANAGER_SETTINGS_ACTION_DEFAULTS,
                   MANAGER_SETTINGS_CODE_FTP_LISTENING,
                   "STOP FTP BEFORE LOCAL CHANGES", "ftp refusal defaults");
    result = manager_settings_undo(&io, &state, 1);
    require_result(&result, MANAGER_SETTINGS_ACTION_UNDO,
                   MANAGER_SETTINGS_CODE_FTP_LISTENING,
                   "STOP FTP BEFORE LOCAL CHANGES", "ftp refusal undo");
    require(fs->operation_counts[FAKE_OP_STAT] == 0, "ftp refusals never stat");
    require_file(fs, PORT_INI, CANON_45, "ftp refusal leaves the file");

    /* Guard: no physical button on a. */
    require(manager_settings_set_pending_text(
                &state, ISAAC_VITA_SETTINGS_KEY_BUTTON_CROSS, "x", 1u) == 0,
            "cross -> x");
    result = manager_settings_save(&io, &state, 0);
    require_result(&result, MANAGER_SETTINGS_ACTION_SAVE,
                   MANAGER_SETTINGS_CODE_NEED_A_AND_B,
                   "SAVE REFUSED - NEED A AND B", "guard refusal");
    require_file(fs, PORT_INI, CANON_45, "guard refusal leaves the file");
    require(manager_settings_set_pending_text(
                &state, ISAAC_VITA_SETTINGS_KEY_BUTTON_CROSS, "a", 1u) == 0,
            "cross -> a");

    /* Unserializable pending value. */
    state.pending.fps_lock = 50u;
    result = manager_settings_save(&io, &state, 0);
    require_result(&result, MANAGER_SETTINGS_ACTION_SAVE,
                   MANAGER_SETTINGS_CODE_BAD_FILE, "SAVE FAILED - BAD FILE",
                   "bad file refusal");
    require_file(fs, PORT_INI, CANON_45, "bad file refusal leaves the file");
    state.pending.fps_lock = 30u;

    /* Every archive index taken. */
    fs->pretend_exists_prefix = MANAGER_STORAGE_BACKUP_ROOT "/port-";
    result = manager_settings_save(&io, &state, 0);
    fs->pretend_exists_prefix = NULL;
    require_result(&result, MANAGER_SETTINGS_ACTION_SAVE,
                   MANAGER_SETTINGS_CODE_NO_FREE_BACKUP_SLOT,
                   "SAVE FAILED - NO FREE BACKUP SLOT", "capacity refusal");
    require_file(fs, PORT_INI, CANON_45, "capacity refusal leaves the file");
    require_no_inflight(fs, "capacity refusal writes nothing");

    /* Exclusive open fails with no temp in the way (ENOSPC, EIO, a lost
     * descriptor on the device): classified by the stat that follows as an
     * I/O error, not as a FILE CONFLICT (step 1 has just removed any stale
     * temp, so a genuine EEXIST is the least likely cause here). */
    fake_fail(fs, FAKE_OP_OPEN_WRITE, 1);
    result = manager_settings_save(&io, &state, 0);
    fake_no_fail(fs);
    require_result(&result, MANAGER_SETTINGS_ACTION_SAVE,
                   MANAGER_SETTINGS_CODE_IO_ERROR,
                   "SAVE FAILED - IO ERROR", "exclusive open failure");
    require_file(fs, PORT_INI, CANON_45, "open failure leaves the file");
    require_no_inflight(fs, "open failure leaves no sidecar");

    fake_fail(fs, FAKE_OP_WRITE, 1);
    result = manager_settings_save(&io, &state, 0);
    fake_no_fail(fs);
    require_result(&result, MANAGER_SETTINGS_ACTION_SAVE,
                   MANAGER_SETTINGS_CODE_IO_ERROR, "SAVE FAILED - IO ERROR",
                   "write failure");
    require_file(fs, PORT_INI, CANON_45, "write failure leaves the file");
    require_no_inflight(fs, "write failure removes the temp");

    /* Readback corruption of the temp: nothing promoted. */
    fs->corrupt_read_path_substring = "port.ini.tmp";
    result = manager_settings_save(&io, &state, 0);
    fs->corrupt_read_path_substring = NULL;
    require_result(&result, MANAGER_SETTINGS_ACTION_SAVE,
                   MANAGER_SETTINGS_CODE_HASH_MISMATCH,
                   "SAVE FAILED - HASH MISMATCH", "readback corruption");
    require_file(fs, PORT_INI, CANON_45, "corruption leaves the file");
    require_no_inflight(fs, "corruption removes the temp");
    require(fake_count_containing(fs, "/port-") == 0,
            "refusals never archive");
    require(state.pending.fps_lock == 30u && state.saved.fps_lock == 45u,
            "refusals keep the pending edit");
    /* Corruption of the promoted file: rolled back to the old bytes. */
    fs->corrupt_read_path_substring = "isaacr001/port.ini";
    result = manager_settings_save(&io, &state, 0);
    fs->corrupt_read_path_substring = NULL;
    require(result.code == MANAGER_SETTINGS_CODE_HASH_MISMATCH,
            "promoted corruption detected");
    require_file(fs, PORT_INI, CANON_45, "promoted corruption rolled back");
    require_no_inflight(fs, "promoted corruption cleans up");

    /* Transient failure of the re-read after a verified write (open_read
     * #3: the two hash readbacks precede it): the result stays SAVE OK and
     * the page keeps the values it just wrote instead of falling into the
     * IO ERROR defaults; UNDO was still refreshed by the failed load. */
    fake_fail(fs, FAKE_OP_OPEN_READ, 3);
    result = manager_settings_save(&io, &state, 0);
    fake_no_fail(fs);
    require_result(&result, MANAGER_SETTINGS_ACTION_SAVE,
                   MANAGER_SETTINGS_CODE_OK, "SAVE OK", "re-read failure save");
    require_file(fs, PORT_INI, CANON_30, "re-read failure: file written");
    require(state.status == MANAGER_SETTINGS_LOAD_OK,
            "re-read failure keeps the OK state");
    require(state.saved.fps_lock == 30u && state.pending.fps_lock == 30u,
            "re-read failure adopts the written values");
    require(manager_settings_pending_count(&state) == 0u,
            "re-read failure leaves nothing pending");
    require(state.undo_available && state.undo_index == 0u,
            "re-read failure still refreshes undo");
    require_file(fs, ARCHIVE("0000"), CANON_45, "re-read failure archived");
    (void)manager_settings_load(&io, &state);
    require(state.status == MANAGER_SETTINGS_LOAD_OK &&
                state.saved.fps_lock == 30u,
            "fresh load agrees with the adopted values");

    /* The transient hits one of refresh_undo's reads instead (open_read #4
     * = the newest archive, #5 = the live file): the write and the re-read
     * succeeded, so SAVE OK with the written values; the UNDO row may stay
     * hidden until the page is reopened, and a fresh load offers it. */
    for (occurrence = 4; occurrence <= 5; ++occurrence) {
        require(manager_settings_set_fps_lock(&state, occurrence == 4 ? 45u : 60u) == 0,
                "pending before the undo-read fault");
        fake_fail(fs, FAKE_OP_OPEN_READ, occurrence);
        result = manager_settings_save(&io, &state, 0);
        fake_no_fail(fs);
        require_result(&result, MANAGER_SETTINGS_ACTION_SAVE,
                       MANAGER_SETTINGS_CODE_OK, "SAVE OK",
                       "undo-read fault save");
        require_file(fs, PORT_INI, occurrence == 4 ? CANON_45 : CANON_60,
                     "undo-read fault: file written");
        require(state.status == MANAGER_SETTINGS_LOAD_OK &&
                    state.saved.fps_lock == (occurrence == 4 ? 45u : 60u) &&
                    manager_settings_pending_count(&state) == 0u,
                "undo-read fault: state follows the file");
        require_no_inflight(fs, "undo-read fault: no sidecar");
        (void)manager_settings_load(&io, &state);
        require(state.undo_available &&
                    state.undo_index == (occurrence == 4 ? 1u : 2u),
                "undo-read fault: fresh load offers undo");
    }
}

/* ---- fault matrix -------------------------------------------------------- */

struct write_scenario {
    const char *name;
    const char *old_text; /* NULL = no port.ini */
    uint16_t old_fps;
    int backup_root;
    const char *prior_archive; /* NULL = none; else ARCHIVE("0000") */
};

static void setup_write_scenario(struct fake_fs *fs,
                                 struct manager_storage_io *io,
                                 const struct write_scenario *scenario)
{
    *io = settings_fs_init(fs, scenario->backup_root);
    if (scenario->prior_archive != NULL)
        fake_add_file(fs, ARCHIVE("0000"), scenario->prior_archive);
    if (scenario->old_text != NULL)
        fake_add_file(fs, PORT_INI, scenario->old_text);
}

static void require_scenario_state(struct fake_fs *fs,
                                   struct manager_storage_io *io,
                                   const struct write_scenario *scenario,
                                   int wrote_new, const char *new_text,
                                   uint16_t new_fps)
{
    struct manager_settings_state state;
    const char *next_archive = scenario->prior_archive != NULL
                                   ? ARCHIVE("0001")
                                   : ARCHIVE("0000");
    require(!(fake_exists(fs, PORT_ROLLBACK) && fake_exists(fs, PORT_TMP)),
            scenario->name);
    if (wrote_new) {
        require_file(fs, PORT_INI, new_text, scenario->name);
        if (scenario->old_text != NULL)
            require_file(fs, next_archive, scenario->old_text, scenario->name);
        else
            require_absent(fs, next_archive, scenario->name);
    } else {
        if (scenario->old_text != NULL)
            require_file(fs, PORT_INI, scenario->old_text, scenario->name);
        else
            require_absent(fs, PORT_INI, scenario->name);
        require_absent(fs, next_archive, scenario->name);
    }
    if (scenario->prior_archive != NULL)
        require_file(fs, ARCHIVE("0000"), scenario->prior_archive,
                     scenario->name);
    require(fake_count_containing(fs, "port-recovered") == 0, scenario->name);
    /* The next manager start repairs whatever the fault left, and keeps
     * doing nothing afterwards. */
    require(manager_storage_recover(io) == MANAGER_STORAGE_OK, scenario->name);
    require_no_inflight(fs, scenario->name);
    if (wrote_new)
        require_file(fs, PORT_INI, new_text, scenario->name);
    else if (scenario->old_text != NULL)
        require_file(fs, PORT_INI, scenario->old_text, scenario->name);
    else
        require_absent(fs, PORT_INI, scenario->name);
    (void)manager_settings_load(io, &state);
    if (wrote_new || scenario->old_text != NULL) {
        require(state.status == MANAGER_SETTINGS_LOAD_OK, scenario->name);
        require(state.saved.fps_lock == (wrote_new ? new_fps : scenario->old_fps),
                scenario->name);
    } else {
        require(state.status == MANAGER_SETTINGS_LOAD_ABSENT, scenario->name);
    }
    require(manager_storage_recover(io) == MANAGER_STORAGE_OK, scenario->name);
    require_no_inflight(fs, scenario->name);
}

static void run_write_fault_matrix(const struct write_scenario *scenario)
{
    struct fake_fs *fs = &g_fs;
    enum fake_operation operation;
    for (operation = FAKE_OP_STAT; operation < FAKE_OP_COUNT; ++operation) {
        int occurrence;
        for (occurrence = 1;; ++occurrence) {
            struct manager_storage_io io;
            int result;
            int total;
            setup_write_scenario(fs, &io, scenario);
            fake_fail(fs, operation, occurrence);
            result = manager_storage_write_text_atomic(
                &io, MANAGER_STORAGE_PORT_INI_NAME, CANON_30,
                sizeof(CANON_30) - 1u);
            total = fs->operation_counts[operation];
            fake_no_fail(fs);
            if (total < occurrence) {
                require(result >= 0, "clean write succeeds");
                require_scenario_state(fs, &io, scenario, 1, CANON_30, 30u);
                break;
            }
            require_scenario_state(fs, &io, scenario, result >= 0, CANON_30,
                                   30u);
        }
        require(occurrence > 1 || operation == FAKE_OP_REMOVE ||
                    (operation == FAKE_OP_MKDIR && scenario->backup_root),
                "every file operation of the write path is exercised");
    }
    /* The same matrix through manager_settings_save: the result code is OK
     * exactly when the new file stands, and a failure keeps the edit. */
    for (operation = FAKE_OP_STAT; operation < FAKE_OP_COUNT; ++operation) {
        int occurrence;
        for (occurrence = 1;; ++occurrence) {
            struct manager_storage_io io;
            struct manager_settings_state state;
            struct manager_settings_result result;
            int total;
            setup_write_scenario(fs, &io, scenario);
            (void)manager_settings_load(&io, &state);
            require(manager_settings_set_fps_lock(&state, 30u) == 0,
                    scenario->name);
            fake_fail(fs, operation, occurrence);
            result = manager_settings_save(&io, &state, 0);
            total = fs->operation_counts[operation];
            fake_no_fail(fs);
            if (total < occurrence) {
                require(manager_settings_result_ok(&result), scenario->name);
                break;
            }
            if (manager_settings_result_ok(&result)) {
                require_file(fs, PORT_INI, CANON_30, scenario->name);
            } else {
                char text[MANAGER_SETTINGS_TEXT_CAPACITY];
                if (scenario->old_text != NULL)
                    require_file(fs, PORT_INI, scenario->old_text,
                                 scenario->name);
                else
                    require_absent(fs, PORT_INI, scenario->name);
                require(state.pending.fps_lock == 30u, scenario->name);
                manager_settings_result_text(&result, text, sizeof(text));
                require_narrow(text, scenario->name);
                require(strncmp(text, "SAVE FAILED - ", 14u) == 0,
                        scenario->name);
            }
            require(!(fake_exists(fs, PORT_ROLLBACK) && fake_exists(fs, PORT_TMP)),
                    scenario->name);
        }
    }
}

static void test_write_faults(void)
{
    static const struct write_scenario scenarios[] = {
        {"old file, backup root, prior archive", CANON_45, 45u, 1, CANON_60},
        {"old hand file, backup root", HAND_45, 45u, 1, NULL},
        {"old file, no backup root", CANON_45, 45u, 0, NULL},
        {"no file, no backup root", NULL, 0u, 0, NULL},
        {"no file, backup root, prior archive", NULL, 0u, 1, CANON_60},
    };
    size_t index;
    for (index = 0u; index < sizeof(scenarios) / sizeof(scenarios[0]); ++index)
        run_write_fault_matrix(&scenarios[index]);
}

/* ---- recover_port -------------------------------------------------------- */

static void test_recover_port(void)
{
    struct fake_fs *fs = &g_fs;
    struct manager_storage_io io;

    /* Rollback only: the old settings come back. */
    io = settings_fs_init(fs, 1);
    fake_add_file(fs, PORT_ROLLBACK, CANON_45);
    require(manager_storage_recover_port(&io) == MANAGER_STORAGE_OK,
            "recover rollback only");
    require_file(fs, PORT_INI, CANON_45, "rollback promoted");
    require_no_inflight(fs, "rollback only cleans");
    require(fake_count_containing(fs, "port-recovered") == 0,
            "rollback only quarantines nothing");

    /* Rollback + new port.ini (power loss between steps 5 and 6): old wins,
     * the new content is quarantined, nothing is lost. */
    io = settings_fs_init(fs, 1);
    fake_add_file(fs, PORT_ROLLBACK, CANON_45);
    fake_add_file(fs, PORT_INI, CANON_30);
    require(manager_storage_recover_port(&io) == MANAGER_STORAGE_OK,
            "recover rollback and target");
    require_file(fs, PORT_INI, CANON_45, "old settings win");
    require_file(fs, RECOVERED("0000"), CANON_30, "new content quarantined");
    require_no_inflight(fs, "rollback and target cleans");
    io = fake_io(fs);
    fake_add_file(fs, PORT_ROLLBACK, CANON_60);
    require(manager_storage_recover_port(&io) == MANAGER_STORAGE_OK,
            "recover second quarantine");
    require_file(fs, PORT_INI, CANON_60, "second rollback promoted");
    require_file(fs, RECOVERED("0001"), CANON_45, "second quarantine index");

    /* Stale tmp only: removed, the live file untouched. */
    io = settings_fs_init(fs, 1);
    fake_add_file(fs, PORT_INI, CANON_45);
    fake_add_file(fs, PORT_TMP, "half written");
    require(manager_storage_recover_port(&io) == MANAGER_STORAGE_OK,
            "recover stale tmp");
    require_file(fs, PORT_INI, CANON_45, "stale tmp leaves the file");
    require_no_inflight(fs, "stale tmp removed");

    /* All three at once, without a backup root (mkdir path). */
    io = settings_fs_init(fs, 0);
    fake_add_file(fs, PORT_ROLLBACK, CANON_45);
    fake_add_file(fs, PORT_INI, CANON_30);
    fake_add_file(fs, PORT_TMP, "half written");
    require(manager_storage_recover(&io) == MANAGER_STORAGE_OK,
            "recover folded into manager_storage_recover");
    require_file(fs, PORT_INI, CANON_45, "folded recover promotes");
    require_file(fs, RECOVERED("0000"), CANON_30, "folded recover quarantines");
    require_no_inflight(fs, "folded recover cleans");
    require(manager_storage_recover(&io) == MANAGER_STORAGE_OK,
            "recover idempotent");
    require_file(fs, PORT_INI, CANON_45, "recover idempotent file");
    require(fake_count_containing(fs, "port-recovered") == 1,
            "recover idempotent quarantine");

    /* Nothing to do: no writes at all. */
    io = settings_fs_init(fs, 1);
    fake_add_file(fs, PORT_INI, CANON_45);
    fake_add_file(fs, ARCHIVE("0000"), CANON_60);
    memset(fs->operation_counts, 0, sizeof(fs->operation_counts));
    require(manager_storage_recover_port(&io) == MANAGER_STORAGE_OK,
            "recover clean");
    require(fs->operation_counts[FAKE_OP_RENAME] == 0 &&
                fs->operation_counts[FAKE_OP_REMOVE] == 0 &&
                fs->operation_counts[FAKE_OP_OPEN_WRITE] == 0,
            "recover clean issues no rename/remove/write");
    require_file(fs, ARCHIVE("0000"), CANON_60, "recover never touches archives");

    /* Faults during recovery leave both files in place. */
    io = settings_fs_init(fs, 1);
    fake_add_file(fs, PORT_ROLLBACK, CANON_45);
    fake_add_file(fs, PORT_INI, CANON_30);
    fake_fail(fs, FAKE_OP_RENAME, 2);
    require(manager_storage_recover_port(&io) == MANAGER_STORAGE_IO_ERROR,
            "recover promotion rename failure");
    fake_no_fail(fs);
    require_file(fs, PORT_INI, CANON_30, "failed recover restores the target");
    require_file(fs, PORT_ROLLBACK, CANON_45, "failed recover keeps the marker");
    require(manager_storage_recover_port(&io) == MANAGER_STORAGE_OK,
            "recover retry");
    require_file(fs, PORT_INI, CANON_45, "recover retry promotes");

    /* A directory where the marker should be is a conflict, not a promotion. */
    io = settings_fs_init(fs, 1);
    fake_add_dir(fs, PORT_ROLLBACK);
    fake_add_file(fs, PORT_INI, CANON_30);
    require(manager_storage_recover_port(&io) == MANAGER_STORAGE_CONFLICT,
            "recover marker conflict");
    require_file(fs, PORT_INI, CANON_30, "conflict leaves the file");

    /* A failing save slot (a directory where its rollback sidecar belongs)
     * is reported by manager_storage_recover, but port.ini recovery still
     * runs (design 1.3 detail / 9: at every manager start and return), so
     * the page never shows the unpromoted new file. */
    io = settings_fs_init(fs, 1);
    fake_add_dir(fs, SLOT2_ROLLBACK);
    fake_add_file(fs, PORT_ROLLBACK, CANON_45);
    fake_add_file(fs, PORT_INI, CANON_30);
    fake_add_file(fs, PORT_TMP, "half written");
    require(manager_storage_recover(&io) == MANAGER_STORAGE_CONFLICT,
            "slot conflict is reported");
    require_file(fs, PORT_INI, CANON_45, "slot conflict: rollback promoted");
    require_file(fs, RECOVERED("0000"), CANON_30,
                 "slot conflict: new content quarantined");
    require_no_inflight(fs, "slot conflict: port sidecars cleaned");
    require(fake_exists(fs, SLOT2_ROLLBACK), "slot conflict: slot untouched");
    require(manager_storage_recover(&io) == MANAGER_STORAGE_CONFLICT,
            "slot conflict persists");
    require_file(fs, PORT_INI, CANON_45, "slot conflict: recover idempotent");
    require(fake_count_containing(fs, "port-recovered") == 1,
            "slot conflict: one quarantine");
}

/* ---- defaults ------------------------------------------------------------ */

static void test_defaults(void)
{
    struct fake_fs *fs = &g_fs;
    struct manager_storage_io io = settings_fs_init(fs, 0);
    struct manager_settings_state state;
    struct manager_settings_result result;

    fake_add_file(fs, PORT_INI, HAND_45);
    (void)manager_settings_load(&io, &state);
    result = manager_settings_defaults(&io, &state, 0);
    require_result(&result, MANAGER_SETTINGS_ACTION_DEFAULTS,
                   MANAGER_SETTINGS_CODE_OK, "DEFAULTS OK", "defaults");
    require_absent(fs, PORT_INI, "defaults removes port.ini");
    require_file(fs, ARCHIVE("0000"), HAND_45, "defaults archives verbatim");
    require_no_inflight(fs, "defaults leaves no sibling");
    require(state.status == MANAGER_SETTINGS_LOAD_ABSENT, "defaults reloads");
    require_defaults(&state.saved, "defaults saved");
    require(state.undo_available && state.undo_index == 0u,
            "defaults offers undo");
    require(manager_storage_recover(&io) == MANAGER_STORAGE_OK,
            "recover after defaults");
    require(manager_storage_recover(&io) == MANAGER_STORAGE_OK,
            "second recover after defaults");
    require_absent(fs, PORT_INI, "defaults stable across recover");
    require_file(fs, ARCHIVE("0000"), HAND_45, "archive stable across recover");
    require(fake_count_containing(fs, "port-recovered") == 0,
            "defaults never quarantines");

    result = manager_settings_defaults(&io, &state, 0);
    require_result(&result, MANAGER_SETTINGS_ACTION_DEFAULTS,
                   MANAGER_SETTINGS_CODE_NO_FILE, "DEFAULTS OK - NO PORT.INI",
                   "defaults twice");
    require(fake_count_containing(fs, "/port-") == 1,
            "defaults twice archives nothing new");

    /* Capacity and rename failure leave the file. */
    io = settings_fs_init(fs, 1);
    fake_add_file(fs, PORT_INI, CANON_45);
    (void)manager_settings_load(&io, &state);
    fs->pretend_exists_prefix = MANAGER_STORAGE_BACKUP_ROOT "/port-";
    result = manager_settings_defaults(&io, &state, 0);
    fs->pretend_exists_prefix = NULL;
    require_result(&result, MANAGER_SETTINGS_ACTION_DEFAULTS,
                   MANAGER_SETTINGS_CODE_NO_FREE_BACKUP_SLOT,
                   "DEFAULTS FAILED - NO FREE BACKUP SLOT", "defaults capacity");
    require_file(fs, PORT_INI, CANON_45, "defaults capacity leaves the file");
    fake_fail(fs, FAKE_OP_RENAME, 1);
    result = manager_settings_defaults(&io, &state, 0);
    fake_no_fail(fs);
    require_result(&result, MANAGER_SETTINGS_ACTION_DEFAULTS,
                   MANAGER_SETTINGS_CODE_IO_ERROR, "DEFAULTS FAILED - IO ERROR",
                   "defaults rename failure");
    require_file(fs, PORT_INI, CANON_45, "defaults failure leaves the file");
    require(state.saved.fps_lock == 45u, "defaults failure keeps the state");
    /* Sync failure after the rename is a warning; the file is archived. */
    fake_fail(fs, FAKE_OP_SYNC_DEVICE, 2);
    result = manager_settings_defaults(&io, &state, 0);
    fake_no_fail(fs);
    require_result(&result, MANAGER_SETTINGS_ACTION_DEFAULTS,
                   MANAGER_SETTINGS_CODE_SYNC_WARNING,
                   "DEFAULTS OK - SYNC WARNING", "defaults sync warning");
    require_absent(fs, PORT_INI, "defaults sync warning archived");
    require_file(fs, ARCHIVE("0000"), CANON_45, "defaults sync warning archive");
}

/* ---- undo ---------------------------------------------------------------- */

static void test_undo(void)
{
    struct fake_fs *fs = &g_fs;
    struct manager_storage_io io = settings_fs_init(fs, 0);
    struct manager_settings_state state;
    struct manager_settings_result result;
    static const char bad_archive[] = "schema=1\nfps\0lock=45\n";
    static char big[ISAAC_VITA_SETTINGS_MAX_BYTES + 1000u];

    /* No archive: hidden and refused. */
    fake_add_file(fs, PORT_INI, HAND_45);
    (void)manager_settings_load(&io, &state);
    require(!state.undo_available, "no archive hides undo");
    result = manager_settings_undo(&io, &state, 0);
    require_result(&result, MANAGER_SETTINGS_ACTION_UNDO,
                   MANAGER_SETTINGS_CODE_NOTHING_TO_UNDO,
                   "UNDO REFUSED - NOTHING TO UNDO", "undo without archive");

    /* SAVE 30 archives the hand file; UNDO brings its exact bytes back and
     * archives the canonical 30; UNDO again redoes. */
    require(manager_settings_set_fps_lock(&state, 30u) == 0, "pending 30");
    result = manager_settings_save(&io, &state, 0);
    require(manager_settings_result_ok(&result), "save before undo");
    require(state.undo_available && state.undo_index == 0u, "undo offered");
    result = manager_settings_undo(&io, &state, 0);
    require_result(&result, MANAGER_SETTINGS_ACTION_UNDO,
                   MANAGER_SETTINGS_CODE_OK, "UNDO OK", "undo");
    require_file(fs, PORT_INI, HAND_45, "undo restores the exact bytes");
    require_file(fs, ARCHIVE("0000"), HAND_45, "undo keeps the archive");
    require_file(fs, ARCHIVE("0001"), CANON_30, "undo archives the current");
    require_no_inflight(fs, "undo sidecars");
    require(state.saved.fps_lock == 45u && state.status == MANAGER_SETTINGS_LOAD_OK,
            "undo reloads");
    require(state.undo_available && state.undo_index == 1u, "redo offered");
    result = manager_settings_undo(&io, &state, 0);
    require_result(&result, MANAGER_SETTINGS_ACTION_UNDO,
                   MANAGER_SETTINGS_CODE_OK, "UNDO OK", "redo");
    require_file(fs, PORT_INI, CANON_30, "redo restores 30");
    require_file(fs, ARCHIVE("0002"), HAND_45, "redo archives the hand file");
    require(state.saved.fps_lock == 30u, "redo reloads");
    require(fake_count_containing(fs, "/port-") == 3, "archives are never deleted");

    /* Equal hashes: hidden and refused (newest archive == live file). */
    result = manager_settings_save(&io, &state, 0);
    require(manager_settings_result_ok(&result), "no-change save");
    require_file(fs, ARCHIVE("0003"), CANON_30, "no-change save archive");
    require(!state.undo_available, "equal hashes hide undo");
    result = manager_settings_undo(&io, &state, 0);
    require_result(&result, MANAGER_SETTINGS_ACTION_UNDO,
                   MANAGER_SETTINGS_CODE_NOTHING_TO_UNDO,
                   "UNDO REFUSED - NOTHING TO UNDO", "undo equal hashes");
    require_file(fs, PORT_INI, CANON_30, "refused undo leaves the file");
    require(fake_count_containing(fs, "/port-") == 4, "refused undo archives nothing");

    /* UNDO after DEFAULTS restores the archived settings; nothing to
     * archive because port.ini was absent. */
    result = manager_settings_defaults(&io, &state, 0);
    require(manager_settings_result_ok(&result), "defaults before undo");
    require_file(fs, ARCHIVE("0004"), CANON_30, "defaults archive");
    require(state.undo_available && state.undo_index == 4u,
            "undo offered after defaults");
    result = manager_settings_undo(&io, &state, 0);
    require_result(&result, MANAGER_SETTINGS_ACTION_UNDO,
                   MANAGER_SETTINGS_CODE_OK, "UNDO OK", "undo defaults");
    require_file(fs, PORT_INI, CANON_30, "undo defaults restores");
    require(fake_count_containing(fs, "/port-") == 5,
            "undo of an absent file archives nothing");
    require(!state.undo_available, "restored state offers no no-op undo");

    /* Bad archive: offered (it differs from the live file; the
     * detail hides UNDO only for none / equal hashes), refused on the
     * press, live file and archives untouched. */
    io = settings_fs_init(fs, 1);
    fake_add_file(fs, PORT_INI, CANON_45);
    (void)fake_add_node(fs, ARCHIVE("0000"), MANAGER_STORAGE_NODE_FILE,
                        bad_archive, sizeof(bad_archive) - 1u);
    (void)manager_settings_load(&io, &state);
    require(state.undo_available && state.undo_index == 0u,
            "bad archive is offered");
    result = manager_settings_undo(&io, &state, 0);
    require_result(&result, MANAGER_SETTINGS_ACTION_UNDO,
                   MANAGER_SETTINGS_CODE_BAD_ARCHIVE,
                   "UNDO REFUSED - BAD ARCHIVE", "undo bad archive");
    require_file(fs, PORT_INI, CANON_45, "bad archive leaves the file");
    require(fake_count_containing(fs, "/port-") == 1,
            "refused bad archive archives nothing");
    require_no_inflight(fs, "refused bad archive leaves no sidecar");
    (void)fake_remove(fs, ARCHIVE("0000"));
    fake_add_file(fs, ARCHIVE("0000"), "fps_lock=30\n");
    (void)manager_settings_load(&io, &state);
    require(state.undo_available, "schema-less archive is offered");
    result = manager_settings_undo(&io, &state, 0);
    require(result.code == MANAGER_SETTINGS_CODE_BAD_ARCHIVE,
            "schema-less archive refused");
    require_file(fs, PORT_INI, CANON_45, "schema-less archive leaves the file");

    /* Oversized newest archive: a port.ini larger than the read buffer
     * loads INVALID (storage TOO_LARGE), X rewrites it and step 6 archives
     * the oversized bytes verbatim.  No hash to compare, so the row stays
     * (the older good archive would otherwise become unreachable without a
     * trace) and the press refuses with BAD ARCHIVE. */
    memset(big, '#', sizeof(big));
    io = settings_fs_init(fs, 1);
    fake_add_file(fs, ARCHIVE("0000"), CANON_60);
    (void)fake_add_node(fs, PORT_INI, MANAGER_STORAGE_NODE_FILE, big,
                        sizeof(big));
    require(manager_settings_load(&io, &state) == MANAGER_STORAGE_TOO_LARGE,
            "oversized live file refused by storage");
    require(state.status == MANAGER_SETTINGS_LOAD_INVALID,
            "oversized live file is invalid");
    require(state.undo_available && state.undo_index == 0u,
            "oversized live file: the good archive is offered");
    require(manager_settings_set_fps_lock(&state, 45u) == 0,
            "pending 45 over the oversized file");
    result = manager_settings_save(&io, &state, 0);
    require(manager_settings_result_ok(&result), "rewrite of the oversized file");
    require_file(fs, PORT_INI, CANON_45, "rewrite writes canonical 45");
    require(fake_find(fs, ARCHIVE("0001")) >= 0 &&
                fs->nodes[fake_find(fs, ARCHIVE("0001"))].size == sizeof(big),
            "oversized file archived verbatim");
    require(state.undo_available && state.undo_index == 1u,
            "oversized newest archive is offered");
    result = manager_settings_undo(&io, &state, 0);
    require_result(&result, MANAGER_SETTINGS_ACTION_UNDO,
                   MANAGER_SETTINGS_CODE_BAD_ARCHIVE,
                   "UNDO REFUSED - BAD ARCHIVE", "undo oversized archive");
    require_file(fs, PORT_INI, CANON_45, "oversized archive leaves the file");
    require(fake_count_containing(fs, "/port-") == 2,
            "refused oversized archive archives nothing");
    require_no_inflight(fs, "refused oversized archive leaves no sidecar");
    (void)manager_settings_load(&io, &state);
    require(state.undo_available && state.undo_index == 1u,
            "oversized newest archive stays offered after reload");

    /* Write failure during undo: the live file stands, pending kept. */
    io = settings_fs_init(fs, 1);
    fake_add_file(fs, PORT_INI, CANON_45);
    fake_add_file(fs, ARCHIVE("0000"), CANON_60);
    (void)manager_settings_load(&io, &state);
    require(state.undo_available, "undo offered before fault");
    fake_fail(fs, FAKE_OP_RENAME, 2);
    result = manager_settings_undo(&io, &state, 0);
    fake_no_fail(fs);
    require_result(&result, MANAGER_SETTINGS_ACTION_UNDO,
                   MANAGER_SETTINGS_CODE_IO_ERROR, "UNDO FAILED - IO ERROR",
                   "undo promotion failure");
    require_file(fs, PORT_INI, CANON_45, "failed undo leaves the file");
    require_no_inflight(fs, "failed undo cleans");
    require(manager_storage_recover(&io) == MANAGER_STORAGE_OK, "recover after undo fault");
    require_file(fs, PORT_INI, CANON_45, "recover after undo fault");
}

/* ---- log head ------------------------------------------------------------ */

static void require_last_boot_text(const struct manager_settings_last_boot *boot,
                                   const char *expected, const char *message)
{
    char text[MANAGER_SETTINGS_TEXT_CAPACITY];
    manager_settings_last_boot_text(boot, text, sizeof(text));
    require(strcmp(text, expected) == 0, message);
    require_narrow(text, message);
}

static void test_last_boot(void)
{
    struct fake_fs *fs = &g_fs;
    struct manager_storage_io io = settings_fs_init(fs, 0);
    struct manager_settings_last_boot boot;
    static char log[FAKE_DATA_CAPACITY];
    /* receipt status= token -> LAST BOOT text (design 9 verdicts; the
     * io-error code and an unknown token fold to the narrow set). */
    static const char *const k_verdicts[][2] = {
        { "schema", "FILE SCHEMA - DEFAULTS USED" },
        { "garbage", "FILE GARBAGE - DEFAULTS USED" },
        { "too-large", "FILE TOO-LARGE - DEFAULTS USED" },
        { "io-error(0x80010013)", "FILE IO-ERROR - DEFAULTS USED" },
        { "(?)", "FILE UNKNOWN - DEFAULTS USED" },
    };
    size_t used;
    size_t verdict;

    require(manager_settings_read_last_boot(&io, NULL, &boot) ==
                MANAGER_STORAGE_ABSENT,
            "no log result");
    require(boot.kind == MANAGER_SETTINGS_LAST_BOOT_NO_LOG, "no log kind");
    require_last_boot_text(&boot, "NO LOG", "no log text");

    fake_add_file(fs, LOG_PATH, ROTATE_LINE BUILD_LINE
                  "[0.150 thr 0x40010003] KAGE VITA CLOCK: cpu=444\n");
    require(manager_settings_read_last_boot(&io, NULL, &boot) ==
                MANAGER_STORAGE_OK,
            "ignoring build result");
    require(boot.kind == MANAGER_SETTINGS_LAST_BOOT_IGNORES,
            "build line without receipt");
    require_last_boot_text(&boot, "GAME BUILD IGNORES PORT.INI",
                           "ignoring build text");

    (void)fake_remove(fs, LOG_PATH);
    fake_add_file(fs, LOG_PATH,
                  ROTATE_LINE BUILD_LINE
                  "[0.200 thr 0x40010003] guest boundary begin: PE=app0:/x\n"
                  RECEIPT_LINE_45 REJECTED_LINE
                  "[0.300 thr 0x40010003] KAGE VITA CADENCE30: bid=x\n");
    require(manager_settings_read_last_boot(&io, LOG_PATH, &boot) ==
                MANAGER_STORAGE_OK,
            "receipt result");
    require(boot.kind == MANAGER_SETTINGS_LAST_BOOT_RECEIPT, "receipt kind");
    require(boot.fps_lock == 45u && !boot.fps_ignored, "receipt fps_lock");
    require(boot.remap == MANAGER_SETTINGS_REMAP_DEFAULT_GUARD, "receipt guard");
    require(strcmp(boot.status, "ok") == 0, "receipt status");
    require_last_boot_text(&boot, "FPS 45 REMAP DEFAULT GUARD", "receipt text");

    /* Receipt before any build line, custom remap, ignored fps suffix,
     * CRLF endings and no trailing newline. */
    (void)fake_remove(fs, LOG_PATH);
    fake_add_file(fs, LOG_PATH,
                  "[0.201 thr 0x40010003] ISAAC VITA SETTINGS: "
                  "path=ux0:/data/isaacr001/port.ini status=absent bytes=0 "
                  "lines=0 rejected=0 unknown=0 dup=0 "
                  "fps_lock=60(ignored:no-fullspeed-scheduler) "
                  "raster=960x544(ignored:not-compiled) remap=custom "
                  "chords=rb/back restart_hold=1 stick_swap=0\r\n"
                  "[0.300 thr 0x40010003] build later");
    require(manager_settings_read_last_boot(&io, LOG_PATH, &boot) ==
                MANAGER_STORAGE_OK,
            "custom receipt result");
    require(boot.kind == MANAGER_SETTINGS_LAST_BOOT_RECEIPT &&
                boot.fps_lock == 60u && boot.fps_ignored == 1u &&
                boot.remap == MANAGER_SETTINGS_REMAP_CUSTOM &&
                strcmp(boot.status, "absent") == 0,
            "custom receipt fields");
    /* The game build could not apply the lock (a STABLE_30 eboot): the
     * row says so instead of showing the value as applied. */
    require_last_boot_text(&boot, "FPS 60 IGNORED REMAP CUSTOM",
                           "custom receipt text");

    /* status= other than ok/absent: that boot ran on the defaults whatever
     * the file said, so the row names the verdict instead of echoing
     * "FPS 60"; the token is folded to the narrow set. */
    for (verdict = 0u; verdict < sizeof(k_verdicts) / sizeof(k_verdicts[0]);
         ++verdict) {
        (void)snprintf(log, sizeof(log),
                       BUILD_LINE "[0.201 thr 0x40010003] ISAAC VITA SETTINGS: "
                       "path=ux0:/data/isaacr001/port.ini status=%s bytes=9 "
                       "lines=1 rejected=0 unknown=0 dup=0 "
                       "fps_lock=60(ignored:no-fullspeed-scheduler) "
                       "raster=960x544(ignored:not-compiled) remap=default "
                       "chords=rb/back restart_hold=1 stick_swap=0\n",
                       k_verdicts[verdict][0]);
        (void)fake_remove(fs, LOG_PATH);
        fake_add_file(fs, LOG_PATH, log);
        require(manager_settings_read_last_boot(&io, LOG_PATH, &boot) ==
                    MANAGER_STORAGE_OK,
                "verdict receipt result");
        /* status[] keeps VALUE_MAX bytes of the token (the io-error code is
         * cut); the rendered word never needs more. */
        require(boot.kind == MANAGER_SETTINGS_LAST_BOOT_RECEIPT &&
                    boot.fps_lock == 60u && boot.fps_ignored == 1u &&
                    strncmp(boot.status, k_verdicts[verdict][0],
                            sizeof(boot.status) - 1u) == 0,
                "verdict receipt fields");
        require_last_boot_text(&boot, k_verdicts[verdict][1],
                               "verdict receipt text");
    }

    /* Only the REJECTED line, a truncated receipt key or an unprefixed
     * body: no receipt. */
    (void)fake_remove(fs, LOG_PATH);
    fake_add_file(fs, LOG_PATH, BUILD_LINE REJECTED_LINE
                  "ISAAC VITA SETTINGSX: fps_lock=30\n");
    require(manager_settings_read_last_boot(&io, LOG_PATH, &boot) ==
                MANAGER_STORAGE_OK,
            "rejected-only result");
    require(boot.kind == MANAGER_SETTINGS_LAST_BOOT_IGNORES,
            "REJECTED line is not the receipt");
    (void)fake_remove(fs, LOG_PATH);
    fake_add_file(fs, LOG_PATH, "ISAAC VITA SETTINGS: status=ok fps_lock=40 "
                                "remap=default\n");
    require(manager_settings_read_last_boot(&io, LOG_PATH, &boot) ==
                MANAGER_STORAGE_OK,
            "unprefixed receipt result");
    require(boot.kind == MANAGER_SETTINGS_LAST_BOOT_RECEIPT &&
                boot.fps_lock == 40u &&
                boot.remap == MANAGER_SETTINGS_REMAP_DEFAULT,
            "unprefixed receipt parsed");
    require_last_boot_text(&boot, "FPS 40 REMAP DEFAULT", "default text");

    /* Empty log and a log without a build line: NO LOG. */
    (void)fake_remove(fs, LOG_PATH);
    fake_add_file(fs, LOG_PATH, "");
    require(manager_settings_read_last_boot(&io, LOG_PATH, &boot) ==
                MANAGER_STORAGE_OK,
            "empty log result");
    require(boot.kind == MANAGER_SETTINGS_LAST_BOOT_NO_LOG, "empty log kind");
    (void)fake_remove(fs, LOG_PATH);
    fake_add_file(fs, LOG_PATH, ROTATE_LINE "[0.2 thr 0x1] something else\n");
    require(manager_settings_read_last_boot(&io, LOG_PATH, &boot) ==
                MANAGER_STORAGE_OK,
            "no build result");
    require(boot.kind == MANAGER_SETTINGS_LAST_BOOT_NO_LOG, "no build kind");

    /* 16 KiB cap: receipt beyond the cap -> IGNORES; receipt inside -> found;
     * receipt cut by the cap -> IGNORES. */
    used = 0u;
    used += (size_t)snprintf(log + used, sizeof(log) - used, "%s", BUILD_LINE);
    while (used < MANAGER_SETTINGS_LOG_HEAD_BYTES + 200u) {
        used += (size_t)snprintf(
            log + used, sizeof(log) - used,
            "[1.%03u thr 0x40010003] KAGE VITA FILLER: line %05u of the boot "
            "log with some padding text to reach the cap ........\n",
            (unsigned int)(used % 1000u), (unsigned int)used);
    }
    require(used > MANAGER_SETTINGS_LOG_HEAD_BYTES, "filler crosses the cap");
    used += (size_t)snprintf(log + used, sizeof(log) - used, "%s",
                             RECEIPT_LINE_45);
    require(used < sizeof(log), "log fits the fake node");
    (void)fake_remove(fs, LOG_PATH);
    (void)fake_add_node(fs, LOG_PATH, MANAGER_STORAGE_NODE_FILE, log, used);
    require(manager_settings_read_last_boot(&io, LOG_PATH, &boot) ==
                MANAGER_STORAGE_OK,
            "cap result");
    require(boot.kind == MANAGER_SETTINGS_LAST_BOOT_IGNORES,
            "receipt beyond the 16 KiB cap is not seen");
    /* Same filler, receipt placed inside the head. */
    used = 0u;
    used += (size_t)snprintf(log + used, sizeof(log) - used, "%s", BUILD_LINE);
    while (used < MANAGER_SETTINGS_LOG_HEAD_BYTES - 1500u) {
        used += (size_t)snprintf(
            log + used, sizeof(log) - used,
            "[1.%03u thr 0x40010003] KAGE VITA FILLER: line %05u of the boot "
            "log with some padding text to reach the cap ........\n",
            (unsigned int)(used % 1000u), (unsigned int)used);
    }
    used += (size_t)snprintf(log + used, sizeof(log) - used, "%s",
                             RECEIPT_LINE_ABSENT);
    while (used < MANAGER_SETTINGS_LOG_HEAD_BYTES + 500u) {
        used += (size_t)snprintf(
            log + used, sizeof(log) - used,
            "[2.%03u thr 0x40010003] KAGE VITA FILLER: after the receipt\n",
            (unsigned int)(used % 1000u));
    }
    (void)fake_remove(fs, LOG_PATH);
    (void)fake_add_node(fs, LOG_PATH, MANAGER_STORAGE_NODE_FILE, log, used);
    require(manager_settings_read_last_boot(&io, LOG_PATH, &boot) ==
                MANAGER_STORAGE_OK,
            "receipt inside the cap result");
    require(boot.kind == MANAGER_SETTINGS_LAST_BOOT_RECEIPT &&
                boot.fps_lock == 60u &&
                boot.remap == MANAGER_SETTINGS_REMAP_CUSTOM,
            "receipt inside the cap is found");
    /* Receipt line straddling the cap: the cut line is ignored. */
    used = 0u;
    used += (size_t)snprintf(log + used, sizeof(log) - used, "%s", BUILD_LINE);
    while (used < MANAGER_SETTINGS_LOG_HEAD_BYTES - 100u) {
        used += (size_t)snprintf(
            log + used, sizeof(log) - used,
            "[1.%03u thr 0x40010003] KAGE VITA FILLER: line %05u of the boot "
            "log with some padding text to reach the cap ........\n",
            (unsigned int)(used % 1000u), (unsigned int)used);
    }
    require(used < MANAGER_SETTINGS_LOG_HEAD_BYTES, "straddle setup");
    used += (size_t)snprintf(log + used, sizeof(log) - used, "%s",
                             RECEIPT_LINE_45);
    require(used > MANAGER_SETTINGS_LOG_HEAD_BYTES, "receipt straddles the cap");
    used += (size_t)snprintf(log + used, sizeof(log) - used, "%s",
                             "[3.000 thr 0x40010003] tail\n");
    (void)fake_remove(fs, LOG_PATH);
    (void)fake_add_node(fs, LOG_PATH, MANAGER_STORAGE_NODE_FILE, log, used);
    require(manager_settings_read_last_boot(&io, LOG_PATH, &boot) ==
                MANAGER_STORAGE_OK,
            "straddle result");
    require(boot.kind == MANAGER_SETTINGS_LAST_BOOT_IGNORES,
            "a receipt cut by the cap is ignored");
    /* Just under the cap with the receipt as the unterminated last line:
     * the whole file fit, so the line counts. */
    used = 0u;
    used += (size_t)snprintf(log + used, sizeof(log) - used, "%s", BUILD_LINE);
    while (used + sizeof(RECEIPT_LINE_45) - 2u + 64u <
           MANAGER_SETTINGS_LOG_HEAD_BYTES) {
        used += (size_t)snprintf(log + used, sizeof(log) - used,
                                 "[1.%03u thr 0x40010003] filler..............\n",
                                 (unsigned int)(used % 1000u));
    }
    used += (size_t)snprintf(log + used, sizeof(log) - used, "%s",
                             RECEIPT_LINE_45);
    --used; /* drop the trailing newline */
    require(used <= MANAGER_SETTINGS_LOG_HEAD_BYTES &&
                used + 80u > MANAGER_SETTINGS_LOG_HEAD_BYTES,
            "fits just under the cap");
    (void)fake_remove(fs, LOG_PATH);
    (void)fake_add_node(fs, LOG_PATH, MANAGER_STORAGE_NODE_FILE, log, used);
    require(manager_settings_read_last_boot(&io, LOG_PATH, &boot) ==
                MANAGER_STORAGE_OK,
            "unterminated receipt result");
    require(boot.kind == MANAGER_SETTINGS_LAST_BOOT_RECEIPT && boot.fps_lock == 45u,
            "unterminated last line counts when the file fit");

    /* I/O failure: NO LOG with the storage code. */
    fake_fail(fs, FAKE_OP_OPEN_READ, 1);
    require(manager_settings_read_last_boot(&io, LOG_PATH, &boot) ==
                MANAGER_STORAGE_IO_ERROR,
            "log open failure");
    fake_no_fail(fs);
    require(boot.kind == MANAGER_SETTINGS_LAST_BOOT_NO_LOG, "log failure kind");
    require(manager_settings_read_last_boot(NULL, LOG_PATH, &boot) ==
                MANAGER_STORAGE_INVALID,
            "log NULL io");
    require(manager_settings_read_last_boot(&io, LOG_PATH, NULL) ==
                MANAGER_STORAGE_INVALID,
            "log NULL out");
}

/* ---- glyph set ----------------------------------------------------------- */

static void test_narrow_strings(void)
{
    unsigned int action;
    unsigned int code;
    struct manager_settings_state state;
    struct manager_settings_last_boot boot;
    char text[MANAGER_SETTINGS_TEXT_CAPACITY];

    for (action = 0u; action <= (unsigned int)MANAGER_SETTINGS_ACTION_COUNT;
         ++action) {
        for (code = 0u; code <= (unsigned int)MANAGER_SETTINGS_CODE_COUNT;
             ++code) {
            struct manager_settings_result result;
            result.action = (uint8_t)action;
            result.code = (uint8_t)code;
            manager_settings_result_text(&result, text, sizeof(text));
            require_narrow(text, "result text glyph set");
        }
    }
    {
        struct manager_settings_result result;
        result.action = (uint8_t)MANAGER_SETTINGS_ACTION_SAVE;
        result.code = (uint8_t)MANAGER_SETTINGS_CODE_IO_ERROR;
        manager_settings_result_text(&result, text, 8u);
        require(strcmp(text, "SAVE FA") == 0, "result text truncates safely");
        manager_settings_result_text(NULL, text, sizeof(text));
        require(text[0] == '\0', "NULL result");
        manager_settings_result_text(&result, NULL, 4u);
        manager_settings_result_text(&result, text, 0u);
    }
    memset(&state, 0, sizeof(state));
    for (code = 0u; code <= (unsigned int)MANAGER_SETTINGS_LOAD_IO_ERROR + 1u;
         ++code) {
        state.status = (enum manager_settings_load_status)code;
        state.ignored = 1u;
        manager_settings_port_ini_text(&state, text, sizeof(text));
        require_narrow(text, "port.ini text glyph set");
        state.ignored = 12u;
        manager_settings_port_ini_text(&state, text, sizeof(text));
        require_narrow(text, "port.ini text glyph set (plural)");
    }
    manager_settings_port_ini_text(NULL, text, sizeof(text));
    require(text[0] == '\0', "NULL state text");
    memset(&boot, 0, sizeof(boot));
    for (code = 0u; code <= (unsigned int)MANAGER_SETTINGS_LAST_BOOT_RECEIPT + 1u;
         ++code) {
        unsigned int remap;
        boot.kind = (enum manager_settings_last_boot_kind)code;
        boot.fps_lock = 45u;
        for (remap = 0u; remap <= (unsigned int)MANAGER_SETTINGS_REMAP_CUSTOM;
             ++remap) {
            boot.remap = (enum manager_settings_remap_word)remap;
            manager_settings_last_boot_text(&boot, text, sizeof(text));
            require_narrow(text, "last boot text glyph set");
        }
    }
    manager_settings_last_boot_text(NULL, text, sizeof(text));
    require(text[0] == '\0', "NULL boot text");
    require(manager_settings_text_is_narrow("SAVE OK - 1 LINE_IGNORED.") &&
                !manager_settings_text_is_narrow("SAVE: OK") &&
                !manager_settings_text_is_narrow("save ok") &&
                !manager_settings_text_is_narrow("A(B)") &&
                !manager_settings_text_is_narrow(NULL) &&
                manager_settings_text_is_narrow(""),
            "narrow predicate");
}

/* ---- fault matrices beyond the write path ------------------------------ */

/* DEFAULTS: every vtable operation, every occurrence, in three filesystem
 * shapes (no backup root / root / root with a prior archive).  Legal end
 * states: OK / SYNC WARNING -> port.ini archived under the next index and
 * gone; any failure -> port.ini byte-identical and no new archive (the
 * single rename is the only mutation).  recover() afterwards changes
 * nothing. */
struct defaults_shape {
    int backup_root;
    int prior_archive;   /* port-0000.ini = 60 already there */
    const char *archive; /* where the removed file lands */
};

static void run_defaults_fault_matrix(void)
{
    static const struct defaults_shape k_shapes[] = {
        { 0, 0, ARCHIVE("0000") },
        { 1, 0, ARCHIVE("0000") },
        { 1, 1, ARCHIVE("0001") },
    };
    struct fake_fs *fs = &g_fs;
    size_t shape;
    for (shape = 0u; shape < sizeof(k_shapes) / sizeof(k_shapes[0]); ++shape) {
        const struct defaults_shape *scenario = &k_shapes[shape];
        const int prior = scenario->prior_archive ? 1 : 0;
        enum fake_operation operation;
        for (operation = FAKE_OP_STAT; operation < FAKE_OP_COUNT; ++operation) {
            int occurrence;
            for (occurrence = 1;; ++occurrence) {
                struct manager_storage_io io =
                    settings_fs_init(fs, scenario->backup_root);
                struct manager_settings_state state;
                struct manager_settings_result result;
                char text[MANAGER_SETTINGS_TEXT_CAPACITY];
                int total;
                if (prior)
                    fake_add_file(fs, ARCHIVE("0000"), CANON_60);
                fake_add_file(fs, PORT_INI, CANON_45);
                (void)manager_settings_load(&io, &state);
                fake_fail(fs, operation, occurrence);
                result = manager_settings_defaults(&io, &state, 0);
                total = fs->operation_counts[operation];
                fake_no_fail(fs);
                manager_settings_result_text(&result, text, sizeof(text));
                require_narrow(text, "defaults matrix status");
                if (manager_settings_result_ok(&result)) {
                    require_absent(fs, PORT_INI, "defaults matrix: ok removes");
                    require_file(fs, scenario->archive, CANON_45,
                                 "defaults matrix: ok archives");
                    require(state.status == MANAGER_SETTINGS_LOAD_ABSENT ||
                                state.status == MANAGER_SETTINGS_LOAD_IO_ERROR,
                            "defaults matrix: reloaded state");
                } else {
                    require_file(fs, PORT_INI, CANON_45,
                                 "defaults matrix: failure keeps the file");
                    require(fake_count_containing(fs, "/port-") == prior,
                            "defaults matrix: failure archives nothing");
                    require(strncmp(text, "DEFAULTS FAILED - ", 18u) == 0,
                            "defaults matrix: failure text");
                }
                if (prior)
                    require_file(fs, ARCHIVE("0000"), CANON_60,
                                 "defaults matrix: prior archive untouched");
                require_no_inflight(fs, "defaults matrix: no sidecar");
                require(manager_storage_recover(&io) == MANAGER_STORAGE_OK,
                        "defaults matrix: recover ok");
                if (manager_settings_result_ok(&result))
                    require_absent(fs, PORT_INI,
                                   "defaults matrix: recover keeps defaults");
                else
                    require_file(fs, PORT_INI, CANON_45,
                                 "defaults matrix: recover keeps file");
                if (total < occurrence) {
                    require(manager_settings_result_ok(&result),
                            "defaults matrix: clean run succeeds");
                    break;
                }
            }
        }
    }
}

/* UNDO (read half + write_text_atomic): port-0000.ini = 60 over a live 45,
 * and over no live file (the state after DEFAULTS).  Legal end states: OK
 * -> live 60 and, when a live file was replaced, archive 0001 = 45 (redo
 * offered); with no live file nothing is archived and the row hides;
 * failure -> the live file byte-identical or still absent.  Never both
 * sidecars; recover()+load() afterwards agree with the file. */
static void run_undo_fault_matrix(void)
{
    struct fake_fs *fs = &g_fs;
    int live;
    for (live = 1; live >= 0; --live) {
        enum fake_operation operation;
        for (operation = FAKE_OP_STAT; operation < FAKE_OP_COUNT; ++operation) {
            int occurrence;
            for (occurrence = 1;; ++occurrence) {
                struct manager_storage_io io = settings_fs_init(fs, 1);
                struct manager_settings_state state;
                struct manager_settings_result result;
                char text[MANAGER_SETTINGS_TEXT_CAPACITY];
                int total;
                int ok;
                if (live)
                    fake_add_file(fs, PORT_INI, CANON_45);
                fake_add_file(fs, ARCHIVE("0000"), CANON_60);
                (void)manager_settings_load(&io, &state);
                require(state.undo_available, "undo matrix: undo offered");
                fake_fail(fs, operation, occurrence);
                result = manager_settings_undo(&io, &state, 0);
                total = fs->operation_counts[operation];
                fake_no_fail(fs);
                ok = manager_settings_result_ok(&result);
                manager_settings_result_text(&result, text, sizeof(text));
                require_narrow(text, "undo matrix status");
                require(!(fake_exists(fs, PORT_ROLLBACK) && fake_exists(fs, PORT_TMP)),
                        "undo matrix: never both sidecars");
                if (ok) {
                    require_file(fs, PORT_INI, CANON_60, "undo matrix: ok restores");
                    if (live)
                        require_file(fs, ARCHIVE("0001"), CANON_45,
                                     "undo matrix: ok archives the current file");
                    else
                        require_absent(fs, ARCHIVE("0001"),
                                       "undo matrix: nothing to archive");
                    require(state.saved.fps_lock == 60u,
                            "undo matrix: state follows the restored file");
                } else {
                    if (live)
                        require_file(fs, PORT_INI, CANON_45,
                                     "undo matrix: failure keeps the live file");
                    else
                        require_absent(fs, PORT_INI,
                                       "undo matrix: failure creates no file");
                    require(strncmp(text, "UNDO ", 5u) == 0,
                            "undo matrix: failure text");
                }
                require(manager_storage_recover(&io) == MANAGER_STORAGE_OK,
                        "undo matrix: recover ok");
                require_no_inflight(fs, "undo matrix: recover cleans");
                require_file(fs, ARCHIVE("0000"), CANON_60,
                             "undo matrix: the archive is never touched");
                (void)manager_settings_load(&io, &state);
                require(state.status == ((ok || live) ? MANAGER_SETTINGS_LOAD_OK
                                                      : MANAGER_SETTINGS_LOAD_ABSENT),
                        "undo matrix: load after recover");
                require(state.saved.fps_lock == (ok ? 60u : live ? 45u : 60u),
                        "undo matrix: load agrees with the file");
                /* Redo exists only when a live file was replaced: after a
                 * successful DEFAULTS + UNDO the newest archive equals the
                 * live file and the row hides; a failed one leaves port.ini
                 * absent, which differs, so the row stays. */
                require(state.undo_available == (live || !ok),
                        "undo matrix: undo row after the attempt");
                if (total < occurrence) {
                    require(ok, "undo matrix: clean run succeeds");
                    break;
                }
            }
        }
    }
}

/* recover_port from the fully torn state (rollback 45 + new port.ini 30 +
 * stale tmp, no backup root), with clean save slots and with a slot whose
 * rollback sidecar is a directory (CONFLICT): whatever operation fails, a
 * second recover() finishes the port.ini repair: live 45, exactly one
 * quarantine holding 30, no sidecars.  Old settings win, the new content
 * is kept; the slot failure is reported but never skips the repair. */
static void run_recover_fault_matrix(int slot_conflict)
{
    struct fake_fs *fs = &g_fs;
    const int expected = slot_conflict ? MANAGER_STORAGE_CONFLICT
                                       : MANAGER_STORAGE_OK;
    enum fake_operation operation;
    for (operation = FAKE_OP_STAT; operation < FAKE_OP_COUNT; ++operation) {
        int occurrence;
        for (occurrence = 1;; ++occurrence) {
            struct manager_storage_io io = settings_fs_init(fs, 0);
            struct manager_settings_state state;
            int first;
            int total;
            if (slot_conflict)
                fake_add_dir(fs, SLOT2_ROLLBACK);
            fake_add_file(fs, PORT_ROLLBACK, CANON_45);
            fake_add_file(fs, PORT_INI, CANON_30);
            fake_add_file(fs, PORT_TMP, "half written");
            fake_fail(fs, operation, occurrence);
            first = manager_storage_recover(&io);
            total = fs->operation_counts[operation];
            fake_no_fail(fs);
            /* The faulted attempt never loses a version: 45 is the live
             * file or still the rollback, 30 is live or quarantined. */
            require(fake_exists(fs, PORT_INI) || fake_exists(fs, PORT_ROLLBACK),
                    "recover matrix: a live or rollback file survives");
            require(manager_storage_recover(&io) == expected,
                    "recover matrix: second recover result");
            require_file(fs, PORT_INI, CANON_45, "recover matrix: old wins");
            require(fake_count_containing(fs, "port-recovered") == 1,
                    "recover matrix: one quarantine");
            require_file(fs, RECOVERED("0000"), CANON_30,
                         "recover matrix: new content quarantined");
            require_no_inflight(fs, "recover matrix: sidecars gone");
            (void)manager_settings_load(&io, &state);
            require(state.status == MANAGER_SETTINGS_LOAD_OK &&
                        state.saved.fps_lock == 45u,
                    "recover matrix: load sees the promoted rollback");
            if (total < occurrence) {
                require(first == expected,
                        "recover matrix: clean run result");
                break;
            }
        }
    }
}

static void test_fault_matrices(void)
{
    run_defaults_fault_matrix();
    run_undo_fault_matrix();
    run_recover_fault_matrix(0);
    run_recover_fault_matrix(1);
}

int main(void)
{
    test_text_primitives();
    test_load();
    test_pending();
    test_save_render_scale();
    test_save();
    test_save_refusals();
    test_write_faults();
    test_recover_port();
    test_defaults();
    test_undo();
    test_fault_matrices();
    test_last_boot();
    test_narrow_strings();
    printf("manager settings host tests: PASS (%u checks)\n", checks);
    return 0;
}
