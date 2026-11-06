/*
 * Manager settings core.
 *
 * SDK-free by construction: every file touch goes through the
 * manager_storage_io vtable (manager_storage_vita.c on the device, the fake
 * filesystem in host_tests), the grammar lives in the shared parser TU and
 * the only libc dependencies are string.h and snprintf.  No allocation, no
 * VLA, static buffers only.  Compiled with MANAGER_SETTINGS_UI=1 into the
 * manager and into host_tests/manager_settings_host_test.
 */
#include "manager_settings.h"

#if defined(MANAGER_SETTINGS_UI)

#include "manager_sha256.h"

#include <stdio.h>
#include <string.h>

#define MANAGER_SETTINGS_ARCHIVE_PREFIX "port"
#define MANAGER_SETTINGS_ARCHIVE_EXTENSION ".ini"

/* ---- small helpers ------------------------------------------------------ */

static const uint16_t k_fps_ring[] = { 60u, 45u, 40u, 30u };
static const uint8_t k_scale_ring[] = { 1u, 2u };

static void manager_settings_hash(const void *bytes, size_t length,
                                  unsigned char digest[32])
{
    struct manager_sha256_context hash;
    manager_sha256_init(&hash);
    if (length != 0u)
        manager_sha256_update(&hash, bytes, length);
    manager_sha256_final(&hash, digest);
}

static struct manager_settings_result manager_settings_make(
    enum manager_settings_action action, enum manager_settings_code code)
{
    struct manager_settings_result result;
    result.action = (uint8_t)action;
    result.code = (uint8_t)code;
    return result;
}

static enum manager_settings_code manager_settings_code_from_storage(
    int storage)
{
    switch (storage) {
    case MANAGER_STORAGE_OK:
        return MANAGER_SETTINGS_CODE_OK;
    case MANAGER_STORAGE_SYNC_WARNING:
        return MANAGER_SETTINGS_CODE_SYNC_WARNING;
    case MANAGER_STORAGE_ABSENT:
        return MANAGER_SETTINGS_CODE_NO_FILE;
    case MANAGER_STORAGE_INVALID:
    case MANAGER_STORAGE_TOO_LARGE:
        return MANAGER_SETTINGS_CODE_BAD_FILE;
    case MANAGER_STORAGE_CONFLICT:
        return MANAGER_SETTINGS_CODE_FILE_CONFLICT;
    case MANAGER_STORAGE_HASH_MISMATCH:
        return MANAGER_SETTINGS_CODE_HASH_MISMATCH;
    case MANAGER_STORAGE_CAPACITY:
        return MANAGER_SETTINGS_CODE_NO_FREE_BACKUP_SLOT;
    case MANAGER_STORAGE_IO_ERROR:
    default:
        return MANAGER_SETTINGS_CODE_IO_ERROR;
    }
}

static void manager_settings_reset(struct manager_settings_state *state)
{
    memset(state, 0, sizeof(*state));
    isaac_vita_settings_defaults(&state->saved);
    isaac_vita_settings_defaults(&state->pending);
    isaac_vita_settings_receipt_clear(&state->receipt);
    state->status = MANAGER_SETTINGS_LOAD_ABSENT;
    state->storage_result = MANAGER_STORAGE_ABSENT;
}

/* ---- load ---------------------------------------------------------------- */

/* Newest archive + live file for the UNDO hash comparison and the UNDO
 * restore.  Static like the log-head buffer below: the settings path runs on
 * the single manager thread only, and undo() -> write -> load() ->
 * refresh_undo() would otherwise stack two of these 8 KiB pairs on top of
 * the storage layer's 32 KiB copy buffer.  undo() is done with the bytes
 * before it calls load(), so the two users never overlap. */
static struct manager_settings_scratch {
    char path[MANAGER_STORAGE_PATH_CAPACITY];
    char archive[MANAGER_SETTINGS_FILE_CAPACITY];
    char current[MANAGER_SETTINGS_FILE_CAPACITY];
} s_scratch;

void manager_settings_refresh_undo(const struct manager_storage_io *io,
                                   struct manager_settings_state *state)
{
    struct manager_settings_scratch *scratch = &s_scratch;
    unsigned char archive_digest[32];
    unsigned char current_digest[32];
    size_t archive_length = 0u;
    size_t current_length = 0u;
    unsigned int index = 0u;
    int result;

