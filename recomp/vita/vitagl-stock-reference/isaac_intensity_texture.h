/* Lossless large RGBA-page storage. Included only by optional patch 0037
 * (ISAAC_VITA_VITAGL_INTENSITY_TEXTURE; an earlier draft numbered 0036,
 * which had no glGenTextures reset: its exclusion was sticky per slot).
 * No guest/source pointer or RGBA shadow survives the synchronous upload.
 * Pixel identity, not a filename or presumed font owner, is the admission key.
 */
#ifndef ISAAC_INTENSITY_TEXTURE_H
#define ISAAC_INTENSITY_TEXTURE_H
#if defined(HAVE_TEX_CACHE) || defined(HAVE_UNPURE_TEXTURES)
#error "Isaac intensity storage has no file-cache or rebased-mip implementation"
#endif

enum { ISAAC_INTENSITY_COMPACT = 1, ISAAC_INTENSITY_EXCLUDED = 2 };
/* Per GL texture OBJECT, not per slot index: EXCLUDED (an earlier ordinary
 * definition, sub-image, non-filter/wrap parameter or FBO attachment of this
 * object) holds until glGenTextures hands the slot out as a NEW name, which
 * resets it (isaac_intensity_generated). The native allocator is unchanged. */
static uint8_t isaac_intensity_state[TEXTURES_NUM];
/* Once-only receipt: the first texture definition that reaches the admission
 * check proves the ON archive is the one running. Per-event lines follow. */
static uint8_t isaac_intensity_armed;
/* phase=skip / phase=reject lines are bounded per admitted shape and kind,
 * so a long session prints at most 4 * 2 * 32 of them. A policy bound, not a
 * measured constant; the line carries n=<k>/<cap> so the reader sees it. */
#define ISAAC_INTENSITY_LOG_CAP 32U
static uint8_t isaac_intensity_skip_lines[5], isaac_intensity_reject_lines[5];

/* Durable copy for native.log. sceClibPrintf reaches only the Cat-A-Log tty,
 * which can drop lines, so gl_vita_backend.c (same knob) reads these words
 * after its texture calls and logs "KAGE VITA INTENSITY:" when word 0 moved.
 * Word 0 counts the printed "Isaac intensity:" lines and word 1 describes the
 * last one (phase << 16 | shape << 8 | detail). Per-shape words (base +
 * shape 1..4) count EVERY event, also those past the line cap. Live pages and
 * bytes are the compact storage currently held. test_intensity_texture.py
 * pins this layout against the runtime reader and executes both. */
enum { ISAAC_INTENSITY_ARMED = 1, ISAAC_INTENSITY_UPLOAD, ISAAC_INTENSITY_UPLOAD_FAILED,
       ISAAC_INTENSITY_SKIP, ISAAC_INTENSITY_REJECT, ISAAC_INTENSITY_DEOPT,
       ISAAC_INTENSITY_DEOPT_FAILED, ISAAC_INTENSITY_REDEFINE_REJECTED };
enum { ISAAC_INTENSITY_W_GEN = 0, ISAAC_INTENSITY_W_LAST = 1,
       ISAAC_INTENSITY_W_UPLOAD_BASE = 1, ISAAC_INTENSITY_W_FAILED_BASE = 5,
       ISAAC_INTENSITY_W_SKIP_BASE = 9, ISAAC_INTENSITY_W_REJECT_BASE = 13,
       ISAAC_INTENSITY_W_U8 = 18, ISAAC_INTENSITY_W_A8L8 = 19,
       ISAAC_INTENSITY_W_DEOPT = 20, ISAAC_INTENSITY_W_DEOPT_FAILED = 21,
       ISAAC_INTENSITY_W_REDEFINE_REJECTED = 22, ISAAC_INTENSITY_W_LIVE_PAGES = 23,
       ISAAC_INTENSITY_W_LIVE_BYTES = 24, ISAAC_INTENSITY_WORDS = 25 };
/* Word-1 detail: the storage of an upload or the reason of a skip; the same
 * strings are printed on the tty lines. */
enum { ISAAC_INTENSITY_D_U8 = 1, ISAAC_INTENSITY_D_A8L8, ISAAC_INTENSITY_D_EXCLUDED,
       ISAAC_INTENSITY_D_ATTACHED, ISAAC_INTENSITY_D_OVERRIDDEN, ISAAC_INTENSITY_D_MIPS,
       ISAAC_INTENSITY_D_LAYOUT, ISAAC_INTENSITY_D_LIVE };
