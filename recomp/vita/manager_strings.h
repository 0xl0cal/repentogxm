/*
 * Manager UI2 string table (every user-visible string of the UI2 pages;
 * knob ISAAC_VITA_MANAGER_UI2 -> MANAGER_UI2=1).
 *
 * One X-macro list holds every user-visible string of the UI2 pages in
 * English and Russian, so the two columns cannot drift apart: the enum and
 * both tables are generated from it.  Each entry names the slot it is drawn
 * in; manager_ui_host_test measures every string of both columns in that
 * slot's face (format directives replaced by a typical value) and fails on
 * any string wider than the slot, and checks that both columns carry the
 * same printf directives in the same order.
 *
 * Tab names of the Russian column are the ones of the approved Russian
 * design mock-up of the MODS page (mock C, the renderer's
 * render_c_mods_ru), copied verbatim.  Technical tokens (port.ini,
 * disable.it, fps_lock, paths) are not translated.  Russian plurals are
 * avoided by the "noun: N" form so no plural rules are needed.
 */
#ifndef ISAAC_MANAGER_STRINGS_H
#define ISAAC_MANAGER_STRINGS_H

#if defined(MANAGER_UI2)

enum manager_lang {
    MANAGER_LANG_EN = 0,
    MANAGER_LANG_RU,
    MANAGER_LANG_COUNT
};

/* Slot classes (face + widest allowed text, see manager_strings.c). */
enum manager_string_slot {
    MSLOT_TAB = 0,  /* small, 110 px: header tabs */
    MSLOT_TITLE,    /* title, 270 px: page title left of the tabs (x<=68) */
    MSLOT_LABEL,    /* body, 480 px: list row labels */
    MSLOT_VALUE,    /* small, 420 px: right-aligned row values */
    MSLOT_HINT,     /* body, 280 px: one hint-bar label */
    MSLOT_LINE,     /* small, 880 px: status row, notes, help lines */
    MSLOT_BODY,     /* body, 880 px: body text lines */
    MSLOT_WORD,     /* small, 220 px: words spliced into other strings */
    MSLOT_BOX,      /* small, 432 px: confirmation box lines */
    MSLOT_BOXTITLE, /* body, 432 px: confirmation box question */
    MSLOT_CELL,     /* body, 236 px: CONTROLS key column */
    MSLOT_ACTION,   /* body, 350 px: CONTROLS action column */
    MSLOT_COUNT
};

/* Game skin builds (owner's device review 2026-10-10): one name per page in
 * its tab, its HOME row and its title, and the CONTROLS pocket zone says
 * the whole word like its "active" neighbour.  Other builds keep their
 * table byte for byte. */
#if defined(MANAGER_UI2_GAME_SKIN)
#define MSTR_RU_TAB_SAVES "Сохранения"
#define MSTR_RU_TAB_ACHIEVEMENTS "Достижения"
#define MSTR_EN_SAVES_PAGE "Saves"
#define MSTR_RU_HOME_SAVES "Сохранения"
#define MSTR_RU_ZONE_POCKET "карманный"
#else
#define MSTR_RU_TAB_SAVES "Сейвы"
#define MSTR_RU_TAB_ACHIEVEMENTS "Ачивки"
#define MSTR_EN_SAVES_PAGE "Saves and backups"
#define MSTR_RU_HOME_SAVES "Сохранения и копии"
#define MSTR_RU_ZONE_POCKET "карман"
#endif

