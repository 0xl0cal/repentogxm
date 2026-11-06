/* options.ini MaxRenderScale normalizer; see host_vita_render_scale.h.
 *
 * Called once by the port.ini loader (host_vita_runtime_settings.c) with
 * the parsed render_scale, before the guest starts and long before the
 * game opens options.ini through its own CRT.  No allocator, no stdio: the
 * file lands in one static buffer, the rewrite in a second, and the receipt
 * goes through isaac_vita_log.  Every sceIo* call sits behind a seam so the
 * host oracle (host_vita_render_scale_oracle.c) can drive the transform,
 * the publish protocol and its failure paths without a Vita. */
#include "host_vita_render_scale.h"

#if defined(ISAAC_VITA_RENDER_SCALE_DEFAULT)

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "platform.h"
#if !defined(ISAAC_VITA_RENDER_SCALE_ORACLE)
#include <psp2/io/fcntl.h>
#endif

#define RENDER_SCALE_DIR \
    ISAAC_VITA_NATIVE_DATA_ROOT "/Documents/My Games/Binding of Isaac Repentance"
#define RENDER_SCALE_PATH RENDER_SCALE_DIR "/options.ini"
#define RENDER_SCALE_TMP RENDER_SCALE_DIR "/options.ini.rs-tmp"
#define RENDER_SCALE_BAK RENDER_SCALE_DIR "/options.ini.rs-bak"
#define RENDER_SCALE_DEVICE "ux0:"
/* SCE_ERROR_ERRNO_ENOENT: the only open failure that means "absent". */
#define RENDER_SCALE_SCE_ENOENT 0x80010002u
/* SCE_ERROR_ERRNO_EIO: reported for a zero-length write. */
#define RENDER_SCALE_SCE_EIO 0x80010005u
#define RENDER_SCALE_CREATE_MODE 0777u
/* One byte past the cap: a full buffer is the too-large signal. */
#define RENDER_SCALE_IN_BYTES (ISAAC_VITA_RENDER_SCALE_FILE_MAX + 1u)

static const char k_key[] = "MaxRenderScale";
static const char k_section[] = "[Options]";
#define RENDER_SCALE_KEY_LEN ((uint32_t)(sizeof k_key - 1u))
#define RENDER_SCALE_SECTION_LEN ((uint32_t)(sizeof k_section - 1u))

static const char *const k_status_names[ISAAC_VITA_RENDER_SCALE_STATUS_COUNT] = {
    "unchanged", "rewritten", "appended", "absent", "empty", "too-large",
    "binary", "no-section", "duplicate", "read-error", "write-error",
    "rename-error",
};

_Static_assert(ISAAC_VITA_RENDER_SCALE_OUT_BYTES >=
                   ISAAC_VITA_RENDER_SCALE_FILE_MAX + 2u + 14u + 1u + 2u + 2u,
               "the rewrite buffer holds the file plus one inserted line");

static char s_in[RENDER_SCALE_IN_BYTES];
static char s_out[ISAAC_VITA_RENDER_SCALE_OUT_BYTES];

/* ---- I/O seams --------------------------------------------------------- */

#if defined(ISAAC_VITA_RENDER_SCALE_ORACLE)
static int rs_open_read(const char *path)
{
    return isaac_vita_render_scale_oracle_open_read(path);
}

static int rs_open_create(const char *path)
{
    return isaac_vita_render_scale_oracle_open_create(path);
}

static int rs_read(int descriptor, void *buffer, uint32_t size)
{
    return isaac_vita_render_scale_oracle_read(descriptor, buffer, size);
}

static int rs_write(int descriptor, const void *buffer, uint32_t size)
{
    return isaac_vita_render_scale_oracle_write(descriptor, buffer, size);
}

static int rs_close(int descriptor)
{
    return isaac_vita_render_scale_oracle_close(descriptor);
}

static int rs_sync_fd(int descriptor)
{
    return isaac_vita_render_scale_oracle_sync_fd(descriptor);
}

static int rs_remove(const char *path)
{
    return isaac_vita_render_scale_oracle_remove(path);
}