    state->undo_available = 0;
    state->undo_index = 0u;
    if (io == NULL)
        return;
    if (manager_storage_newest_named(io, MANAGER_SETTINGS_ARCHIVE_PREFIX,
                                     MANAGER_SETTINGS_ARCHIVE_EXTENSION, &index,
                                     scratch->path, sizeof(scratch->path)) !=
        MANAGER_STORAGE_OK)
        return;
    result = manager_storage_read_text(io, scratch->path, scratch->archive,
                                       sizeof(scratch->archive),
                                       &archive_length);
    /* Section 1.3 (detail): the row hides only when no archive exists or
     * the newest one equals the live file.  Whether the archive parses is
     * decided on the press (UNDO REFUSED - BAD ARCHIVE), so a garbage
     * newest archive is visible and refuses rather than vanishing.  The
     * same holds for an archive the read cannot return (an oversized
     * port.ini rewritten by X is archived verbatim; a directory in the
     * way): no hash to compare, offered, the press refuses. */
    if (result == MANAGER_STORAGE_TOO_LARGE || result == MANAGER_STORAGE_CONFLICT) {
        state->undo_available = 1;
        state->undo_index = index;
        return;
    }
    if (result != MANAGER_STORAGE_OK)
        return; /* vanished or I/O trouble */
    result = manager_storage_read_text(io, MANAGER_STORAGE_PORT_INI_PATH,
                                       scratch->current,
                                       sizeof(scratch->current),
                                       &current_length);
    if (result == MANAGER_STORAGE_OK) {
        manager_settings_hash(scratch->archive, archive_length, archive_digest);
        manager_settings_hash(scratch->current, current_length, current_digest);
        if (memcmp(archive_digest, current_digest, sizeof(archive_digest)) == 0)
            return; /* a just-restored state does not offer a no-op */
    } else if (result != MANAGER_STORAGE_ABSENT &&
               result != MANAGER_STORAGE_TOO_LARGE &&
               result != MANAGER_STORAGE_CONFLICT) {
        return; /* I/O trouble: edits are refused anyway */
    }
    state->undo_available = 1;
    state->undo_index = index;
}

/* After a verified write whose re-read failed (a transient I/O error on the
 * bytes the SHA-256 readback just confirmed): the page keeps showing the
 * written values instead of the IO-ERROR defaults; the write's own result
 * code stands.  load() has already reset the state; UNDO is refreshed once
 * more here because the same transient may have hit one of its reads. */
static void manager_settings_adopt_written(
    const struct manager_storage_io *io, struct manager_settings_state *state,
    const struct isaac_vita_settings *written,
    const struct isaac_vita_settings_receipt *receipt)
{
    state->saved = *written;
    if (receipt->remap_guard)
        isaac_vita_settings_reset_remap(&state->saved);
    state->pending = state->saved;
    state->receipt = *receipt;
    state->ignored = receipt->rejected + receipt->unknown;
    state->status = state->ignored != 0u ? MANAGER_SETTINGS_LOAD_OK_WITH_IGNORED
                                         : MANAGER_SETTINGS_LOAD_OK;
    state->storage_result = MANAGER_STORAGE_OK;
    manager_settings_refresh_undo(io, state);
}

int manager_settings_load(const struct manager_storage_io *io,
                          struct manager_settings_state *state)
{
    char buffer[MANAGER_SETTINGS_FILE_CAPACITY];
    size_t length = 0u;
    int result;

