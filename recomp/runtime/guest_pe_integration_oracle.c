/* PC integration oracle for guest.c's publication/cleanup boundary. */
#include <stdint.h>
#include <stdio.h>
#include <windows.h>

#include "guest.h"
#include "guest_pe.h"

#define ORACLE_RESERVATION_SIZE 0x00900000U

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

static int fail(const char *message)
{
    fprintf(stderr, "guest PE integration oracle: FAIL: %s\n", message);
    return 1;
}

int main(int argc, char **argv)
{
    guest_import invalid_import = {
        GUEST_PE_EXPECTED_IMAGE_SIZE, "oracle!invalid-IAT-slot"
    };
    void *reservation;

    if (argc != 2)
        return fail("expected exact unpacked PE path");
    if (GUEST_IMAGE_BASE != GUEST_PE_PC_TARGET_BASE)
        return fail("oracle was not compiled for the frozen PC base");
    reservation = VirtualAlloc((LPVOID)(uintptr_t)GUEST_IMAGE_BASE,
                                 ORACLE_RESERVATION_SIZE,
                                 MEM_RESERVE, PAGE_NOACCESS);
    if (reservation != (void *)(uintptr_t)GUEST_IMAGE_BASE)
        return fail("could not reserve the exact PC guest range");

    /* This fails after the authenticated image and TLS block were created.
     * The following clean retry proves both were rolled back. */
    guest_register_imports(&invalid_import, 1U);
    if (guest_image_load(argv[1]) == 0)
        return fail("invalid IAT metadata was accepted");
    if (guest_image_contains(GUEST_IMAGE_BASE, 1U))
        return fail("failed publication left a visible image");

    guest_register_imports(NULL, 0U);
    if (guest_image_load(argv[1]) != 0)
        return fail("retry after post-map failure did not succeed");
    if (!guest_image_contains(GUEST_IMAGE_BASE,
                              GUEST_PE_EXPECTED_IMAGE_SIZE))
        return fail("published exact image range is missing");
    if (guest_image_load(argv[1]) == 0)
        return fail("live image replacement was accepted");
    if (!guest_image_contains(GUEST_IMAGE_BASE, 1U))
        return fail("rejected replacement damaged the live image");
    guest_image_free();
    guest_image_free();             /* cleanup is intentionally idempotent */

    if (guest_image_load(argv[1]) != 0)
        return fail("retry after explicit free did not succeed");
    guest_image_free();
    if (guest_image_contains(GUEST_IMAGE_BASE, 1U))
        return fail("explicit free left a visible image");
    if (!VirtualFree(reservation, 0U, MEM_RELEASE))
        return fail("could not release oracle reservation");
    puts("guest PE integration oracle: PASS (publish/rollback/retry)");
    return 0;
}
