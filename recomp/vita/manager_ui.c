/*
 * Manager UI2 page controller (see manager_ui.h).
 *
 * Geometry is copied from the design mock-up renderer (render_c_home,
 * render_c_mods_ru, c_frame, c_hints): HOME launch highlight
 * (24,76,936,112) with "▸  Launch the game" at (48,85); list rows from
 * y=122, label f16 at (48,y+6), value f12 right-aligned at (900,y+8), "›"
 * at (916,y+6), sub-line f12 at (64,y+28) (+16), 32 px pitch, separator
 * (32..928, y+4) width 2 then +12, "Recovery" at (48,y+20) and the last
 * start at (48,y+38); MODS info right-aligned at (928,70), rows from y=92
 * at 36 px, highlight (24,y-2,916,y+32) radius 8, check glyph (44,y+4),
 * name (76,y+5), sub (900,y+8) right, scroll track (928,92,932,92+9*36);
 * status f12 at (32,470).  Pages the mocks do not draw (SAVES cards,
 * SETTINGS sections, CONTROLS, ABOUT, the boxes) reuse those x columns and
 * pitches; their y positions are chosen here and checked on the rendered
 * frames.
 */
#if defined(MANAGER_UI2)

#include "manager_ui.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define UI_SELECT ((uint32_t)MANAGER_SETTINGS_UI_BUTTON_SELECT)
#define UI_START ((uint32_t)MANAGER_SETTINGS_UI_BUTTON_START)
#define UI_UP ((uint32_t)MANAGER_SETTINGS_UI_BUTTON_UP)
#define UI_RIGHT ((uint32_t)MANAGER_SETTINGS_UI_BUTTON_RIGHT)
#define UI_DOWN ((uint32_t)MANAGER_SETTINGS_UI_BUTTON_DOWN)
#define UI_LEFT ((uint32_t)MANAGER_SETTINGS_UI_BUTTON_LEFT)
#define UI_LTRIGGER ((uint32_t)MANAGER_SETTINGS_UI_BUTTON_LTRIGGER)
#define UI_RTRIGGER ((uint32_t)MANAGER_SETTINGS_UI_BUTTON_RTRIGGER)
#define UI_TRIANGLE ((uint32_t)MANAGER_SETTINGS_UI_BUTTON_TRIANGLE)
#define UI_CIRCLE ((uint32_t)MANAGER_SETTINGS_UI_BUTTON_CIRCLE)
#define UI_CROSS ((uint32_t)MANAGER_SETTINGS_UI_BUTTON_CROSS)
#define UI_SQUARE ((uint32_t)MANAGER_SETTINGS_UI_BUTTON_SQUARE)
#define UI_REPEATABLE (UI_UP | UI_DOWN | UI_LEFT | UI_RIGHT)

#define UI_RESOURCE_ANIMATIONS "resources/packed/animations.a"
#define UI_RESOURCE_REPENTANCE "resources/packed/repentance.a"

/* Listing scratch: the new directory list is compared with the one held in
 * ui->mods so display names already read are kept (manager_ui_refresh). */
static struct manager_mod_entry ui_mod_list[MANAGER_UI_MAX_MODS];

static const enum manager_string_id ui_tab_ids[MANAGER_UI_PAGE_COUNT] = {
    MSTR_TAB_HOME,         MSTR_TAB_MODS,     MSTR_TAB_SAVES,
    MSTR_TAB_SETTINGS,     MSTR_TAB_ACHIEVEMENTS,
    MSTR_TAB_CONTROLS,     MSTR_TAB_ABOUT};
static const enum manager_string_id ui_title_ids[MANAGER_UI_PAGE_COUNT] = {
    MSTR_TITLE_HOME,       MSTR_TITLE_MODS,   MSTR_TITLE_SAVES,
    MSTR_TITLE_SETTINGS,   MSTR_TITLE_ACHIEVEMENTS,
    MSTR_TITLE_CONTROLS,   MSTR_TITLE_ABOUT};

/* ---- text helpers ----------------------------------------------------- */

static const char *ui_s(const struct manager_ui *ui, enum manager_string_id id)
{
    return manager_str(ui->lang, id);
}

static void ui_fmt(char *out, size_t capacity, const char *format, ...)
    __attribute__((format(printf, 3, 4)));

static void ui_fmt(char *out, size_t capacity, const char *format, ...)
{
    va_list arguments;
    int written;
    va_start(arguments, format);
    written = vsnprintf(out, capacity, format, arguments);
    va_end(arguments);
    if (written < 0 && capacity != 0u)
        out[0] = '\0';
}

static void ui_status(struct manager_ui *ui, enum manager_tone tone,
                      const char *format, ...)
    __attribute__((format(printf, 3, 4)));

static void ui_status(struct manager_ui *ui, enum manager_tone tone,
                      const char *format, ...)
{
    va_list arguments;
    int written;
    va_start(arguments, format);
    written = vsnprintf(ui->status, sizeof(ui->status), format, arguments);
    va_end(arguments);
    if (written < 0)
        ui->status[0] = '\0';
    ui->status_tone = (uint8_t)tone;
}

static void ui_status_id(struct manager_ui *ui, enum manager_tone tone,
                         enum manager_string_id id)
{
    ui_status(ui, tone, "%s", ui_s(ui, id));
}

static enum manager_string_id ui_storage_word(int result)
{
    switch (result) {
    case MANAGER_STORAGE_OK:
        return MSTR_RES_OK;
    case MANAGER_STORAGE_SYNC_WARNING:
        return MSTR_RES_SYNC_WARNING;
    case MANAGER_STORAGE_HASH_MISMATCH:
        return MSTR_RES_HASH;
    case MANAGER_STORAGE_CONFLICT:
        return MSTR_RES_CONFLICT;
    case MANAGER_STORAGE_TOO_LARGE:
        return MSTR_RES_TOO_LARGE;
    case MANAGER_STORAGE_CAPACITY:
        return MSTR_RES_CAPACITY;
    case MANAGER_STORAGE_INVALID:
        return MSTR_RES_INVALID;
    default:
        return MSTR_RES_IO;
    }
}

static enum manager_string_id ui_settings_word(uint8_t code)
{
    switch (code) {
    case MANAGER_SETTINGS_CODE_OK:
    case MANAGER_SETTINGS_CODE_NO_FILE:
        return MSTR_RES_OK;
    case MANAGER_SETTINGS_CODE_SYNC_WARNING:
        return MSTR_RES_SYNC_WARNING;
    case MANAGER_SETTINGS_CODE_NEED_A_AND_B:
        return MSTR_RES_NEED_A_AND_B;
    case MANAGER_SETTINGS_CODE_BAD_FILE:
        return MSTR_RES_BAD_FILE;
    case MANAGER_SETTINGS_CODE_NO_FREE_BACKUP_SLOT:
        return MSTR_RES_CAPACITY;
    case MANAGER_SETTINGS_CODE_HASH_MISMATCH:
        return MSTR_RES_HASH;
    case MANAGER_SETTINGS_CODE_FILE_CONFLICT:
        return MSTR_RES_CONFLICT;
    case MANAGER_SETTINGS_CODE_NOTHING_TO_UNDO:
        return MSTR_RES_NOTHING_TO_UNDO;
    case MANAGER_SETTINGS_CODE_BAD_ARCHIVE:
        return MSTR_RES_BAD_ARCHIVE;
    default:
        return MSTR_RES_IO;
    }
}

static void ui_size_text(const struct manager_ui *ui, uint64_t bytes,
                         char *out, size_t capacity)
{
    if (bytes < 1024u) {
        ui_fmt(out, capacity, ui_s(ui, MSTR_SIZE_BYTES), (unsigned int)bytes);
    } else if (bytes < 1024u * 1024u) {
        ui_fmt(out, capacity, ui_s(ui, MSTR_SIZE_KIB),
               (unsigned int)((bytes + 512u) / 1024u));
    } else {
        const uint64_t tenths = (bytes * 10u + 524288u) / 1048576u;
        ui_fmt(out, capacity, ui_s(ui, MSTR_SIZE_MIB),
               (unsigned int)(tenths / 10u), (unsigned int)(tenths % 10u));
    }
}

static const char *ui_basename(const char *path)
{
    const char *slash = strrchr(path, '/');
    return slash != NULL ? slash + 1 : path;
}

/* ---- model helpers ---------------------------------------------------- */

static enum manager_lang ui_resolve_lang(const struct manager_ui *ui,
                                         unsigned int ini_lang)
{
    if (ui->config.fonts_fallback)
        return MANAGER_LANG_EN; /* the 5x7 face has no Cyrillic */
    if (ini_lang == MANAGER_INI_LANG_RU)
        return MANAGER_LANG_RU;
    if (ini_lang == MANAGER_INI_LANG_EN)
        return MANAGER_LANG_EN;
    return ui->config.system_lang == MANAGER_LANG_RU ? MANAGER_LANG_RU
                                                     : MANAGER_LANG_EN;
}

static enum manager_theme_fallback ui_apply_theme(struct manager_ui *ui,
                                                  unsigned int theme)
{
    enum manager_theme_fallback why = MANAGER_THEME_FALLBACK_NONE;
    ui->theme = manager_theme_select(
        ui->themes,
        theme < MANAGER_THEME_COUNT ? (enum manager_theme_id)theme
                                    : MANAGER_THEME_GAME,
        &why);
    return why;
}

static int ui_theme_status(struct manager_ui *ui,
                           enum manager_theme_fallback why)
{
    if (why == MANAGER_THEME_FALLBACK_NOT_BUILT) {
        ui_status_id(ui, MANAGER_TONE_WARNING, MSTR_STATUS_THEME_NOT_BUILT);
        return 1;
    }
    if (why == MANAGER_THEME_FALLBACK_FAILED) {
        ui_status_id(ui, MANAGER_TONE_WARNING, MSTR_STATUS_THEME_FAILED);
        return 1;
    }
    return 0;
}

static int ui_stat(const struct manager_ui *ui, const char *path,
                   enum manager_storage_node_kind *kind, uint64_t *size)
{
    enum manager_storage_node_kind local_kind = MANAGER_STORAGE_NODE_OTHER;
    uint64_t local_size = 0u;
    int result;
    if (ui->io == NULL || ui->io->stat_path == NULL)
        return -1;
    result = ui->io->stat_path(ui->io->context, path, &local_kind,
                               &local_size);
    if (kind != NULL)
        *kind = local_kind;
    if (size != NULL)
        *size = local_size;
    return result;
}

static uint8_t ui_resource(const struct manager_ui *ui, const char *name,
                           uint64_t expected)
{
    char path[MANAGER_STORAGE_PATH_CAPACITY];
    enum manager_storage_node_kind kind = MANAGER_STORAGE_NODE_OTHER;
    uint64_t size = 0u;
    ui_fmt(path, sizeof(path), "%s/%s", MANAGER_STORAGE_ROOT, name);
    if (ui_stat(ui, path, &kind, &size) != 0 ||
        kind != MANAGER_STORAGE_NODE_FILE)
        return MANAGER_UI_RESOURCE_MISSING;
    return size == expected ? MANAGER_UI_RESOURCE_OK
                            : MANAGER_UI_RESOURCE_DIFFERENT;
}

static void ui_refresh_resources(struct manager_ui *ui)
{
    ui->resource_animations = ui_resource(
        ui, UI_RESOURCE_ANIMATIONS, MANAGER_UI_RESOURCE_ANIMATIONS_BYTES);
    ui->resource_repentance = ui_resource(
        ui, UI_RESOURCE_REPENTANCE, MANAGER_UI_RESOURCE_REPENTANCE_BYTES);
}

static void ui_refresh_mods(struct manager_ui *ui, int with_meta)
{
    size_t count = 0u;
    size_t rejected = 0u;
    size_t index;
    const size_t old_count = ui->mod_count;
    const int had_meta = ui->mod_meta_loaded;
    int all_known = 1;
    int result;

    memset(ui_mod_list, 0, sizeof(ui_mod_list));
    result = manager_storage_list_mods(ui->io, ui_mod_list,
                                       MANAGER_UI_MAX_MODS, &count, &rejected);
    ui->mod_scan_result = result;
    for (index = 0u; index < count; ++index) {
        struct manager_ui_mod *mod = &ui->mods[index];
        /* Same directory at the same index: its display name is current. */
        const int same = had_meta && index < old_count &&
                         strcmp(mod->entry.name, ui_mod_list[index].name) == 0;
        mod->entry = ui_mod_list[index];
        if (same)
            continue;
        if (with_meta) {
            (void)manager_mods_meta_read(ui->io, mod->entry.name, &mod->meta);
        } else {
            (void)snprintf(mod->meta.name, sizeof(mod->meta.name), "%s",
                           mod->entry.name);
            mod->meta.version[0] = '\0';
            mod->meta.from_metadata = 0u;
            all_known = 0;
        }
    }
    ui->mod_count = count;
    ui->mod_rejected = rejected;
    ui->mod_meta_loaded = with_meta || (had_meta && all_known);
    if (count == 0u)
        ui->mod_selection = 0u;
    else if (ui->mod_selection >= count)
        ui->mod_selection = count - 1u;
}

static size_t ui_mods_enabled(const struct manager_ui *ui)
{
    size_t index;
    size_t enabled = 0u;
    for (index = 0u; index < ui->mod_count; ++index)
        enabled += ui->mods[index].entry.disabled ? 0u : 1u;
    return enabled;
}

/* Indices of the backups shown for the selected slot (or all). */
static size_t ui_backup_view(const struct manager_ui *ui,
                             size_t view[MANAGER_UI_MAX_BACKUPS])
{
    size_t index;
    size_t count = 0u;
    for (index = 0u; index < ui->backup_count; ++index) {
        if (ui->show_all_backups || ui->backups[index].slot == ui->save_slot)
            view[count++] = index;
    }
    return count;
}

static void ui_clamp_backup_selection(struct manager_ui *ui)
{
    size_t view[MANAGER_UI_MAX_BACKUPS];
    const size_t count = ui_backup_view(ui, view);
    if (count == 0u)
        ui->backup_selection = 0u;
    else if (ui->backup_selection >= count)
        ui->backup_selection = count - 1u;
}

static void ui_refresh_saves(struct manager_ui *ui)
{
    int result;
    memset(ui->slots, 0, sizeof(ui->slots));
    ui->backup_count = 0u;
    ui->backup_rejected = 0u;
    result = manager_storage_get_save_slots(ui->io, ui->slots);
    if (result >= 0)
        result = manager_storage_list_save_backups(
            ui->io, ui->backups, MANAGER_UI_MAX_BACKUPS, &ui->backup_count,
            &ui->backup_rejected);
    ui->save_scan_result = result;
    ui_clamp_backup_selection(ui);
}

static unsigned int ui_save_files_present(const struct manager_ui *ui)
{
    unsigned int index;
    unsigned int present = 0u;
    for (index = 0u; index < 3u; ++index)
        present += ui->slots[index].present ? 1u : 0u;
    return present;
}

static void ui_detect_eid(struct manager_ui *ui)
{
    enum manager_storage_node_kind kind = MANAGER_STORAGE_NODE_OTHER;
    ui->eid_present =
        ui_stat(ui, MANAGER_STORAGE_MOD_ROOT "/" MANAGER_UI_EID_DIRECTORY,
                &kind, NULL) == 0 &&
        kind == MANAGER_STORAGE_NODE_DIRECTORY;
    ui->eid_saved = 0;
    if (ui->eid_present)
        ui->eid_saved =
            ui_stat(ui,
                    MANAGER_STORAGE_MOD_ROOT "/" MANAGER_UI_EID_DIRECTORY
                                             "/disable.it",
                    NULL, NULL) != 0;
    ui->eid_pending = ui->eid_saved;
}

static int ui_fps_is_uneven(uint16_t fps)
{
    return fps == 45u || fps == 40u;
}

static int ui_edits_refused(const struct manager_ui *ui)
{
    return ui->settings.state.status == MANAGER_SETTINGS_LOAD_IO_ERROR;
}

#if defined(MANAGER_DEBUG_OVERLAY_ROW)
/* port.ini debug_overlay goes through the shared parser's generic key API
 * (key_from_text / get_text / set_text; manager_settings_pending_count and
 * the serializer then cover it like every other key), so the row works with
 * any host_vita_settings_parse.c that knows the key with exactly these
 * values and the default off, and stays absent with one that does not. */
static const char *const ui_overlay_values[3] = {"off", "compact", "full"};
#define UI_OVERLAY_KEY_NAME "debug_overlay"

static int ui_overlay_key(void)
{
    enum isaac_vita_settings_key key;
    struct isaac_vita_settings probe;
    char text[ISAAC_VITA_SETTINGS_VALUE_MAX + 1u];
    unsigned int index;
    if (isaac_vita_settings_key_from_text(UI_OVERLAY_KEY_NAME,
                                          sizeof(UI_OVERLAY_KEY_NAME) - 1u,
                                          &key) != 0)
        return -1;
    isaac_vita_settings_defaults(&probe);
    if (isaac_vita_settings_get_text(&probe, key, text, sizeof(text)) == 0u ||
        strcmp(text, ui_overlay_values[0]) != 0)
        return -1;
    for (index = 0u; index < 3u; ++index) {
        if (isaac_vita_settings_set_text(&probe, key, ui_overlay_values[index],
                                         strlen(ui_overlay_values[index])) != 0 ||
            isaac_vita_settings_get_text(&probe, key, text, sizeof(text)) ==
                0u ||
            strcmp(text, ui_overlay_values[index]) != 0)
            return -1;
    }
    return (int)key;
}

/* Index 0..2 of the key's value in a settings struct; 0 when out of set. */
static unsigned int ui_overlay_index(const struct manager_ui *ui,
                                     const struct isaac_vita_settings *settings)
{
    char text[ISAAC_VITA_SETTINGS_VALUE_MAX + 1u];
    unsigned int index;
    if (ui->overlay_key < 0 ||
        isaac_vita_settings_get_text(
            settings, (enum isaac_vita_settings_key)ui->overlay_key, text,
            sizeof(text)) == 0u)
        return 0u;
    for (index = 0u; index < 3u; ++index) {
        if (strcmp(text, ui_overlay_values[index]) == 0)
            return index;
    }
    return 0u;
}
#endif

unsigned int manager_ui_settings_rows(const struct manager_ui *ui,
                                      uint8_t rows[MANAGER_UI_SET_COUNT])
{
    unsigned int count = 0u;
    rows[count++] = MANAGER_UI_SET_FPS;
    rows[count++] = MANAGER_UI_SET_RENDER;
    if (ui->eid_present)
        rows[count++] = MANAGER_UI_SET_EID;
#if defined(MANAGER_DEBUG_OVERLAY_ROW)
    if (ui->overlay_key >= 0)
        rows[count++] = MANAGER_UI_SET_DEBUG_OVERLAY;
#endif
    rows[count++] = MANAGER_UI_SET_LANG;
    rows[count++] = MANAGER_UI_SET_AUTOLAUNCH;
    rows[count++] = MANAGER_UI_SET_THEME;
    if (ui->settings.state.undo_available)
        rows[count++] = MANAGER_UI_SET_UNDO;
    rows[count++] = MANAGER_UI_SET_DEFAULTS;
    return count;
}