    if (state == NULL)
        return MANAGER_STORAGE_INVALID;
    manager_settings_reset(state);
    if (io == NULL) {
        state->status = MANAGER_SETTINGS_LOAD_IO_ERROR;
        state->storage_result = MANAGER_STORAGE_INVALID;
        state->receipt.status = (uint8_t)ISAAC_VITA_SETTINGS_STATUS_IO_ERROR;
        return MANAGER_STORAGE_INVALID;
    }
    result = manager_storage_read_text(io, MANAGER_STORAGE_PORT_INI_PATH,
                                       buffer, sizeof(buffer), &length);
    state->storage_result = result;
    if (result == MANAGER_STORAGE_OK) {
        const enum isaac_vita_settings_status status =
            isaac_vita_settings_parse(buffer, length, &state->saved,
                                      &state->receipt);
        if (status == ISAAC_VITA_SETTINGS_STATUS_OK) {
            if (state->receipt.remap_guard)
                isaac_vita_settings_reset_remap(&state->saved);
            state->ignored = state->receipt.rejected + state->receipt.unknown;
            state->status = state->ignored != 0u
                                ? MANAGER_SETTINGS_LOAD_OK_WITH_IGNORED
                                : MANAGER_SETTINGS_LOAD_OK;
        } else {
            /* schema / garbage / too-large: the parser left defaults. */
            state->status = MANAGER_SETTINGS_LOAD_INVALID;
        }
    } else if (result == MANAGER_STORAGE_ABSENT) {
        state->status = MANAGER_SETTINGS_LOAD_ABSENT;
        state->receipt.status = (uint8_t)ISAAC_VITA_SETTINGS_STATUS_ABSENT;
    } else if (result == MANAGER_STORAGE_TOO_LARGE) {
        state->status = MANAGER_SETTINGS_LOAD_INVALID;
        state->receipt.status = (uint8_t)ISAAC_VITA_SETTINGS_STATUS_TOO_LARGE;
    } else {
        state->status = MANAGER_SETTINGS_LOAD_IO_ERROR;
        state->receipt.status = (uint8_t)ISAAC_VITA_SETTINGS_STATUS_IO_ERROR;
    }
    state->pending = state->saved;
    manager_settings_refresh_undo(io, state);
    return result;
}

/* ---- pending edits ------------------------------------------------------- */

int manager_settings_set_pending_text(struct manager_settings_state *state,
                                      enum isaac_vita_settings_key key,
                                      const char *text, size_t length)
{
    if (state == NULL || text == NULL)
        return -1;
    return isaac_vita_settings_set_text(&state->pending, key, text, length);
}

int manager_settings_set_fps_lock(struct manager_settings_state *state,
                                  uint16_t fps_lock)
{
    size_t index;
    if (state == NULL)
        return -1;
    for (index = 0u; index < sizeof(k_fps_ring) / sizeof(k_fps_ring[0]);
         ++index) {
        if (k_fps_ring[index] == fps_lock) {
            state->pending.fps_lock = fps_lock;
            return 0;
        }
    }
    return -1;
}

uint16_t manager_settings_fps_lock_step(uint16_t current, int direction)
{
    const size_t count = sizeof(k_fps_ring) / sizeof(k_fps_ring[0]);
    size_t index;
    for (index = 0u; index < count; ++index) {
        if (k_fps_ring[index] == current)
            break;
    }
    if (index == count)
        return k_fps_ring[0];
    if (direction > 0)
        return index + 1u < count ? k_fps_ring[index + 1u] : k_fps_ring[index];
    if (direction < 0)
        return index > 0u ? k_fps_ring[index - 1u] : k_fps_ring[index];
    return k_fps_ring[index];
}

int manager_settings_set_render_scale(struct manager_settings_state *state,
                                      uint8_t render_scale)
{
    size_t index;
    if (state == NULL)
        return -1;
    for (index = 0u; index < sizeof(k_scale_ring) / sizeof(k_scale_ring[0]);
         ++index) {
        if (k_scale_ring[index] == render_scale) {
            state->pending.render_scale = render_scale;
            return 0;
        }
    }
    return -1;
}

uint8_t manager_settings_render_scale_step(uint8_t current, int direction)
{
    const size_t count = sizeof(k_scale_ring) / sizeof(k_scale_ring[0]);
    size_t index;
    for (index = 0u; index < count; ++index) {
        if (k_scale_ring[index] == current)
            break;
    }
    if (index == count)
        return k_scale_ring[0];
    if (direction > 0)
        return index + 1u < count ? k_scale_ring[index + 1u] : k_scale_ring[index];
    if (direction < 0)
        return index > 0u ? k_scale_ring[index - 1u] : k_scale_ring[index];
    return k_scale_ring[index];
}