static const char *const isaac_intensity_details[] = { "", "U8_RRRR", "A8L8",
    "excluded", "attached", "overridden", "mips", "layout", "live" };
static uint32_t isaac_intensity_words[ISAAC_INTENSITY_WORDS];

static void isaac_intensity_count(unsigned word)
{
    if (isaac_intensity_words[word] != UINT32_MAX)
        ++isaac_intensity_words[word];
}

/* Exactly once per printed "Isaac intensity:" line. */
static void isaac_intensity_line(unsigned phase, unsigned shape, unsigned detail)
{
    isaac_intensity_count(ISAAC_INTENSITY_W_GEN);
    isaac_intensity_words[ISAAC_INTENSITY_W_LAST] =
        (uint32_t)phase << 16 | (uint32_t)shape << 8 | (uint32_t)detail;
}

/* Consumer-declared, like the other Isaac endpoints; not part of vitaGL.h.
 * Copies the words only when word 0 differs from `seen`; returns word 0. */
uint32_t vglIsaacIntensitySnapshot(uint32_t seen, uint32_t out[ISAAC_INTENSITY_WORDS])
{
    const uint32_t gen = isaac_intensity_words[ISAAC_INTENSITY_W_GEN];
    if (gen != seen)
        for (unsigned i = 0; i < ISAAC_INTENSITY_WORDS; ++i)
            out[i] = isaac_intensity_words[i];
    return gen;
}

/* 1..4 for the four admitted shapes, 0 for any other. Shapes are copied from
 * the eight-page ZH census (Chinese font pages: 512x512,
 * 2048x1024 x2, 2048x2048 x4, 4096x2048), but do NOT identify a font. */
static unsigned isaac_intensity_shape(GLsizei width, GLsizei height)
{
    if (width == 512 && height == 512)
        return 1;
    if (width == 2048 && height == 1024)
        return 2;
    if (width == 2048 && height == 2048)
        return 3;
    if (width == 4096 && height == 2048)
        return 4;
    return 0;
}

/* A compact page's storage is width * height * (1 | 2) bytes: the admitted
 * widths are multiples of 8, so the linear rows carry no padding. */
static void isaac_intensity_live(texture *tex, int add)
{
    uint32_t width, height;
    vglGetTexSizes(&tex->gxm_tex, &width, &height);
    const uint32_t bytes = width * height *
        (tex->format == SCE_GXM_TEXTURE_FORMAT_A8L8 ? 2U : 1U);
    if (add) {
        ++isaac_intensity_words[ISAAC_INTENSITY_W_LIVE_PAGES];
        isaac_intensity_words[ISAAC_INTENSITY_W_LIVE_BYTES] += bytes;
    } else {
        --isaac_intensity_words[ISAAC_INTENSITY_W_LIVE_PAGES];
        isaac_intensity_words[ISAAC_INTENSITY_W_LIVE_BYTES] -= bytes;
    }
}

/* Called from glGenTextures at the stock TEX_UNUSED -> TEX_UNINITIALIZED
 * transition, next to its own field resets. A new name has no history: stock
 * hands out the LOWEST unused slot, so a slot-sticky mark refused almost every
 * page loaded after a mass delete (the Options language switch). Safe: a slot
 * is TEX_UNUSED only after gpu_free_texture (or a delete that never defined
 * it), and no framebuffer points at it then. Stock glDeleteTextures of an
 * attached object frees it AT ONCE when its single reference is the active
 * read/write FBO (textures.c glDeleteTextures: gpu_free_texture, fb->tex =
 * NULL; ref_counter stays 1 until this generator zeroes it); any other
 * attached object stays TEX_VALID + dirty until the FBO drops it (shared.h
 * _glFramebufferTexture2D / framebuffers.c glDeleteFramebuffers free it at
 * ref_counter 0 and clear fb->tex). A COMPACT bit cannot be set here: every
 * free goes through Release first. */
static inline void isaac_intensity_generated(GLuint slot)
{
    isaac_intensity_state[slot] = 0;
}