static int rs_rename(const char *source, const char *destination)
{
    return isaac_vita_render_scale_oracle_rename(source, destination);
}

static int rs_sync_device(const char *device)
{
    return isaac_vita_render_scale_oracle_sync_device(device);
}
#else
static int rs_open_read(const char *path)
{
    return sceIoOpen(path, SCE_O_RDONLY, 0);
}

static int rs_open_create(const char *path)
{
    return sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_EXCL,
                     RENDER_SCALE_CREATE_MODE);
}

static int rs_read(int descriptor, void *buffer, uint32_t size)
{
    return sceIoRead(descriptor, buffer, size);
}

static int rs_write(int descriptor, const void *buffer, uint32_t size)
{
    return sceIoWrite(descriptor, buffer, size);
}

static int rs_close(int descriptor)
{
    return sceIoClose(descriptor);
}

static int rs_sync_fd(int descriptor)
{
    return sceIoSyncByFd(descriptor, 0);
}

static int rs_remove(const char *path)
{
    return sceIoRemove(path);
}

static int rs_rename(const char *source, const char *destination)
{
    return sceIoRename(source, destination);
}

static int rs_sync_device(const char *device)
{
    return sceIoSync(device, 0);
}
#endif

/* ---- text helpers ------------------------------------------------------ */

static int rs_is_blank(char value)
{
    return value == ' ' || value == '\t';
}

static char rs_lower(char value)
{
    return (value >= 'A' && value <= 'Z') ? (char)(value + ('a' - 'A')) : value;
}

/* text[0..length) equals word[0..word_length) ignoring ASCII case. */
static int rs_equals_ci(const char *text, uint32_t length, const char *word,
                        uint32_t word_length)
{
    uint32_t index;
    if (length != word_length)
        return 0;
    for (index = 0u; index < length; ++index) {
        if (rs_lower(text[index]) != rs_lower(word[index]))
            return 0;
    }
    return 1;
}

struct rs_line {
    uint32_t begin; /* first content byte */
    uint32_t end;   /* one past the last content byte (ending excluded) */
    uint32_t next;  /* first byte of the next line (== length at EOF) */
};

/* Splits on '\n'; a '\r' right before it (or a lone trailing '\r' at EOF)
 * belongs to the ending.  Returns 0 once pos reaches length. */
static int rs_next_line(const char *in, uint32_t length, uint32_t pos,
                        struct rs_line *line)
{
    uint32_t index = pos;
    if (pos >= length)
        return 0;
    while (index < length && in[index] != '\n')
        ++index;
    line->begin = pos;
    line->end = index;
    line->next = index < length ? index + 1u : length;
    if (line->end > pos && in[line->end - 1u] == '\r')
        --line->end;
    return 1;
}

/* "<blanks>MaxRenderScale<blanks>=<blanks>value<blanks>" (key ignoring
 * case) -> 1 with the trimmed value token [*value_begin, *value_end). */
static int rs_match_key(const char *in, uint32_t begin, uint32_t end,
                        uint32_t *value_begin, uint32_t *value_end)
{
    uint32_t index = begin;
    while (index < end && rs_is_blank(in[index]))
        ++index;
    if (end - index < RENDER_SCALE_KEY_LEN ||
        !rs_equals_ci(in + index, RENDER_SCALE_KEY_LEN, k_key,
                      RENDER_SCALE_KEY_LEN))
        return 0;
    index += RENDER_SCALE_KEY_LEN;
    while (index < end && rs_is_blank(in[index]))
        ++index;
    if (index >= end || in[index] != '=')
        return 0;
    ++index;
    while (index < end && rs_is_blank(in[index]))
        ++index;
    *value_begin = index;
    while (end > index && rs_is_blank(in[end - 1u]))
        --end;
    *value_end = end;
    return 1;
}

/* First non-blank byte is '[': a section header.  *is_options is set when
 * the trimmed line is "[Options]" ignoring case. */
static int rs_match_section(const char *in, uint32_t begin, uint32_t end,
                            int *is_options)
{
    uint32_t index = begin;
    while (index < end && rs_is_blank(in[index]))
        ++index;
    if (index >= end || in[index] != '[')
        return 0;
    while (end > index && rs_is_blank(in[end - 1u]))
        --end;
    *is_options = rs_equals_ci(in + index, end - index, k_section,
                               RENDER_SCALE_SECTION_LEN);
    return 1;
}