unsigned int manager_settings_pending_count(
    const struct manager_settings_state *state)
{
    unsigned int count = 0u;
    unsigned int key;
    if (state == NULL)
        return 0u;
    for (key = 1u; key < (unsigned int)ISAAC_VITA_SETTINGS_KEY_COUNT; ++key) {
        char saved[ISAAC_VITA_SETTINGS_VALUE_MAX + 1u];
        char pending[ISAAC_VITA_SETTINGS_VALUE_MAX + 1u];
        (void)isaac_vita_settings_get_text(
            &state->saved, (enum isaac_vita_settings_key)key, saved,
            sizeof(saved));
        (void)isaac_vita_settings_get_text(
            &state->pending, (enum isaac_vita_settings_key)key, pending,
            sizeof(pending));
        if (strcmp(saved, pending) != 0)
            ++count;
    }
    return count;
}

void manager_settings_discard(struct manager_settings_state *state)
{
    if (state != NULL)
        state->pending = state->saved;
}

/* ---- save / defaults / undo ---------------------------------------------- */

struct manager_settings_result manager_settings_save(
    const struct manager_storage_io *io, struct manager_settings_state *state,
    int ftp_listening)
{
    char text[ISAAC_VITA_SETTINGS_MAX_BYTES + 1u];
    size_t length;
    int storage;
    enum manager_settings_code code;

    if (io == NULL || state == NULL)
        return manager_settings_make(MANAGER_SETTINGS_ACTION_SAVE,
                                     MANAGER_SETTINGS_CODE_IO_ERROR);
    if (ftp_listening)
        return manager_settings_make(MANAGER_SETTINGS_ACTION_SAVE,
                                     MANAGER_SETTINGS_CODE_FTP_LISTENING);
    if (!isaac_vita_settings_validate_remap(&state->pending))
        return manager_settings_make(MANAGER_SETTINGS_ACTION_SAVE,
                                     MANAGER_SETTINGS_CODE_NEED_A_AND_B);
    length = isaac_vita_settings_serialize(&state->pending, text, sizeof(text));
    if (length == 0u)
        return manager_settings_make(MANAGER_SETTINGS_ACTION_SAVE,
                                     MANAGER_SETTINGS_CODE_BAD_FILE);
    storage = manager_storage_write_text_atomic(
        io, MANAGER_STORAGE_PORT_INI_NAME, text, length);
    code = manager_settings_code_from_storage(storage);
    if (storage >= 0) {
        /* Re-read the verified file: saved == pending, receipt authoritative,
         * undo refreshed (the previous version is now the newest archive).
         * A failing re-read keeps the values just written (the bytes on
         * disk are known) rather than dropping the page into IO ERROR. */
        struct isaac_vita_settings written;
        struct isaac_vita_settings_receipt receipt;
        (void)isaac_vita_settings_parse(text, length, &written, &receipt);
        if (manager_settings_load(io, state) < 0)
            manager_settings_adopt_written(io, state, &written, &receipt);
    }
    return manager_settings_make(MANAGER_SETTINGS_ACTION_SAVE, code);
}

struct manager_settings_result manager_settings_defaults(
    const struct manager_storage_io *io, struct manager_settings_state *state,
    int ftp_listening)
{
    int storage;
    enum manager_settings_code code;

    if (io == NULL || state == NULL)
        return manager_settings_make(MANAGER_SETTINGS_ACTION_DEFAULTS,
                                     MANAGER_SETTINGS_CODE_IO_ERROR);
    if (ftp_listening)
        return manager_settings_make(MANAGER_SETTINGS_ACTION_DEFAULTS,
                                     MANAGER_SETTINGS_CODE_FTP_LISTENING);
    storage = manager_storage_archive_and_remove(io,
                                                 MANAGER_STORAGE_PORT_INI_NAME);
    code = manager_settings_code_from_storage(storage);
    if (storage >= 0)
        (void)manager_settings_load(io, state);
    return manager_settings_make(MANAGER_SETTINGS_ACTION_DEFAULTS, code);
}