static uint8_t *isaac_intensity_slot(texture *tex)
{
    uintptr_t base = (uintptr_t)texture_slots, address = (uintptr_t)tex;
    if (address < base || address - base >= sizeof texture_slots ||
            (address - base) % sizeof(texture))
        return NULL;
    return &isaac_intensity_state[(address - base) / sizeof(texture)];
}

void isaacIntensityRelease(texture *tex)
{
    uint8_t *state = isaac_intensity_slot(tex);
    if (state && (*state & ISAAC_INTENSITY_COMPACT)) {
        isaac_intensity_live(tex, 0); /* still the compact descriptor */
        *state &= (uint8_t)~ISAAC_INTENSITY_COMPACT;
    }
}

/* Allocate/validate BEFORE retiring a live U8/A8L8 image. Copy its actual GPU
 * allocation, not a retained source, then publish the new descriptor once.
 * The common retirement keeps the old last_frame (including deferred free).
 * On failure no data/descriptor/owner state changes and the caller must stop.
 */
GLenum isaacIntensityPrepare(texture *tex, int exclude)
{
    uint8_t *state = isaac_intensity_slot(tex);
    if (!state)
        return GL_NO_ERROR;
    if (*state & ISAAC_INTENSITY_COMPACT) {
        uint32_t width, height;
        vglGetTexSizes(&tex->gxm_tex, &width, &height);
        if (tex->status != TEX_VALID || !tex->data || tex->palette_data ||
                tex->ref_counter || tex->overridden || tex->mip_count != 1 ||
                (tex->format != SCE_GXM_TEXTURE_FORMAT_U8_RRRR &&
                 tex->format != SCE_GXM_TEXTURE_FORMAT_A8L8) ||
                vglGetTexFormat(&tex->gxm_tex) != tex->format ||
                vglGetTexData(&tex->gxm_tex) != tex->data ||
                sceGxmTextureGetType(&tex->gxm_tex) != SCE_GXM_TEXTURE_LINEAR ||
                !width || !height || width > GXM_TEX_MAX_SIZE ||
                height > GXM_TEX_MAX_SIZE)
            return GL_INVALID_OPERATION;
        uint32_t stride = VGL_ALIGN(width, 8);
        uint32_t pixels = stride * height; /* GXM maximum bounds this product. */
        uint32_t *rgba = gpu_alloc_mapped_for_gpu((size_t)pixels * 4U);
        if (!rgba) {
            isaac_intensity_count(ISAAC_INTENSITY_W_DEOPT_FAILED);
            isaac_intensity_line(ISAAC_INTENSITY_DEOPT_FAILED,
                isaac_intensity_shape((GLsizei)width, (GLsizei)height), 0);
            sceClibPrintf("Isaac intensity: phase=deopt-failed rgba=%u old=compact-kept\n",
                pixels * 4U);
            return GL_OUT_OF_MEMORY;
        }
        texture staged = *tex;
        /* Preserve dimensions, wrap/filter/LOD and the native no-mips state. */
        if (sceGxmTextureSetData(&staged.gxm_tex, rgba) != 0 ||
                sceGxmTextureSetFormat(&staged.gxm_tex,
                    SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR) != 0) {
            vgl_free(rgba); /* Unpublished, never submitted to GXM. */
            return GL_INVALID_OPERATION;
        }
        const uint8_t *compact = (const uint8_t *)tex->data;
        const int luminance_alpha = tex->format == SCE_GXM_TEXTURE_FORMAT_A8L8;
        for (uint32_t i = 0; i < pixels; ++i) {
            if (luminance_alpha)
                rgba[i] = read_la88(compact + i * 2U);
            else {
                uint32_t value = compact[i];
                rgba[i] = value | (value << 8) | (value << 16) | (value << 24);
            }
        }
        staged.data = rgba;
        staged.format = SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR;
        staged.write_cb = write_rgba8888;
#ifndef TEXTURES_SPEEDHACK
        staged.last_frame = OBJ_NOT_USED;
#endif
        gpu_free_texture_data(tex);
        *tex = staged;
        isaac_intensity_count(ISAAC_INTENSITY_W_DEOPT);
        isaac_intensity_line(ISAAC_INTENSITY_DEOPT,
            isaac_intensity_shape((GLsizei)width, (GLsizei)height), 0);
        sceClibPrintf("Isaac intensity: phase=deopt size=%ux%u rgba=%u shadow=0\n",
            width, height, pixels * 4U);
    }
    if (exclude)
        *state |= ISAAC_INTENSITY_EXCLUDED;
    return GL_NO_ERROR;
}