static unsigned int ui_settings_position(const struct manager_ui *ui,
                                         const uint8_t *rows,
                                         unsigned int count)
{
    unsigned int index;
    for (index = 0u; index < count; ++index) {
        if (rows[index] == ui->settings_row)
            return index;
    }
    return 0u;
}

static void ui_clamp_settings_row(struct manager_ui *ui)
{
    uint8_t rows[MANAGER_UI_SET_COUNT];
    const unsigned int count = manager_ui_settings_rows(ui, rows);
    unsigned int index;
    for (index = 0u; index < count; ++index) {
        if (rows[index] == ui->settings_row)
            return;
    }
    /* The row vanished (UNDO after its archive was used): the one above. */
    ui->settings_row = ui->settings_row == MANAGER_UI_SET_UNDO
                           ? MANAGER_UI_SET_THEME
                           : rows[0];
}

static int ui_row_pending(const struct manager_ui *ui, unsigned int row)
{
    const struct manager_settings_state *state = &ui->settings.state;
    switch (row) {
    case MANAGER_UI_SET_FPS:
        return state->pending.fps_lock != state->saved.fps_lock;
    case MANAGER_UI_SET_RENDER:
        return state->pending.render_scale != state->saved.render_scale;
    case MANAGER_UI_SET_EID:
        return ui->eid_present && ui->eid_pending != ui->eid_saved;
    case MANAGER_UI_SET_LANG:
        return ui->ini_pending.lang != ui->ini_saved.lang;
    case MANAGER_UI_SET_AUTOLAUNCH:
        return ui->ini_pending.autolaunch != ui->ini_saved.autolaunch;
    case MANAGER_UI_SET_THEME:
        return ui->ini_pending.theme != ui->ini_saved.theme;
#if defined(MANAGER_DEBUG_OVERLAY_ROW)
    case MANAGER_UI_SET_DEBUG_OVERLAY:
        return ui_overlay_index(ui, &state->pending) !=
               ui_overlay_index(ui, &state->saved);
#endif
    default:
        return 0;
    }
}

unsigned int manager_ui_pending_count(const struct manager_ui *ui)
{
    unsigned int count = manager_settings_pending_count(&ui->settings.state);
    count += ui_row_pending(ui, MANAGER_UI_SET_EID) ? 1u : 0u;
    count += ui_row_pending(ui, MANAGER_UI_SET_LANG) ? 1u : 0u;
    count += ui_row_pending(ui, MANAGER_UI_SET_AUTOLAUNCH) ? 1u : 0u;
    count += ui_row_pending(ui, MANAGER_UI_SET_THEME) ? 1u : 0u;
    return count;
}

static enum manager_string_id ui_setting_label(unsigned int row)
{
#if defined(MANAGER_DEBUG_OVERLAY_ROW)
    static const enum manager_string_id labels[MANAGER_UI_SET_COUNT] = {
        MSTR_SET_FPS,  MSTR_SET_RENDER,     MSTR_SET_EID,
        MSTR_SET_LANG, MSTR_SET_AUTOLAUNCH, MSTR_SET_THEME,
        MSTR_SET_UNDO, MSTR_SET_DEFAULTS,   MSTR_SET_OVERLAY};
#else
    static const enum manager_string_id labels[MANAGER_UI_SET_COUNT] = {
        MSTR_SET_FPS,  MSTR_SET_RENDER,     MSTR_SET_EID,
        MSTR_SET_LANG, MSTR_SET_AUTOLAUNCH, MSTR_SET_THEME,
        MSTR_SET_UNDO, MSTR_SET_DEFAULTS};
#endif
    return labels[row < MANAGER_UI_SET_COUNT ? row : 0u];
}

static void ui_setting_value(const struct manager_ui *ui, unsigned int row,
                             char *out, size_t capacity)
{
    const struct manager_settings_state *state = &ui->settings.state;
    out[0] = '\0';
    switch (row) {
    case MANAGER_UI_SET_FPS:
        ui_fmt(out, capacity, "%u", (unsigned int)state->pending.fps_lock);
        break;
    case MANAGER_UI_SET_RENDER:
        ui_fmt(out, capacity, "%s",
               ui_s(ui, state->pending.render_scale >= 2u
                            ? MSTR_SET_RENDER_SHARP
                            : MSTR_SET_RENDER_FAST));
        break;
    case MANAGER_UI_SET_EID:
        ui_fmt(out, capacity, "%s",
               ui_s(ui, ui->eid_pending ? MSTR_WORD_ON : MSTR_WORD_OFF));
        break;
    case MANAGER_UI_SET_LANG: {
        /* The 5x7 fallback has no Cyrillic: "Russian" instead. */
        const enum manager_string_id russian = ui->config.fonts_fallback
                                                   ? MSTR_SET_LANG_RU_LATIN
                                                   : MSTR_SET_LANG_RU;
        if (ui->ini_pending.lang == MANAGER_INI_LANG_EN)
            ui_fmt(out, capacity, "%s", ui_s(ui, MSTR_SET_LANG_EN));
        else if (ui->ini_pending.lang == MANAGER_INI_LANG_RU)
            ui_fmt(out, capacity, "%s", ui_s(ui, russian));
        else
            ui_fmt(out, capacity, ui_s(ui, MSTR_SET_LANG_AUTO),
                   ui_s(ui, ui->config.system_lang == MANAGER_LANG_RU
                                ? russian
                                : MSTR_SET_LANG_EN));
        break;
    }
    case MANAGER_UI_SET_AUTOLAUNCH:
        if (ui->ini_pending.autolaunch == 0u)
            ui_fmt(out, capacity, "%s", ui_s(ui, MSTR_WORD_OFF));
        else
            ui_fmt(out, capacity, ui_s(ui, MSTR_SET_AUTOLAUNCH_SEC),
                   (unsigned int)ui->ini_pending.autolaunch);
        break;
    case MANAGER_UI_SET_THEME: {
        static const enum manager_string_id words[MANAGER_THEME_COUNT] = {
            MSTR_SET_THEME_GAME, MSTR_SET_THEME_DARK, MSTR_SET_THEME_LIGHT};
        ui_fmt(out, capacity, "%s",
               ui_s(ui, words[ui->ini_pending.theme < MANAGER_THEME_COUNT
                                  ? ui->ini_pending.theme
                                  : 0u]));
        break;
    }
    case MANAGER_UI_SET_UNDO:
        ui_fmt(out, capacity, ui_s(ui, MSTR_SET_UNDO_VALUE),
               state->undo_index);
        break;
    case MANAGER_UI_SET_DEFAULTS:
        ui_fmt(out, capacity, "%s", ui_s(ui, MSTR_SET_DEFAULTS_VALUE));
        break;
#if defined(MANAGER_DEBUG_OVERLAY_ROW)
    case MANAGER_UI_SET_DEBUG_OVERLAY: {
        static const enum manager_string_id words[3] = {
            MSTR_WORD_OFF, MSTR_SET_OVERLAY_COMPACT, MSTR_SET_OVERLAY_FULL};
        ui_fmt(out, capacity, "%s",
               ui_s(ui, words[ui_overlay_index(ui, &state->pending)]));
        break;
    }
#endif
    default:
        break;
    }
}

static enum manager_string_id ui_setting_help(const struct manager_ui *ui,
                                              unsigned int row)
{
    switch (row) {
    case MANAGER_UI_SET_FPS:
        return ui_fps_is_uneven(ui->settings.state.pending.fps_lock)
                   ? MSTR_SET_FPS_HELP_UNEVEN
                   : MSTR_SET_FPS_HELP;
    case MANAGER_UI_SET_RENDER:
        return MSTR_SET_RENDER_HELP;
    case MANAGER_UI_SET_EID:
        return MSTR_SET_EID_HELP;
    case MANAGER_UI_SET_LANG:
        return MSTR_SET_LANG_HELP;
    case MANAGER_UI_SET_AUTOLAUNCH:
        return MSTR_SET_AUTOLAUNCH_HELP;
    case MANAGER_UI_SET_THEME:
        return MSTR_SET_THEME_HELP;
    case MANAGER_UI_SET_UNDO:
        return MSTR_SET_UNDO_HELP;
#if defined(MANAGER_DEBUG_OVERLAY_ROW)
    case MANAGER_UI_SET_DEBUG_OVERLAY:
        return MSTR_SET_OVERLAY_HELP;
#endif
    default:
        return MSTR_SET_DEFAULTS_HELP;
    }
}

/* Last game start, from the first 16 KiB of first-arm-fault.log;
 * `labelled` prefixes "Last game start: " (ABOUT has it as a heading). */
static void ui_last_start_text(const struct manager_ui *ui, char *out,
                               size_t capacity, int labelled)
{
    const struct manager_settings_last_boot *boot = &ui->settings.last_boot;
    char detail[192];
    if (boot->kind == MANAGER_SETTINGS_LAST_BOOT_IGNORES) {
        ui_fmt(detail, sizeof(detail), "%s", ui_s(ui, MSTR_LAST_IGNORES));
    } else if (boot->kind == MANAGER_SETTINGS_LAST_BOOT_RECEIPT) {
        char port[96];
        char fps[96];
        char token[16];
        size_t used = 0u;
        while (used + 1u < sizeof(token) && boot->status[used] != '\0' &&
               ((boot->status[used] >= 'a' && boot->status[used] <= 'z') ||
                (boot->status[used] >= '0' && boot->status[used] <= '9') ||
                boot->status[used] == '-')) {
            token[used] = boot->status[used];
            ++used;
        }
        token[used] = '\0';
        if (boot->status[0] == '\0' || strcmp(boot->status, "ok") == 0)
            ui_fmt(port, sizeof(port), "%s", ui_s(ui, MSTR_LAST_PORT_OK));
        else if (strcmp(boot->status, "absent") == 0)
            ui_fmt(port, sizeof(port), "%s", ui_s(ui, MSTR_LAST_PORT_ABSENT));
        else
            ui_fmt(port, sizeof(port), ui_s(ui, MSTR_LAST_PORT_BAD),
                   token[0] != '\0' ? token : ui_s(ui, MSTR_WORD_UNKNOWN));
        if (boot->fps_ignored)
            ui_fmt(fps, sizeof(fps), ui_s(ui, MSTR_LAST_FPS_IGNORED),
                   (unsigned int)boot->fps_lock);
        else
            ui_fmt(fps, sizeof(fps), "fps_lock %u",
                   (unsigned int)boot->fps_lock);
        ui_fmt(detail, sizeof(detail), "%s · %s%s%s", port, fps,
               boot->remap == MANAGER_SETTINGS_REMAP_CUSTOM ? " · " : "",
               boot->remap == MANAGER_SETTINGS_REMAP_CUSTOM
                   ? ui_s(ui, MSTR_LAST_REMAP_CUSTOM)
                   : "");
    } else {
        ui_fmt(detail, sizeof(detail), "%s", ui_s(ui, MSTR_LAST_NO_LOG));
    }
    if (labelled)
        ui_fmt(out, capacity, ui_s(ui, MSTR_LAST_START), detail);
    else
        ui_fmt(out, capacity, "%s", detail);
}

static void ui_recovery_text(const struct manager_ui *ui, char *out,
                             size_t capacity)
{
    const int result = ui->config.recovery_result;
    if (result == MANAGER_STORAGE_OK)
        ui_fmt(out, capacity, "%s", ui_s(ui, MSTR_HOME_RECOVERY_OK));
    else if (result > 0)
        ui_fmt(out, capacity, "%s", ui_s(ui, MSTR_HOME_RECOVERY_WARN));
    else
        ui_fmt(out, capacity, ui_s(ui, MSTR_HOME_RECOVERY_FAIL),
               ui_s(ui, ui_storage_word(result)));
}

static enum manager_string_id ui_resource_word(uint8_t state)
{
    if (state == MANAGER_UI_RESOURCE_OK)
        return MSTR_ABOUT_RES_OK;
    if (state == MANAGER_UI_RESOURCE_DIFFERENT)
        return MSTR_ABOUT_RES_DIFF;
    return MSTR_ABOUT_RES_MISSING;
}

static enum manager_string_id ui_resources_summary(const struct manager_ui *ui)
{
    if (ui->resource_animations == MANAGER_UI_RESOURCE_MISSING ||
        ui->resource_repentance == MANAGER_UI_RESOURCE_MISSING)
        return MSTR_ABOUT_RES_MISSING;
    if (ui->resource_animations == MANAGER_UI_RESOURCE_DIFFERENT ||
        ui->resource_repentance == MANAGER_UI_RESOURCE_DIFFERENT)
        return MSTR_ABOUT_RES_DIFF;
    return MSTR_ABOUT_RES_OK;
}

/* ---- navigation ------------------------------------------------------- */

#if defined(MANAGER_TROPHY_IMPORT)
static void ui_trophy_refresh(struct manager_ui *ui);
#endif

static int ui_page_writes(enum manager_ui_page page)
{
#if defined(MANAGER_TROPHY_IMPORT)
    if (page == MANAGER_UI_PAGE_ACHIEVEMENTS)
        return 1; /* the trophy import request */
#endif
    return page == MANAGER_UI_PAGE_MODS || page == MANAGER_UI_PAGE_SAVES ||
           page == MANAGER_UI_PAGE_SETTINGS;
}

static void ui_open_settings(struct manager_ui *ui)
{
    manager_settings_ui_open(&ui->settings, ui->io);
    ui->settings.armed = MANAGER_SETTINGS_UI_ARM_NONE;
    ui->ini_pending = ui->ini_saved;
    ui_detect_eid(ui);
    ui->settings_row = MANAGER_UI_SET_FPS;
}

static void ui_goto(struct manager_ui *ui, enum manager_ui_page page)
{
    ui->page = page;
    ui->confirm = MANAGER_UI_CONFIRM_NONE;
    ui->detail_open = 0;
    ui->settings.armed = MANAGER_SETTINGS_UI_ARM_NONE;
    ui_status_id(ui, MANAGER_TONE_NORMAL, MSTR_STATUS_READY);
    switch (page) {
    case MANAGER_UI_PAGE_MODS:
        ui_refresh_mods(ui, 1);
        if (ui->mod_scan_result < 0)
            ui_status(ui, MANAGER_TONE_ERROR, ui_s(ui, MSTR_MODS_SCAN_FAILED),
                      ui_s(ui, ui_storage_word(ui->mod_scan_result)));
        break;
    case MANAGER_UI_PAGE_SAVES:
        ui_refresh_saves(ui);
        if (ui->save_scan_result < 0)
            ui_status(ui, MANAGER_TONE_ERROR, ui_s(ui, MSTR_SAVES_SCAN_FAILED),
                      ui_s(ui, ui_storage_word(ui->save_scan_result)));
        break;
    case MANAGER_UI_PAGE_SETTINGS:
        ui_open_settings(ui);
        if (ui_edits_refused(ui))
            ui_status_id(ui, MANAGER_TONE_ERROR, MSTR_SET_IOERR);
        break;
    case MANAGER_UI_PAGE_ABOUT:
        ui_refresh_resources(ui);
        break;
#if defined(MANAGER_TROPHY_IMPORT)
    case MANAGER_UI_PAGE_ACHIEVEMENTS:
        ui_trophy_refresh(ui);
        break;
#endif
    default:
        break;
    }
    if (ui->ftp_on && ui_page_writes(page) &&
        ui->status_tone != MANAGER_TONE_ERROR)
        ui_status_id(ui, MANAGER_TONE_WARNING, MSTR_STATUS_READONLY);
}

static void ui_leave_to(struct manager_ui *ui, enum manager_ui_page page)
{
    if (ui->page == MANAGER_UI_PAGE_SETTINGS &&
        manager_ui_pending_count(ui) != 0u) {
        ui->settings.armed = MANAGER_SETTINGS_UI_ARM_DISCARD;
        ui->confirm = MANAGER_UI_CONFIRM_SETTINGS;
        ui->leave_target = page;
        return;
    }
    ui_goto(ui, page);
}

/* ---- HOME ------------------------------------------------------------- */

static void ui_ftp_toggle(struct manager_ui *ui)
{
    if (ui->ftp_on) {
        if (ui->host.ftp_stop != NULL)
            ui->host.ftp_stop(ui->host.context);
        ui->ftp_on = 0;
        ui->ftp_failed = 0;
        ui->ftp_ip[0] = '\0';
        ui->ftp_port = 0u;
        ui_status_id(ui, MANAGER_TONE_NORMAL, MSTR_STATUS_FTP_OFF);
        return;
    }
    ui->ftp_ip[0] = '\0';
    ui->ftp_port = 0u;
    if (ui->host.ftp_start == NULL ||
        ui->host.ftp_start(ui->host.context, ui->ftp_ip, sizeof(ui->ftp_ip),
                           &ui->ftp_port) < 0) {
        if (ui->host.ftp_stop != NULL)
            ui->host.ftp_stop(ui->host.context);
        ui->ftp_on = 0;
        ui->ftp_failed = 1;
        ui->ftp_ip[0] = '\0';
        ui->ftp_port = 0u;
        ui_status_id(ui, MANAGER_TONE_ERROR, MSTR_STATUS_FTP_FAILED);
        return;
    }
    ui->ftp_ip[sizeof(ui->ftp_ip) - 1u] = '\0';
    ui->ftp_on = 1;
    ui->ftp_failed = 0;
    ui_status_id(ui, MANAGER_TONE_WARNING, MSTR_STATUS_FTP_ON);
}

static unsigned int ui_home(struct manager_ui *ui, uint32_t pressed)
{
    static const enum manager_ui_page targets[MANAGER_UI_HOME_COUNT] = {
        MANAGER_UI_PAGE_HOME,         MANAGER_UI_PAGE_HOME,
        MANAGER_UI_PAGE_MODS,         MANAGER_UI_PAGE_SAVES,
        MANAGER_UI_PAGE_SETTINGS,     MANAGER_UI_PAGE_ACHIEVEMENTS,
        MANAGER_UI_PAGE_CONTROLS,     MANAGER_UI_PAGE_ABOUT};
    if ((pressed & UI_UP) != 0u) {
        if (ui->home_row > 0u)
            --ui->home_row;
    } else if ((pressed & UI_DOWN) != 0u) {
        if (ui->home_row + 1u < MANAGER_UI_HOME_COUNT)
            ++ui->home_row;
    } else if ((pressed & UI_CROSS) != 0u) {
        if (ui->home_row == MANAGER_UI_HOME_LAUNCH) {
            if (ui->ftp_on) {
                ui->confirm = MANAGER_UI_CONFIRM_LAUNCH_FTP;
                return 0u;
            }
            return MANAGER_UI_LAUNCH;
        }
        if (ui->home_row == MANAGER_UI_HOME_FTP)
            ui_ftp_toggle(ui);
        else
            ui_goto(ui, targets[ui->home_row]);
    }
    return 0u;
}