struct manager_settings_result manager_settings_undo(
    const struct manager_storage_io *io, struct manager_settings_state *state,
    int ftp_listening)
{
    struct manager_settings_scratch *scratch = &s_scratch;
    struct isaac_vita_settings parsed;
    struct isaac_vita_settings_receipt receipt;
    size_t archive_length = 0u;
    size_t current_length = 0u;
    unsigned int index = 0u;
    int result;
    int storage;
    enum manager_settings_code code;

    if (io == NULL || state == NULL)
        return manager_settings_make(MANAGER_SETTINGS_ACTION_UNDO,
                                     MANAGER_SETTINGS_CODE_IO_ERROR);
    if (ftp_listening)
        return manager_settings_make(MANAGER_SETTINGS_ACTION_UNDO,
                                     MANAGER_SETTINGS_CODE_FTP_LISTENING);
    result = manager_storage_newest_named(io, MANAGER_SETTINGS_ARCHIVE_PREFIX,
                                          MANAGER_SETTINGS_ARCHIVE_EXTENSION,
                                          &index, scratch->path,
                                          sizeof(scratch->path));
    if (result == MANAGER_STORAGE_ABSENT)
        return manager_settings_make(MANAGER_SETTINGS_ACTION_UNDO,
                                     MANAGER_SETTINGS_CODE_NOTHING_TO_UNDO);
    if (result != MANAGER_STORAGE_OK)
        return manager_settings_make(MANAGER_SETTINGS_ACTION_UNDO,
                                     manager_settings_code_from_storage(result));
    result = manager_storage_read_text(io, scratch->path, scratch->archive,
                                       sizeof(scratch->archive),
                                       &archive_length);
    if (result == MANAGER_STORAGE_TOO_LARGE || result == MANAGER_STORAGE_ABSENT ||
        result == MANAGER_STORAGE_CONFLICT)
        return manager_settings_make(MANAGER_SETTINGS_ACTION_UNDO,
                                     MANAGER_SETTINGS_CODE_BAD_ARCHIVE);
    if (result != MANAGER_STORAGE_OK)
        return manager_settings_make(MANAGER_SETTINGS_ACTION_UNDO,
                                     manager_settings_code_from_storage(result));
    if (isaac_vita_settings_parse(scratch->archive, archive_length, &parsed,
                                  &receipt) != ISAAC_VITA_SETTINGS_STATUS_OK)
        return manager_settings_make(MANAGER_SETTINGS_ACTION_UNDO,
                                     MANAGER_SETTINGS_CODE_BAD_ARCHIVE);
    result = manager_storage_read_text(io, MANAGER_STORAGE_PORT_INI_PATH,
                                       scratch->current,
                                       sizeof(scratch->current),
                                       &current_length);
    if (result == MANAGER_STORAGE_OK) {
        unsigned char archive_digest[32];
        unsigned char current_digest[32];
        manager_settings_hash(scratch->archive, archive_length, archive_digest);
        manager_settings_hash(scratch->current, current_length, current_digest);
        if (memcmp(archive_digest, current_digest, sizeof(archive_digest)) == 0)
            return manager_settings_make(MANAGER_SETTINGS_ACTION_UNDO,
                                         MANAGER_SETTINGS_CODE_NOTHING_TO_UNDO);
    } else if (result != MANAGER_STORAGE_ABSENT &&
               result != MANAGER_STORAGE_TOO_LARGE &&
               result != MANAGER_STORAGE_CONFLICT) {
        return manager_settings_make(MANAGER_SETTINGS_ACTION_UNDO,
                                     manager_settings_code_from_storage(result));
    }
    /* The archive's exact bytes go back; the current file (if any) becomes
     * the newest archive, so a second UNDO redoes.  load() reuses the
     * scratch; the parsed copy carries the restored values for the re-read
     * fallback. */
    storage = manager_storage_write_text_atomic(
        io, MANAGER_STORAGE_PORT_INI_NAME, scratch->archive, archive_length);
    code = manager_settings_code_from_storage(storage);
    if (storage >= 0 && manager_settings_load(io, state) < 0)
        manager_settings_adopt_written(io, state, &parsed, &receipt);
    return manager_settings_make(MANAGER_SETTINGS_ACTION_UNDO, code);
}

/* ---- status text --------------------------------------------------------- */