static void rs_set_old(struct isaac_vita_render_scale_edit *edit,
                       const char *in, uint32_t begin, uint32_t end)
{
    uint32_t length = end - begin;
    if (length > ISAAC_VITA_RENDER_SCALE_OLD_MAX)
        length = ISAAC_VITA_RENDER_SCALE_OLD_MAX;
    memcpy(edit->old_value, in + begin, length);
    edit->old_value[length] = '\0';
}

/* ---- the transform ----------------------------------------------------- */

void isaac_vita_render_scale_transform(const char *in, uint32_t in_length,
                                       const char *target, char *out,
                                       uint32_t out_capacity,
                                       struct isaac_vita_render_scale_edit *edit)
{
    struct rs_line line;
    const char *ending = "\n";
    uint32_t ending_length = 1u;
    uint32_t target_length = (uint32_t)strlen(target);
    uint32_t pos = 0u;
    uint32_t matches = 0u;
    uint32_t value_begin = 0u;
    uint32_t value_end = 0u;
    uint32_t insert_at = 0u;
    int insert_found = 0;
    int options_seen = 0;
    int first_line = 1;
    uint32_t out_length;
    uint32_t cursor;

    memset(edit, 0, sizeof *edit);
    edit->old_value[0] = '-';
    if (in_length == 0u) {
        edit->status = (uint8_t)ISAAC_VITA_RENDER_SCALE_EMPTY;
        return;
    }
    if (in_length > ISAAC_VITA_RENDER_SCALE_FILE_MAX) {
        edit->status = (uint8_t)ISAAC_VITA_RENDER_SCALE_TOO_LARGE;
        return;
    }
    if (memchr(in, 0, in_length) != NULL) {
        edit->status = (uint8_t)ISAAC_VITA_RENDER_SCALE_BINARY;
        return;
    }
    while (rs_next_line(in, in_length, pos, &line)) {
        uint32_t begin;
        uint32_t end;
        int is_options = 0;

        if (first_line) {
            first_line = 0;
            if (line.next - line.end == 2u) {
                ending = "\r\n";
                ending_length = 2u;
            }
        }
        if (rs_match_section(in, line.begin, line.end, &is_options)) {
            if (options_seen && !insert_found) {
                insert_at = line.begin;
                insert_found = 1;
            }
            if (is_options)
                options_seen = 1;
        } else if (rs_match_key(in, line.begin, line.end, &begin, &end)) {
            if (matches == 0u) {
                value_begin = begin;
                value_end = end;
            }
            ++matches;
        }
        pos = line.next;
    }
    if (matches != 0u)
        rs_set_old(edit, in, value_begin, value_end);
    if (matches > 1u) {
        edit->status = (uint8_t)ISAAC_VITA_RENDER_SCALE_DUPLICATE;
        return;
    }
    if (matches == 1u) {
        if (value_end - value_begin == target_length &&
            memcmp(in + value_begin, target, target_length) == 0) {
            edit->status = (uint8_t)ISAAC_VITA_RENDER_SCALE_UNCHANGED;
            return;
        }
        out_length = in_length - (value_end - value_begin) + target_length;
        if (out_length > out_capacity) {
            edit->status = (uint8_t)ISAAC_VITA_RENDER_SCALE_TOO_LARGE;
            return;
        }
        memcpy(out, in, value_begin);
        memcpy(out + value_begin, target, target_length);
        memcpy(out + value_begin + target_length, in + value_end,
               in_length - value_end);
        edit->status = (uint8_t)ISAAC_VITA_RENDER_SCALE_REWRITTEN;
        edit->out_length = out_length;
        return;
    }
    if (!options_seen) {
        edit->status = (uint8_t)ISAAC_VITA_RENDER_SCALE_NO_SECTION;
        return;
    }
    /* Key absent: insert before the section after [Options], or at EOF
     * (completing the last line's missing newline first). */
    if (!insert_found)
        insert_at = in_length;
    out_length = in_length + RENDER_SCALE_KEY_LEN + 1u + target_length +
                 ending_length;
    if (insert_at == in_length && in[in_length - 1u] != '\n')
        out_length += ending_length;
    if (out_length > out_capacity) {
        edit->status = (uint8_t)ISAAC_VITA_RENDER_SCALE_TOO_LARGE;
        return;
    }
    memcpy(out, in, insert_at);
    cursor = insert_at;
    if (insert_at == in_length && in[in_length - 1u] != '\n') {
        memcpy(out + cursor, ending, ending_length);
        cursor += ending_length;
    }
    memcpy(out + cursor, k_key, RENDER_SCALE_KEY_LEN);
    cursor += RENDER_SCALE_KEY_LEN;
    out[cursor++] = '=';
    memcpy(out + cursor, target, target_length);
    cursor += target_length;
    memcpy(out + cursor, ending, ending_length);
    cursor += ending_length;
    memcpy(out + cursor, in + insert_at, in_length - insert_at);
    edit->status = (uint8_t)ISAAC_VITA_RENDER_SCALE_APPENDED;
    edit->out_length = out_length;
}

