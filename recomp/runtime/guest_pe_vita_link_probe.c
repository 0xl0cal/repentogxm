/* Link-only Vita probe which keeps guest.c's complete loader edge alive. */
#include <stdint.h>
#include <stdlib.h>

#include "guest.h"

void *isaac_vita_fixed_alloc(uint32_t address, uint32_t size)
{
    (void)address;
    return malloc(size);
}

void isaac_vita_fixed_free(void *address)
{
    free(address);
}

int guest_host_import(CPU *__restrict c, const char *name)
{
    (void)c;
    (void)name;
    return 0;
}

int guest_host_dynamic(CPU *__restrict c, uint32_t token)
{
    (void)c;
    (void)token;
    return 0;
}

int main(int argc, char **argv)
{
    int result;
    guest_register_imports(NULL, 0U);
    if (argc < 2)
        return 0;
    result = guest_image_load(argv[1]);
    guest_image_free();
    return result;
}