void manager_settings_result_text(const struct manager_settings_result *result,
                                  char *out, size_t capacity)
{
    static const char *const k_actions[MANAGER_SETTINGS_ACTION_COUNT] = {
        "SAVE", "DEFAULTS", "UNDO",
    };
    const char *action = "SETTINGS";
    const char *suffix;

    if (out == NULL || capacity == 0u)
        return;
    out[0] = '\0';
    if (result == NULL)
        return;
    if (result->action < (uint8_t)MANAGER_SETTINGS_ACTION_COUNT)
        action = k_actions[result->action];
    switch (result->code) {
    case MANAGER_SETTINGS_CODE_OK:
        suffix = "OK";
        break;
    case MANAGER_SETTINGS_CODE_SYNC_WARNING:
        suffix = "OK - SYNC WARNING";
        break;
    case MANAGER_SETTINGS_CODE_NO_FILE:
        suffix = "OK - NO PORT.INI";
        break;
    case MANAGER_SETTINGS_CODE_FTP_LISTENING:
        /* The manager's existing refusal text (manager_main.c). */
        (void)snprintf(out, capacity, "%s", "STOP FTP BEFORE LOCAL CHANGES");
        return;
    case MANAGER_SETTINGS_CODE_NEED_A_AND_B:
        suffix = "REFUSED - NEED A AND B";
        break;
    case MANAGER_SETTINGS_CODE_BAD_FILE:
        suffix = "FAILED - BAD FILE";
        break;
    case MANAGER_SETTINGS_CODE_NO_FREE_BACKUP_SLOT:
        suffix = "FAILED - NO FREE BACKUP SLOT";
        break;
    case MANAGER_SETTINGS_CODE_HASH_MISMATCH:
        suffix = "FAILED - HASH MISMATCH";
        break;
    case MANAGER_SETTINGS_CODE_FILE_CONFLICT:
        suffix = "FAILED - FILE CONFLICT";
        break;
    case MANAGER_SETTINGS_CODE_IO_ERROR:
        suffix = "FAILED - IO ERROR";
        break;
    case MANAGER_SETTINGS_CODE_NOTHING_TO_UNDO:
        suffix = "REFUSED - NOTHING TO UNDO";
        break;
    case MANAGER_SETTINGS_CODE_BAD_ARCHIVE:
        suffix = "REFUSED - BAD ARCHIVE";
        break;
    default:
        suffix = "FAILED";
        break;
    }
    (void)snprintf(out, capacity, "%s %s", action, suffix);
}

void manager_settings_port_ini_text(const struct manager_settings_state *state,
                                    char *out, size_t capacity)
{
    if (out == NULL || capacity == 0u)
        return;
    out[0] = '\0';
    if (state == NULL)
        return;
    switch (state->status) {
    case MANAGER_SETTINGS_LOAD_ABSENT:
        (void)snprintf(out, capacity, "%s", "PORT.INI ABSENT");
        break;
    case MANAGER_SETTINGS_LOAD_OK:
        (void)snprintf(out, capacity, "%s", "PORT.INI OK");
        break;
    case MANAGER_SETTINGS_LOAD_OK_WITH_IGNORED:
        (void)snprintf(out, capacity, "PORT.INI OK - %u LINE%s IGNORED",
                       (unsigned int)state->ignored,
                       state->ignored == 1u ? "" : "S");
        break;
    case MANAGER_SETTINGS_LOAD_INVALID:
        (void)snprintf(out, capacity, "%s", "PORT.INI INVALID - DEFAULTS SHOWN");
        break;
    case MANAGER_SETTINGS_LOAD_IO_ERROR:
    default:
        (void)snprintf(out, capacity, "%s", "PORT.INI IO ERROR");
        break;
    }
}

int manager_settings_text_is_narrow(const char *text)
{
    if (text == NULL)
        return 0;
    while (*text != '\0') {
        const unsigned char value = (unsigned char)*text++;
        if (!((value >= 'A' && value <= 'Z') ||
              (value >= '0' && value <= '9') || value == ' ' ||
              value == '.' || value == '-' || value == '_'))
            return 0;
    }
    return 1;
}

/* ---- per-boot log head --------------------------------------------------- */

static int manager_settings_starts_with(const char *text, size_t length,
                                        const char *prefix)
{
    const size_t prefix_length = strlen(prefix);
    return length >= prefix_length && memcmp(text, prefix, prefix_length) == 0;
}

static const char *manager_settings_find(const char *haystack, size_t length,
                                         const char *needle)
{
    const size_t needle_length = strlen(needle);
    size_t offset;
    if (needle_length == 0u || length < needle_length)
        return NULL;
    for (offset = 0u; offset + needle_length <= length; ++offset) {
        if (memcmp(haystack + offset, needle, needle_length) == 0)
            return haystack + offset;
    }
    return NULL;
}

