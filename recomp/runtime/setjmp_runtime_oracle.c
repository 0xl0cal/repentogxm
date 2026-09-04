/* Focused behavioural oracle for the shared MSVC-x86 setjmp/longjmp bridge.
 *
 * The fatal libpng path calls longjmp from the same translated owner frame.
 * Its call return therefore reuses the guest stack word which originally held
 * `_setjmp3`'s return.  Real x86 longjmp restores ESP+4 and jumps to the saved
 * EIP; it never expects that dead call-frame word to retain its old contents.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "guest.h"

#define ORACLE_SETJMP_RETURN  0x005a0f57U
#define ORACLE_LONGJMP_RETURN 0x005a12deU
#define ORACLE_VALUE          7U

static uint32_t s_stack[128];
static uint32_t s_env[16];
static uint32_t s_saved_esp;
static unsigned s_initial_returns;
static unsigned s_resumed_returns;
static unsigned s_after_longjmp;
static int s_owner_ok;

/* guest.c's generic dispatcher owns these two platform seams.  The focused
 * oracle never dispatches an import, but explicit rejecting stubs keep the
 * production object link-complete on toolchains which resolve dead sections. */
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

static void setjmp_owner(CPU *__restrict c)
{
    guest_jump_site site = {0};
    unsigned i;

    for (i = 0U; i < 16U; ++i)
        s_env[i] = 0xa5a5a5a5U;

    c->ebp = 0x13572468U;
    c->ebx = 0x24681357U;
    c->edi = 0x89abcdefU;
    c->esi = 0xfedcba98U;
    s_saved_esp = c->esp;

    gpush(c, 0U);                    /* `_setjmp3` unwind count */
    gpush(c, (uint32_t)(uintptr_t)s_env);
    gpush(c, ORACLE_SETJMP_RETURN);
    guest_setjmp_prepare(c, &site);
    if (setjmp(site.native_env) == 0) {
        guest_setjmp_finish(c, &site, 0);
        ++s_initial_returns;
    } else {
        guest_setjmp_finish(c, &site, 1);
        ++s_resumed_returns;
    }
    if (c->fault)
        return;
    if (!guest_stack_adjust(c, 8U, ORACLE_SETJMP_RETURN))
        return;                      /* translated cdecl caller cleanup */

    if (c->eax == 0U) {
        /* Exactly the live PNG shape: with no guest callee frame between the
         * two calls, this return lands on `_setjmp3`'s now-dead return slot. */
        c->ebp = 0xeeee0001U;
        c->ebx = 0xeeee0002U;
        c->edi = 0xeeee0003U;
        c->esi = 0xeeee0004U;
        gpush(c, ORACLE_VALUE);
        gpush(c, (uint32_t)(uintptr_t)s_env);
        gpush(c, ORACLE_LONGJMP_RETURN);
        if (c->esp != s_saved_esp - 12U ||
            ld32(c->esp) != ORACLE_LONGJMP_RETURN)
            return;
        guest_longjmp(c, (uint32_t)(uintptr_t)s_env, ORACLE_VALUE);
        ++s_after_longjmp;           /* a correct bridge never reaches this */
    }

    s_owner_ok = c->eax == ORACLE_VALUE && c->esp == s_saved_esp &&
                 c->ebp == 0x13572468U && c->ebx == 0x24681357U &&
                 c->edi == 0x89abcdefU && c->esi == 0xfedcba98U &&
                 ld32(s_saved_esp - 12U) == ORACLE_LONGJMP_RETURN &&
                 s_env[4] == s_saved_esp - 12U &&
                 s_env[5] == ORACLE_SETJMP_RETURN;
    guest_setjmp_leave(c, &site);
}

int main(void)
{
    CPU cpu;
    int stopped;

    if ((uintptr_t)s_stack > UINT32_MAX || (uintptr_t)s_env > UINT32_MAX)
        return 2;
    guest_cpu_init(&cpu);
    if (guest_stack_bind(&cpu,
                         (uint32_t)(uintptr_t)&s_stack[0],
                         (uint32_t)(uintptr_t)&s_stack[120]) != 0)
        return 2;

    stopped = guest_run_until_stop(&cpu, setjmp_owner);
    if (stopped != GUEST_RUN_RETURNED || cpu.fault || !s_owner_ok ||
        s_initial_returns != 1U || s_resumed_returns != 1U ||
        s_after_longjmp != 0U || cpu.jump_sites ||
        cpu.stack_fault_kind != GUEST_STACK_FAULT_NONE) {
        fprintf(stderr,
                "setjmp runtime oracle: FAIL stop=%d fault=%s addr=%08x "
                "initial=%u resumed=%u after=%u owner=%d\n",
                stopped, cpu.fault ? cpu.fault : "<none>", cpu.fault_addr,
                s_initial_returns, s_resumed_returns, s_after_longjmp,
                s_owner_ok);
        return 1;
    }

    guest_stack_free(&cpu);
    puts("setjmp runtime oracle: PASS (dead return slot reused)");
    return 0;
}