/* X(id, slot, english, russian) */
#define MANAGER_STRINGS(X)                                                     \
    X(MSTR_TAB_HOME, MSLOT_TAB, "Home", "Главная")                             \
    X(MSTR_TAB_MODS, MSLOT_TAB, "Mods", "Моды")                                \
    X(MSTR_TAB_SAVES, MSLOT_TAB, "Saves", MSTR_RU_TAB_SAVES)                   \
    X(MSTR_TAB_SETTINGS, MSLOT_TAB, "Settings", "Настройки")                   \
    X(MSTR_TAB_ACHIEVEMENTS, MSLOT_TAB, "Achievements",                        \
      MSTR_RU_TAB_ACHIEVEMENTS)                                                \
    X(MSTR_TAB_CONTROLS, MSLOT_TAB, "Controls", "Управление")                  \
    X(MSTR_TAB_ABOUT, MSLOT_TAB, "About", "О порте")                           \
    X(MSTR_TITLE_HOME, MSLOT_TITLE, "Isaac Manager", "Менеджер Isaac")         \
    X(MSTR_TITLE_MODS, MSLOT_TITLE, "Mods", "Моды")                            \
    X(MSTR_TITLE_SAVES, MSLOT_TITLE, MSTR_EN_SAVES_PAGE, "Сохранения")         \
    X(MSTR_TITLE_SETTINGS, MSLOT_TITLE, "Settings", "Настройки")               \
    X(MSTR_TITLE_ACHIEVEMENTS, MSLOT_TITLE, "Achievements", "Достижения")      \
    X(MSTR_TITLE_CONTROLS, MSLOT_TITLE, "Controls", "Управление")              \
    X(MSTR_TITLE_ABOUT, MSLOT_TITLE, "About", "О порте")                       \
    /* HOME */                                                                 \
    X(MSTR_HOME_LAUNCH, MSLOT_LABEL, "Launch the game", "Запустить игру")      \
    X(MSTR_HOME_COUNTDOWN, MSLOT_LABEL, "Launching the game in %u s",          \
      "Запуск игры через %u с")                                                \
    X(MSTR_HOME_COUNTDOWN_STAY, MSLOT_VALUE, "press any button to stay",       \
      "любая кнопка — остаться")                                               \
    X(MSTR_HOME_FTP, MSLOT_LABEL, "FTP server", "FTP-сервер")                  \
    X(MSTR_HOME_FTP_OFF, MSLOT_VALUE, "off", "выкл.")                          \
    X(MSTR_HOME_FTP_ON, MSLOT_VALUE, "on · %s:%u", "вкл. · %s:%u")             \
    X(MSTR_HOME_FTP_FAILED, MSLOT_VALUE, "did not start", "не запустился")     \
    X(MSTR_HOME_FTP_SUB, MSLOT_LINE,                                           \
      "ux0:data/isaacr001 · port 1337 · no password",                          \
      "ux0:data/isaacr001 · порт 1337 · без пароля")                           \
    X(MSTR_HOME_MODS, MSLOT_LABEL, "Mods", "Моды")                             \
    X(MSTR_HOME_MODS_VALUE, MSLOT_VALUE, "%u of %u enabled",                   \
      "включено %u из %u")                                                     \
    X(MSTR_HOME_MODS_NONE, MSLOT_VALUE, "no mods installed", "модов нет")      \
    X(MSTR_HOME_SAVES, MSLOT_LABEL, MSTR_EN_SAVES_PAGE, MSTR_RU_HOME_SAVES)    \
    X(MSTR_HOME_SAVES_VALUE, MSLOT_VALUE, "%u of 3 files · %u backups",        \
      "файлов %u из 3 · копий: %u")                                            \
    X(MSTR_HOME_SETTINGS, MSLOT_LABEL, "Settings", "Настройки")                \
    X(MSTR_HOME_SETTINGS_VALUE, MSLOT_VALUE, "FPS lock %u · room render %s",   \
      "лимит FPS %u · комнаты %s")                                             \
    X(MSTR_HOME_ACHIEVEMENTS, MSLOT_LABEL, "Achievements", "Достижения")       \
    X(MSTR_HOME_ACH_VALUE, MSLOT_VALUE, "no data yet", "данных пока нет")      \
    X(MSTR_HOME_CONTROLS, MSLOT_LABEL, "Controls", "Управление")               \
    X(MSTR_HOME_ABOUT, MSLOT_LABEL, "About", "О порте")                        \
    X(MSTR_HOME_ABOUT_VALUE, MSLOT_VALUE, "build %s · resources %s",           \
      "сборка %s · ресурсы: %s")                                               \
    X(MSTR_HOME_READONLY, MSLOT_WORD, " · read-only", " · только чтение")      \
    X(MSTR_HOME_RECOVERY_OK, MSLOT_LINE, "Recovery: ok",                       \
      "Восстановление: ок")                                                    \
    X(MSTR_HOME_RECOVERY_WARN, MSLOT_LINE,                                     \
      "Recovery: done, but a sync call failed",                                \
      "Восстановление: выполнено, но синхронизация не удалась")                \
    X(MSTR_HOME_RECOVERY_FAIL, MSLOT_LINE, "Recovery: failed — %s",            \
      "Восстановление: ошибка — %s")                                           \
    X(MSTR_HOME_LAUNCH_FTP_Q, MSLOT_BOXTITLE, "FTP is on — stop it and launch?", \
      "FTP включён — выключить и запустить?")                                  \
    X(MSTR_HOME_LAUNCH_FTP_D, MSLOT_BOX, "Transfers in progress are cut off.", \
      "Незавершённые передачи прервутся.")                                     \
    /* last game start */                                                      \
    X(MSTR_LAST_START, MSLOT_LINE, "Last game start: %s",                      \
      "Последний запуск игры: %s")                                             \
    X(MSTR_LAST_NO_LOG, MSLOT_WORD, "no log yet", "лога пока нет")             \
    X(MSTR_LAST_IGNORES, MSLOT_WORD, "this game build ignores port.ini",       \
      "эта сборка игры не читает port.ini")                                    \
    X(MSTR_LAST_PORT_OK, MSLOT_WORD, "port.ini ok", "port.ini ок")             \
    X(MSTR_LAST_PORT_ABSENT, MSLOT_WORD, "no port.ini", "без port.ini")        \
    X(MSTR_LAST_PORT_BAD, MSLOT_VALUE, "port.ini %s — defaults used",           \
      "port.ini %s — значения по умолчанию")                                   \
    X(MSTR_LAST_FPS_IGNORED, MSLOT_WORD, "fps_lock %u ignored by this build",  \
      "fps_lock %u не применён сборкой")                                       \
    X(MSTR_LAST_REMAP_CUSTOM, MSLOT_WORD, "custom remap", "своя раскладка")    \
    /* common words */                                                         \
    X(MSTR_WORD_OK, MSLOT_WORD, "ok", "ок")                                    \
    X(MSTR_WORD_MISSING, MSLOT_WORD, "missing", "нет")                         \
    X(MSTR_WORD_ON, MSLOT_WORD, "on", "вкл.")                                  \
    X(MSTR_WORD_OFF, MSLOT_WORD, "off", "выкл.")                               \
    X(MSTR_WORD_UNKNOWN, MSLOT_WORD, "unknown", "неизвестно")                      X(MSTR_SIZE_BYTES, MSLOT_WORD, "%u B", "%u Б")                                 X(MSTR_SIZE_KIB, MSLOT_WORD, "%u KiB", "%u КиБ")                               X(MSTR_SIZE_MIB, MSLOT_WORD, "%u.%u MiB", "%u,%u МиБ")                     \
    /* storage results */                                                      \
    X(MSTR_RES_OK, MSLOT_WORD, "ok", "ок")                                     \
    X(MSTR_RES_SYNC_WARNING, MSLOT_WORD, "written, sync warning",              \
      "записано, сбой синхронизации")                                          \
    X(MSTR_RES_HASH, MSLOT_WORD, "hash mismatch", "не совпал хеш")             \
    X(MSTR_RES_CONFLICT, MSLOT_WORD, "file conflict", "конфликт файлов")       \
    X(MSTR_RES_TOO_LARGE, MSLOT_WORD, "save too large",                        \
      "слишком большой файл")                                                  \
    X(MSTR_RES_CAPACITY, MSLOT_WORD, "no free backup number",                  \
      "нет свободного номера копии")                                           \
    X(MSTR_RES_IO, MSLOT_WORD, "I/O error", "ошибка ввода-вывода")             \
    X(MSTR_RES_INVALID, MSLOT_WORD, "invalid request", "неверный запрос")      \
    X(MSTR_RES_NEED_A_AND_B, MSLOT_WORD, "remap needs A and B",                \
      "раскладке нужны A и B")                                                 \
    X(MSTR_RES_BAD_FILE, MSLOT_WORD, "file is not valid", "файл некорректен")  \
    X(MSTR_RES_NOTHING_TO_UNDO, MSLOT_WORD, "nothing to undo",                 \
      "нечего отменять")                                                       \
    X(MSTR_RES_BAD_ARCHIVE, MSLOT_WORD, "archive does not parse",              \
      "архив не читается")                                                     \
    /* status row */                                                           \
    X(MSTR_STATUS_READY, MSLOT_LINE, "Ready", "Готово")                        \
    X(MSTR_STATUS_FTP_ON, MSLOT_LINE,                                          \
      "FTP server on — anyone on this network can change ux0:data/isaacr001",  \
      "FTP-сервер включён — любой в этой сети может менять ux0:data/isaacr001") \
    X(MSTR_STATUS_FTP_OFF, MSLOT_LINE, "FTP server stopped",                   \
      "FTP-сервер остановлен")                                                 \
    X(MSTR_STATUS_FTP_FAILED, MSLOT_LINE, "FTP server did not start",          \
      "FTP-сервер не запустился")                                              \
    X(MSTR_STATUS_FTP_DIED, MSLOT_LINE, "FTP server stopped after an error",   \
      "FTP-сервер остановился из-за ошибки")                                   \
    X(MSTR_STATUS_READONLY, MSLOT_LINE,                                        \
      "Read-only while the FTP server is on — stop it on Home",                \
      "Только чтение, пока работает FTP-сервер — выключите его на главной")    \
    X(MSTR_STATUS_RETURNING, MSLOT_LINE, "Returning to the game…",             \
      "Возврат в игру…")                                                       \
    X(MSTR_STATUS_RETURN_FAILED, MSLOT_LINE,                                   \
      "The game did not start — press × on Launch to retry",                   \
      "Игра не запустилась — нажмите × на «Запустить» ещё раз")                \
    X(MSTR_STATUS_RECOVERY_FAILED, MSLOT_LINE,                                 \
      "Recovery before launch failed: %s",                                     \
      "Восстановление перед запуском не удалось: %s")                          \
    X(MSTR_STATUS_THEME_NOT_BUILT, MSLOT_LINE,                                 \
      "Game theme is not in this build — using Light",                         \
      "Темы «Игра» нет в этой сборке — используется «Светлая»")                \
    X(MSTR_STATUS_THEME_FAILED, MSLOT_LINE,                                    \
      "Game theme could not be read from the game files — using Light",        \
      "Тему «Игра» не удалось прочитать из файлов игры — используется «Светлая»") \
    X(MSTR_STATUS_FONT_FALLBACK, MSLOT_LINE,                                   \
      "Font files missing or damaged — built-in font in use",                  \
      "Font files missing or damaged — built-in font in use")                  \
    X(MSTR_STATUS_COUNTDOWN_CANCEL, MSLOT_LINE, "Auto-launch cancelled",       \
      "Автозапуск отменён")                                                    \
    /* hint bar */                                                             \
    X(MSTR_HINT_BACK, MSLOT_HINT, "Back", "Назад")                             \
    X(MSTR_HINT_SELECT, MSLOT_HINT, "Select", "Выбор")                         \
    X(MSTR_HINT_PAGE, MSLOT_HINT, "Page", "Страница")                          \
    X(MSTR_HINT_OPEN, MSLOT_HINT, "Open", "Открыть")                           \
    X(MSTR_HINT_LAUNCH, MSLOT_HINT, "Launch", "Запустить")                     \
    X(MSTR_HINT_FTP_START, MSLOT_HINT, "Start FTP", "Включить FTP")            \
    X(MSTR_HINT_FTP_STOP, MSLOT_HINT, "Stop FTP", "Выключить FTP")             \
    X(MSTR_HINT_STAY, MSLOT_HINT, "Stay here", "Остаться")                     \
    X(MSTR_HINT_ALL, MSLOT_HINT, "Enable all / disable all",                   \
      "Все вкл./выкл.")                                                        \
    X(MSTR_HINT_DETAILS, MSLOT_HINT, "Details", "Подробно")                    \
    X(MSTR_HINT_TOGGLE, MSLOT_HINT, "Toggle", "Переключить")                   \
    X(MSTR_HINT_CLOSE, MSLOT_HINT, "Close", "Закрыть")                         \
    X(MSTR_HINT_NO, MSLOT_HINT, "No", "Нет")                                   \
    X(MSTR_HINT_YES, MSLOT_HINT, "Yes", "Да")                                  \
    X(MSTR_HINT_ROW, MSLOT_HINT, "Row", "Строка")                              \
    X(MSTR_HINT_VALUE, MSLOT_HINT, "Value", "Значение")                        \
    X(MSTR_HINT_DEFAULTS, MSLOT_HINT, "Defaults", "Сброс")                     \
    X(MSTR_HINT_SAVE, MSLOT_HINT, "Save", "Сохранить")                         \
    X(MSTR_HINT_SAVE_N, MSLOT_HINT, "Save (%u)", "Сохранить (%u)")             \
    X(MSTR_HINT_RUN, MSLOT_HINT, "Run", "Выполнить")                           \
    X(MSTR_HINT_FILE, MSLOT_HINT, "File", "Файл")                              \
    X(MSTR_HINT_BACKUP, MSLOT_HINT, "Backup", "Копия")                         \
    X(MSTR_HINT_BACKUP_NOW, MSLOT_HINT, "Back up File %d",                     \
      "Копия файла %d")                                                        \
    X(MSTR_HINT_RESTORE, MSLOT_HINT, "Restore…", "Восстановить…")              \
    X(MSTR_HINT_SHOW_ALL, MSLOT_HINT, "All slots", "Все файлы")                \
    X(MSTR_HINT_SHOW_SLOT, MSLOT_HINT, "File %d only", "Только файл %d")       \
    /* MODS */                                                                 \
    X(MSTR_MODS_INFO, MSLOT_VALUE, "%u of %u enabled", "включено %u из %u")    \
    X(MSTR_MODS_INFO_SKIPPED, MSLOT_VALUE, "%u of %u enabled · %u skipped",    \
      "включено %u из %u · пропущено: %u")                                     \
    X(MSTR_MODS_EMPTY, MSLOT_BODY,                                             \
      "No mod directories in ux0:data/isaacr001/mods",                         \
      "В ux0:data/isaacr001/mods нет папок модов")                             \
    X(MSTR_MODS_SKIPPED_NOTE, MSLOT_LINE,                                      \
      "Skipped: %u — not a mod directory (a file, or a name with : \\ / or "   \
      "control characters)",                                                   \
      "Пропущено: %u — не папка мода (файл или имя с : \\ / или "              \
      "управляющими символами)")                                               \
    X(MSTR_MODS_SCAN_FAILED, MSLOT_LINE, "Mods could not be listed: %s",       \
      "Список модов не прочитан: %s")                                          \
    X(MSTR_MODS_ENABLED, MSLOT_LINE, "%s enabled — disable.it removed",        \
      "%s включён — disable.it удалён")                                        \
    X(MSTR_MODS_DISABLED, MSLOT_LINE, "%s disabled — disable.it written",      \
      "%s выключен — записан disable.it")                                      \
    X(MSTR_MODS_TOGGLE_FAILED, MSLOT_LINE, "%s not changed: %s",               \
      "%s не изменён: %s")                                                     \
    X(MSTR_MODS_ALL_ENABLE_Q, MSLOT_BOXTITLE, "Enable all mods (%u)?",         \
      "Включить все моды (%u)?")                                               \
    X(MSTR_MODS_ALL_DISABLE_Q, MSLOT_BOXTITLE, "Disable all mods (%u)?",       \
      "Выключить все моды (%u)?")                                              \
    X(MSTR_MODS_ALL_DETAIL, MSLOT_BOX,                                         \
      "Each mod gets or loses its disable.it file.",                           \
      "У каждого мода появится или исчезнет disable.it.")                      \
    X(MSTR_MODS_ALL_DONE, MSLOT_LINE, "Mods changed: %u",                      \
      "Изменено модов: %u")                                                    \
    X(MSTR_MODS_ALL_PARTIAL, MSLOT_LINE, "Mods changed: %u, failed: %u",       \
      "Изменено модов: %u, ошибок: %u")                                        \
    X(MSTR_MODS_DETAIL_DIR, MSLOT_BOX, "Directory: mods/%s", "Папка: mods/%s") \
    X(MSTR_MODS_DETAIL_VERSION, MSLOT_BOX, "Version: %s", "Версия: %s")        \
    X(MSTR_MODS_DETAIL_NOVERSION, MSLOT_BOX,                                   \
      "Version: not given in metadata.xml",                                    \
      "Версия: не указана в metadata.xml")                                     \
    X(MSTR_MODS_DETAIL_ON, MSLOT_BOX, "State: enabled (no disable.it)",        \
      "Состояние: включён (disable.it нет)")                                   \
    X(MSTR_MODS_DETAIL_OFF, MSLOT_BOX, "State: disabled (disable.it present)", \
      "Состояние: выключен (есть disable.it)")                                 \
    X(MSTR_MODS_DETAIL_META, MSLOT_BOX, "Name from metadata.xml",              \
      "Имя из metadata.xml")                                                   \
    X(MSTR_MODS_DETAIL_NOMETA, MSLOT_BOX,                                      \
      "No readable metadata.xml — directory name shown",                       \
      "metadata.xml не прочитан — показано имя папки")                         \
    /* SAVES */                                                                \
    X(MSTR_SAVES_FILE, MSLOT_LABEL, "File %d", "Файл %d")                      \
    X(MSTR_SAVES_NO_SAVE, MSLOT_WORD, "no save", "нет сохранения")             \
    X(MSTR_SAVES_RUN, MSLOT_WORD, "run in progress", "есть забег")             \
    X(MSTR_SAVES_NO_RUN, MSLOT_WORD, "no run in progress", "забега нет")       \
    X(MSTR_SAVES_BACKUPS_OF, MSLOT_VALUE, "Backups of File %d (%u)",           \
      "Копии файла %d (%u)")                                                   \
    X(MSTR_SAVES_BACKUPS_ALL, MSLOT_VALUE, "All backups (%u)",                 \
      "Все копии (%u)")                                                        \
    X(MSTR_SAVES_NO_BACKUPS, MSLOT_BODY,                                       \
      "No restorable backups yet — □ makes one",                               \
      "Копий пока нет — □ создаёт копию")                                      \
    X(MSTR_SAVES_SKIPPED, MSLOT_LINE,                                          \
      "Skipped: %u — names the manager does not restore",                      \
      "Пропущено: %u — имена, которые менеджер не восстанавливает")            \
    X(MSTR_SAVES_SCAN_FAILED, MSLOT_LINE, "Saves could not be listed: %s",     \
      "Сохранения не прочитаны: %s")                                           \
    X(MSTR_SAVES_KIND_MANAGER, MSLOT_WORD, "manager", "менеджер")              \
    X(MSTR_SAVES_KIND_GAME, MSLOT_WORD, "game", "игра")                        \
    X(MSTR_SAVES_KIND_SYNC, MSLOT_WORD, "PC sync", "с ПК")                     \
    X(MSTR_SAVES_WITH_RUN, MSLOT_WORD, " · with run", " · с забегом")          \
    X(MSTR_SAVES_BACKUP_DONE, MSLOT_LINE, "Backup of File %d written — %s",    \
      "Копия файла %d записана — %s")                                          \
    X(MSTR_SAVES_RUN_SAVED, MSLOT_WORD, " (run state saved)",                  \
      " (забег сохранён)")                                                     \
    X(MSTR_SAVES_RUN_NONE, MSLOT_WORD, " (no run state)", " (без забега)")     \
    X(MSTR_SAVES_BACKUP_FAILED, MSLOT_LINE, "Backup of File %d failed: %s",    \
      "Копия файла %d не создана: %s")                                         \
    X(MSTR_SAVES_MISSING, MSLOT_LINE, "File %d has no save to back up",        \
      "У файла %d нет сохранения")                                             \
    X(MSTR_SAVES_RESTORE_Q, MSLOT_BOXTITLE, "Restore into File %d?",           \
      "Восстановить в файл %d?")                                               \
    X(MSTR_SAVES_RESTORE_KEEP, MSLOT_BOX,                                      \
      "The current File %d is kept as a new backup.",                          \
      "Текущий файл %d сохранится как новая копия.")                           \
    X(MSTR_SAVES_RESTORE_RUN, MSLOT_BOX,                                       \
      "The run in progress is replaced too.",                                  \
      "Начатый забег тоже будет заменён.")                                     \
    X(MSTR_SAVES_RESTORE_KEEPRUN, MSLOT_BOX,                                   \
      "The run in progress stays as it is.",                                   \
      "Начатый забег останется как есть.")                                     \
    X(MSTR_SAVES_RESTORE_DONE, MSLOT_LINE, "File %d restored from %s",         \
      "Файл %d восстановлен из %s")                                            \
    X(MSTR_SAVES_RESTORE_FAILED, MSLOT_LINE,                                   \
      "Restore failed: %s — File %d unchanged",                                \
      "Восстановить не удалось: %s — файл %d не изменён")                      \
    X(MSTR_SAVES_RUN_RESTORED, MSLOT_WORD, " (run state restored)",            \
      " (забег восстановлен)")                                                 \
    X(MSTR_SAVES_RUN_KEPT, MSLOT_WORD, " (run state kept)",                    \
      " (забег не тронут)")                                                    \
    /* SETTINGS */                                                             \
    X(MSTR_SET_SECTION_GAME, MSLOT_LABEL, "GAME", "ИГРА")                      \
    X(MSTR_SET_SECTION_MANAGER, MSLOT_LABEL, "MANAGER", "МЕНЕДЖЕР")            \
    X(MSTR_SET_SECTION_FILES, MSLOT_LABEL, "FILES", "ФАЙЛЫ")                   \
    X(MSTR_SET_FPS, MSLOT_LABEL, "FPS lock", "Лимит FPS")                      \
    X(MSTR_SET_FPS_HELP, MSLOT_LINE,                                           \
      "The game simulates 30 updates/s; the lock caps the in-between frames shown", \
      "Игра считает 30 обновлений/с; лимит ограничивает показ промежуточных кадров") \
    X(MSTR_SET_FPS_HELP_UNEVEN, MSLOT_LINE,                                    \
      "45 and 40 alternate 16.7/33.3 ms frames; the game keeps simulating 30 updates/s", \
      "45 и 40 чередуют кадры 16,7/33,3 мс; игра по-прежнему считает 30 обновлений/с") \
    X(MSTR_SET_RENDER, MSLOT_LABEL, "Room render", "Отрисовка комнат")         \
    X(MSTR_SET_RENDER_FAST, MSLOT_WORD, "480x270 fast", "480x270 быстро")      \
    X(MSTR_SET_RENDER_SHARP, MSLOT_WORD, "960x540 sharp", "960x540 чётко")     \
    X(MSTR_SET_RENDER_HELP, MSLOT_LINE,                                        \
      "Rooms drawn at the 480x270 world raster and upscaled once; sharp = 2x, costs more", \
      "Комнаты рисуются в 480x270 и растягиваются; «чётко» = 2x, дороже")      \
    X(MSTR_SET_EID, MSLOT_LABEL, "EID mod", "Мод EID")                         \
    X(MSTR_SET_EID_HELP, MSLOT_LINE,                                           \
      "External Item Descriptions, mods/836319872 — same as its toggle on the Mods page", \
      "External Item Descriptions, mods/836319872 — то же, что переключатель на странице модов") \
    X(MSTR_SET_LANG, MSLOT_LABEL, "Language", "Язык")                          \
    X(MSTR_SET_LANG_AUTO, MSLOT_WORD, "Auto (%s)", "Авто (%s)")                \
    X(MSTR_SET_LANG_EN, MSLOT_WORD, "English", "English")                      \
    X(MSTR_SET_LANG_RU, MSLOT_WORD, "Русский", "Русский")                      \
    /* the language row under the 5x7 fallback face, which has no Cyrillic */ \
    X(MSTR_SET_LANG_RU_LATIN, MSLOT_WORD, "Russian", "Russian")                \
    X(MSTR_SET_LANG_HELP, MSLOT_LINE,                                          \
      "Auto follows the system language: Russian, or English for any other",   \
      "«Авто» — по языку системы: русский, иначе английский")                  \
    X(MSTR_SET_AUTOLAUNCH, MSLOT_LABEL, "Auto-launch", "Автозапуск")           \
    X(MSTR_SET_AUTOLAUNCH_SEC, MSLOT_WORD, "%u s", "%u с")                     \
    X(MSTR_SET_AUTOLAUNCH_HELP, MSLOT_LINE,                                    \
      "Start the game after the delay unless a button is pressed",             \
      "Запускать игру через это время, если не нажата кнопка")                 \
    X(MSTR_SET_THEME, MSLOT_LABEL, "Theme", "Тема")                            \
    X(MSTR_SET_THEME_GAME, MSLOT_WORD, "Game", "Игра")                         \
    X(MSTR_SET_THEME_DARK, MSLOT_WORD, "Dark", "Тёмная")                       \
    X(MSTR_SET_THEME_LIGHT, MSLOT_WORD, "Light", "Светлая")                    \
    X(MSTR_SET_THEME_HELP, MSLOT_LINE,                                         \
      "Game = the game's own menu art from your resources; Dark and Light always work", \
      "«Игра» — рисунки меню из ресурсов игры; «Тёмная» и «Светлая» работают всегда") \
    X(MSTR_SET_UNDO, MSLOT_LABEL, "Undo last save", "Отменить сохранение")     \
    X(MSTR_SET_UNDO_VALUE, MSLOT_WORD, "port-%04u.ini", "port-%04u.ini")       \
    X(MSTR_SET_UNDO_HELP, MSLOT_LINE,                                          \
      "Puts the previous port.ini back; press × on this row",                  \
      "Возвращает предыдущий port.ini; нажмите × на этой строке")              \
    X(MSTR_SET_DEFAULTS, MSLOT_LABEL, "Restore defaults", "Сбросить настройки") \
    X(MSTR_SET_DEFAULTS_VALUE, MSLOT_WORD, "removes port.ini",                 \
      "удаляет port.ini")                                                      \
    X(MSTR_SET_DEFAULTS_HELP, MSLOT_LINE,                                      \
      "port.ini is archived and removed; the game starts with its defaults",   \
      "port.ini архивируется и удаляется; игра стартует со значениями по умолчанию") \
    X(MSTR_SET_INFO_OK, MSLOT_VALUE, "port.ini ok", "port.ini ок")             \
    X(MSTR_SET_INFO_ABSENT, MSLOT_VALUE, "no port.ini — defaults",             \
      "port.ini нет — по умолчанию")                                           \
    X(MSTR_SET_INFO_IGNORED, MSLOT_VALUE, "port.ini ok · lines ignored: %u",   \
      "port.ini ок · пропущено строк: %u")                                     \
    X(MSTR_SET_INFO_INVALID, MSLOT_VALUE, "port.ini invalid — defaults shown", \
      "port.ini с ошибками — показаны значения по умолчанию")                  \
    X(MSTR_SET_INFO_IOERR, MSLOT_VALUE, "port.ini unreadable",                 \
      "port.ini не читается")                                                  \
    X(MSTR_SET_INFO_CHANGES, MSLOT_WORD, " · changes: %u", " · изменений: %u") \
    X(MSTR_SET_WAS, MSLOT_WORD, "was %s", "было %s")                           \
    X(MSTR_SET_CHANGED, MSLOT_LINE, "%s: %s — × saves", "%s: %s — × сохраняет") \
    X(MSTR_SET_SAVED, MSLOT_LINE, "Settings saved", "Настройки сохранены")     \
    X(MSTR_SET_SAVED_WARN, MSLOT_LINE, "Settings saved — %s",                  \
      "Настройки сохранены — %s")                                              \
    X(MSTR_SET_SAVED_IGNORED, MSLOT_LINE,                                      \
      "Settings saved — but the last game build ignored port.ini",             \
      "Настройки сохранены — но последняя сборка игры не читала port.ini")     \
    X(MSTR_SET_SAVE_FAILED, MSLOT_LINE, "Settings not saved: %s",              \
      "Настройки не сохранены: %s")                                            \
    X(MSTR_SET_INI_FAILED, MSLOT_LINE, "manager.ini not saved: %s",            \
      "manager.ini не сохранён: %s")                                           \
    X(MSTR_SET_EID_FAILED, MSLOT_LINE, "EID mod not changed: %s",              \
      "Мод EID не изменён: %s")                                                \
    X(MSTR_SET_NOTHING, MSLOT_LINE, "No changes to save",                      \
      "Нечего сохранять")                                                      \
    X(MSTR_SET_UNEVEN_Q, MSLOT_BOXTITLE, "Save FPS lock %u?",                  \
      "Сохранить лимит FPS %u?")                                               \
    X(MSTR_SET_UNEVEN_D, MSLOT_BOX,                                            \
      "45 and 40 give an uneven cadence (16.7/33.3 ms).",                      \
      "45 и 40 дают неровный темп (16,7/33,3 мс).")                            \
    X(MSTR_SET_DISCARD_Q, MSLOT_BOXTITLE, "Discard unsaved changes (%u)?",     \
      "Отменить несохранённые изменения (%u)?")                                \
    X(MSTR_SET_DISCARD_D, MSLOT_BOX, "Nothing has been written yet.",          \
      "Ещё ничего не записано.")                                               \
    X(MSTR_SET_DISCARDED, MSLOT_LINE, "Changes discarded", "Изменения отменены") \
    X(MSTR_SET_DEFAULTS_Q, MSLOT_BOXTITLE, "Restore defaults?",                \
      "Сбросить настройки?")                                                   \
    X(MSTR_SET_DEFAULTS_D, MSLOT_BOX,                                          \
      "port.ini moves to .manager-backups and is removed.",                    \
      "port.ini уйдёт в .manager-backups и будет удалён.")                     \
    X(MSTR_SET_DEFAULTS_DONE, MSLOT_LINE,                                      \
      "port.ini archived and removed — defaults in use",                       \
      "port.ini заархивирован и удалён — действуют значения по умолчанию")     \
    X(MSTR_SET_DEFAULTS_NOFILE, MSLOT_LINE,                                    \
      "No port.ini — defaults already in use",                                 \
      "port.ini нет — значения по умолчанию уже действуют")                    \
    X(MSTR_SET_DEFAULTS_FAILED, MSLOT_LINE, "Defaults not restored: %s",       \
      "Сброс не выполнен: %s")                                                 \
    X(MSTR_SET_UNDO_DONE, MSLOT_LINE, "Previous port.ini restored",            \
      "Предыдущий port.ini восстановлен")                                      \
    X(MSTR_SET_UNDO_FAILED, MSLOT_LINE, "Undo failed: %s",                     \
      "Отмена не удалась: %s")                                                 \
    X(MSTR_SET_IOERR, MSLOT_LINE, "port.ini could not be read — edits refused", \
      "port.ini не читается — изменения запрещены")                            \
    /* ACHIEVEMENTS */                                                         \
    X(MSTR_ACH_EMPTY_1, MSLOT_BODY, "No achievement data yet.",                \
      "Данных о достижениях пока нет.")                                        \
    X(MSTR_ACH_EMPTY_2, MSLOT_LINE,                                            \
      "Unlocks are recorded from the next game start on a build with achievements enabled.", \
      "Открытия записываются с ближайшего запуска сборки, в которой включены достижения.") \
    /* CONTROLS (the facts of the pre-UI2 CONTROLS page) */                    \
    X(MSTR_CTL_SECTION_GAME, MSLOT_LABEL, "IN GAME", "В ИГРЕ")                 \
    X(MSTR_CTL_SECTION_TOUCH, MSLOT_LABEL, "TOUCH SCREEN", "СЕНСОРНЫЙ ЭКРАН")  \
    X(MSTR_CTL_K_ACTIVE, MSLOT_CELL, "R · top-left touch",                    \
      "R · касание слева вверху")                                              \
    X(MSTR_CTL_A_ACTIVE, MSLOT_ACTION, "use active item", "активный предмет")   \
    X(MSTR_CTL_K_MAP, MSLOT_CELL, "top-right touch", "касание справа вверху") \
    X(MSTR_CTL_A_MAP, MSLOT_ACTION, "hold map", "карта (держать)")              \
    X(MSTR_CTL_K_POCKET, MSLOT_CELL, "bottom-right touch",                    \
      "касание справа внизу")                                                  \
    X(MSTR_CTL_A_POCKET, MSLOT_ACTION, "use pocket item", "карманный предмет")  \
    X(MSTR_CTL_K_SELECT_TAP, MSLOT_CELL, "SELECT tap", "SELECT коротко")      \
    X(MSTR_CTL_A_SELECT_TAP, MSLOT_ACTION, "swap held slots",                   \
      "поменять слоты")                                                        \
    X(MSTR_CTL_K_SELECT_HOLD, MSLOT_CELL, "SELECT hold", "SELECT держать")    \
    X(MSTR_CTL_A_SELECT_HOLD, MSLOT_ACTION, "drop pocket item or trinket",      \
      "бросить карманный предмет или брелок")                                  \
    X(MSTR_CTL_K_L, MSLOT_CELL, "L", "L")                                     \
    X(MSTR_CTL_A_L, MSLOT_ACTION, "bomb", "бомба")                              \
    X(MSTR_CTL_K_START, MSLOT_CELL, "START", "START")                         \
    X(MSTR_CTL_A_START, MSLOT_ACTION, "pause", "пауза")                         \
    X(MSTR_CTL_K_START_L, MSLOT_CELL, "START + L", "START + L")               \
    X(MSTR_CTL_A_START_L, MSLOT_ACTION, "pocket item", "карманный предмет")     \
    X(MSTR_CTL_K_START_R, MSLOT_CELL, "START + R", "START + R")               \
    X(MSTR_CTL_A_START_R, MSLOT_ACTION, "hold map", "карта")                    \
    X(MSTR_CTL_NOTE, MSLOT_LINE, "The game decides which slot a swap affects.", \
      "Какой слот меняется, решает игра.")                                     \
    X(MSTR_CTL_Z_ACTIVE, MSLOT_WORD, "use active", "активный")                 \
    X(MSTR_CTL_Z_MAP, MSLOT_WORD, "hold map", "карта")                         \
    X(MSTR_CTL_Z_POCKET, MSLOT_WORD, "use pocket", MSTR_RU_ZONE_POCKET)        \
    /* ABOUT */                                                                \
    X(MSTR_ABOUT_APP, MSLOT_BODY,                                              \
      "The Binding of Isaac: Repentance — PS Vita port",                       \
      "The Binding of Isaac: Repentance — порт для PS Vita")                   \
    X(MSTR_ABOUT_VERSION, MSLOT_LINE, "Version %s · build %s · manager ui2",   \
      "Версия %s · сборка %s · менеджер ui2")                                  \
    X(MSTR_ABOUT_RESOURCES, MSLOT_LABEL, "RESOURCES", "РЕСУРСЫ")               \
    X(MSTR_ABOUT_RES_OK, MSLOT_WORD, "ok", "ок")                               \
    X(MSTR_ABOUT_RES_DIFF, MSLOT_WORD, "present, not the stock size",          \
      "есть, размер не как у оригинала")                                       \
    X(MSTR_ABOUT_RES_MISSING, MSLOT_WORD, "missing", "нет")                    \
    X(MSTR_ABOUT_COUNTS, MSLOT_LINE,                                           \
      "mods: %u directories · saves: %u of 3 files present",                   \
      "моды: папок %u · сохранения: файлов %u из 3")                           \
    X(MSTR_ABOUT_LAST, MSLOT_LABEL, "LAST GAME START", "ПОСЛЕДНИЙ ЗАПУСК ИГРЫ") \
    X(MSTR_ABOUT_LICENCES, MSLOT_LABEL, "LICENCES", "ЛИЦЕНЗИИ")                \
    X(MSTR_ABOUT_LIC_FTP, MSLOT_LINE,                                          \
      "libftpvita (xerpi) — licenses/libftpvita.txt",                          \
      "libftpvita (xerpi) — licenses/libftpvita.txt")                          \
    X(MSTR_ABOUT_LIC_DEJAVU, MSLOT_LINE,                                       \
      "DejaVu fonts (Bitstream Vera licence) — licenses/dejavu.txt",           \
      "Шрифты DejaVu (лицензия Bitstream Vera) — licenses/dejavu.txt")         \
    X(MSTR_ABOUT_LIC_GAME, MSLOT_LINE,                                         \
      "Game content by Nicalis / Edmund McMillen; not included in this package", \
      "Содержимое игры — Nicalis / Edmund McMillen; в пакет не входит")        \
    X(MSTR_ABOUT_REPORT, MSLOT_LINE,                                           \
      "Reporting a problem: attach ux0:data/isaacr001/first-arm-fault.log and the game's log.txt", \
      "Сообщая о проблеме, приложите ux0:data/isaacr001/first-arm-fault.log и log.txt игры")