static size_t manager_settings_token_length(const char *text, size_t length)
{
    size_t used = 0u;
    while (used < length && text[used] != ' ' && text[used] != '\t')
        ++used;
    return used;
}

/* "[%u.%03u thr 0x%x] body" (platform.c isaac_vita_log); a line without the
 * prefix is taken as body. */
static void manager_settings_line_body(const char *line, size_t length,
                                       const char **body, size_t *body_length)
{
    *body = line;
    *body_length = length;
    if (length != 0u && line[0] == '[') {
        const char *end = manager_settings_find(line, length, "] ");
        if (end != NULL) {
            *body = end + 2u;
            *body_length = length - (size_t)(*body - line);
        }
    }
}

static void manager_settings_parse_receipt(const char *body, size_t length,
                                           struct manager_settings_last_boot *out)
{
    const char *field;
    size_t remaining;
    size_t token;

    out->kind = MANAGER_SETTINGS_LAST_BOOT_RECEIPT;
    out->fps_lock = 0u;
    out->fps_ignored = 0u;
    out->remap = MANAGER_SETTINGS_REMAP_DEFAULT;
    out->status[0] = '\0';
    field = manager_settings_find(body, length, " status=");
    if (field != NULL) {
        field += 8u;
        remaining = length - (size_t)(field - body);
        token = manager_settings_token_length(field, remaining);
        if (token > sizeof(out->status) - 1u)
            token = sizeof(out->status) - 1u;
        memcpy(out->status, field, token);
        out->status[token] = '\0';
    }
    field = manager_settings_find(body, length, " fps_lock=");
    if (field != NULL) {
        unsigned int value = 0u;
        size_t digits = 0u;
        field += 10u;
        remaining = length - (size_t)(field - body);
        while (digits < remaining && digits < 5u && field[digits] >= '0' &&
               field[digits] <= '9') {
            value = value * 10u + (unsigned int)(field[digits] - '0');
            ++digits;
        }
        out->fps_lock = value <= 65535u ? (uint16_t)value : 0u;
        /* "fps_lock=45(ignored:no-fullspeed-scheduler)": requested but not
         * applied by that game build.  The needle stops before
         * the colon so the status-literal pin (no ':' anywhere in this TU's
         * strings) holds. */
        out->fps_ignored = manager_settings_starts_with(
                               field + digits, remaining - digits, "(ignored")
                               ? 1u
                               : 0u;
    }
    field = manager_settings_find(body, length, " remap=");
    if (field != NULL) {
        field += 7u;
        remaining = length - (size_t)(field - body);
        token = manager_settings_token_length(field, remaining);
        if (manager_settings_starts_with(field, token, "default")) {
            out->remap = manager_settings_find(field, token, "(guard)") != NULL
                             ? MANAGER_SETTINGS_REMAP_DEFAULT_GUARD
                             : MANAGER_SETTINGS_REMAP_DEFAULT;
        } else if (manager_settings_starts_with(field, token, "custom")) {
            out->remap = MANAGER_SETTINGS_REMAP_CUSTOM;
        }
    }
}

int manager_settings_read_last_boot(const struct manager_storage_io *io,
                                    const char *path,
                                    struct manager_settings_last_boot *out)
{
    /* 16 KiB + NUL; the manager is single-threaded, so one static head
     * buffer keeps the log read off the main thread's stack. */
    static char buffer[MANAGER_SETTINGS_LOG_HEAD_BYTES + 1u];
    size_t length = 0u;
    size_t start = 0u;
    size_t position;
    int truncated = 0;
    int saw_build = 0;
    int result;