/* ---- MODS ------------------------------------------------------------- */

static void ui_mods_toggle(struct manager_ui *ui)
{
    struct manager_ui_mod *mod = &ui->mods[ui->mod_selection];
    const int enable = mod->entry.disabled;
    char name[MANAGER_MODS_META_NAME_CAPACITY];
    const int result =
        manager_storage_set_mod_enabled(ui->io, mod->entry.name, enable);
    (void)snprintf(name, sizeof(name), "%s", mod->meta.name);
    if (result < 0)
        ui_status(ui, MANAGER_TONE_ERROR, ui_s(ui, MSTR_MODS_TOGGLE_FAILED),
                  name, ui_s(ui, ui_storage_word(result)));
    else
        ui_status(ui,
                  result == MANAGER_STORAGE_SYNC_WARNING ? MANAGER_TONE_WARNING
                                                         : MANAGER_TONE_OK,
                  ui_s(ui, enable ? MSTR_MODS_ENABLED : MSTR_MODS_DISABLED),
                  name);
    ui_refresh_mods(ui, 1);
}

static int ui_mods_any_disabled(const struct manager_ui *ui)
{
    return ui_mods_enabled(ui) < ui->mod_count;
}

static void ui_mods_all(struct manager_ui *ui)
{
    const int enable = ui_mods_any_disabled(ui);
    unsigned int changed = 0u;
    unsigned int failed = 0u;
    size_t index;
    for (index = 0u; index < ui->mod_count; ++index) {
        const struct manager_mod_entry *entry = &ui->mods[index].entry;
        if ((entry->disabled != 0) != (enable != 0))
            continue;
        if (manager_storage_set_mod_enabled(ui->io, entry->name, enable) < 0)
            ++failed;
        else
            ++changed;
    }
    if (failed != 0u)
        ui_status(ui, MANAGER_TONE_ERROR, ui_s(ui, MSTR_MODS_ALL_PARTIAL),
                  changed, failed);
    else
        ui_status(ui, MANAGER_TONE_OK, ui_s(ui, MSTR_MODS_ALL_DONE), changed);
    ui_refresh_mods(ui, 1);
}

static void ui_mods(struct manager_ui *ui, uint32_t pressed)
{
    if ((pressed & UI_CIRCLE) != 0u) {
        ui_goto(ui, MANAGER_UI_PAGE_HOME);
    } else if ((pressed & UI_UP) != 0u) {
        if (ui->mod_selection > 0u)
            --ui->mod_selection;
    } else if ((pressed & UI_DOWN) != 0u) {
        if (ui->mod_selection + 1u < ui->mod_count)
            ++ui->mod_selection;
    } else if ((pressed & UI_CROSS) != 0u && ui->mod_count != 0u) {
        if (ui->ftp_on)
            ui_status_id(ui, MANAGER_TONE_WARNING, MSTR_STATUS_READONLY);
        else
            ui_mods_toggle(ui);
    } else if ((pressed & UI_SQUARE) != 0u && ui->mod_count != 0u) {
        if (ui->ftp_on)
            ui_status_id(ui, MANAGER_TONE_WARNING, MSTR_STATUS_READONLY);
        else
            ui->confirm = MANAGER_UI_CONFIRM_MODS_ALL;
    } else if ((pressed & UI_TRIANGLE) != 0u && ui->mod_count != 0u) {
        ui->detail_open = 1;
    }
}

/* ---- SAVES ------------------------------------------------------------ */

static void ui_saves_backup(struct manager_ui *ui)
{
    struct manager_save_backup created;
    char word[128];
    const int slot = ui->save_slot;
    int result;
    if (!ui->slots[slot - 1].present) {
        ui_status(ui, MANAGER_TONE_WARNING, ui_s(ui, MSTR_SAVES_MISSING), slot);
        return;
    }
    memset(&created, 0, sizeof(created));
    result = manager_storage_backup_slot(ui->io, slot, &created);
    if (result < 0) {
        ui_status(ui, MANAGER_TONE_ERROR, ui_s(ui, MSTR_SAVES_BACKUP_FAILED),
                  slot, ui_s(ui, ui_storage_word(result)));
    } else {
#if defined(ISAAC_VITA_MANAGER_SAVE_GAMESTATE)
        ui_fmt(word, sizeof(word), "%s%s", ui_s(ui, ui_storage_word(result)),
               ui_s(ui, created.has_gamestate ? MSTR_SAVES_RUN_SAVED
                                              : MSTR_SAVES_RUN_NONE));
#else
        ui_fmt(word, sizeof(word), "%s", ui_s(ui, ui_storage_word(result)));
#endif
        ui_status(ui,
                  result == MANAGER_STORAGE_SYNC_WARNING ? MANAGER_TONE_WARNING
                                                         : MANAGER_TONE_OK,
                  ui_s(ui, MSTR_SAVES_BACKUP_DONE), slot, word);
    }
    ui_refresh_saves(ui);
}

static const struct manager_save_backup *ui_selected_backup(
    const struct manager_ui *ui)
{
    size_t view[MANAGER_UI_MAX_BACKUPS];
    const size_t count = ui_backup_view(ui, view);
    if (count == 0u || ui->backup_selection >= count)
        return NULL;
    return &ui->backups[view[ui->backup_selection]];
}

static void ui_saves_restore(struct manager_ui *ui)
{
    const struct manager_save_backup *selected = ui_selected_backup(ui);
    struct manager_save_backup backup;
    char name[MANAGER_STORAGE_NAME_CAPACITY + 64u];
    int result;
    if (selected == NULL)
        return;
    backup = *selected;
    result = manager_storage_restore_backup(ui->io, &backup);
    if (result < 0) {
        ui_status(ui, MANAGER_TONE_ERROR, ui_s(ui, MSTR_SAVES_RESTORE_FAILED),
                  ui_s(ui, ui_storage_word(result)), backup.slot);
    } else {
#if defined(ISAAC_VITA_MANAGER_SAVE_GAMESTATE)
        ui_fmt(name, sizeof(name), "%s%s", ui_basename(backup.path),
               ui_s(ui, backup.has_gamestate ? MSTR_SAVES_RUN_RESTORED
                                             : MSTR_SAVES_RUN_KEPT));
#else
        ui_fmt(name, sizeof(name), "%s", ui_basename(backup.path));
#endif
        ui_status(ui,
                  result == MANAGER_STORAGE_SYNC_WARNING ? MANAGER_TONE_WARNING
                                                         : MANAGER_TONE_OK,
                  ui_s(ui, MSTR_SAVES_RESTORE_DONE), backup.slot, name);
    }
    ui_refresh_saves(ui);
}

static void ui_saves(struct manager_ui *ui, uint32_t pressed)
{
    size_t view[MANAGER_UI_MAX_BACKUPS];
    const size_t count = ui_backup_view(ui, view);
    if ((pressed & UI_CIRCLE) != 0u) {
        ui_goto(ui, MANAGER_UI_PAGE_HOME);
    } else if ((pressed & UI_LEFT) != 0u) {
        ui->save_slot = ui->save_slot == 1 ? 3 : ui->save_slot - 1;
        ui->backup_selection = 0u;
    } else if ((pressed & UI_RIGHT) != 0u) {
        ui->save_slot = ui->save_slot == 3 ? 1 : ui->save_slot + 1;
        ui->backup_selection = 0u;
    } else if ((pressed & UI_UP) != 0u) {
        if (ui->backup_selection > 0u)
            --ui->backup_selection;
    } else if ((pressed & UI_DOWN) != 0u) {
        if (ui->backup_selection + 1u < count)
            ++ui->backup_selection;
    } else if ((pressed & UI_TRIANGLE) != 0u) {
        ui->show_all_backups = !ui->show_all_backups;
        ui->backup_selection = 0u;
    } else if ((pressed & UI_SQUARE) != 0u) {
        if (ui->ftp_on)
            ui_status_id(ui, MANAGER_TONE_WARNING, MSTR_STATUS_READONLY);
        else
            ui_saves_backup(ui);
    } else if ((pressed & UI_CROSS) != 0u && count != 0u) {
        if (ui->ftp_on)
            ui_status_id(ui, MANAGER_TONE_WARNING, MSTR_STATUS_READONLY);
        else
            ui->confirm = MANAGER_UI_CONFIRM_RESTORE;
    }
}

/* ---- SETTINGS --------------------------------------------------------- */

static void ui_settings_changed_status(struct manager_ui *ui, unsigned int row)
{
    char value[96];
    ui_setting_value(ui, row, value, sizeof(value));
    ui_status(ui, MANAGER_TONE_NORMAL, ui_s(ui, MSTR_SET_CHANGED),
              ui_s(ui, ui_setting_label(row)), value);
}

static void ui_settings_step(struct manager_ui *ui, int direction)
{
    struct manager_settings_state *state = &ui->settings.state;
    const unsigned int row = ui->settings_row;
    if (row == MANAGER_UI_SET_UNDO || row == MANAGER_UI_SET_DEFAULTS)
        return;
    if (ui->ftp_on) {
        ui_status_id(ui, MANAGER_TONE_WARNING, MSTR_STATUS_READONLY);
        return;
    }
    switch (row) {
    case MANAGER_UI_SET_FPS:
    case MANAGER_UI_SET_RENDER:
        if (ui_edits_refused(ui)) {
            ui_status_id(ui, MANAGER_TONE_ERROR, MSTR_SET_IOERR);
            return;
        }
        if (row == MANAGER_UI_SET_FPS)
            (void)manager_settings_set_fps_lock(
                state, manager_settings_fps_lock_step(state->pending.fps_lock,
                                                      direction));
        else
            (void)manager_settings_set_render_scale(
                state, manager_settings_render_scale_step(
                           state->pending.render_scale, direction));
        break;
    case MANAGER_UI_SET_EID:
        ui->eid_pending = direction > 0;
        break;
    case MANAGER_UI_SET_LANG:
        if (direction > 0 && ui->ini_pending.lang + 1u < MANAGER_INI_LANG_COUNT)
            ++ui->ini_pending.lang;
        else if (direction < 0 && ui->ini_pending.lang > 0u)
            --ui->ini_pending.lang;
        ui->lang = ui_resolve_lang(ui, ui->ini_pending.lang);
        break;
    case MANAGER_UI_SET_AUTOLAUNCH:
        ui->ini_pending.autolaunch =
            manager_ini_autolaunch_step(ui->ini_pending.autolaunch, direction);
        break;
    case MANAGER_UI_SET_THEME:
        if (direction > 0 && ui->ini_pending.theme + 1u < MANAGER_THEME_COUNT)
            ++ui->ini_pending.theme;
        else if (direction < 0 && ui->ini_pending.theme > 0u)
            --ui->ini_pending.theme;
        if (ui_theme_status(ui, ui_apply_theme(ui, ui->ini_pending.theme)))
            return;
        break;
#if defined(MANAGER_DEBUG_OVERLAY_ROW)
    case MANAGER_UI_SET_DEBUG_OVERLAY: {
        unsigned int index = ui_overlay_index(ui, &state->pending);
        if (ui->overlay_key < 0)
            return;
        if (ui_edits_refused(ui)) {
            ui_status_id(ui, MANAGER_TONE_ERROR, MSTR_SET_IOERR);
            return;
        }
        if (direction > 0 && index < 2u)
            ++index;
        else if (direction < 0 && index > 0u)
            --index;
        (void)manager_settings_set_pending_text(
            state, (enum isaac_vita_settings_key)ui->overlay_key,
            ui_overlay_values[index], strlen(ui_overlay_values[index]));
        break;
    }
#endif
    default:
        return;
    }
    ui_settings_changed_status(ui, row);
}

static void ui_settings_restore_look(struct manager_ui *ui)
{
    ui->lang = ui_resolve_lang(ui, ui->ini_saved.lang);
    (void)ui_apply_theme(ui, ui->ini_saved.theme);
}

static void ui_settings_save(struct manager_ui *ui, int confirmed)
{
    struct manager_settings_state *state = &ui->settings.state;
    const unsigned int port_pending = manager_settings_pending_count(state);
    int warning = 0;
    int port_saved = 0;
    int result;

    if (manager_ui_pending_count(ui) == 0u) {
        ui_status_id(ui, MANAGER_TONE_NORMAL, MSTR_SET_NOTHING);
        return;
    }
    if (ui->ftp_on) {
        ui_status_id(ui, MANAGER_TONE_WARNING, MSTR_STATUS_READONLY);
        return;
    }
    if (port_pending != 0u && ui_edits_refused(ui)) {
        ui_status_id(ui, MANAGER_TONE_ERROR, MSTR_SET_IOERR);
        return;
    }
    if (!confirmed && port_pending != 0u &&
        ui_fps_is_uneven(state->pending.fps_lock) &&
        state->pending.fps_lock != state->saved.fps_lock) {
        ui->settings.armed = MANAGER_SETTINGS_UI_ARM_SAVE;
        ui->confirm = MANAGER_UI_CONFIRM_SETTINGS;
        return;
    }
    if (port_pending != 0u) {
        const struct manager_settings_result saved =
            manager_settings_save(ui->io, state, 0);
        if (!manager_settings_result_ok(&saved)) {
            ui_status(ui, MANAGER_TONE_ERROR, ui_s(ui, MSTR_SET_SAVE_FAILED),
                      ui_s(ui, ui_settings_word(saved.code)));
            return;
        }
        warning |= saved.code == MANAGER_SETTINGS_CODE_SYNC_WARNING;
        port_saved = 1;
    }
    if (ui_row_pending(ui, MANAGER_UI_SET_EID)) {
        result = manager_storage_set_mod_enabled(ui->io, MANAGER_UI_EID_DIRECTORY,
                                                 ui->eid_pending);
        if (result < 0) {
            ui_status(ui, MANAGER_TONE_ERROR, ui_s(ui, MSTR_SET_EID_FAILED),
                      ui_s(ui, ui_storage_word(result)));
            return;
        }
        warning |= result == MANAGER_STORAGE_SYNC_WARNING;
        ui->eid_saved = ui->eid_pending;
        ui_refresh_mods(ui, ui->mod_meta_loaded);
    }
    if (!manager_ini_equal(&ui->ini_pending, &ui->ini_saved)) {
        result = manager_ini_save(ui->io, &ui->ini_pending);
        if (result < 0) {
            ui_status(ui, MANAGER_TONE_ERROR, ui_s(ui, MSTR_SET_INI_FAILED),
                      ui_s(ui, ui_storage_word(result)));
            return;
        }
        warning |= result == MANAGER_STORAGE_SYNC_WARNING;
        ui->ini_saved = ui->ini_pending;
    }
    ui_clamp_settings_row(ui);
    if (warning)
        ui_status(ui, MANAGER_TONE_WARNING, ui_s(ui, MSTR_SET_SAVED_WARN),
                  ui_s(ui, MSTR_RES_SYNC_WARNING));
    else if (port_saved &&
             ui->settings.last_boot.kind == MANAGER_SETTINGS_LAST_BOOT_IGNORES)
        ui_status_id(ui, MANAGER_TONE_WARNING, MSTR_SET_SAVED_IGNORED);
    else
        ui_status_id(ui, MANAGER_TONE_OK, MSTR_SET_SAVED);
}

static void ui_settings_defaults(struct manager_ui *ui)
{
    const struct manager_settings_result result =
        manager_settings_defaults(ui->io, &ui->settings.state, ui->ftp_on);
    if (result.code == MANAGER_SETTINGS_CODE_NO_FILE)
        ui_status_id(ui, MANAGER_TONE_NORMAL, MSTR_SET_DEFAULTS_NOFILE);
    else if (result.code == MANAGER_SETTINGS_CODE_FTP_LISTENING)
        ui_status_id(ui, MANAGER_TONE_WARNING, MSTR_STATUS_READONLY);
    else if (manager_settings_result_ok(&result))
        ui_status_id(ui,
                     result.code == MANAGER_SETTINGS_CODE_SYNC_WARNING
                         ? MANAGER_TONE_WARNING
                         : MANAGER_TONE_OK,
                     MSTR_SET_DEFAULTS_DONE);
    else
        ui_status(ui, MANAGER_TONE_ERROR, ui_s(ui, MSTR_SET_DEFAULTS_FAILED),
                  ui_s(ui, ui_settings_word(result.code)));
    ui_clamp_settings_row(ui);
}

static void ui_settings_undo(struct manager_ui *ui)
{
    const struct manager_settings_result result =
        manager_settings_undo(ui->io, &ui->settings.state, ui->ftp_on);
    if (result.code == MANAGER_SETTINGS_CODE_FTP_LISTENING)
        ui_status_id(ui, MANAGER_TONE_WARNING, MSTR_STATUS_READONLY);
    else if (manager_settings_result_ok(&result))
        ui_status_id(ui,
                     result.code == MANAGER_SETTINGS_CODE_SYNC_WARNING
                         ? MANAGER_TONE_WARNING
                         : MANAGER_TONE_OK,
                     MSTR_SET_UNDO_DONE);
    else
        ui_status(ui, MANAGER_TONE_ERROR, ui_s(ui, MSTR_SET_UNDO_FAILED),
                  ui_s(ui, ui_settings_word(result.code)));
    ui_clamp_settings_row(ui);
}

static void ui_settings_discard(struct manager_ui *ui)
{
    manager_settings_discard(&ui->settings.state);
    ui->eid_pending = ui->eid_saved;
    ui->ini_pending = ui->ini_saved;
    ui_settings_restore_look(ui);
}

static void ui_settings(struct manager_ui *ui, uint32_t pressed)
{
    uint8_t rows[MANAGER_UI_SET_COUNT];
    const unsigned int count = manager_ui_settings_rows(ui, rows);
    unsigned int position = ui_settings_position(ui, rows, count);
    if ((pressed & UI_CIRCLE) != 0u) {
        ui_leave_to(ui, MANAGER_UI_PAGE_HOME);
    } else if ((pressed & UI_UP) != 0u) {
        if (position > 0u)
            ui->settings_row = rows[position - 1u];
    } else if ((pressed & UI_DOWN) != 0u) {
        if (position + 1u < count)
            ui->settings_row = rows[position + 1u];
    } else if ((pressed & UI_LEFT) != 0u) {
        ui_settings_step(ui, -1);
    } else if ((pressed & UI_RIGHT) != 0u) {
        ui_settings_step(ui, 1);
    } else if ((pressed & UI_CROSS) != 0u) {
        if (ui->settings_row == MANAGER_UI_SET_UNDO) {
            if (ui->ftp_on)
                ui_status_id(ui, MANAGER_TONE_WARNING, MSTR_STATUS_READONLY);
            else
                ui_settings_undo(ui);
        } else if (ui->settings_row == MANAGER_UI_SET_DEFAULTS) {
            if (ui->ftp_on) {
                ui_status_id(ui, MANAGER_TONE_WARNING, MSTR_STATUS_READONLY);
            } else {
                ui->settings.armed = MANAGER_SETTINGS_UI_ARM_DEFAULTS;
                ui->confirm = MANAGER_UI_CONFIRM_SETTINGS;
            }
        } else {
            ui_settings_save(ui, 0);
        }
    }
}