#if defined(MANAGER_TROPHY_IMPORT)
/* ACHIEVEMENTS: the trophy import row (knob ISAAC_VITA_MANAGER_TROPHY_IMPORT
 * -> MANAGER_TROPHY_IMPORT=1).  The contract is the game runtime's
 * (recomp/vita/TROPHIES.md, import request): the manager writes one digit
 * 1..3 into ux0:data/isaacr001/trophy-import.req, the game consumes it at
 * its next start and leaves one result line in trophy-import.res. */
#if defined(MANAGER_UI2_GAME_SKIN)
#define MANAGER_STRINGS_TROPHY_GAME(X)                                        \
    X(MSTR_TROPHY_HOME, MSLOT_VALUE, "local Vita trophies", "трофеи PS Vita")  \
    X(MSTR_TROPHY_SUMMARY, MSLOT_BODY, "Achievements become local Vita trophies.", \
      "Достижения выдаются как трофеи PS Vita.")                               \
    X(MSTR_TROPHY_DELIVERY, MSLOT_LINE,                                       \
      "Other sets arrive at the next launch; import earlier progress below.", \
      "Другие наборы — при следующем запуске; прежний прогресс — через импорт ниже.")
#else
#define MANAGER_STRINGS_TROPHY_GAME(X)
#endif
#define MANAGER_STRINGS_TROPHY(X)                                              \
    X(MSTR_TROPHY_SECTION, MSLOT_LABEL, "TROPHIES", "ТРОФЕИ")                  \
    X(MSTR_TROPHY_GRANT, MSLOT_LABEL, "Grant trophies from save",              \
      "Выдать трофеи из сейва")                                                \
    X(MSTR_TROPHY_GRANT_HELP, MSLOT_LINE,                                      \
      "At its next start the game turns this save's achievements into trophies; repeating is safe", \
      "При следующем запуске игра выдаст трофеи за достижения этого сейва; повтор безопасен") \
    X(MSTR_TROPHY_PENDING, MSLOT_LINE,                                         \
      "Waiting for the next game start: request for File %d",                  \
      "Ждёт следующего запуска игры: запрос для файла %d")                     \
    X(MSTR_TROPHY_PENDING_BAD, MSLOT_LINE,                                     \
      "trophy-import.req is waiting but is not a slot number 1-3",             \
      "trophy-import.req ждёт запуска, но это не номер слота 1-3")             \
    X(MSTR_TROPHY_NO_RESULT, MSLOT_LINE, "No import result yet",               \
      "Результата импорта пока нет")                                           \
    X(MSTR_TROPHY_RESULT_BAD, MSLOT_LINE,                                      \
      "trophy-import.res could not be read",                                   \
      "trophy-import.res не читается")                                         \
    X(MSTR_TROPHY_LAST, MSLOT_LINE,                                            \
      "Last import: File %u — %s · unlocked %u, already %u, failed %u",        \
      "Последний импорт: файл %u — %s · выдано %u, уже были %u, ошибок %u")    \
    X(MSTR_TROPHY_LAST_SHORT, MSLOT_LINE, "Last import: File %u — %s",         \
      "Последний импорт: файл %u — %s")                                        \
    X(MSTR_TROPHY_ST_DONE, MSLOT_WORD, "done", "готово")                       \
    X(MSTR_TROPHY_ST_PARTIAL, MSLOT_WORD, "partial, retried at next start",    \
      "частично, повтор при запуске")                                          \
    X(MSTR_TROPHY_ST_UNAVAILABLE, MSLOT_WORD, "trophies off in that build",    \
      "трофеи в той сборке выключены")                                         \
    X(MSTR_TROPHY_ST_NO_SAVE, MSLOT_WORD, "no save in that slot",              \
      "в слоте нет сейва")                                                     \
    X(MSTR_TROPHY_ST_BAD_REQUEST, MSLOT_WORD, "request file not valid",        \
      "файл запроса неверный")                                                 \
    X(MSTR_TROPHY_ST_UNREADABLE, MSLOT_VALUE, "save not readable (%s)",        \
      "сейв не читается (%s)")                                                 \
    X(MSTR_TROPHY_REQUESTED, MSLOT_LINE,                                       \
      "Request written — File %d trophies are granted at the next game start", \
      "Запрос записан — трофеи файла %d выдаются при следующем запуске игры")  \
    X(MSTR_TROPHY_REQUEST_FAILED, MSLOT_LINE, "Request not written: %s",       \
      "Запрос не записан: %s")                                                 \
    X(MSTR_HINT_REQUEST, MSLOT_HINT, "Request", "Запросить")                    \
    MANAGER_STRINGS_TROPHY_GAME(X)
