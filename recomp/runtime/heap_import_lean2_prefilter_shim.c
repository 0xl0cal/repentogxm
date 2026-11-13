/* Link-time companion of host_vita_heap_free_prefilter_oracle.c for
 * vita/test_heap_import_lean2.sh (host gcc only, never in the eboot).
 *
 * That oracle's own isaac_vita_log drops every line.  The test links with
 * -Wl,--wrap=isaac_vita_log so the heap object's calls land here instead:
 * each line is printed as "[log] ..." and lean2 verify mismatches are
 * counted.  After main returns, the ISAAC_VITA_HEAP_IMPORT_LEAN2 receipt
 * (verify checks/disagreements per kind, nonzero sidecars) is printed as a
 * "[lean2]" line; a comparator build prints "[lean2] comparator". */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "host_vita_heap.h"

static unsigned s_log_lines, s_mismatch_lines;

void __wrap_isaac_vita_log(const char *format, ...);
void __wrap_isaac_vita_log(const char *format, ...)
{
    char line[512];
    va_list arguments;

    va_start(arguments, format);
    vsnprintf(line, sizeof line, format, arguments);
    va_end(arguments);
    ++s_log_lines;
    if (strstr(line, "lean2 verify: FIRST MISMATCH"))
        ++s_mismatch_lines;
    printf("[log] %s\n", line);
}

__attribute__((destructor)) static void lean2_prefilter_receipt(void)
{
#if defined(ISAAC_VITA_HEAP_IMPORT_LEAN2) && ISAAC_VITA_HEAP_IMPORT_LEAN2
    isaac_vita_guest_heap_lean2_stats stats;

    if (!isaac_vita_guest_heap_lean2_stats_get(&stats)) {
        puts("[lean2] stats unavailable");
    } else {
        printf("[lean2] verify=%d sidecar_nonzero=%u checks=%u/%u/%u/%u "
               "bad=%u/%u/%u/%u mismatch_lines=%u log_lines=%u\n",
               stats.verify, stats.sidecar_nonzero, stats.checks[0],
               stats.checks[1], stats.checks[2], stats.checks[3],
               stats.bad[0], stats.bad[1], stats.bad[2], stats.bad[3],
               s_mismatch_lines, s_log_lines);
    }
#else
    printf("[lean2] comparator log_lines=%u\n", s_log_lines);
#endif
    fflush(stdout);
}