static GLenum isaac_intensity_parameter(texture *tex, GLenum pname, GLint param)
{
    if ((pname == GL_TEXTURE_MIN_FILTER || pname == GL_TEXTURE_MAG_FILTER) &&
            (param == GL_NEAREST || param == GL_LINEAR))
        return GL_NO_ERROR;
    if (pname == GL_TEXTURE_WRAP_S || pname == GL_TEXTURE_WRAP_T)
        return GL_NO_ERROR;
    return isaacIntensityPrepare(tex, 1);
}

/* Escape via a complete base-level RGBA redefinition is transactional too.
 * Calling stock gpu_alloc_texture after deopt would free the old data BEFORE
 * discovering OOM, leaving a stale native descriptor. Unsupported definitions
 * (notably later mip levels) are explicitly rejected while still compact:
 * their stock allocator is not a checked transaction. Ordinary objects never
 * enter this helper and retain their original behavior.
 */
static int isaac_intensity_redefine(texture *tex, GLint level,
    GLint internal_format, GLsizei width, GLsizei height,
    GLenum format, GLenum type, const void *data, GLint row_length)
{
    uint8_t *state = isaac_intensity_slot(tex);
    if (!state || !(*state & ISAAC_INTENSITY_COMPACT))
        return 0;
    if (level != 0 || internal_format != GL_RGBA || format != GL_RGBA ||
            type != GL_UNSIGNED_BYTE || width <= 0 || height <= 0 ||
            width > GXM_TEX_MAX_SIZE || height > GXM_TEX_MAX_SIZE ||
            (row_length != 0 && row_length != width) ||
            tex->status != TEX_VALID || !tex->data || tex->palette_data ||
            tex->ref_counter || tex->overridden) {
        vgl_error = GL_INVALID_OPERATION;
        uint32_t old_width, old_height;
        vglGetTexSizes(&tex->gxm_tex, &old_width, &old_height);
        isaac_intensity_count(ISAAC_INTENSITY_W_REDEFINE_REJECTED);
        isaac_intensity_line(ISAAC_INTENSITY_REDEFINE_REJECTED,
            isaac_intensity_shape((GLsizei)old_width, (GLsizei)old_height), 0);
        sceClibPrintf("Isaac intensity: phase=redefine-rejected level=%d old=compact-kept\n", level);
        return -1;
    }
    uint32_t stride = VGL_ALIGN((uint32_t)width, 8);
    size_t bytes = (size_t)stride * (uint32_t)height * 4U;
    uint8_t *rgba = gpu_alloc_mapped_for_gpu(bytes);
    if (!rgba) {
        vgl_error = GL_OUT_OF_MEMORY;
        return -1;
    }
    texture staged = *tex;
    staged.format = SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR;
    if (sceGxmTextureInitLinear(&staged.gxm_tex, rgba, staged.format,
            (uint32_t)width, (uint32_t)height, 1) != 0 ||
            sceGxmTextureSetUAddrMode(&staged.gxm_tex, tex->u_mode) != 0 ||
            sceGxmTextureSetVAddrMode(&staged.gxm_tex, tex->v_mode) != 0 ||
            sceGxmTextureSetMinFilter(&staged.gxm_tex, tex->min_filter) != 0 ||
            sceGxmTextureSetMagFilter(&staged.gxm_tex, tex->mag_filter) != 0 ||
            sceGxmTextureSetMipFilter(&staged.gxm_tex, tex->mip_filter) != 0 ||
            sceGxmTextureSetLodBias(&staged.gxm_tex, tex->lod_bias) != 0 ||
            sceGxmTextureSetMipmapCount(&staged.gxm_tex, 0) != 0) {
        vgl_free(rgba);
        vgl_error = GL_INVALID_OPERATION;
        return -1;
    }
    /* Zero padding is deterministic but never reads beyond the source rows. */
    vgl_memset(rgba, 0, bytes);
    if (data)
        for (uint32_t y = 0; y < (uint32_t)height; ++y)
            vgl_fast_memcpy(rgba + (size_t)y * stride * 4U,
                (const uint8_t *)data + (size_t)y * (uint32_t)width * 4U,
                (size_t)(uint32_t)width * 4U);
    staged.data = rgba;
    staged.write_cb = write_rgba8888;
    staged.mip_count = 1;
#ifndef TEXTURES_SPEEDHACK
    staged.last_frame = OBJ_NOT_USED;
#endif
    gpu_free_texture_data(tex);
    *tex = staged;
    *state |= ISAAC_INTENSITY_EXCLUDED;
    return 1;
}