#else
#define MANAGER_STRINGS_TROPHY(X)
#endif
#if defined(MANAGER_DEBUG_OVERLAY_ROW)
/* SETTINGS: the debug overlay row (knob ISAAC_VITA_MANAGER_DEBUG_OVERLAY_ROW
 * -> MANAGER_DEBUG_OVERLAY_ROW=1): port.ini debug_overlay = off|compact|full,
 * read by debug builds of the game only. */
#define MANAGER_STRINGS_OVERLAY(X)                                             \
    X(MSTR_SET_OVERLAY, MSLOT_LABEL, "Debug overlay", "Отладочный оверлей")    \
    X(MSTR_SET_OVERLAY_COMPACT, MSLOT_WORD, "compact", "компактный")           \
    X(MSTR_SET_OVERLAY_FULL, MSLOT_WORD, "full", "полный")                     \
    X(MSTR_SET_OVERLAY_HELP, MSLOT_LINE,                                       \
      "port.ini debug_overlay; only debug builds of the game draw it",         \
      "port.ini debug_overlay; рисуют только отладочные сборки игры")
#else
#define MANAGER_STRINGS_OVERLAY(X)
#endif
/* Strings of the opt-in manager rows, appended after the common table (an
 * OFF build sees exactly the common table). */
#if defined(MANAGER_TROPHY_IMPORT) || defined(MANAGER_DEBUG_OVERLAY_ROW)
#define MANAGER_STRINGS_KNOBS(X) MANAGER_STRINGS_TROPHY(X) MANAGER_STRINGS_OVERLAY(X)
#endif

#define MANAGER_STRING_ENUM(id, slot, en, ru) id,
enum manager_string_id {
#if defined(MANAGER_STRINGS_KNOBS)
    MANAGER_STRINGS(MANAGER_STRING_ENUM)
    MANAGER_STRINGS_KNOBS(MANAGER_STRING_ENUM) MSTR_COUNT
#else
    MANAGER_STRINGS(MANAGER_STRING_ENUM) MSTR_COUNT
#endif
};
#undef MANAGER_STRING_ENUM

/* The string in the language (English for an out-of-range language). */
const char *manager_str(enum manager_lang lang, enum manager_string_id id);
enum manager_string_slot manager_str_slot(enum manager_string_id id);
/* Face class and widest allowed width of a slot (the host test's limit). */
int manager_str_slot_width(enum manager_string_slot slot);
/* 0 title, 1 body, 2 small (enum manager_dl_face order). */
int manager_str_slot_face(enum manager_string_slot slot);

#endif /* MANAGER_UI2 */

#endif
