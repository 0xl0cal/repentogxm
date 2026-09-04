#ifndef MANUAL_KAGE_VITA_VSYNC_H
#define MANUAL_KAGE_VITA_VSYNC_H

#include <stdint.h>

typedef int (*guest_kage_vita_vsync_setter)(int enabled);

/* Repentance advances one Update per Present whenever its published VSync
 * byte is non-zero.  vitaGL/Vita3K cannot provide a reliable 60 Hz pacing
 * contract, so Vita always publishes actual=0 and leaves the frozen guest's
 * monotonic 1/60 limiter in charge.  The requested Options byte remains
 * independent and is preserved exactly for the menu/config file.
 *
 * Keep the guest byte unchanged if the backend cannot be reconciled.  That
 * makes a later SetVSync call retry instead of silently publishing success. */
static inline int guest_kage_vita_vsync_reconcile(
    uint8_t requested,
    uint8_t *stored_requested,
    uint8_t *guest_actual,
    int backend_actual,
    guest_kage_vita_vsync_setter set_backend,
    int *backend_changed)
{
    uint8_t prior_actual;

    if (!stored_requested || !guest_actual)
        return 0;
    *stored_requested = requested;
    prior_actual = *guest_actual;
    if (backend_changed)
        *backend_changed = 0;

    if (prior_actual != 0u || backend_actual) {
        if (!set_backend || !set_backend(0))
            return 0;
        if (backend_changed)
            *backend_changed = 1;
    }
    *guest_actual = 0u;
    return 1;
}

#endif