    if (out == NULL)
        return MANAGER_STORAGE_INVALID;
    memset(out, 0, sizeof(*out));
    out->kind = MANAGER_SETTINGS_LAST_BOOT_NO_LOG;
    if (io == NULL)
        return MANAGER_STORAGE_INVALID;
    if (path == NULL)
        path = MANAGER_SETTINGS_LOG_PATH;
    result = manager_storage_read_head(io, path, buffer, sizeof(buffer),
                                       &length, &truncated);
    if (result != MANAGER_STORAGE_OK)
        return result;
    for (position = 0u; position <= length; ++position) {
        const char *body;
        size_t body_length;
        size_t line_length;
        if (position < length && buffer[position] != '\n')
            continue;
        if (position == length) {
            /* Unterminated tail: complete when the whole file fit, a cut
             * line (ignored) when the 16 KiB cap truncated it. */
            if (start == length || truncated)
                break;
        }
        line_length = position - start;
        if (line_length != 0u && buffer[start + line_length - 1u] == '\r')
            --line_length;
        manager_settings_line_body(buffer + start, line_length, &body,
                                   &body_length);
        if (manager_settings_starts_with(body, body_length, "build ")) {
            saw_build = 1;
        } else if (manager_settings_starts_with(body, body_length,
                                                "ISAAC VITA SETTINGS: ")) {
            manager_settings_parse_receipt(body, body_length, out);
            return MANAGER_STORAGE_OK;
        }
        start = position + 1u;
    }
    out->kind = saw_build ? MANAGER_SETTINGS_LAST_BOOT_IGNORES
                          : MANAGER_SETTINGS_LAST_BOOT_NO_LOG;
    return MANAGER_STORAGE_OK;
}

/* status= tokens under which the receipt's values were applied: "ok",
 * "absent" (defaults, honestly reported) or a receipt without the field. */
static int manager_settings_status_applied(const char *status)
{
    return status[0] == '\0' || strcmp(status, "ok") == 0 ||
           strcmp(status, "absent") == 0;
}

/* "too-large" -> "TOO-LARGE", "io-error(0x80010013)" -> "IO-ERROR"; an
 * unrecognisable token renders as "UNKNOWN". */
#define MANAGER_SETTINGS_STATUS_WORD_CAPACITY 16u
static void manager_settings_status_word(const char *status, char *out,
                                         size_t capacity)
{
    size_t used = 0u;
    while (used + 1u < capacity && status[used] != '\0') {
        const char value = status[used];
        if (value >= 'a' && value <= 'z')
            out[used] = (char)(value - 'a' + 'A');
        else if ((value >= '0' && value <= '9') || value == '-')
            out[used] = value;
        else
            break;
        ++used;
    }
    out[used] = '\0';
    if (used == 0u)
        (void)snprintf(out, capacity, "%s", "UNKNOWN");
}

void manager_settings_last_boot_text(
    const struct manager_settings_last_boot *last_boot, char *out,
    size_t capacity)
{
    if (out == NULL || capacity == 0u)
        return;
    out[0] = '\0';
    if (last_boot == NULL)
        return;
    switch (last_boot->kind) {
    case MANAGER_SETTINGS_LAST_BOOT_RECEIPT:
        if (!manager_settings_status_applied(last_boot->status)) {
            /* schema / garbage / too-large / io-error(0x...): that boot ran
             * on the compile-time defaults whatever the file said, so the
             * row names the verdict instead of echoing "FPS 60".  The token
             * is folded to the narrow set (upper-case, cut at the first
             * byte outside a-z 0-9 '-'). */
            char status[MANAGER_SETTINGS_STATUS_WORD_CAPACITY];
            manager_settings_status_word(last_boot->status, status,
                                         sizeof(status));
            (void)snprintf(out, capacity, "FILE %s - DEFAULTS USED", status);
            break;
        }
        (void)snprintf(out, capacity, "FPS %u%s REMAP %s",
                       (unsigned int)last_boot->fps_lock,
                       last_boot->fps_ignored ? " IGNORED" : "",
                       last_boot->remap == MANAGER_SETTINGS_REMAP_CUSTOM
                           ? "CUSTOM"
                           : last_boot->remap ==
                                     MANAGER_SETTINGS_REMAP_DEFAULT_GUARD
                                 ? "DEFAULT GUARD"
                                 : "DEFAULT");
        break;
    case MANAGER_SETTINGS_LAST_BOOT_IGNORES:
        (void)snprintf(out, capacity, "%s", "GAME BUILD IGNORES PORT.INI");
        break;
    case MANAGER_SETTINGS_LAST_BOOT_NO_LOG:
    default:
        (void)snprintf(out, capacity, "%s", "NO LOG");
        break;
    }
}

#endif /* MANAGER_SETTINGS_UI */
