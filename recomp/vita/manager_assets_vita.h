/*
 * Manager UI2 font files (the DejaVu IMF1 atlases; knob
 * ISAAC_VITA_MANAGER_UI2 -> MANAGER_UI2=1).  SDK side of the font loading:
 * the three IMF1 atlases the VPK ships under app0:/manager/ are read into one
 * static pool and validated by manager_imf_parse(); if any of them is
 * missing, damaged or larger than the pool, all three faces fall back to the
 * pre-UI2 5x7 bitmap (scales 3 / 2 / 2) and the caller shows a status line
 * and forces English (the bitmap has no Cyrillic).
 */
#ifndef ISAAC_MANAGER_ASSETS_VITA_H
#define ISAAC_MANAGER_ASSETS_VITA_H

#if defined(MANAGER_UI2)

#include "manager_theme.h"

/* Paths inside the VPK (vita_create_vpk FILE pairs in CMakeLists.txt). */
#define MANAGER_ASSETS_TITLE_FONT "app0:/manager/font-title-24.imf"
#define MANAGER_ASSETS_BODY_FONT "app0:/manager/font-body-16.imf"
#define MANAGER_ASSETS_SMALL_FONT "app0:/manager/font-small-12.imf"
/* The committed atlases total 103,588 bytes (27,788 + 18,316 + 57,484). */
#define MANAGER_ASSETS_POOL_BYTES (128u * 1024u)

/* Fills fonts->face[] for MANAGER_FACE_TITLE/BODY/SMALL.  Returns 0 with the
 * DejaVu faces, 1 with the built-in fallback faces. */
int manager_assets_load_fonts(struct manager_theme_fonts *fonts);

#endif /* MANAGER_UI2 */

#endif
