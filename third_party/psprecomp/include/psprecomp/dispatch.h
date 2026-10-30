/* psprecomp — the address → function dispatch table.
 *
 * Most calls in recompiled code are direct: `jal 0x1234` becomes a plain C
 * call to psp_func_00001234(). Some are not — `jalr $t9` and `jr $t9` take
 * their target from a register, and MIPS uses that for function pointers,
 * vtables, callbacks and switch tables.
 *
 * Those go through here: the generated code registers every function it
 * defines, and an indirect transfer looks the target up at run time. This is
 * the same mechanism lynxrecomp uses for computed jumps, and it is what lets a
 * partially-discovered module still run — an unregistered target is a named,
 * catchable failure rather than a jump into nowhere.
 */
#ifndef PSPRECOMP_DISPATCH_H
#define PSPRECOMP_DISPATCH_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*psp_fn_t)(void);

/* Register one recompiled function. The generated code calls this for every
 * function it defines, from a single init routine. */
void psp_register(uint32_t addr, psp_fn_t fn);
void psp_register_label(uint32_t addr, psp_fn_t fn);
void psp_dispatch_set_budget(uint64_t calls, void (*on_exceeded)(void));
uint64_t psp_dispatch_calls(void);

/* Look up a target. Returns NULL if nothing is registered there. */
psp_fn_t psp_lookup(uint32_t addr);

/* Call through the table. An unregistered address invokes the miss handler
 * (below) rather than crashing, so bring-up gets a report naming the address
 * instead of an access violation. */
void psp_dispatch(uint32_t addr);
void psp_unregister_range(uint32_t lo, uint32_t hi);

/* Guaranteed tail calls between recompiled functions.
 *
 * A guest branch that crosses a C function boundary (discovery split one
 * routine into several bodies) must not consume host stack, or a guest loop
 * becomes unbounded host recursion. PSP_TAIL(f()) is `return f();` with the
 * tail call made mandatory where the compiler supports it; caller and callee
 * must both be void(void), which is why a body takes its entry address from
 * psp_tail_pc instead of a parameter. psp_dispatch_tail() is psp_dispatch()
 * in that shape: it transfers to the function registered at psp_tail_pc. */
#if defined(__has_attribute)
#  if __has_attribute(musttail)
#    define PSP_TAIL(call) __attribute__((musttail)) return call
#  endif
#endif
#ifndef PSP_TAIL
#  define PSP_TAIL(call) do { call; return; } while (0)
#endif
extern uint32_t psp_tail_pc;
void psp_dispatch_tail(void);

/* Called when psp_dispatch hits an unregistered address. The default prints
 * the address and aborts; a host can override it to log and continue, which is
 * useful while discovery is still incomplete. */
/* ---- function-entry trace ------------------------------------------------
 * Generated functions call psp_trace_enter() on entry when the code is built
 * with PSPRECOMP_TRACE; otherwise the call compiles away entirely.
 *
 * This exists because a dispatch miss reports the bad *target* and says
 * nothing about who jumped there. Recompiled functions are ordinary C, so the
 * information is on the host stack — but reading it portably is more trouble
 * than recording it, and a short history of entered functions also shows how
 * the code arrived, not just where it was. */
void psp_trace_enter(uint32_t addr);
void psp_trace_dump(void);
void psp_trace_reset(void);
uint32_t psp_trace_last(void);
/* addr == PSP_TRACE_WATCH_ALL calls fn on every function entry (host filters). */
#define PSP_TRACE_WATCH_ALL 0xFFFFFFFFu
void psp_trace_watch(uint32_t addr, void (*fn)(uint32_t));
void psp_trace_loop(uint32_t addr);
void psp_trace_mark(uint32_t addr);
void psp_trace_sp(uint32_t fn, uint32_t sp_in, uint32_t sp_out);
void psp_trace_sp_call(uint32_t callee, uint32_t sp_before, uint32_t sp_after);
uint64_t psp_sp_call_violations(void);
uint64_t psp_sp_violations(void);
uint32_t psp_sp_first_bad(void);
int32_t  psp_sp_first_delta(void);
void psp_trace_watch_label(uint32_t addr, void (*fn)(uint32_t));
void psp_trace_marks_init(uint32_t lo, uint32_t words);
int psp_trace_was_marked(uint32_t addr);
uint32_t psp_trace_loop_addr(void);
uint64_t psp_trace_loop_hits(void);

/* Optional hook that prints extra context when a dispatch miss happens. The
 * HLE layer installs one so a wild pointer is reported alongside the firmware
 * calls that recently returned zero -- dispatch itself must not depend on the
 * HLE layer, hence the indirection. */
typedef void (*psp_miss_ctx_fn_t)(void);
void psp_set_miss_context(psp_miss_ctx_fn_t fn);
void psp_miss_context(void);

typedef void (*psp_miss_fn_t)(uint32_t addr);
void psp_set_miss_handler(psp_miss_fn_t fn);

/* Number of registered functions, and how many misses have occurred — the
 * latter is a direct measure of how much the analysis is still missing. */
uint32_t psp_dispatch_count(void);
uint64_t psp_dispatch_misses(void);

void psp_dispatch_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* PSPRECOMP_DISPATCH_H */