#if defined(MANAGER_TROPHY_IMPORT)
/* ---- ACHIEVEMENTS: trophy import (manager_ui.h) ------------------------- */

static int ui_trophy_space(char character)
{
    return character == ' ' || character == '\t' || character == '\r' ||
           character == '\n';
}

int manager_ui_trophy_parse_request(const char *text, size_t length)
{
    size_t index = 0u;
    int slot;
    if (text == NULL || length > MANAGER_TROPHY_REQUEST_MAX_BYTES)
        return 0;
    while (index < length && ui_trophy_space(text[index]))
        ++index;
    if (index >= length || text[index] < '1' || text[index] > '3')
        return 0;
    slot = text[index] - '0';
    ++index;
    while (index < length && ui_trophy_space(text[index]))
        ++index;
    return index == length ? slot : 0;
}

static int ui_trophy_number(const char *value, size_t length,
                            unsigned int limit, unsigned int *out)
{
    unsigned int number = 0u;
    size_t index;
    if (length == 0u || length > 5u)
        return 0;
    for (index = 0u; index < length; ++index) {
        if (value[index] < '0' || value[index] > '9')
            return 0;
        number = number * 10u + (unsigned int)(value[index] - '0');
    }
    if (number > limit)
        return 0;
    *out = number;
    return 1;
}

static int ui_trophy_key(const char *key, size_t length, const char *name)
{
    return strlen(name) == length && memcmp(key, name, length) == 0;
}

static int ui_trophy_parse_line(const char *text,
                                 struct manager_trophy_result *result);

int manager_ui_trophy_parse_result(const char *text,
                                   struct manager_trophy_result *result)
{
    if (result == NULL)
        return 0;
    if (ui_trophy_parse_line(text, result))
        return 1;
    memset(result, 0, sizeof(*result));
    return 0;
}

static int ui_trophy_parse_line(const char *text,
                                 struct manager_trophy_result *result)
{
    static const char prefix[] = "TROPHY IMPORT: ";
    static const char *const counts[6] = {"saved",   "mapped", "unlocked",
                                          "already", "failed", "noset"};
    const char *cursor;
    int have_slot = 0;
    int have_status = 0;
    memset(result, 0, sizeof(*result));
    if (text == NULL || strncmp(text, prefix, sizeof(prefix) - 1u) != 0)
        return 0;
    cursor = text + sizeof(prefix) - 1u;
    for (;;) {
        const char *key;
        const char *equals;
        const char *value;
        size_t key_length;
        size_t value_length;
        unsigned int index;
        while (*cursor == ' ')
            ++cursor;
        if (*cursor == '\0' || *cursor == '\n' || *cursor == '\r')
            break;
        key = cursor;
        while (*cursor != '\0' && *cursor != ' ' && *cursor != '\n' &&
               *cursor != '\r')
            ++cursor;
        equals = (const char *)memchr(key, '=', (size_t)(cursor - key));
        if (equals == NULL)
            return 0;
        key_length = (size_t)(equals - key);
        value = equals + 1;
        value_length = (size_t)(cursor - value);
        if (ui_trophy_key(key, key_length, "slot")) {
            /* slot=0 is the runtime's bad-request line */
            if (!ui_trophy_number(value, value_length, 3u, &result->slot))
                return 0;
            have_slot = 1;
        } else if (ui_trophy_key(key, key_length, "status")) {
            size_t index_char;
            if (value_length == 0u || value_length >= sizeof(result->status))
                return 0;
            for (index_char = 0u; index_char < value_length; ++index_char) {
                const char c = value[index_char];
                if (!((c >= 'a' && c <= 'z') || c == '-'))
                    return 0;
            }
            memcpy(result->status, value, value_length);
            result->status[value_length] = '\0';
            have_status = 1;
        } else if (ui_trophy_key(key, key_length, "request")) {
            if (ui_trophy_key(value, value_length, "kept"))
                result->request_kept = 1;
            else if (!ui_trophy_key(value, value_length, "deleted"))
                return 0;
        } else {
            unsigned int *fields[6];
            fields[0] = &result->saved;
            fields[1] = &result->mapped;
            fields[2] = &result->unlocked;
            fields[3] = &result->already;
            fields[4] = &result->failed;
            fields[5] = &result->noset;
            for (index = 0u; index < 6u; ++index) {
                if (!ui_trophy_key(key, key_length, counts[index]))
                    continue;
                if (!ui_trophy_number(value, value_length, 9999u,
                                      fields[index]))
                    return 0;
                break;
            }
            /* any other key: written by a newer runtime, ignored */
        }
    }
    return have_slot && have_status;
}

/* Reads trophy-import.req and trophy-import.res; absent files are normal,
 * anything unreadable or malformed is shown as such, never an error box. */
static void ui_trophy_refresh(struct manager_ui *ui)
{
    char text[MANAGER_TROPHY_RESULT_MAX_BYTES + 1u];
    size_t length = 0u;
    int result = manager_storage_read_text(
        ui->io, MANAGER_TROPHY_REQUEST_PATH, text,
        MANAGER_TROPHY_REQUEST_MAX_BYTES + 1u, &length);
    if (result == MANAGER_STORAGE_ABSENT) {
        ui->trophy_pending = 0;
    } else {
        const int slot = result == MANAGER_STORAGE_OK
                             ? manager_ui_trophy_parse_request(text, length)
                             : 0;
        ui->trophy_pending = slot != 0 ? slot : -1;
    }
    length = 0u;
    result = manager_storage_read_text(ui->io, MANAGER_TROPHY_RESULT_PATH, text,
                                       sizeof(text), &length);
    if (result == MANAGER_STORAGE_ABSENT)
        ui->trophy_result_state = 0;
    else if (result == MANAGER_STORAGE_OK &&
             manager_ui_trophy_parse_result(text, &ui->trophy_result))
        ui->trophy_result_state = 1;
    else
        ui->trophy_result_state = -1;
}

static void ui_trophy_request(struct manager_ui *ui)
{
    char digit;
    int result;
    if (ui->ftp_on) {
        ui_status_id(ui, MANAGER_TONE_WARNING, MSTR_STATUS_READONLY);
        return;
    }
    digit = (char)('0' + ui->trophy_slot);
    result = manager_storage_write_text_atomic(
        ui->io, MANAGER_TROPHY_REQUEST_NAME, &digit, 1u);
    if (result == MANAGER_STORAGE_OK || result == MANAGER_STORAGE_SYNC_WARNING)
        ui_status(ui, MANAGER_TONE_OK, ui_s(ui, MSTR_TROPHY_REQUESTED),
                  ui->trophy_slot);
    else
        ui_status(ui, MANAGER_TONE_ERROR, ui_s(ui, MSTR_TROPHY_REQUEST_FAILED),
                  ui_s(ui, ui_storage_word(result)));
    ui_trophy_refresh(ui);
}

static void ui_achievements(struct manager_ui *ui, uint32_t pressed)
{
    if ((pressed & UI_CIRCLE) != 0u) {
        ui_goto(ui, MANAGER_UI_PAGE_HOME);
    } else if ((pressed & UI_LEFT) != 0u) {
        if (ui->trophy_slot > 1)
            --ui->trophy_slot;
    } else if ((pressed & UI_RIGHT) != 0u) {
        if (ui->trophy_slot < 3)
            ++ui->trophy_slot;
    } else if ((pressed & UI_CROSS) != 0u) {
        ui_trophy_request(ui);
    }
}
#endif /* MANAGER_TROPHY_IMPORT */

/* ---- confirmation box ------------------------------------------------- */

static unsigned int ui_confirm(struct manager_ui *ui, uint32_t pressed)
{
    const enum manager_ui_confirm confirm = ui->confirm;
    const enum manager_settings_ui_arm armed = ui->settings.armed;
    if ((pressed & UI_CIRCLE) != 0u) {
        ui->confirm = MANAGER_UI_CONFIRM_NONE;
        ui->settings.armed = MANAGER_SETTINGS_UI_ARM_NONE;
        return 0u;
    }
    if ((pressed & UI_CROSS) == 0u)
        return 0u;
    ui->confirm = MANAGER_UI_CONFIRM_NONE;
    ui->settings.armed = MANAGER_SETTINGS_UI_ARM_NONE;
    switch (confirm) {
    case MANAGER_UI_CONFIRM_LAUNCH_FTP:
        return MANAGER_UI_LAUNCH;
    case MANAGER_UI_CONFIRM_MODS_ALL:
        ui_mods_all(ui);
        break;
    case MANAGER_UI_CONFIRM_RESTORE:
        ui_saves_restore(ui);
        break;
    case MANAGER_UI_CONFIRM_SETTINGS:
        if (armed == MANAGER_SETTINGS_UI_ARM_SAVE) {
            ui_settings_save(ui, 1);
        } else if (armed == MANAGER_SETTINGS_UI_ARM_DEFAULTS) {
            ui_settings_defaults(ui);
        } else if (armed == MANAGER_SETTINGS_UI_ARM_DISCARD) {
            const enum manager_ui_page target = ui->leave_target;
            ui_settings_discard(ui);
            ui_goto(ui, target);
            ui_status_id(ui, MANAGER_TONE_NORMAL, MSTR_SET_DISCARDED);
        }
        break;
    default:
        break;
    }
    return 0u;
}

/* ---- public ----------------------------------------------------------- */

void manager_ui_init(struct manager_ui *ui, const struct manager_storage_io *io,
                     const struct manager_ui_host *host,
                     const struct manager_ui_config *config,
                     struct manager_theme_set *themes, uint32_t now)
{
    enum manager_theme_fallback why;
    memset(ui, 0, sizeof(*ui));
    ui->io = io;
    if (host != NULL)
        ui->host = *host;
    if (config != NULL)
        ui->config = *config;
    if (ui->config.app_version == NULL)
        ui->config.app_version = "";
    if (ui->config.build_id == NULL)
        ui->config.build_id = "";
    ui->themes = themes;
    ui->now = now;
    ui->save_slot = 1;
    ui->page = MANAGER_UI_PAGE_HOME;
    ui->home_row = MANAGER_UI_HOME_LAUNCH;
#if defined(MANAGER_TROPHY_IMPORT)
    ui->trophy_slot = 1;
#endif
#if defined(MANAGER_DEBUG_OVERLAY_ROW)
    ui->overlay_key = ui_overlay_key();
#endif

    ui->ini_status = manager_ini_load(io, &ui->ini_saved, NULL);
    ui->ini_pending = ui->ini_saved;
    ui->lang = ui_resolve_lang(ui, ui->ini_saved.lang);
    why = ui_apply_theme(ui, ui->ini_saved.theme);

    ui_refresh_mods(ui, 0);
    ui_refresh_saves(ui);
    ui_open_settings(ui);
    ui_refresh_resources(ui);

    ui_status_id(ui, MANAGER_TONE_NORMAL, MSTR_STATUS_READY);
    if (ui->config.recovery_result < 0) {
        char text[128];
        ui_recovery_text(ui, text, sizeof(text));
        ui_status(ui, MANAGER_TONE_ERROR, "%s", text);
    } else if (ui->config.fonts_fallback) {
        ui_status_id(ui, MANAGER_TONE_WARNING, MSTR_STATUS_FONT_FALLBACK);
    } else {
        (void)ui_theme_status(ui, why);
    }

    /* Autolaunch is suppressed when FTP listens (never at start),
     * recovery warned or failed, or the last game build ignored port.ini. */
    if (ui->ini_saved.autolaunch != 0u &&
        ui->config.recovery_result == MANAGER_STORAGE_OK &&
        ui->settings.last_boot.kind != MANAGER_SETTINGS_LAST_BOOT_IGNORES) {
        ui->countdown_active = 1;
        ui->countdown_deadline =
            now + (uint32_t)ui->ini_saved.autolaunch * MANAGER_UI_TICKS_PER_SECOND;
        ui->countdown_shown = ui->ini_saved.autolaunch;
    }
}

const struct manager_theme *manager_ui_theme(const struct manager_ui *ui)
{
    return ui->theme;
}

unsigned int manager_ui_update(struct manager_ui *ui, uint32_t held,
                               uint32_t now)
{
    unsigned int result = 0u;
    uint32_t pressed = held & ~ui->held_previous;
    ui->now = now;

    /* Key repeat: the lowest newly pressed direction repeats while held. */
    if ((pressed & UI_REPEATABLE) != 0u) {
        const uint32_t fresh = pressed & UI_REPEATABLE;
        ui->repeat_button = fresh & (~fresh + 1u);
        ui->repeat_at = now + MANAGER_UI_REPEAT_DELAY;
    } else if (ui->repeat_button != 0u && (held & ui->repeat_button) != 0u &&
               (int32_t)(now - ui->repeat_at) >= 0) {
        pressed |= ui->repeat_button;
        ui->repeat_at = now + MANAGER_UI_REPEAT_INTERVAL;
    }
    if (ui->repeat_button != 0u && (held & ui->repeat_button) == 0u)
        ui->repeat_button = 0u;
    ui->held_previous = held;

    /* The server can die under us (network loss): same handling as today's
     * main loop, stop it and say so. */
    if (ui->ftp_on && ui->host.ftp_listening != NULL &&
        !ui->host.ftp_listening(ui->host.context)) {
        if (ui->host.ftp_stop != NULL)
            ui->host.ftp_stop(ui->host.context);
        ui->ftp_on = 0;
        ui->ftp_failed = 1;
        ui->ftp_ip[0] = '\0';
        ui->ftp_port = 0u;
        if (ui->confirm == MANAGER_UI_CONFIRM_LAUNCH_FTP)
            ui->confirm = MANAGER_UI_CONFIRM_NONE;
        ui_status_id(ui, MANAGER_TONE_ERROR, MSTR_STATUS_FTP_DIED);
        result |= MANAGER_UI_REDRAW;
    }

    if (ui->countdown_active) {
        if (pressed != 0u) {
            ui->countdown_active = 0;
            ui_status_id(ui, MANAGER_TONE_NORMAL, MSTR_STATUS_COUNTDOWN_CANCEL);
            return result | MANAGER_UI_REDRAW;
        }
        if ((int32_t)(now - ui->countdown_deadline) >= 0) {
            ui->countdown_active = 0;
            return result | MANAGER_UI_REDRAW | MANAGER_UI_LAUNCH;
        }
        {
            const unsigned int remaining =
                (unsigned int)((ui->countdown_deadline - now +
                                MANAGER_UI_TICKS_PER_SECOND - 1u) /
                               MANAGER_UI_TICKS_PER_SECOND);
            if (remaining != ui->countdown_shown) {
                ui->countdown_shown = remaining;
                result |= MANAGER_UI_REDRAW;
            }
        }
        return result;
    }
    if (pressed == 0u)
        return result;
    result |= MANAGER_UI_REDRAW;

    if (ui->confirm != MANAGER_UI_CONFIRM_NONE)
        return result | ui_confirm(ui, pressed);
    if (ui->detail_open) {
        if ((pressed & (UI_CIRCLE | UI_TRIANGLE | UI_CROSS)) != 0u)
            ui->detail_open = 0;
        return result;
    }
    if ((pressed & (UI_LTRIGGER | UI_RTRIGGER)) != 0u) {
        const unsigned int page = (unsigned int)ui->page;
        const unsigned int target =
            (pressed & UI_LTRIGGER) != 0u
                ? (page + MANAGER_UI_PAGE_COUNT - 1u) % MANAGER_UI_PAGE_COUNT
                : (page + 1u) % MANAGER_UI_PAGE_COUNT;
        ui_leave_to(ui, (enum manager_ui_page)target);
        return result;
    }
    switch (ui->page) {
    case MANAGER_UI_PAGE_HOME:
        result |= ui_home(ui, pressed);
        break;
    case MANAGER_UI_PAGE_MODS:
        ui_mods(ui, pressed);
        break;
    case MANAGER_UI_PAGE_SAVES:
        ui_saves(ui, pressed);
        break;
    case MANAGER_UI_PAGE_SETTINGS:
        ui_settings(ui, pressed);
        break;
#if defined(MANAGER_TROPHY_IMPORT)
    case MANAGER_UI_PAGE_ACHIEVEMENTS:
        ui_achievements(ui, pressed);
        break;
#endif
    default:
        if ((pressed & UI_CIRCLE) != 0u)
            ui_goto(ui, MANAGER_UI_PAGE_HOME);
        break;
    }
    return result;
}

int manager_ui_prepare_launch(struct manager_ui *ui)
{
    int result;
    ui->confirm = MANAGER_UI_CONFIRM_NONE;
    ui->countdown_active = 0;
    if (ui->host.ftp_stop != NULL)
        ui->host.ftp_stop(ui->host.context);
    ui->ftp_on = 0;
    ui->ftp_ip[0] = '\0';
    ui->ftp_port = 0u;
    result = manager_storage_recover(ui->io);
    if (result < 0) {
        ui_status(ui, MANAGER_TONE_ERROR, ui_s(ui, MSTR_STATUS_RECOVERY_FAILED),
                  ui_s(ui, ui_storage_word(result)));
        return result;
    }
    ui_status_id(ui, MANAGER_TONE_NORMAL, MSTR_STATUS_RETURNING);
    return 0;
}

void manager_ui_launch_failed(struct manager_ui *ui)
{
    ui_status_id(ui, MANAGER_TONE_ERROR, MSTR_STATUS_RETURN_FAILED);
}

/* ---- display list ----------------------------------------------------- */

struct ui_builder {
    const struct manager_ui *ui;
    const struct manager_theme *theme;
    struct manager_display_list *dl;
};

/* UI_G(b, flat, game): a coordinate or face of the mock C layout, or
 * of the game layout (MANAGER_DL_GAME_* in manager_ui_draw.h) when the
 * resolved theme is the game skin.  Without MANAGER_UI2_GAME_SKIN it is
 * the flat value, so those builds compile the mock C layout unchanged. */
#if defined(MANAGER_UI2_GAME_SKIN)
#define UI_GAME(b) MANAGER_DL_GAME_LAYOUT((b)->theme)
#define UI_G(b, flat, game) (UI_GAME(b) ? (game) : (flat))
#else
#define UI_G(b, flat, game) (flat)
#endif

