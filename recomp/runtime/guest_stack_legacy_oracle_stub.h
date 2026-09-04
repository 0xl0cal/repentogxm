#ifndef GUEST_STACK_LEGACY_ORACLE_STUB_H
#define GUEST_STACK_LEGACY_ORACLE_STUB_H

/* Test-only counter for standalone oracle links that intentionally omit
 * runtime/guest.c.  Production targets must never compile or link this file. */
unsigned guest_stack_legacy_oracle_violation_calls(void);

#endif
