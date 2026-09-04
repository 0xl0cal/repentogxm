/* assert() replacement for the vendored stb_vorbis v1.04 (ISAAC_NV_ASSERT_HEADER).
 *
 * The game shipped its decoder with assertions enabled (the PE calls
 * _wassert with the v1.04 expressions).  A native decode running on the
 * audio worker must never abort() the process, so a failed assertion is
 * recorded in the owning mirror and unwinds to the frame boundary through
 * the mirror's jmp_buf; the mirror then reports end of stream. */
#ifndef ISAAC_HOST_VITA_NATIVE_VORBIS_ASSERT_H
#define ISAAC_HOST_VITA_NATIVE_VORBIS_ASSERT_H

void isaac_nv_assert_fail(const char *expression, int line);

#undef assert
#define assert(expression) \
    ((expression) ? (void)0 : isaac_nv_assert_fail(#expression, __LINE__))

#endif