static int ui_measure(const struct ui_builder *b, enum manager_face_role face,
                      const char *text)
{
    return manager_dl_measure(b->theme, face, text);
}

#if defined(MANAGER_UI2_GAME_SKIN)
/* Pen y of SMALL text sharing a baseline with BODY text at pen y. */
static int ui_small_on_body(const struct ui_builder *b, int y)
{
    return y + manager_face_ascent(b->theme->face[MANAGER_FACE_BODY]) -
           manager_face_ascent(b->theme->face[MANAGER_FACE_SMALL]);
}

/* Page info on the title row: right-aligned at x1, baseline on the
 * title's, never over the title. */
static void ui_game_info(const struct ui_builder *b, int right,
                         const char *text)
{
    const int title_end =
        b->theme->title_x +
        ui_measure(b, MANAGER_FACE_TITLE,
                   manager_dl_text_at(b->dl, b->dl->title)) +
        24;
    manager_dl_text(
        b->dl, right,
        MANAGER_DL_GAME_TITLE_Y +
            manager_face_ascent(b->theme->face[MANAGER_FACE_TITLE]) -
            manager_face_ascent(b->theme->face[MANAGER_FACE_SMALL]),
        MANAGER_FACE_SMALL, MANAGER_COLOR_MUTED, MANAGER_DL_RIGHT, title_end,
        text);
}

/* Rows of `pitch` that fit between y and the content bottom. */
static size_t ui_game_rows(int y, int pitch)
{
    const int room = MANAGER_DL_GAME_BOTTOM - y;
    return room > 0 ? (size_t)(room / pitch) : 0u;
}
#endif

static int ui_glyph_width(const struct ui_builder *b, enum manager_glyph glyph)
{
    return b->theme->ops->draw_hint_glyph(b->theme, NULL, 0, 0, glyph, 0u);
}

static void ui_hint(const struct ui_builder *b, int right,
                    enum manager_glyph glyph, enum manager_string_id id)
{
    manager_dl_add_hint(b->dl, right, glyph, ui_s(b->ui, id));
}

/* A list row: optional highlight, label, right value, "›", sub-line.
 * Returns the row height. */
static int ui_row(const struct ui_builder *b, int y, int selected,
                  const char *label, const char *value, const char *sub,
                  int more, int pending)
{
    const int height = sub != NULL && sub[0] != '\0' ? UI_G(b, 48, 52) : 32;
    const int label_width = ui_measure(b, MANAGER_FACE_BODY, label);
    const enum manager_color text =
        selected ? MANAGER_COLOR_HIGHLIGHT_TEXT : MANAGER_COLOR_TEXT;
    const enum manager_color muted =
        selected ? MANAGER_COLOR_HIGHLIGHT_MUTED : MANAGER_COLOR_MUTED;
    if (selected)
        manager_dl_box(b->dl, MANAGER_DL_HIGHLIGHT,
                       UI_G(b, 24, MANAGER_DL_GAME_BAND_X0), y,
                       UI_G(b, 936, MANAGER_DL_GAME_BAND_X1), y + height - 1);
    if (pending)
        manager_dl_glyph(b->dl, UI_G(b, 34, MANAGER_DL_GAME_PENDING_X),
                         y + UI_G(b, 6, MANAGER_DL_GAME_ROW_TEXT_DY),
                         MANAGER_GLYPH_PENDING, text, MANAGER_DL_LEFT);
    manager_dl_text(b->dl, UI_G(b, 48, MANAGER_DL_GAME_TEXT_X),
                    y + UI_G(b, 6, MANAGER_DL_GAME_ROW_TEXT_DY),
                    MANAGER_FACE_BODY, text, MANAGER_DL_LEFT,
                    UI_G(b, 500, MANAGER_DL_GAME_TEXT_X + 520), label);
    if (value != NULL && value[0] != '\0')
        manager_dl_text(b->dl, UI_G(b, 900, MANAGER_DL_GAME_VALUE_R),
                        y + UI_G(b, 8, MANAGER_DL_GAME_ROW_TEXT_DY),
                        UI_G(b, MANAGER_FACE_SMALL, MANAGER_FACE_BODY), muted,
                        MANAGER_DL_RIGHT,
                        UI_G(b, 48, MANAGER_DL_GAME_TEXT_X) + label_width + 24,
                        value);
    if (more)
        manager_dl_glyph(b->dl, UI_G(b, 916, MANAGER_DL_GAME_MORE_X),
                         y + UI_G(b, 6, MANAGER_DL_GAME_ROW_TEXT_DY),
                         MANAGER_GLYPH_MORE, muted, MANAGER_DL_LEFT);
    if (sub != NULL && sub[0] != '\0')
        manager_dl_text(b->dl, UI_G(b, 64, MANAGER_DL_GAME_SUB_X),
                        y + UI_G(b, 28, 30), MANAGER_FACE_SMALL, muted,
                        MANAGER_DL_LEFT,
                        UI_G(b, 912, MANAGER_DL_GAME_BAND_X1 - 8), sub);
    return height;
}

static void ui_heading(const struct ui_builder *b, int x, int y,
                       enum manager_string_id id)
{
    manager_dl_text(b->dl, x, y, MANAGER_FACE_SMALL, MANAGER_COLOR_MUTED,
                    MANAGER_DL_LEFT, 928, ui_s(b->ui, id));
}

static void ui_scrollbar(const struct ui_builder *b, int x, int y, int rows,
                         int pitch, size_t top, size_t count)
{
    const int height = rows * pitch;
    int thumb0;
    int thumb1;
    if (count <= (size_t)rows)
        return;
    thumb0 = y + (int)((size_t)height * top / count);
    thumb1 = y + (int)((size_t)height * (top + (size_t)rows) / count) - 1;
    manager_dl_scroll(b->dl, x, y, x + 4, y + height - 1, thumb0, thumb1);
}

/* First visible row: the cursor stays centred once past the middle. */
static size_t ui_list_top(size_t selection, size_t count, size_t rows)
{
    size_t top;
    if (count <= rows || selection < rows / 2u)
        return 0u;
    top = selection - rows / 2u;
    if (top + rows > count)
        top = count - rows;
    return top;
}

static void ui_build_home(const struct ui_builder *b)
{
    const struct manager_ui *ui = b->ui;
    struct manager_display_list *dl = b->dl;
    char text[256];
    char value[256];
    char readonly[64];
    int y = 122;
    unsigned int row;
    const int launch_selected = ui->home_row == MANAGER_UI_HOME_LAUNCH;
    const enum manager_color launch_color =
        launch_selected ? MANAGER_COLOR_HIGHLIGHT_TEXT : MANAGER_COLOR_TEXT;
    const int launch_x = UI_G(b, 48, MANAGER_DL_GAME_TEXT_X) +
                         ui_glyph_width(b, MANAGER_GLYPH_LAUNCH) +
                         2 * ui_measure(b, MANAGER_FACE_BODY, " ");
    /* Game layout: the launch row is a plain 32 px row at the top. */
    const int launch_y = UI_G(b, 85, MANAGER_DL_GAME_TOP +
                                         MANAGER_DL_GAME_ROW_TEXT_DY);

    if (launch_selected || ui->countdown_active)
        manager_dl_box(dl, MANAGER_DL_HIGHLIGHT,
                       UI_G(b, 24, MANAGER_DL_GAME_BAND_X0),
                       UI_G(b, 76, MANAGER_DL_GAME_TOP),
                       UI_G(b, 936, MANAGER_DL_GAME_BAND_X1),
                       UI_G(b, 112, MANAGER_DL_GAME_TOP + 31));
    manager_dl_glyph(dl, UI_G(b, 48, MANAGER_DL_GAME_TEXT_X), launch_y,
                     MANAGER_GLYPH_LAUNCH,
                     ui->countdown_active ? MANAGER_COLOR_HIGHLIGHT_TEXT
                                          : launch_color,
                     MANAGER_DL_LEFT);
    if (ui->countdown_active) {
        ui_fmt(text, sizeof(text), ui_s(ui, MSTR_HOME_COUNTDOWN),
               ui->countdown_shown);
        manager_dl_text(dl, launch_x, launch_y, MANAGER_FACE_BODY,
                        MANAGER_COLOR_HIGHLIGHT_TEXT, MANAGER_DL_LEFT, 560,
                        text);
        manager_dl_text(dl, UI_G(b, 912, MANAGER_DL_GAME_VALUE_R),
                        UI_G(b, 87, launch_y),
                        UI_G(b, MANAGER_FACE_SMALL, MANAGER_FACE_BODY),
                        MANAGER_COLOR_HIGHLIGHT_MUTED, MANAGER_DL_RIGHT, 580,
                        ui_s(ui, MSTR_HOME_COUNTDOWN_STAY));
    } else {
        manager_dl_text(dl, launch_x, launch_y, MANAGER_FACE_BODY,
                        launch_color, MANAGER_DL_LEFT,
                        UI_G(b, 912, MANAGER_DL_GAME_BAND_X1 - 8),
                        ui_s(ui, MSTR_HOME_LAUNCH));
    }

    ui_fmt(readonly, sizeof(readonly), "%s",
           ui->ftp_on ? ui_s(ui, MSTR_HOME_READONLY) : "");
    for (row = MANAGER_UI_HOME_FTP; row < MANAGER_UI_HOME_COUNT; ++row) {
        const int selected = ui->home_row == row && !ui->countdown_active;
        const char *sub = NULL;
        enum manager_string_id label = MSTR_HOME_ABOUT;
        value[0] = '\0';
        switch (row) {
        case MANAGER_UI_HOME_FTP:
            label = MSTR_HOME_FTP;
            if (ui->ftp_on)
                ui_fmt(value, sizeof(value), ui_s(ui, MSTR_HOME_FTP_ON),
                       ui->ftp_ip, (unsigned int)ui->ftp_port);
            else
                ui_fmt(value, sizeof(value), "%s",
                       ui_s(ui, ui->ftp_failed ? MSTR_HOME_FTP_FAILED
                                               : MSTR_HOME_FTP_OFF));
            sub = ui_s(ui, MSTR_HOME_FTP_SUB);
            break;
        case MANAGER_UI_HOME_MODS:
            label = MSTR_HOME_MODS;
            if (ui->mod_count == 0u)
                ui_fmt(value, sizeof(value), "%s%s",
                       ui_s(ui, MSTR_HOME_MODS_NONE), readonly);
            else {
                ui_fmt(text, sizeof(text), ui_s(ui, MSTR_HOME_MODS_VALUE),
                       (unsigned int)ui_mods_enabled(ui),
                       (unsigned int)ui->mod_count);
                ui_fmt(value, sizeof(value), "%s%s", text, readonly);
            }
            break;
        case MANAGER_UI_HOME_SAVES:
            label = MSTR_HOME_SAVES;
            ui_fmt(text, sizeof(text), ui_s(ui, MSTR_HOME_SAVES_VALUE),
                   ui_save_files_present(ui), (unsigned int)ui->backup_count);
            ui_fmt(value, sizeof(value), "%s%s", text, readonly);
            break;
        case MANAGER_UI_HOME_SETTINGS:
            label = MSTR_HOME_SETTINGS;
            ui_fmt(text, sizeof(text), ui_s(ui, MSTR_HOME_SETTINGS_VALUE),
                   (unsigned int)ui->settings.state.saved.fps_lock,
                   ui_s(ui, ui->settings.state.saved.render_scale >= 2u
                                ? MSTR_SET_RENDER_SHARP
                                : MSTR_SET_RENDER_FAST));
            ui_fmt(value, sizeof(value), "%s%s", text, readonly);
            break;
        case MANAGER_UI_HOME_ACHIEVEMENTS:
            label = MSTR_HOME_ACHIEVEMENTS;
#if defined(MANAGER_TROPHY_IMPORT)
            ui_fmt(value, sizeof(value), "%s",
                   ui_s(ui, UI_G(b, MSTR_HOME_ACH_VALUE, MSTR_TROPHY_HOME)));
#else
            ui_fmt(value, sizeof(value), "%s", ui_s(ui, MSTR_HOME_ACH_VALUE));
#endif
            break;
        case MANAGER_UI_HOME_CONTROLS:
            label = MSTR_HOME_CONTROLS;
            break;
        default:
            label = MSTR_HOME_ABOUT;
            ui_fmt(value, sizeof(value), ui_s(ui, MSTR_HOME_ABOUT_VALUE),
                   ui->config.build_id[0] != '\0' ? ui->config.build_id
                                                  : ui_s(ui, MSTR_WORD_UNKNOWN),
                   ui_s(ui, ui_resources_summary(ui)));
            break;
        }
        y += ui_row(b, y, selected, ui_s(ui, label), value, sub, 1, 0);
        if (row == MANAGER_UI_HOME_FTP) {
            manager_dl_separator(dl, UI_G(b, 32, MANAGER_DL_GAME_X0),
                                 UI_G(b, 928, MANAGER_DL_GAME_X1),
                                 y + UI_G(b, 4, 5));
            y += UI_G(b, 12, 10);
        }
    }
    manager_dl_separator(dl, UI_G(b, 32, MANAGER_DL_GAME_X0),
                         UI_G(b, 928, MANAGER_DL_GAME_X1), y + UI_G(b, 4, 5));
    ui_recovery_text(ui, text, sizeof(text));
    manager_dl_text(dl, UI_G(b, 48, MANAGER_DL_GAME_TEXT_X), y + UI_G(b, 20, 12),
                    MANAGER_FACE_SMALL,
                    ui->config.recovery_result < 0 ? MANAGER_COLOR_ERROR
                    : ui->config.recovery_result > 0 ? MANAGER_COLOR_WARNING
                                                      : MANAGER_COLOR_MUTED,
                    MANAGER_DL_LEFT, UI_G(b, 928, MANAGER_DL_GAME_X1), text);
    ui_last_start_text(ui, text, sizeof(text), 1);
    manager_dl_text(dl, UI_G(b, 48, MANAGER_DL_GAME_TEXT_X), y + UI_G(b, 38, 31),
                    MANAGER_FACE_SMALL, MANAGER_COLOR_MUTED, MANAGER_DL_LEFT,
                    UI_G(b, 928, MANAGER_DL_GAME_X1), text);

    if (ui->countdown_active) {
        ui_hint(b, 1, MANAGER_GLYPH_CIRCLE, MSTR_HINT_STAY);
        return;
    }
    ui_hint(b, 0, MANAGER_GLYPH_UP_DOWN, MSTR_HINT_SELECT);
    ui_hint(b, 0, MANAGER_GLYPH_L_R, MSTR_HINT_PAGE);
    if (ui->home_row == MANAGER_UI_HOME_LAUNCH)
        ui_hint(b, 1, MANAGER_GLYPH_CROSS, MSTR_HINT_LAUNCH);
    else if (ui->home_row == MANAGER_UI_HOME_FTP)
        ui_hint(b, 1, MANAGER_GLYPH_CROSS,
                ui->ftp_on ? MSTR_HINT_FTP_STOP : MSTR_HINT_FTP_START);
    else
        ui_hint(b, 1, MANAGER_GLYPH_CROSS, MSTR_HINT_OPEN);
}

#if defined(MANAGER_UI2_GAME_SKIN)
/* A metadata.xml can name a mod after its own directory: on the device
 * EID's mods/836319872/metadata.xml reads <name>836319872</name>
 * <version>1.0</version> (not the workshop file's text), and the row then
 * showed the id twice.  Such a name counts as no name; EID, whose
 * directory the manager knows, gets its real name. */
#define UI_EID_NAME "External Item Descriptions"
static const char *ui_mod_name(const struct manager_ui_mod *mod,
                               int *show_directory)
{
    const int named_after_directory =
        mod->meta.from_metadata &&
        strcmp(mod->meta.name, mod->entry.name) == 0;
    const int eid =
        strcmp(mod->entry.name, MANAGER_UI_EID_DIRECTORY) == 0 &&
        (named_after_directory || !mod->meta.from_metadata);
    *show_directory =
        eid || (mod->meta.from_metadata && !named_after_directory);
    return eid ? UI_EID_NAME : mod->meta.name;
}
#define UI_MOD_NAME name
#else
#define UI_MOD_NAME mod->meta.name
#endif

