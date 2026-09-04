#include "kage_vita_world_seam_diag.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define LOG_CAPACITY 32768u

static char s_log[LOG_CAPACITY];
static size_t s_log_size;
static size_t s_max_line_size;
static unsigned s_checks;

#define CHECK(condition)                                                     \
    do {                                                                     \
        ++s_checks;                                                          \
        if (!(condition)) {                                                  \
            fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition);   \
            return 1;                                                        \
        }                                                                    \
    } while (0)

int kage_vita_world_seam_diag_oracle_log(const char *format, ...)
{
    va_list args;
    int written;
    if (s_log_size >= sizeof s_log)
        return -1;
    va_start(args, format);
    written = vsnprintf(
        s_log + s_log_size, sizeof s_log - s_log_size, format, args);
    va_end(args);
    if (written < 0 || (size_t)written >= sizeof s_log - s_log_size)
        return -1;
    if ((size_t)written > s_max_line_size)
        s_max_line_size = (size_t)written;
    s_log_size += (size_t)written;
    if (s_log_size + 1u >= sizeof s_log)
        return -1;
    s_log[s_log_size++] = '\n';
    s_log[s_log_size] = '\0';
    return written;
}

static unsigned count_lines(void)
{
    unsigned result = 0u;
    size_t index;
    for (index = 0u; index < s_log_size; ++index) {
        if (s_log[index] == '\n')
            ++result;
    }
    return result;
}

static void setup_state(void)
{
    kage_vita_world_seam_diag_note_active_texture(0x84c0u);
    kage_vita_world_seam_diag_note_gen_texture(17u);
    kage_vita_world_seam_diag_note_bind_texture(0x0de1u, 17u);
    kage_vita_world_seam_diag_note_tex_image(
        0x0de1u, 0, 432, 240);
    kage_vita_world_seam_diag_note_gen_texture(23u);
    kage_vita_world_seam_diag_note_bind_texture(0x0de1u, 23u);
    kage_vita_world_seam_diag_note_tex_image(
        0x0de1u, 0, 960, 540);
    kage_vita_world_seam_diag_note_bind_framebuffer(0x8d40u, 5u);
    kage_vita_world_seam_diag_note_framebuffer_texture(
        0x8d40u, 0x8ce0u, 0x0de1u, 23u, 0);
    kage_vita_world_seam_diag_note_bind_texture(0x0de1u, 17u);
    kage_vita_world_seam_diag_note_use_program(4u);
    kage_vita_world_seam_diag_note_viewport(0, 0, 960, 540);
    kage_vita_world_seam_diag_note_clear_color(0.0f, 0.0f, 0.0f, 1.0f);
}

static void one_receipt(uint32_t stage, uint32_t stage_type)
{
    kage_vita_world_seam_diag_begin(stage, stage_type);
    kage_vita_world_seam_diag_note_clear(0x00004100u);
    kage_vita_world_seam_diag_note_draw();
    kage_vita_world_seam_diag_post_room();

    kage_vita_world_seam_diag_note_bind_texture(0x0de1u, 23u);
    kage_vita_world_seam_diag_note_use_program(7u);
    kage_vita_world_seam_diag_note_draw();
    kage_vita_world_seam_diag_post_lua();

    kage_vita_world_seam_diag_note_bind_framebuffer(0x8d40u, 0u);
    kage_vita_world_seam_diag_note_bind_texture(0x0de1u, 17u);
    kage_vita_world_seam_diag_note_use_program(9u);
    kage_vita_world_seam_diag_note_draw();
    kage_vita_world_seam_diag_pre_hud();
}

int main(void)
{
    uint32_t index;

    kage_vita_world_seam_diag_reset();
    setup_state();
    one_receipt(1u, 0u);
    CHECK(count_lines() == 4u);
    CHECK(strstr(s_log,
        "q=1 k=C s=1 t=0 p=begin") != NULL);
    CHECK(strstr(s_log,
        "p=room n=1/1/0/0/0/0/0") != NULL);
    CHECK(strstr(s_log,
        "cur=5/17/4/11/1/1b000f0/0/3c0021c") != NULL);
    CHECK(strstr(s_log,
        "clr=5/4100/3315f515") != NULL);
    CHECK(strstr(s_log,
        "p=lua n=1/0/0/1/1/0/0") != NULL);
    CHECK(strstr(s_log,
        "lst=5/17/7/17/1/3c0021c") != NULL);
    CHECK(strstr(s_log,
        "p=late n=1/0/1/1/1/0/0") != NULL);
    CHECK(strstr(s_log,
        "lst=0/0/9/11/1/1b000f0") != NULL);

    one_receipt(2u, 0u);
    CHECK(count_lines() == 4u);
    one_receipt(8u, 1u);
    one_receipt(8u, 1u);
    CHECK(count_lines() == 12u);
    CHECK(strstr(s_log,
        "q=2 k=T s=8 t=1 p=begin") != NULL);
    CHECK(strstr(s_log,
        "q=3 k=T s=8 t=1 p=late") != NULL);
    one_receipt(8u, 1u);
    CHECK(count_lines() == 12u);

    /* Maximal legal scalar state proves the logger's 384-byte body bound,
     * not merely the small names and viewport used by the semantic checks. */
    kage_vita_world_seam_diag_reset();
    setup_state();
    kage_vita_world_seam_diag_note_bind_framebuffer(0x8d40u, UINT32_MAX);
    kage_vita_world_seam_diag_note_use_program(UINT32_MAX);
    kage_vita_world_seam_diag_note_viewport(
        INT16_MIN, INT16_MAX, UINT16_MAX, UINT16_MAX);
    kage_vita_world_seam_diag_begin(13u, UINT32_MAX);
    for (index = 0u; index <= UINT16_MAX; ++index) {
        kage_vita_world_seam_diag_note_clear(UINT32_MAX);
        kage_vita_world_seam_diag_note_draw();
        kage_vita_world_seam_diag_note_bind_framebuffer(
            0x8d40u, UINT32_MAX);
        kage_vita_world_seam_diag_note_use_program(UINT32_MAX);
        kage_vita_world_seam_diag_note_bind_texture(0x0de1u, 17u);
        kage_vita_world_seam_diag_note_viewport(
            INT16_MIN, INT16_MAX, UINT16_MAX, UINT16_MAX);
        kage_vita_world_seam_diag_note_framebuffer_texture(
            0x8d40u, 0x8ce0u, 0x0de1u, 23u, 0);
    }
    kage_vita_world_seam_diag_post_room();
    kage_vita_world_seam_diag_post_lua();
    kage_vita_world_seam_diag_pre_hud();
    CHECK(count_lines() == 16u);
    CHECK(s_max_line_size > 0u && s_max_line_size < 384u);

    printf("world-seam diagnostic oracle: PASS (%u checks)\n", s_checks);
    return 0;
}