const char *isaac_vita_render_scale_target(uint8_t render_scale)
{
    return render_scale == 1u ? "1" : "99";
}

const char *isaac_vita_render_scale_status_name(
    enum isaac_vita_render_scale_status status)
{
    if ((unsigned)status >= (unsigned)ISAAC_VITA_RENDER_SCALE_STATUS_COUNT)
        return "?";
    return k_status_names[status];
}

/* ---- files ------------------------------------------------------------- */

/* Stale temp removed; a stranded aside copy goes back when options.ini is
 * missing (returns 1) or away when options.ini exists. */
static int rs_recover(void)
{
    int descriptor;

    (void)rs_remove(RENDER_SCALE_TMP);
    descriptor = rs_open_read(RENDER_SCALE_BAK);
    if (descriptor < 0)
        return 0;
    (void)rs_close(descriptor);
    descriptor = rs_open_read(RENDER_SCALE_PATH);
    if (descriptor >= 0) {
        (void)rs_close(descriptor);
        (void)rs_remove(RENDER_SCALE_BAK);
        return 0;
    }
    if ((uint32_t)descriptor != RENDER_SCALE_SCE_ENOENT)
        return 0;
    return rs_rename(RENDER_SCALE_BAK, RENDER_SCALE_PATH) >= 0 ? 1 : 0;
}

/* Fills s_in[0..*length).  Returns 0 when the bytes are the file (a count
 * of RENDER_SCALE_IN_BYTES means "more than the cap"), otherwise the status
 * (ABSENT on ENOENT, READ_ERROR with *rc on any other failure). */
static int rs_read_file(uint32_t *length, uint32_t *rc)
{
    int descriptor;
    uint32_t total = 0u;
    int status = 0;

    *length = 0u;
    *rc = 0u;
    descriptor = rs_open_read(RENDER_SCALE_PATH);
    if (descriptor < 0) {
        if ((uint32_t)descriptor == RENDER_SCALE_SCE_ENOENT)
            return (int)ISAAC_VITA_RENDER_SCALE_ABSENT;
        *rc = (uint32_t)descriptor;
        return (int)ISAAC_VITA_RENDER_SCALE_READ_ERROR;
    }
    while (total < RENDER_SCALE_IN_BYTES) {
        uint32_t room = RENDER_SCALE_IN_BYTES - total;
        int got = rs_read(descriptor, s_in + total, room);

        if (got < 0) {
            *rc = (uint32_t)got;
            status = (int)ISAAC_VITA_RENDER_SCALE_READ_ERROR;
            break;
        }
        if (got == 0)
            break;
        if ((uint32_t)got > room)
            got = (int)room;
        total += (uint32_t)got;
    }
    (void)rs_close(descriptor);
    *length = total;
    return status;
}

/* Temp (O_EXCL) -> write -> sync -> close -> options.ini aside -> temp into
 * place -> aside removed -> device sync.  Returns 0, or the status with
 * *rc; the previous options.ini is back in place on every failure. */