static void ui_build_mods(const struct ui_builder *b)
{
    const struct manager_ui *ui = b->ui;
    struct manager_display_list *dl = b->dl;
    char text[256];
    const size_t enabled = ui_mods_enabled(ui);
#if defined(MANAGER_UI2_GAME_SKIN)
    /* Game layout: rows of 34 from the content top to its bottom, one
     * fewer when the skipped-note needs the last line. */
    const size_t game_rows = ui_game_rows(
        MANAGER_DL_GAME_TOP + (ui->mod_rejected != 0u ? 22 : 0), 34);
#endif
    const size_t top = ui_list_top(
        ui->mod_selection, ui->mod_count,
        UI_G(b, MANAGER_UI_MOD_ROWS, game_rows));
    size_t index;

    if (ui->mod_rejected != 0u)
        ui_fmt(text, sizeof(text), ui_s(ui, MSTR_MODS_INFO_SKIPPED),
               (unsigned int)enabled, (unsigned int)ui->mod_count,
               (unsigned int)ui->mod_rejected);
    else
        ui_fmt(text, sizeof(text), ui_s(ui, MSTR_MODS_INFO),
               (unsigned int)enabled, (unsigned int)ui->mod_count);
#if defined(MANAGER_UI2_GAME_SKIN)
    if (UI_GAME(b)) {
        /* On the title row: "1 / 4" left of "1 of 4 enabled". */
        const int info_width = ui_measure(b, MANAGER_FACE_SMALL, text);
        ui_game_info(b, MANAGER_DL_GAME_X1, text);
        if (ui->mod_count != 0u) {
            ui_fmt(text, sizeof(text), "%u / %u",
                   (unsigned int)(ui->mod_selection + 1u),
                   (unsigned int)ui->mod_count);
            ui_game_info(b, MANAGER_DL_GAME_X1 - info_width - 32, text);
        } else {
            manager_dl_text(dl, MANAGER_DL_GAME_TEXT_X,
                            MANAGER_DL_GAME_TOP + MANAGER_DL_GAME_ROW_TEXT_DY,
                            MANAGER_FACE_BODY, MANAGER_COLOR_MUTED,
                            MANAGER_DL_LEFT, MANAGER_DL_GAME_X1,
                            ui_s(ui, MSTR_MODS_EMPTY));
        }
    } else
#endif
    {
        manager_dl_text(dl, 928, 70, MANAGER_FACE_SMALL, MANAGER_COLOR_MUTED,
                        MANAGER_DL_RIGHT, 400, text);
        if (ui->mod_count != 0u) {
            ui_fmt(text, sizeof(text), "%u / %u",
                   (unsigned int)(ui->mod_selection + 1u),
                   (unsigned int)ui->mod_count);
            manager_dl_text(dl, 32, 70, MANAGER_FACE_SMALL,
                            MANAGER_COLOR_MUTED, MANAGER_DL_LEFT, 380, text);
        } else {
            manager_dl_text(dl, 48, 100, MANAGER_FACE_BODY,
                            MANAGER_COLOR_MUTED, MANAGER_DL_LEFT, 912,
                            ui_s(ui, MSTR_MODS_EMPTY));
        }
    }
    for (index = top;
         index < ui->mod_count &&
         index < top + UI_G(b, MANAGER_UI_MOD_ROWS, game_rows);
         ++index) {
        const struct manager_ui_mod *mod = &ui->mods[index];
        const int y = UI_G(b, 92, MANAGER_DL_GAME_TOP) +
                      (int)(index - top) * UI_G(b, 36, 34);
        const int selected = index == ui->mod_selection;
        const int on = !mod->entry.disabled;
        int sub_width;
#if defined(MANAGER_UI2_GAME_SKIN)
        int show_directory = 0;
        const char *name = ui_mod_name(mod, &show_directory);
        if (show_directory && mod->meta.version[0] != '\0')
            ui_fmt(text, sizeof(text), "%s · v%s", mod->entry.name,
                   mod->meta.version);
        else if (show_directory)
            ui_fmt(text, sizeof(text), "%s", mod->entry.name);
#else
        if (mod->meta.from_metadata && mod->meta.version[0] != '\0')
            ui_fmt(text, sizeof(text), "%s · v%s", mod->entry.name,
                   mod->meta.version);
        else if (mod->meta.from_metadata)
            ui_fmt(text, sizeof(text), "%s", mod->entry.name);
#endif
        else if (mod->meta.version[0] != '\0')
            ui_fmt(text, sizeof(text), "v%s", mod->meta.version);
        else
            text[0] = '\0';
        sub_width = ui_measure(b, UI_G(b, MANAGER_FACE_SMALL, MANAGER_FACE_BODY),
                               text);
        if (sub_width > UI_G(b, 300, 340))
            sub_width = UI_G(b, 300, 340);
        if (selected)
            manager_dl_box(dl, MANAGER_DL_HIGHLIGHT,
                           UI_G(b, 24, MANAGER_DL_GAME_BAND_X0),
                           y - UI_G(b, 2, 0),
                           UI_G(b, 916, MANAGER_DL_GAME_BAND_X1), y + 32);
        manager_dl_glyph(dl, UI_G(b, 44, MANAGER_DL_GAME_TEXT_X),
                         y + UI_G(b, 4, MANAGER_DL_GAME_ROW_TEXT_DY),
                         on ? MANAGER_GLYPH_CHECK_ON : MANAGER_GLYPH_CHECK_OFF,
                         selected ? MANAGER_COLOR_HIGHLIGHT_TEXT
                                  : MANAGER_COLOR_TEXT,
                         MANAGER_DL_LEFT);
        manager_dl_text(dl, UI_G(b, 76, MANAGER_DL_GAME_TEXT_X + 30),
                        y + UI_G(b, 5, MANAGER_DL_GAME_ROW_TEXT_DY),
                        MANAGER_FACE_BODY,
                        selected ? (on ? MANAGER_COLOR_HIGHLIGHT_TEXT
                                       : MANAGER_COLOR_HIGHLIGHT_MUTED)
                                 : (on ? MANAGER_COLOR_TEXT
                                       : MANAGER_COLOR_MUTED),
                        MANAGER_DL_LEFT,
                        UI_G(b, 900, MANAGER_DL_GAME_VALUE_R) - sub_width - 16,
                        UI_MOD_NAME);
        if (text[0] != '\0')
            manager_dl_text(dl, UI_G(b, 900, MANAGER_DL_GAME_VALUE_R),
                            y + UI_G(b, 8, MANAGER_DL_GAME_ROW_TEXT_DY),
                            UI_G(b, MANAGER_FACE_SMALL, MANAGER_FACE_BODY),
                            selected ? MANAGER_COLOR_HIGHLIGHT_MUTED
                                     : MANAGER_COLOR_MUTED,
                            MANAGER_DL_RIGHT, 600, text);
    }
    ui_scrollbar(b, UI_G(b, 928, MANAGER_DL_GAME_SCROLL_X),
                 UI_G(b, 92, MANAGER_DL_GAME_TOP),
                 (int)UI_G(b, MANAGER_UI_MOD_ROWS, game_rows), UI_G(b, 36, 34),
                 top, ui->mod_count);
    if (ui->mod_rejected != 0u) {
        ui_fmt(text, sizeof(text), ui_s(ui, MSTR_MODS_SKIPPED_NOTE),
               (unsigned int)ui->mod_rejected);
        manager_dl_text(dl, UI_G(b, 32, MANAGER_DL_GAME_TEXT_X),
                        UI_G(b, 440, MANAGER_DL_GAME_BOTTOM - 20),
                        MANAGER_FACE_SMALL, MANAGER_COLOR_MUTED,
                        MANAGER_DL_LEFT, UI_G(b, 928, MANAGER_DL_GAME_X1),
                        text);
    }
    ui_hint(b, 0, MANAGER_GLYPH_CIRCLE, MSTR_HINT_BACK);
    ui_hint(b, 0, MANAGER_GLYPH_UP_DOWN, MSTR_HINT_SELECT);
    if (ui->mod_count != 0u) {
        if (!ui->ftp_on)
            ui_hint(b, 1, MANAGER_GLYPH_SQUARE, MSTR_HINT_ALL);
        ui_hint(b, 1, MANAGER_GLYPH_TRIANGLE, MSTR_HINT_DETAILS);
        if (!ui->ftp_on)
            ui_hint(b, 1, MANAGER_GLYPH_CROSS, MSTR_HINT_TOGGLE);
    }
}

static void ui_build_saves(const struct ui_builder *b)
{
    const struct manager_ui *ui = b->ui;
    struct manager_display_list *dl = b->dl;
    size_t view[MANAGER_UI_MAX_BACKUPS];
    const size_t count = ui_backup_view(ui, view);
#if defined(MANAGER_UI2_GAME_SKIN)
    /* Game layout: cards from the content top, the copies heading under
     * them, rows of 34 from 198 to the content bottom. */
    const size_t game_rows = ui_game_rows(198, 34);
#endif
    const size_t top =
        ui_list_top(ui->backup_selection, count,
                    UI_G(b, MANAGER_UI_BACKUP_ROWS, game_rows));
    char text[256];
    char size[128];
    int slot;
    size_t index;

    for (slot = 1; slot <= 3; ++slot) {
        const struct manager_save_slot *info = &ui->slots[slot - 1];
        const int x0 = UI_G(b, 32 + (slot - 1) * 304,
                            MANAGER_DL_GAME_X0 + (slot - 1) * 296);
        const int card_y = UI_G(b, 76, MANAGER_DL_GAME_TOP);
        const int selected = slot == ui->save_slot;
        manager_dl_panel(dl, x0, card_y, x0 + UI_G(b, 288, 280),
                         UI_G(b, 164, MANAGER_DL_GAME_TOP + 80),
                         selected ? MANAGER_PANEL_CARD_SELECTED
                                  : MANAGER_PANEL_CARD);
        ui_fmt(text, sizeof(text), ui_s(ui, MSTR_SAVES_FILE), slot);
        manager_dl_text(dl, x0 + 16, card_y + UI_G(b, 10, 11), MANAGER_FACE_BODY,
                        MANAGER_COLOR_TEXT, MANAGER_DL_LEFT,
                        x0 + UI_G(b, 272, 264), text);
        if (info->present)
            ui_size_text(ui, info->size, size, sizeof(size));
        else
            ui_fmt(size, sizeof(size), "%s", ui_s(ui, MSTR_SAVES_NO_SAVE));
        manager_dl_text(dl, x0 + 16, card_y + UI_G(b, 36, 38),
                        MANAGER_FACE_SMALL, MANAGER_COLOR_MUTED,
                        MANAGER_DL_LEFT, x0 + UI_G(b, 272, 264), size);
#if defined(ISAAC_VITA_MANAGER_SAVE_GAMESTATE)
        if (info->present)
            manager_dl_text(dl, x0 + 16, card_y + UI_G(b, 56, 57),
                            MANAGER_FACE_SMALL, MANAGER_COLOR_MUTED,
                            MANAGER_DL_LEFT, x0 + UI_G(b, 272, 264),
                            ui_s(ui, info->gamestate_present
                                         ? MSTR_SAVES_RUN
                                         : MSTR_SAVES_NO_RUN));
#endif
    }
    if (ui->show_all_backups)
        ui_fmt(text, sizeof(text), ui_s(ui, MSTR_SAVES_BACKUPS_ALL),
               (unsigned int)count);
    else
        ui_fmt(text, sizeof(text), ui_s(ui, MSTR_SAVES_BACKUPS_OF),
               ui->save_slot, (unsigned int)count);
    manager_dl_text(dl, UI_G(b, 32, MANAGER_DL_GAME_TEXT_X), UI_G(b, 178, 176),
                    MANAGER_FACE_SMALL, MANAGER_COLOR_MUTED, MANAGER_DL_LEFT,
                    480, text);
    if (ui->backup_rejected != 0u) {
        ui_fmt(text, sizeof(text), ui_s(ui, MSTR_SAVES_SKIPPED),
               (unsigned int)ui->backup_rejected);
        manager_dl_text(dl, UI_G(b, 928, MANAGER_DL_GAME_VALUE_R),
                        UI_G(b, 178, 176), MANAGER_FACE_SMALL,
                        MANAGER_COLOR_MUTED, MANAGER_DL_RIGHT, 500, text);
    }
    if (count == 0u)
        manager_dl_text(dl, UI_G(b, 48, MANAGER_DL_GAME_TEXT_X),
                        UI_G(b, 210, 198 + MANAGER_DL_GAME_ROW_TEXT_DY),
                        MANAGER_FACE_BODY, MANAGER_COLOR_MUTED, MANAGER_DL_LEFT,
                        UI_G(b, 912, MANAGER_DL_GAME_X1),
                        ui_s(ui, MSTR_SAVES_NO_BACKUPS));
    for (index = top;
         index < count &&
         index < top + UI_G(b, MANAGER_UI_BACKUP_ROWS, game_rows);
         ++index) {
        const struct manager_save_backup *backup = &ui->backups[view[index]];
        const int y = UI_G(b, 200, 198) + (int)(index - top) * UI_G(b, 36, 34);
        const int selected = index == ui->backup_selection;
        const enum manager_color muted =
            selected ? MANAGER_COLOR_HIGHLIGHT_MUTED : MANAGER_COLOR_MUTED;
        enum manager_string_id kind = MSTR_SAVES_KIND_MANAGER;
        int size_width;
        if (backup->origin == MANAGER_SAVE_BACKUP_DATED)
            kind = MSTR_SAVES_KIND_GAME;
        else if (backup->origin == MANAGER_SAVE_BACKUP_SYNC)
            kind = MSTR_SAVES_KIND_SYNC;
        size[0] = '\0';
        if (ui->show_all_backups) {
            /* All slots in one list: which file each copy belongs to. */
            ui_fmt(size, sizeof(size), ui_s(ui, MSTR_SAVES_FILE), backup->slot);
            ui_fmt(size + strlen(size), sizeof(size) - strlen(size), " · ");
        }
        ui_size_text(ui, backup->size, size + strlen(size),
                     sizeof(size) - strlen(size));
#if defined(ISAAC_VITA_MANAGER_SAVE_GAMESTATE)
        if (backup->has_gamestate)
            ui_fmt(size + strlen(size), sizeof(size) - strlen(size), "%s",
                   ui_s(ui, MSTR_SAVES_WITH_RUN));
#endif
        size_width = ui_measure(
            b, UI_G(b, MANAGER_FACE_SMALL, MANAGER_FACE_BODY), size);
        if (selected)
            manager_dl_box(dl, MANAGER_DL_HIGHLIGHT,
                           UI_G(b, 24, MANAGER_DL_GAME_BAND_X0),
                           y - UI_G(b, 2, 0),
                           UI_G(b, 916, MANAGER_DL_GAME_BAND_X1), y + 32);
        manager_dl_text(dl, UI_G(b, 48, MANAGER_DL_GAME_TEXT_X),
                        y + UI_G(b, 8, MANAGER_DL_GAME_ROW_TEXT_DY),
                        UI_G(b, MANAGER_FACE_SMALL, MANAGER_FACE_BODY), muted,
                        MANAGER_DL_LEFT,
                        UI_G(b, 132, MANAGER_DL_GAME_TEXT_X + 92),
                        ui_s(ui, kind));
        manager_dl_text(dl, UI_G(b, 140, MANAGER_DL_GAME_TEXT_X + 100),
                        y + UI_G(b, 5, MANAGER_DL_GAME_ROW_TEXT_DY),
                        MANAGER_FACE_BODY,
                        selected ? MANAGER_COLOR_HIGHLIGHT_TEXT
                                 : MANAGER_COLOR_TEXT,
                        MANAGER_DL_LEFT,
                        UI_G(b, 900, MANAGER_DL_GAME_VALUE_R) - size_width - 16,
                        ui_basename(backup->path));
        manager_dl_text(dl, UI_G(b, 900, MANAGER_DL_GAME_VALUE_R),
                        y + UI_G(b, 8, MANAGER_DL_GAME_ROW_TEXT_DY),
                        UI_G(b, MANAGER_FACE_SMALL, MANAGER_FACE_BODY), muted,
                        MANAGER_DL_RIGHT, 600, size);
    }
    ui_scrollbar(b, UI_G(b, 928, MANAGER_DL_GAME_SCROLL_X), UI_G(b, 200, 198),
                 (int)UI_G(b, MANAGER_UI_BACKUP_ROWS, game_rows),
                 UI_G(b, 36, 34), top, count);

    ui_hint(b, 0, MANAGER_GLYPH_CIRCLE, MSTR_HINT_BACK);
    ui_hint(b, 0, MANAGER_GLYPH_LEFT_RIGHT, MSTR_HINT_FILE);
    ui_hint(b, 0, MANAGER_GLYPH_UP_DOWN, MSTR_HINT_BACKUP);
    if (ui->show_all_backups) {
        ui_fmt(text, sizeof(text), ui_s(ui, MSTR_HINT_SHOW_SLOT), ui->save_slot);
        manager_dl_add_hint(dl, 1, MANAGER_GLYPH_TRIANGLE, text);
    } else {
        ui_hint(b, 1, MANAGER_GLYPH_TRIANGLE, MSTR_HINT_SHOW_ALL);
    }
    if (!ui->ftp_on) {
        ui_fmt(text, sizeof(text), ui_s(ui, MSTR_HINT_BACKUP_NOW), ui->save_slot);
        manager_dl_add_hint(dl, 1, MANAGER_GLYPH_SQUARE, text);
        if (count != 0u)
            ui_hint(b, 1, MANAGER_GLYPH_CROSS, MSTR_HINT_RESTORE);
    }
}

#if defined(MANAGER_UI2_GAME_SKIN)
/* SETTINGS rows in the game layout: section headings and rows as the flat
 * layout lays them out, scrolled when they do not fit between the content
 * top and bottom (overlay knob plus UNDO make nine rows and three
 * headings).  A row and the heading above it are one unit; the view starts
 * at a unit's top, chosen to keep the selected unit near the middle like
 * ui_list_top, units that would cross the bottom are left out, and a
 * track at the right shows where the view is. */
static int ui_game_settings_heading(const struct manager_ui *ui,
                                    unsigned int row)
{
    return row == MANAGER_UI_SET_FPS || row == MANAGER_UI_SET_LANG ||
           row == (ui->settings.state.undo_available ? MANAGER_UI_SET_UNDO
                                                     : MANAGER_UI_SET_DEFAULTS);
}

static void ui_game_settings_rows(const struct ui_builder *b,
                                  const uint8_t *rows, unsigned int count)
{
    const struct manager_ui *ui = b->ui;
    struct manager_display_list *dl = b->dl;
    const int room = MANAGER_DL_GAME_BOTTOM - MANAGER_DL_GAME_TOP;
    int tops[MANAGER_UI_SET_COUNT + 1u];
    int focus = 0;
    int scroll = 0;
    int y;
    unsigned int index;
    tops[0] = 0;
    for (index = 0u; index < count; ++index) {
        const unsigned int row = rows[index];
        const int selected = row == ui->settings_row;
        const char *help = selected ? ui_s(ui, ui_setting_help(ui, row)) : "";
        const int heading = ui_game_settings_heading(ui, row) ? 20 : 0;
        const int height = help[0] != '\0' ? 52 : 32;
        if (selected)
            focus = (int)index;
        tops[index + 1u] = tops[index] + heading + height;
    }
    if (count != 0u && tops[count] > room) {
        /* The unit whose top is nearest above the centred position, moved
         * down while the selected unit would still stick out below. */
        const int centred =
            (tops[focus] + tops[focus + 1]) / 2 - room / 2;
        unsigned int first = 0u;
        while (first < (unsigned int)focus && tops[first + 1u] <= centred &&
               tops[count] - tops[first + 1u] >= room)
            ++first;
        while (first < (unsigned int)focus &&
               tops[focus + 1] - tops[first] > room)
            ++first;
        scroll = tops[first];
        manager_dl_scroll(dl, MANAGER_DL_GAME_SCROLL_X, MANAGER_DL_GAME_TOP,
                          MANAGER_DL_GAME_SCROLL_X + 4,
                          MANAGER_DL_GAME_BOTTOM - 1,
                          MANAGER_DL_GAME_TOP + room * scroll / tops[count],
                          MANAGER_DL_GAME_TOP +
                              room * (scroll + room) / tops[count] - 1);
    }
    for (index = 0u; index < count; ++index) {
        const unsigned int row = rows[index];
        const int selected = row == ui->settings_row;
        const char *help = selected ? ui_s(ui, ui_setting_help(ui, row)) : "";
        char value[128];
        y = MANAGER_DL_GAME_TOP + tops[index] - scroll;
        if (y < MANAGER_DL_GAME_TOP ||
            MANAGER_DL_GAME_TOP + tops[index + 1u] - scroll >
                MANAGER_DL_GAME_BOTTOM)
            continue;
        if (ui_game_settings_heading(ui, row)) {
            ui_heading(b, MANAGER_DL_GAME_TEXT_X, y + 2,
                       row == MANAGER_UI_SET_FPS ? MSTR_SET_SECTION_GAME
                       : row == MANAGER_UI_SET_LANG
                           ? MSTR_SET_SECTION_MANAGER
                           : MSTR_SET_SECTION_FILES);
            y += 20;
        }
        ui_setting_value(ui, row, value, sizeof(value));
        (void)ui_row(b, y, selected, ui_s(ui, ui_setting_label(row)), value,
                     selected ? help : NULL, 0, ui_row_pending(ui, row));
        if (selected && row != MANAGER_UI_SET_UNDO &&
            row != MANAGER_UI_SET_DEFAULTS) {
            const int value_width = ui_measure(b, MANAGER_FACE_BODY, value);
            manager_dl_glyph(dl, MANAGER_DL_GAME_VALUE_R - value_width - 6,
                             y + MANAGER_DL_GAME_ROW_TEXT_DY,
                             MANAGER_GLYPH_PREV, MANAGER_COLOR_HIGHLIGHT_MUTED,
                             MANAGER_DL_RIGHT);
            manager_dl_glyph(dl, MANAGER_DL_GAME_MORE_X,
                             y + MANAGER_DL_GAME_ROW_TEXT_DY,
                             MANAGER_GLYPH_NEXT, MANAGER_COLOR_HIGHLIGHT_MUTED,
                             MANAGER_DL_LEFT);
        }
    }
}
#endif

