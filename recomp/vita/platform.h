#ifndef ISAAC_VITA_PLATFORM_H
#define ISAAC_VITA_PLATFORM_H

/* Keep the measured Win32-visible spelling stable, but always give Vita
 * filesystem APIs the canonical device form.  Once the guest changes its
 * current directory, Vita3K treats `ux0:data` as a relative component and can
 * otherwise build `.../isaacr001/ux0:data`, which is invalid on Windows. */
#define ISAAC_VITA_GUEST_DATA_PARENT  "ux0:data"
#define ISAAC_VITA_NATIVE_DATA_PARENT "ux0:/data"
#define ISAAC_VITA_DATA_ROOT \
    ISAAC_VITA_GUEST_DATA_PARENT "/isaacr001"
#define ISAAC_VITA_NATIVE_DATA_ROOT \
    ISAAC_VITA_NATIVE_DATA_PARENT "/isaacr001"

#ifdef __cplusplus
extern "C" {
#endif

void isaac_vita_log_reset(void);
void isaac_vita_log(const char *format, ...);

#ifdef __cplusplus
}
#endif

#endif