static int rs_publish(const char *bytes, uint32_t length, uint32_t *rc)
{
    int descriptor;
    int result;
    uint32_t done = 0u;

    *rc = 0u;
    descriptor = rs_open_create(RENDER_SCALE_TMP);
    if (descriptor < 0) {
        *rc = (uint32_t)descriptor;
        return (int)ISAAC_VITA_RENDER_SCALE_WRITE_ERROR;
    }
    while (done < length) {
        result = rs_write(descriptor, bytes + done, length - done);
        if (result <= 0) {
            *rc = result < 0 ? (uint32_t)result : RENDER_SCALE_SCE_EIO;
            (void)rs_close(descriptor);
            (void)rs_remove(RENDER_SCALE_TMP);
            return (int)ISAAC_VITA_RENDER_SCALE_WRITE_ERROR;
        }
        if ((uint32_t)result > length - done)
            result = (int)(length - done);
        done += (uint32_t)result;
    }
    result = rs_sync_fd(descriptor);
    if (result >= 0)
        result = rs_close(descriptor);
    else
        (void)rs_close(descriptor);
    if (result < 0) {
        *rc = (uint32_t)result;
        (void)rs_remove(RENDER_SCALE_TMP);
        return (int)ISAAC_VITA_RENDER_SCALE_WRITE_ERROR;
    }
    result = rs_rename(RENDER_SCALE_PATH, RENDER_SCALE_BAK);
    if (result < 0) {
        *rc = (uint32_t)result;
        (void)rs_remove(RENDER_SCALE_TMP);
        return (int)ISAAC_VITA_RENDER_SCALE_RENAME_ERROR;
    }
    result = rs_rename(RENDER_SCALE_TMP, RENDER_SCALE_PATH);
    if (result < 0) {
        *rc = (uint32_t)result;
        (void)rs_rename(RENDER_SCALE_BAK, RENDER_SCALE_PATH);
        (void)rs_remove(RENDER_SCALE_TMP);
        return (int)ISAAC_VITA_RENDER_SCALE_RENAME_ERROR;
    }
    (void)rs_remove(RENDER_SCALE_BAK);
    (void)rs_sync_device(RENDER_SCALE_DEVICE);
    return 0;
}

/* ---- entry ------------------------------------------------------------- */

void isaac_vita_render_scale_apply(uint8_t render_scale)
{
    const char *target = isaac_vita_render_scale_target(render_scale);
    struct isaac_vita_render_scale_edit edit;
    uint32_t bytes;
    uint32_t rc;
    int recovered;
    int status;

    recovered = rs_recover();
    status = rs_read_file(&bytes, &rc);
    if (status != 0) {
        memset(&edit, 0, sizeof edit);
        edit.status = (uint8_t)status;
        edit.old_value[0] = '-';
    } else if (bytes >= RENDER_SCALE_IN_BYTES) {
        memset(&edit, 0, sizeof edit);
        edit.status = (uint8_t)ISAAC_VITA_RENDER_SCALE_TOO_LARGE;
        edit.old_value[0] = '-';
    } else {
        isaac_vita_render_scale_transform(s_in, bytes, target, s_out,
                                          (uint32_t)sizeof s_out, &edit);
        if (edit.status == (uint8_t)ISAAC_VITA_RENDER_SCALE_REWRITTEN ||
            edit.status == (uint8_t)ISAAC_VITA_RENDER_SCALE_APPENDED) {
            status = rs_publish(s_out, edit.out_length, &rc);
            if (status != 0)
                edit.status = (uint8_t)status;
        }
    }
    isaac_vita_log(
        "ISAAC VITA RENDER SCALE: mode=%u target=%s status=%s old=%s "
        "bytes=%u rc=0x%08x recovered=%u",
        (unsigned)render_scale, target,
        isaac_vita_render_scale_status_name(
            (enum isaac_vita_render_scale_status)edit.status),
        edit.old_value, (unsigned)bytes, (unsigned)rc, (unsigned)recovered);
}

#endif /* ISAAC_VITA_RENDER_SCALE_DEFAULT */