static void ui_build_settings(const struct ui_builder *b)
{
    const struct manager_ui *ui = b->ui;
    struct manager_display_list *dl = b->dl;
    const struct manager_settings_state *state = &ui->settings.state;
    uint8_t rows[MANAGER_UI_SET_COUNT];
    const unsigned int count = manager_ui_settings_rows(ui, rows);
    const unsigned int pending = manager_ui_pending_count(ui);
    char text[256];
    char info[256];
    unsigned int index;
    int y = 88;

    switch (state->status) {
    case MANAGER_SETTINGS_LOAD_OK:
        ui_fmt(info, sizeof(info), "%s", ui_s(ui, MSTR_SET_INFO_OK));
        break;
    case MANAGER_SETTINGS_LOAD_OK_WITH_IGNORED:
        ui_fmt(info, sizeof(info), ui_s(ui, MSTR_SET_INFO_IGNORED),
               (unsigned int)state->ignored);
        break;
    case MANAGER_SETTINGS_LOAD_INVALID:
        ui_fmt(info, sizeof(info), "%s", ui_s(ui, MSTR_SET_INFO_INVALID));
        break;
    case MANAGER_SETTINGS_LOAD_IO_ERROR:
        ui_fmt(info, sizeof(info), "%s", ui_s(ui, MSTR_SET_INFO_IOERR));
        break;
    default:
        ui_fmt(info, sizeof(info), "%s", ui_s(ui, MSTR_SET_INFO_ABSENT));
        break;
    }
    if (pending != 0u) {
        ui_fmt(text, sizeof(text), ui_s(ui, MSTR_SET_INFO_CHANGES), pending);
        ui_fmt(info + strlen(info), sizeof(info) - strlen(info), "%s", text);
    }
#if defined(MANAGER_UI2_GAME_SKIN)
    if (UI_GAME(b)) {
        ui_game_info(b, MANAGER_DL_GAME_X1, info);
        ui_game_settings_rows(b, rows, count);
    } else
#endif
    {
        manager_dl_text(dl, 928, 70, MANAGER_FACE_SMALL, MANAGER_COLOR_MUTED,
                        MANAGER_DL_RIGHT, 400, info);

        for (index = 0u; index < count; ++index) {
            const unsigned int row = rows[index];
            const int selected = row == ui->settings_row;
            char value[128];
            int height;
            if (row == MANAGER_UI_SET_FPS || row == MANAGER_UI_SET_LANG ||
                row == (ui->settings.state.undo_available
                            ? MANAGER_UI_SET_UNDO
                            : MANAGER_UI_SET_DEFAULTS)) {
                ui_heading(b, 32, y + 4,
                           row == MANAGER_UI_SET_FPS ? MSTR_SET_SECTION_GAME
                           : row == MANAGER_UI_SET_LANG
                               ? MSTR_SET_SECTION_MANAGER
                               : MSTR_SET_SECTION_FILES);
                y += 22;
            }
            ui_setting_value(ui, row, value, sizeof(value));
            height = ui_row(b, y, selected, ui_s(ui, ui_setting_label(row)),
                            value,
                            selected ? ui_s(ui, ui_setting_help(ui, row))
                                     : NULL,
                            0, ui_row_pending(ui, row));
            if (selected && row != MANAGER_UI_SET_UNDO &&
                row != MANAGER_UI_SET_DEFAULTS) {
                const int value_width =
                    ui_measure(b, MANAGER_FACE_SMALL, value);
                manager_dl_glyph(dl, 900 - value_width - 6, y + 6,
                                 MANAGER_GLYPH_PREV,
                                 MANAGER_COLOR_HIGHLIGHT_MUTED,
                                 MANAGER_DL_RIGHT);
                manager_dl_glyph(dl, 910, y + 6, MANAGER_GLYPH_NEXT,
                                 MANAGER_COLOR_HIGHLIGHT_MUTED,
                                 MANAGER_DL_LEFT);
            }
            y += height;
        }
    }

    ui_hint(b, 0, MANAGER_GLYPH_CIRCLE, MSTR_HINT_BACK);
    ui_hint(b, 0, MANAGER_GLYPH_UP_DOWN, MSTR_HINT_ROW);
    if (ui->ftp_on)
        return;
    if (ui->settings_row == MANAGER_UI_SET_UNDO ||
        ui->settings_row == MANAGER_UI_SET_DEFAULTS) {
        ui_hint(b, 1, MANAGER_GLYPH_CROSS, MSTR_HINT_RUN);
        return;
    }
    ui_hint(b, 0, MANAGER_GLYPH_LEFT_RIGHT, MSTR_HINT_VALUE);
    if (pending != 0u) {
        ui_fmt(text, sizeof(text), ui_s(ui, MSTR_HINT_SAVE_N), pending);
        manager_dl_add_hint(dl, 1, MANAGER_GLYPH_CROSS, text);
    } else {
        ui_hint(b, 1, MANAGER_GLYPH_CROSS, MSTR_HINT_SAVE);
    }
}

#if defined(MANAGER_TROPHY_IMPORT)
/* The trophy import row under the achievements text: heading at y 196, the
 * row (always selected, 48 px with its help line) at 218, the pending
 * request at 282 and the last result at 300 - the SETTINGS row geometry
 * (value at 900 with ◂ ▸ around it). */
static void ui_build_trophy(const struct ui_builder *b)
{
    const struct manager_ui *ui = b->ui;
    struct manager_display_list *dl = b->dl;
    const struct manager_trophy_result *last = &ui->trophy_result;
    /* Game layout: heading 170, row 192 (52 px), lines at 252 and 272. */
    const int y = UI_G(b, 218, 192);
    enum manager_color color = MANAGER_COLOR_MUTED;
    char value[64];
    char word[128];
    char text[256];
    int value_width;

    ui_heading(b, UI_G(b, 32, MANAGER_DL_GAME_TEXT_X), UI_G(b, 196, 170),
               MSTR_TROPHY_SECTION);
    ui_fmt(value, sizeof(value), ui_s(ui, MSTR_SAVES_FILE), ui->trophy_slot);
    (void)ui_row(b, y, 1, ui_s(ui, MSTR_TROPHY_GRANT), value,
                 ui_s(ui, MSTR_TROPHY_GRANT_HELP), 0, 0);
    value_width = ui_measure(
        b, UI_G(b, MANAGER_FACE_SMALL, MANAGER_FACE_BODY), value);
    manager_dl_glyph(dl, UI_G(b, 900, MANAGER_DL_GAME_VALUE_R) - value_width - 6,
                     y + UI_G(b, 6, MANAGER_DL_GAME_ROW_TEXT_DY),
                     MANAGER_GLYPH_PREV, MANAGER_COLOR_HIGHLIGHT_MUTED,
                     MANAGER_DL_RIGHT);
    manager_dl_glyph(dl, UI_G(b, 910, MANAGER_DL_GAME_MORE_X),
                     y + UI_G(b, 6, MANAGER_DL_GAME_ROW_TEXT_DY),
                     MANAGER_GLYPH_NEXT, MANAGER_COLOR_HIGHLIGHT_MUTED,
                     MANAGER_DL_LEFT);

    if (ui->trophy_pending > 0)
        ui_fmt(text, sizeof(text), ui_s(ui, MSTR_TROPHY_PENDING),
               ui->trophy_pending);
    else if (ui->trophy_pending < 0)
        ui_fmt(text, sizeof(text), "%s", ui_s(ui, MSTR_TROPHY_PENDING_BAD));
    else
        text[0] = '\0';
    if (text[0] != '\0')
        manager_dl_text(dl, UI_G(b, 48, MANAGER_DL_GAME_TEXT_X),
                        UI_G(b, 282, 252), MANAGER_FACE_SMALL,
                        ui->trophy_pending > 0 ? MANAGER_COLOR_TEXT
                                               : MANAGER_COLOR_WARNING,
                        MANAGER_DL_LEFT, UI_G(b, 928, MANAGER_DL_GAME_X1),
                        text);

    if (ui->trophy_result_state == 0) {
        ui_fmt(text, sizeof(text), "%s", ui_s(ui, MSTR_TROPHY_NO_RESULT));
    } else if (ui->trophy_result_state < 0) {
        ui_fmt(text, sizeof(text), "%s", ui_s(ui, MSTR_TROPHY_RESULT_BAD));
        color = MANAGER_COLOR_WARNING;
    } else {
        const int counted = strcmp(last->status, "done") == 0 ||
                            strcmp(last->status, "partial") == 0;
        if (strcmp(last->status, "done") == 0) {
            ui_fmt(word, sizeof(word), "%s", ui_s(ui, MSTR_TROPHY_ST_DONE));
            color = MANAGER_COLOR_OK;
        } else if (strcmp(last->status, "partial") == 0) {
            ui_fmt(word, sizeof(word), "%s", ui_s(ui, MSTR_TROPHY_ST_PARTIAL));
            color = MANAGER_COLOR_WARNING;
        } else if (strcmp(last->status, "unavailable") == 0) {
            ui_fmt(word, sizeof(word), "%s",
                   ui_s(ui, MSTR_TROPHY_ST_UNAVAILABLE));
            color = MANAGER_COLOR_WARNING;
        } else if (strcmp(last->status, "no-save") == 0) {
            ui_fmt(word, sizeof(word), "%s", ui_s(ui, MSTR_TROPHY_ST_NO_SAVE));
            color = MANAGER_COLOR_ERROR;
        } else if (strcmp(last->status, "bad-request") == 0) {
            ui_fmt(word, sizeof(word), "%s",
                   ui_s(ui, MSTR_TROPHY_ST_BAD_REQUEST));
            color = MANAGER_COLOR_ERROR;
        } else {
            ui_fmt(word, sizeof(word), ui_s(ui, MSTR_TROPHY_ST_UNREADABLE),
                   last->status);
            color = MANAGER_COLOR_ERROR;
        }
        if (counted)
            ui_fmt(text, sizeof(text), ui_s(ui, MSTR_TROPHY_LAST), last->slot,
                   word, last->unlocked, last->already, last->failed);
        else
            ui_fmt(text, sizeof(text), ui_s(ui, MSTR_TROPHY_LAST_SHORT),
                   last->slot, word);
    }
    manager_dl_text(dl, UI_G(b, 48, MANAGER_DL_GAME_TEXT_X), UI_G(b, 300, 272),
                    MANAGER_FACE_SMALL, color, MANAGER_DL_LEFT,
                    UI_G(b, 928, MANAGER_DL_GAME_X1), text);
    ui_hint(b, 0, MANAGER_GLYPH_LEFT_RIGHT, MSTR_HINT_FILE);
    ui_hint(b, 1, MANAGER_GLYPH_CROSS, MSTR_HINT_REQUEST);
}
#endif

static void ui_build_achievements(const struct ui_builder *b)
{
    char text[256];
#if defined(MANAGER_TROPHY_IMPORT)
    const enum manager_string_id summary =
        UI_G(b, MSTR_ACH_EMPTY_1, MSTR_TROPHY_SUMMARY);
    const enum manager_string_id help =
        UI_G(b, MSTR_ACH_EMPTY_2, MSTR_TROPHY_DELIVERY);
#else
    const enum manager_string_id summary = MSTR_ACH_EMPTY_1;
    const enum manager_string_id help = MSTR_ACH_EMPTY_2;
#endif
    manager_dl_text(b->dl, UI_G(b, 48, MANAGER_DL_GAME_TEXT_X),
                    UI_G(b, 100, 94), MANAGER_FACE_BODY, MANAGER_COLOR_TEXT,
                    MANAGER_DL_LEFT, UI_G(b, 928, MANAGER_DL_GAME_X1),
                    ui_s(b->ui, summary));
    manager_dl_text(b->dl, UI_G(b, 48, MANAGER_DL_GAME_TEXT_X),
                    UI_G(b, 128, 122), MANAGER_FACE_SMALL, MANAGER_COLOR_MUTED,
                    MANAGER_DL_LEFT, UI_G(b, 928, MANAGER_DL_GAME_X1),
                    ui_s(b->ui, help));
    ui_last_start_text(b->ui, text, sizeof(text), 1);
    manager_dl_text(b->dl, UI_G(b, 48, MANAGER_DL_GAME_TEXT_X),
                    UI_G(b, 160, 142), MANAGER_FACE_SMALL, MANAGER_COLOR_MUTED,
                    MANAGER_DL_LEFT, UI_G(b, 928, MANAGER_DL_GAME_X1), text);
    ui_hint(b, 0, MANAGER_GLYPH_CIRCLE, MSTR_HINT_BACK);
    ui_hint(b, 0, MANAGER_GLYPH_L_R, MSTR_HINT_PAGE);
#if defined(MANAGER_TROPHY_IMPORT)
    ui_build_trophy(b);
#endif
}

#if defined(MANAGER_UI2_GAME_SKIN)
/* CONTROLS in the game layout.  Team Meat draws lower case as small
 * capitals, so "SELECT держать" had full-height capitals beside small ones;
 * the runs of Latin capitals (the button names) are drawn in SMALL, whose
 * capitals are about BODY's small-capital height, on the same baseline.
 * (Lower-casing them instead does not work: Team Meat's "l" is a real
 * lower-case l.)  The touch outline (200x112, the screen's 16:9) sits
 * right of the first four rows; its zones are wide enough for the whole
 * word. */
#define UI_CTL_KEY_RIGHT 304
#define UI_CTL_ACTION_X 312
#define UI_CTL_TOUCH_X0 706
#define UI_CTL_TOUCH_X1 906
#define UI_CTL_ROWS_Y (MANAGER_DL_GAME_TOP + 22)
static void ui_game_key(const struct ui_builder *b, int x, int y, int right,
                        const char *text)
{
    char run[128];
    while (*text != '\0' && x < right) {
        const int caps = *text >= 'A' && *text <= 'Z';
        const enum manager_face_role face =
            caps ? MANAGER_FACE_SMALL : MANAGER_FACE_BODY;
        size_t length = 0u;
        while (text[length] != '\0' && length + 1u < sizeof(run) &&
               (text[length] >= 'A' && text[length] <= 'Z') == caps) {
            run[length] = text[length];
            ++length;
        }
        run[length] = '\0';
        manager_dl_text(b->dl, x, caps ? ui_small_on_body(b, y) : y, face,
                        MANAGER_COLOR_TEXT, MANAGER_DL_LEFT, right, run);
        x += ui_measure(b, face, run);
        text += length;
    }
}

static void ui_game_controls(const struct ui_builder *b,
                             const enum manager_string_id *keys,
                             const enum manager_string_id *actions,
                             const enum manager_string_id *zone_words)
{
    /* Zones 84x40 in the outline's corners: top-left, top-right,
     * bottom-right, 6 px clear of its rounded corners. */
    static const short zones[3][4] = {
        {UI_CTL_TOUCH_X0 + 6, UI_CTL_ROWS_Y + 6, UI_CTL_TOUCH_X0 + 89,
         UI_CTL_ROWS_Y + 45},
        {UI_CTL_TOUCH_X1 - 89, UI_CTL_ROWS_Y + 6, UI_CTL_TOUCH_X1 - 6,
         UI_CTL_ROWS_Y + 45},
        {UI_CTL_TOUCH_X1 - 89, UI_CTL_ROWS_Y + 66, UI_CTL_TOUCH_X1 - 6,
         UI_CTL_ROWS_Y + 105}};
    struct manager_display_list *dl = b->dl;
    unsigned int index;
    ui_heading(b, MANAGER_DL_GAME_TEXT_X, MANAGER_DL_GAME_TOP,
               MSTR_CTL_SECTION_GAME);
    for (index = 0u; index < 9u; ++index) {
        const int y = UI_CTL_ROWS_Y + (int)index * 30;
        ui_game_key(b, MANAGER_DL_GAME_TEXT_X, y, UI_CTL_KEY_RIGHT,
                    ui_s(b->ui, keys[index]));
        manager_dl_text(dl, UI_CTL_ACTION_X, y, MANAGER_FACE_BODY,
                        MANAGER_COLOR_MUTED, MANAGER_DL_LEFT,
                        UI_CTL_TOUCH_X0 - 12, ui_s(b->ui, actions[index]));
    }
    manager_dl_text(dl, MANAGER_DL_GAME_TEXT_X, UI_CTL_ROWS_Y + 9 * 30 + 6,
                    MANAGER_FACE_SMALL, MANAGER_COLOR_MUTED, MANAGER_DL_LEFT,
                    MANAGER_DL_GAME_X1, ui_s(b->ui, MSTR_CTL_NOTE));
    ui_heading(b, UI_CTL_TOUCH_X0, MANAGER_DL_GAME_TOP, MSTR_CTL_SECTION_TOUCH);
    manager_dl_frame(dl, UI_CTL_TOUCH_X0, UI_CTL_ROWS_Y, UI_CTL_TOUCH_X1,
                     UI_CTL_ROWS_Y + 111, 6, 2, MANAGER_COLOR_MUTED);
    for (index = 0u; index < 3u; ++index) {
        const int height = zones[index][3] - zones[index][1] + 1;
        manager_dl_fill(dl, zones[index][0], zones[index][1], zones[index][2],
                        zones[index][3], MANAGER_COLOR_SEPARATOR);
        manager_dl_text(
            dl, (zones[index][0] + zones[index][2] + 1) / 2,
            zones[index][1] +
                (height - manager_face_ascent(b->theme->face[MANAGER_FACE_SMALL])) /
                    2,
            MANAGER_FACE_SMALL, MANAGER_COLOR_TEXT, MANAGER_DL_CENTER,
            zones[index][2] - zones[index][0] - 4,
            ui_s(b->ui, zone_words[index]));
    }
    ui_hint(b, 0, MANAGER_GLYPH_CIRCLE, MSTR_HINT_BACK);
    ui_hint(b, 0, MANAGER_GLYPH_L_R, MSTR_HINT_PAGE);
}
#endif

