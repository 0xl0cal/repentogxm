/* Softfp compile/link probe only.  This ELF must never be used as runtime
 * evidence; the executable host oracle covers behavior. */
#include <stdint.h>

#include "kage_vita_input.h"

int main(void)
{
    uint8_t keys[KAGE_PC_KEY_COUNT];

    if (kage_vita_input_initialize())
        (void)kage_vita_input_sample();
    kage_vita_input_keyboard_snapshot(keys);
    kage_vita_input_deactivate();
    return keys[257] != 0u;
}
