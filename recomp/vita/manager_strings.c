/*
 * Manager UI2 string tables (see manager_strings.h).  Both columns and the
 * slot of every string are generated from the one MANAGER_STRINGS list.
 */
#if defined(MANAGER_UI2)

#include "manager_strings.h"

#include <stddef.h>

#define MANAGER_STRING_EN(id, slot, en, ru) en,
#define MANAGER_STRING_RU(id, slot, en, ru) ru,
#define MANAGER_STRING_SLOT(id, slot, en, ru) (unsigned char)(slot),

#if defined(MANAGER_STRINGS_KNOBS)
static const char *const manager_strings_en[MSTR_COUNT] = {
    MANAGER_STRINGS(MANAGER_STRING_EN)
    MANAGER_STRINGS_KNOBS(MANAGER_STRING_EN)
};
static const char *const manager_strings_ru[MSTR_COUNT] = {
    MANAGER_STRINGS(MANAGER_STRING_RU)
    MANAGER_STRINGS_KNOBS(MANAGER_STRING_RU)
};
static const unsigned char manager_string_slots[MSTR_COUNT] = {
    MANAGER_STRINGS(MANAGER_STRING_SLOT)
    MANAGER_STRINGS_KNOBS(MANAGER_STRING_SLOT)
};
#else
static const char *const manager_strings_en[MSTR_COUNT] = {
    MANAGER_STRINGS(MANAGER_STRING_EN)
};
static const char *const manager_strings_ru[MSTR_COUNT] = {
    MANAGER_STRINGS(MANAGER_STRING_RU)
};
static const unsigned char manager_string_slots[MSTR_COUNT] = {
    MANAGER_STRINGS(MANAGER_STRING_SLOT)
};
#endif

#undef MANAGER_STRING_EN
#undef MANAGER_STRING_RU
#undef MANAGER_STRING_SLOT

/* Widest allowed text per slot, in pixels of the slot's face.  The numbers
 * come from the page geometry of manager_ui.c (mock C coordinates):
 * tabs 110 (seven tabs + gaps between x=350 and 928), title 270 (x=68 to
 * the first tab at 350), labels 480, values 420 (right-aligned at x=900),
 * hint labels 280, lines 880 (x=32..912 / 48..928), words 220 (spliced
 * into values), box lines 432 (480 px box, 24 px padding), CONTROLS key
 * column 236 (x=48..284) and action column 350 (x=300..650). */
static const short manager_slot_width[MSLOT_COUNT] = {
    110, /* MSLOT_TAB */
    270, /* MSLOT_TITLE */
    480, /* MSLOT_LABEL */
    420, /* MSLOT_VALUE */
    280, /* MSLOT_HINT */
    880, /* MSLOT_LINE */
    880, /* MSLOT_BODY */
    220, /* MSLOT_WORD */
    432, /* MSLOT_BOX */
    432, /* MSLOT_BOXTITLE */
    236, /* MSLOT_CELL */
    350, /* MSLOT_ACTION */
};

/* 0 title, 1 body, 2 small (enum manager_face_role order). */
static const unsigned char manager_slot_face[MSLOT_COUNT] = {
    2, /* MSLOT_TAB */
    0, /* MSLOT_TITLE */
    1, /* MSLOT_LABEL */
    2, /* MSLOT_VALUE */
    1, /* MSLOT_HINT */
    2, /* MSLOT_LINE */
    1, /* MSLOT_BODY */
    2, /* MSLOT_WORD */
    2, /* MSLOT_BOX */
    1, /* MSLOT_BOXTITLE */
    1, /* MSLOT_CELL */
    1, /* MSLOT_ACTION */
};

const char *manager_str(enum manager_lang lang, enum manager_string_id id)
{
    if ((unsigned int)id >= (unsigned int)MSTR_COUNT)
        return "";
    if (lang == MANAGER_LANG_RU)
        return manager_strings_ru[id];
    return manager_strings_en[id];
}

enum manager_string_slot manager_str_slot(enum manager_string_id id)
{
    if ((unsigned int)id >= (unsigned int)MSTR_COUNT)
        return MSLOT_LINE;
    return (enum manager_string_slot)manager_string_slots[id];
}

int manager_str_slot_width(enum manager_string_slot slot)
{
    if ((unsigned int)slot >= (unsigned int)MSLOT_COUNT)
        return 0;
    return manager_slot_width[slot];
}

int manager_str_slot_face(enum manager_string_slot slot)
{
    if ((unsigned int)slot >= (unsigned int)MSLOT_COUNT)
        return 2;
    return manager_slot_face[slot];
}

#endif /* MANAGER_UI2 */
