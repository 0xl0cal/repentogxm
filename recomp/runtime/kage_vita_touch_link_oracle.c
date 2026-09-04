#include "kage_vita_touch.h"

int main(void)
{
    kage_vita_touch_snapshot snapshot;

    if (kage_vita_touch_initialize())
        (void)kage_vita_touch_sample();
    (void)kage_vita_touch_snapshot_read(&snapshot);
    kage_vita_touch_deactivate();
    return snapshot.actions != KAGE_VITA_TOUCH_ACTION_NONE;
}