/* 0: exact original native upload; 1: handled; -1: rejected transaction.
 * Every input byte, including RGB under alpha=0, must satisfy R=G=B.
 * Pinned read_rgba8888 + write_r8 extracts RED, not alpha. They are equal ONLY
 * for the all-equal U8 path. Otherwise pinned write_ra88 preserves red/luma
 * AND independent alpha as bytes {L,A}; A8L8 is SDK U8U8_GRRR, not L8A8.
 * Gamma is never changed/approximated. Deopt uses the actual read_la88 callback.
 */
static int isaac_intensity_try_upload(texture *tex, GLint level,
    GLint internal_format, GLsizei width, GLsizei height,
    GLenum format, GLenum type, const void *data, GLint row_length)
{
    if (!isaac_intensity_armed) {
        isaac_intensity_armed = 1;
        isaac_intensity_line(ISAAC_INTENSITY_ARMED, 0, 0);
        sceClibPrintf("Isaac intensity: armed patch=0037 shapes=512x512,2048x1024,2048x2048,4096x2048 storage=U8_RRRR|A8L8 slots=%u shadow=0\n",
            (unsigned)TEXTURES_NUM);
    }
    uint8_t *state = isaac_intensity_slot(tex);
    const unsigned shape = isaac_intensity_shape(width, height);
    /* Not an admitted request at all: silent, exactly the stock path. */
    if (!state || !shape || level != 0 ||
            internal_format != GL_RGBA || format != GL_RGBA ||
            type != GL_UNSIGNED_BYTE || !data ||
            (row_length != 0 && row_length != width))
        return 0;
    /* An admitted request refused by this object's own state: stock path,
     * with a bounded receipt saying why (same predicate as before, split). */
    const unsigned skip = (*state & ISAAC_INTENSITY_EXCLUDED) ? ISAAC_INTENSITY_D_EXCLUDED :
        tex->ref_counter ? ISAAC_INTENSITY_D_ATTACHED :
        tex->overridden ? ISAAC_INTENSITY_D_OVERRIDDEN :
        (tex->use_mips || tex->mip_filter != SCE_GXM_TEXTURE_MIP_FILTER_DISABLED) ? ISAAC_INTENSITY_D_MIPS :
        (tex->faces_counter || tex->palette_data) ? ISAAC_INTENSITY_D_LAYOUT :
        (!(*state & ISAAC_INTENSITY_COMPACT) &&
            (tex->status != TEX_UNINITIALIZED || tex->data)) ? ISAAC_INTENSITY_D_LIVE : 0U;
    if (skip) {
        isaac_intensity_count(ISAAC_INTENSITY_W_SKIP_BASE + shape);
        if (isaac_intensity_skip_lines[shape] < ISAAC_INTENSITY_LOG_CAP) {
            ++isaac_intensity_skip_lines[shape];
            isaac_intensity_line(ISAAC_INTENSITY_SKIP, shape, skip);
            sceClibPrintf("Isaac intensity: phase=skip size=%ux%u slot=%u reason=%s n=%u/%u\n",
                (unsigned)width, (unsigned)height,
                (unsigned)(state - isaac_intensity_state), isaac_intensity_details[skip],
                (unsigned)isaac_intensity_skip_lines[shape], ISAAC_INTENSITY_LOG_CAP);
        }
        return 0;
    }
    uint32_t pixels = (uint32_t)width * (uint32_t)height;
    const uint8_t *rgba = (const uint8_t *)data;
    uint32_t storage_bpp = 1U;
    for (uint32_t i = 0; i < pixels; ++i) {
        const uint8_t *p = rgba + i * 4U;
        if (p[0] != p[1] || p[0] != p[2]) {
            isaac_intensity_count(ISAAC_INTENSITY_W_REJECT_BASE + shape);
            if (isaac_intensity_reject_lines[shape] < ISAAC_INTENSITY_LOG_CAP) {
                ++isaac_intensity_reject_lines[shape];
                isaac_intensity_line(ISAAC_INTENSITY_REJECT, shape, 0);
                sceClibPrintf("Isaac intensity: phase=reject size=%ux%u pixel=%u rgba=%u,%u,%u,%u n=%u/%u\n",
                    (unsigned)width, (unsigned)height, i, p[0], p[1], p[2], p[3],
                    (unsigned)isaac_intensity_reject_lines[shape], ISAAC_INTENSITY_LOG_CAP);
            }
            return 0;
        }
        if (p[0] != p[3])
            storage_bpp = 2U;
    }
    const uint32_t storage_bytes = pixels * storage_bpp;
    uint8_t *compact = gpu_alloc_mapped_for_gpu(storage_bytes);
    if (!compact) {
        vgl_error = GL_OUT_OF_MEMORY;
        isaac_intensity_count(ISAAC_INTENSITY_W_FAILED_BASE + shape);
        isaac_intensity_line(ISAAC_INTENSITY_UPLOAD_FAILED, shape, 0);
        sceClibPrintf("Isaac intensity: phase=upload-failed size=%ux%u compact=%u old=kept\n",
            (unsigned)width, (unsigned)height, storage_bytes);
        return -1;
    }
    texture staged = *tex;
    staged.format = storage_bpp == 1U ? SCE_GXM_TEXTURE_FORMAT_U8_RRRR :
        SCE_GXM_TEXTURE_FORMAT_A8L8;
    if (sceGxmTextureInitLinear(&staged.gxm_tex, compact, staged.format,
            (uint32_t)width, (uint32_t)height, 1) != 0 ||
            sceGxmTextureSetUAddrMode(&staged.gxm_tex, tex->u_mode) != 0 ||
            sceGxmTextureSetVAddrMode(&staged.gxm_tex, tex->v_mode) != 0 ||
            sceGxmTextureSetMinFilter(&staged.gxm_tex, tex->min_filter) != 0 ||
            sceGxmTextureSetMagFilter(&staged.gxm_tex, tex->mag_filter) != 0 ||
            sceGxmTextureSetMipFilter(&staged.gxm_tex, tex->mip_filter) != 0 ||
            sceGxmTextureSetLodBias(&staged.gxm_tex, tex->lod_bias) != 0 ||
            sceGxmTextureSetMipmapCount(&staged.gxm_tex, 0) != 0) {
        vgl_free(compact);
        vgl_error = GL_INVALID_OPERATION;
        return -1;
    }
    staged.write_cb = storage_bpp == 1U ? write_r8 : write_ra88;
    if (storage_bpp == 1U) {
        /* Keep the reviewed U8 callback loop, without a new per-pixel indirect
         * call in the previously accepted path. */
        for (uint32_t i = 0; i < pixels; ++i)
            write_r8(compact + i, read_rgba8888(rgba + i * 4U));
    } else {
        for (uint32_t i = 0; i < pixels; ++i)
            write_ra88(compact + i * 2U, read_rgba8888(rgba + i * 4U));
    }
    staged.data = compact;
    staged.mip_count = 1;
    staged.status = TEX_VALID;
#ifndef TEXTURES_SPEEDHACK
    staged.last_frame = OBJ_NOT_USED;
#endif
    if (tex->status == TEX_VALID)
        gpu_free_texture_data(tex);
    *tex = staged;
    *state |= ISAAC_INTENSITY_COMPACT;
    isaac_intensity_live(tex, 1);
    const unsigned storage = storage_bpp == 1U ? ISAAC_INTENSITY_D_U8 : ISAAC_INTENSITY_D_A8L8;
    isaac_intensity_count(ISAAC_INTENSITY_W_UPLOAD_BASE + shape);
    isaac_intensity_count(storage_bpp == 1U ? ISAAC_INTENSITY_W_U8 : ISAAC_INTENSITY_W_A8L8);
    isaac_intensity_line(ISAAC_INTENSITY_UPLOAD, shape, storage);
    sceClibPrintf("Isaac intensity: phase=upload size=%ux%u rgba=%u storage=%s compact=%u saved=%u status=valid shadow=0\n",
        (unsigned)width, (unsigned)height, pixels * 4U,
        isaac_intensity_details[storage], storage_bytes,
        pixels * 4U - storage_bytes);
    return 1;
}
#endif