static void ui_build_controls(const struct ui_builder *b)
{
    static const enum manager_string_id keys[9] = {
        MSTR_CTL_K_ACTIVE,      MSTR_CTL_K_MAP,     MSTR_CTL_K_POCKET,
        MSTR_CTL_K_SELECT_TAP,  MSTR_CTL_K_SELECT_HOLD, MSTR_CTL_K_L,
        MSTR_CTL_K_START,       MSTR_CTL_K_START_L, MSTR_CTL_K_START_R};
    static const enum manager_string_id actions[9] = {
        MSTR_CTL_A_ACTIVE,      MSTR_CTL_A_MAP,     MSTR_CTL_A_POCKET,
        MSTR_CTL_A_SELECT_TAP,  MSTR_CTL_A_SELECT_HOLD, MSTR_CTL_A_L,
        MSTR_CTL_A_START,       MSTR_CTL_A_START_L, MSTR_CTL_A_START_R};
    /* Touch zones on a 16:9 screen outline (x 670..928, y 100..245),
     * inset 6 px so they clear its 6 px corner radius. */
    static const short zones[3][4] = {
        {676, 106, 751, 149}, {847, 106, 922, 149}, {847, 196, 922, 239}};
    static const enum manager_string_id zone_words[3] = {
        MSTR_CTL_Z_ACTIVE, MSTR_CTL_Z_MAP, MSTR_CTL_Z_POCKET};
    struct manager_display_list *dl = b->dl;
    unsigned int index;
#if defined(MANAGER_UI2_GAME_SKIN)
    if (UI_GAME(b)) {
        ui_game_controls(b, keys, actions, zone_words);
        return;
    }
#endif
    ui_heading(b, 32, 78, MSTR_CTL_SECTION_GAME);
    for (index = 0u; index < 9u; ++index) {
        const int y = 100 + (int)index * 30;
        manager_dl_text(dl, 48, y, MANAGER_FACE_BODY, MANAGER_COLOR_TEXT,
                        MANAGER_DL_LEFT, 290, ui_s(b->ui, keys[index]));
        manager_dl_text(dl, 300, y, MANAGER_FACE_BODY, MANAGER_COLOR_MUTED,
                        MANAGER_DL_LEFT, 656, ui_s(b->ui, actions[index]));
    }
    manager_dl_text(dl, 48, 378, MANAGER_FACE_SMALL, MANAGER_COLOR_MUTED,
                    MANAGER_DL_LEFT, 656, ui_s(b->ui, MSTR_CTL_NOTE));
    ui_heading(b, 670, 78, MSTR_CTL_SECTION_TOUCH);
    manager_dl_frame(dl, 670, 100, 928, 245, 6, 2, MANAGER_COLOR_MUTED);
    for (index = 0u; index < 3u; ++index) {
        manager_dl_fill(dl, zones[index][0], zones[index][1], zones[index][2],
                        zones[index][3], MANAGER_COLOR_SEPARATOR);
        manager_dl_text(dl, (zones[index][0] + zones[index][2]) / 2,
                        zones[index][1] + 17, MANAGER_FACE_SMALL,
                        MANAGER_COLOR_TEXT, MANAGER_DL_CENTER, 76,
                        ui_s(b->ui, zone_words[index]));
    }
    ui_hint(b, 0, MANAGER_GLYPH_CIRCLE, MSTR_HINT_BACK);
    ui_hint(b, 0, MANAGER_GLYPH_L_R, MSTR_HINT_PAGE);
}

static void ui_build_about(const struct ui_builder *b)
{
    const struct manager_ui *ui = b->ui;
    struct manager_display_list *dl = b->dl;
    static const enum manager_color resource_colors[3] = {
        MANAGER_COLOR_ERROR, MANAGER_COLOR_TEXT, MANAGER_COLOR_WARNING};
    /* Game layout: the same blocks at TEXT_X, headings aligned with the
     * lines, resources and the last start in BODY with BODY values. */
    const int x = UI_G(b, 48, MANAGER_DL_GAME_TEXT_X);
    const int right = UI_G(b, 928, MANAGER_DL_GAME_X1);
    char text[256];
    ui_fmt(text, sizeof(text), "%s", ui_s(ui, MSTR_ABOUT_APP));
    manager_dl_text(dl, x, UI_G(b, 80, 92), MANAGER_FACE_BODY,
                    MANAGER_COLOR_TEXT, MANAGER_DL_LEFT, right, text);
    ui_fmt(text, sizeof(text), ui_s(ui, MSTR_ABOUT_VERSION),
           ui->config.app_version,
           ui->config.build_id[0] != '\0' ? ui->config.build_id
                                          : ui_s(ui, MSTR_WORD_UNKNOWN));
    manager_dl_text(dl, x, UI_G(b, 104, 118), MANAGER_FACE_SMALL,
                    MANAGER_COLOR_MUTED, MANAGER_DL_LEFT, right, text);

    ui_heading(b, UI_G(b, 32, MANAGER_DL_GAME_TEXT_X), UI_G(b, 134, 146),
               MSTR_ABOUT_RESOURCES);
    manager_dl_text(dl, x, UI_G(b, 152, 166), MANAGER_FACE_BODY,
                    MANAGER_COLOR_TEXT, MANAGER_DL_LEFT, 600,
                    UI_RESOURCE_ANIMATIONS);
    manager_dl_text(dl, UI_G(b, 900, MANAGER_DL_GAME_VALUE_R),
                    UI_G(b, 154, 166),
                    UI_G(b, MANAGER_FACE_SMALL, MANAGER_FACE_BODY),
                    resource_colors[ui->resource_animations % 3u],
                    MANAGER_DL_RIGHT, 600,
                    ui_s(ui, ui_resource_word(ui->resource_animations)));
    manager_dl_text(dl, x, UI_G(b, 176, 192), MANAGER_FACE_BODY,
                    MANAGER_COLOR_TEXT, MANAGER_DL_LEFT, 600,
                    UI_RESOURCE_REPENTANCE);
    manager_dl_text(dl, UI_G(b, 900, MANAGER_DL_GAME_VALUE_R),
                    UI_G(b, 178, 192),
                    UI_G(b, MANAGER_FACE_SMALL, MANAGER_FACE_BODY),
                    resource_colors[ui->resource_repentance % 3u],
                    MANAGER_DL_RIGHT, 600,
                    ui_s(ui, ui_resource_word(ui->resource_repentance)));
    ui_fmt(text, sizeof(text), ui_s(ui, MSTR_ABOUT_COUNTS),
           (unsigned int)ui->mod_count, ui_save_files_present(ui));
    manager_dl_text(dl, x, UI_G(b, 204, 220), MANAGER_FACE_SMALL,
                    MANAGER_COLOR_MUTED, MANAGER_DL_LEFT, right, text);

    ui_heading(b, UI_G(b, 32, MANAGER_DL_GAME_TEXT_X), UI_G(b, 234, 248),
               MSTR_ABOUT_LAST);
    ui_last_start_text(ui, text, sizeof(text), 0);
    manager_dl_text(dl, x, UI_G(b, 252, 268),
                    UI_G(b, MANAGER_FACE_SMALL, MANAGER_FACE_BODY),
                    MANAGER_COLOR_TEXT, MANAGER_DL_LEFT, right, text);

    ui_heading(b, UI_G(b, 32, MANAGER_DL_GAME_TEXT_X), UI_G(b, 282, 302),
               MSTR_ABOUT_LICENCES);
    manager_dl_text(dl, x, UI_G(b, 300, 322), MANAGER_FACE_SMALL,
                    MANAGER_COLOR_MUTED, MANAGER_DL_LEFT, right,
                    ui_s(ui, MSTR_ABOUT_LIC_FTP));
    manager_dl_text(dl, x, UI_G(b, 318, 342), MANAGER_FACE_SMALL,
                    MANAGER_COLOR_MUTED, MANAGER_DL_LEFT, right,
                    ui_s(ui, MSTR_ABOUT_LIC_DEJAVU));
    manager_dl_text(dl, x, UI_G(b, 336, 362), MANAGER_FACE_SMALL,
                    MANAGER_COLOR_MUTED, MANAGER_DL_LEFT, right,
                    ui_s(ui, MSTR_ABOUT_LIC_GAME));
    manager_dl_text(dl, x, UI_G(b, 366, 390), MANAGER_FACE_SMALL,
                    MANAGER_COLOR_MUTED, MANAGER_DL_LEFT, right,
                    ui_s(ui, MSTR_ABOUT_REPORT));
    ui_hint(b, 0, MANAGER_GLYPH_CIRCLE, MSTR_HINT_BACK);
    ui_hint(b, 0, MANAGER_GLYPH_L_R, MSTR_HINT_PAGE);
}

static void ui_build_detail(const struct ui_builder *b)
{
    const struct manager_ui *ui = b->ui;
    struct manager_display_list *dl = b->dl;
    const struct manager_ui_mod *mod = &ui->mods[ui->mod_selection];
    char text[256];
    manager_dl_box(dl, MANAGER_DL_SCRIM, 0, 0, 959, 495);
    /* Five lines, the last small one ending at y 279; 24 px below it. */
    manager_dl_panel(dl, 200, 150, 759, 303, MANAGER_PANEL_BOX);
#if defined(MANAGER_UI2_GAME_SKIN)
    {
        int show_directory = 0;
        manager_dl_text(dl, 224, 170, MANAGER_FACE_BODY, MANAGER_COLOR_TEXT,
                        MANAGER_DL_LEFT, 736,
                        ui_mod_name(mod, &show_directory));
    }
#else
    manager_dl_text(dl, 224, 170, MANAGER_FACE_BODY, MANAGER_COLOR_TEXT,
                    MANAGER_DL_LEFT, 736, mod->meta.name);
#endif
    ui_fmt(text, sizeof(text), ui_s(ui, MSTR_MODS_DETAIL_DIR), mod->entry.name);
    manager_dl_text(dl, 224, 204, MANAGER_FACE_SMALL, MANAGER_COLOR_MUTED,
                    MANAGER_DL_LEFT, 736, text);
    if (mod->meta.version[0] != '\0')
        ui_fmt(text, sizeof(text), ui_s(ui, MSTR_MODS_DETAIL_VERSION),
               mod->meta.version);
    else
        ui_fmt(text, sizeof(text), "%s", ui_s(ui, MSTR_MODS_DETAIL_NOVERSION));
    manager_dl_text(dl, 224, 224, MANAGER_FACE_SMALL, MANAGER_COLOR_MUTED,
                    MANAGER_DL_LEFT, 736, text);
    manager_dl_text(dl, 224, 244, MANAGER_FACE_SMALL, MANAGER_COLOR_MUTED,
                    MANAGER_DL_LEFT, 736,
                    ui_s(ui, mod->entry.disabled ? MSTR_MODS_DETAIL_OFF
                                                 : MSTR_MODS_DETAIL_ON));
    manager_dl_text(dl, 224, 264, MANAGER_FACE_SMALL, MANAGER_COLOR_MUTED,
                    MANAGER_DL_LEFT, 736,
                    ui_s(ui, mod->meta.from_metadata ? MSTR_MODS_DETAIL_META
                                                     : MSTR_MODS_DETAIL_NOMETA));
    dl->hint_count = 0u;
    ui_hint(b, 1, MANAGER_GLYPH_CIRCLE, MSTR_HINT_CLOSE);
}

static void ui_build_confirm(const struct ui_builder *b)
{
    const struct manager_ui *ui = b->ui;
    struct manager_display_list *dl = b->dl;
    char question[256];
    char lines[3][256];
    unsigned int line_count = 0u;
    unsigned int index;
    int x;

    question[0] = '\0';
    switch (ui->confirm) {
    case MANAGER_UI_CONFIRM_LAUNCH_FTP:
        ui_fmt(question, sizeof(question), "%s",
               ui_s(ui, MSTR_HOME_LAUNCH_FTP_Q));
        ui_fmt(lines[line_count++], sizeof(lines[0]), "%s",
               ui_s(ui, MSTR_HOME_LAUNCH_FTP_D));
        break;
    case MANAGER_UI_CONFIRM_MODS_ALL:
        if (ui_mods_any_disabled(ui))
            ui_fmt(question, sizeof(question), ui_s(ui, MSTR_MODS_ALL_ENABLE_Q),
                   (unsigned int)(ui->mod_count - ui_mods_enabled(ui)));
        else
            ui_fmt(question, sizeof(question),
                   ui_s(ui, MSTR_MODS_ALL_DISABLE_Q),
                   (unsigned int)ui->mod_count);
        ui_fmt(lines[line_count++], sizeof(lines[0]), "%s",
               ui_s(ui, MSTR_MODS_ALL_DETAIL));
        break;
    case MANAGER_UI_CONFIRM_RESTORE: {
        const struct manager_save_backup *backup = ui_selected_backup(ui);
        if (backup == NULL)
            break;
        ui_fmt(question, sizeof(question), ui_s(ui, MSTR_SAVES_RESTORE_Q),
               backup->slot);
        ui_fmt(lines[line_count++], sizeof(lines[0]), "%s",
               ui_basename(backup->path));
        ui_fmt(lines[line_count++], sizeof(lines[0]),
               ui_s(ui, MSTR_SAVES_RESTORE_KEEP), backup->slot);
#if defined(ISAAC_VITA_MANAGER_SAVE_GAMESTATE)
        ui_fmt(lines[line_count++], sizeof(lines[0]), "%s",
               ui_s(ui, backup->has_gamestate ? MSTR_SAVES_RESTORE_RUN
                                              : MSTR_SAVES_RESTORE_KEEPRUN));
#endif
        break;
    }
    case MANAGER_UI_CONFIRM_SETTINGS:
        if (ui->settings.armed == MANAGER_SETTINGS_UI_ARM_SAVE) {
            ui_fmt(question, sizeof(question), ui_s(ui, MSTR_SET_UNEVEN_Q),
                   (unsigned int)ui->settings.state.pending.fps_lock);
            ui_fmt(lines[line_count++], sizeof(lines[0]), "%s",
                   ui_s(ui, MSTR_SET_UNEVEN_D));
        } else if (ui->settings.armed == MANAGER_SETTINGS_UI_ARM_DEFAULTS) {
            ui_fmt(question, sizeof(question), "%s",
                   ui_s(ui, MSTR_SET_DEFAULTS_Q));
            ui_fmt(lines[line_count++], sizeof(lines[0]), "%s",
                   ui_s(ui, MSTR_SET_DEFAULTS_D));
        } else {
            ui_fmt(question, sizeof(question), ui_s(ui, MSTR_SET_DISCARD_Q),
                   manager_ui_pending_count(ui));
            ui_fmt(lines[line_count++], sizeof(lines[0]), "%s",
                   ui_s(ui, MSTR_SET_DISCARD_D));
        }
        break;
    default:
        break;
    }
    manager_dl_box(dl, MANAGER_DL_SCRIM, 0, 0, 959, 495);
    manager_dl_panel(dl, 240, 192, 719, 351, MANAGER_PANEL_BOX);
    manager_dl_text(dl, 264, 212, MANAGER_FACE_BODY, MANAGER_COLOR_TEXT,
                    MANAGER_DL_LEFT, 696, question);
    for (index = 0u; index < line_count; ++index)
        manager_dl_text(dl, 264, 244 + (int)index * 18, MANAGER_FACE_SMALL,
                        index == 0u && ui->confirm == MANAGER_UI_CONFIRM_RESTORE
                            ? MANAGER_COLOR_TEXT
                            : MANAGER_COLOR_MUTED,
                        MANAGER_DL_LEFT, 696, lines[index]);
    /* "○ No    × Yes" at the box's bottom right, as on the hint bar. */
    x = 696;
    {
        const char *yes = ui_s(ui, MSTR_HINT_YES);
        const char *no = ui_s(ui, MSTR_HINT_NO);
        const int space = ui_measure(b, MANAGER_FACE_BODY, " ");
        const int yes_width = ui_measure(b, MANAGER_FACE_BODY, yes);
        const int no_width = ui_measure(b, MANAGER_FACE_BODY, no);
        manager_dl_text(dl, x, 318, MANAGER_FACE_BODY, MANAGER_COLOR_TEXT,
                        MANAGER_DL_RIGHT, 0, yes);
        x -= yes_width + space;
        manager_dl_glyph(dl, x, 318, MANAGER_GLYPH_CROSS, MANAGER_COLOR_TEXT,
                         MANAGER_DL_RIGHT);
        x -= ui_glyph_width(b, MANAGER_GLYPH_CROSS) + 36;
        manager_dl_text(dl, x, 318, MANAGER_FACE_BODY, MANAGER_COLOR_TEXT,
                        MANAGER_DL_RIGHT, 0, no);
        x -= no_width + space;
        manager_dl_glyph(dl, x, 318, MANAGER_GLYPH_CIRCLE, MANAGER_COLOR_TEXT,
                         MANAGER_DL_RIGHT);
    }
    dl->hint_count = 0u;
    ui_hint(b, 0, MANAGER_GLYPH_CIRCLE, MSTR_HINT_NO);
    ui_hint(b, 1, MANAGER_GLYPH_CROSS, MSTR_HINT_YES);
}

void manager_ui_build(const struct manager_ui *ui,
                      struct manager_display_list *dl)
{
    struct ui_builder builder;
    unsigned int page;
    builder.ui = ui;
    builder.theme = ui->theme;
    builder.dl = dl;
    manager_dl_reset(dl);
    manager_dl_set_title(dl, ui_s(ui, ui_title_ids[ui->page]));
    for (page = 0u; page < MANAGER_UI_PAGE_COUNT; ++page)
        manager_dl_add_tab(dl, ui_s(ui, ui_tab_ids[page]));
    dl->active_tab = (int8_t)ui->page;
    manager_dl_set_status(dl, (enum manager_tone)ui->status_tone, ui->status);
    switch (ui->page) {
    case MANAGER_UI_PAGE_MODS:
        ui_build_mods(&builder);
        break;
    case MANAGER_UI_PAGE_SAVES:
        ui_build_saves(&builder);
        break;
    case MANAGER_UI_PAGE_SETTINGS:
        ui_build_settings(&builder);
        break;
    case MANAGER_UI_PAGE_ACHIEVEMENTS:
        ui_build_achievements(&builder);
        break;
    case MANAGER_UI_PAGE_CONTROLS:
        ui_build_controls(&builder);
        break;
    case MANAGER_UI_PAGE_ABOUT:
        ui_build_about(&builder);
        break;
    default:
        ui_build_home(&builder);
        break;
    }
    if (ui->detail_open && ui->page == MANAGER_UI_PAGE_MODS &&
        ui->mod_count != 0u)
        ui_build_detail(&builder);
    if (ui->confirm != MANAGER_UI_CONFIRM_NONE)
        ui_build_confirm(&builder);
}

#endif /* MANAGER_UI2 */
