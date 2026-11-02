/* psprecomp — ThreadManForUser.
 *
 * Threads, semaphores and event flags.
 *
 * ## Execution model
 *
 * Recompiled functions are ordinary C: calls are C calls, so a thread's live
 * frames sit on the *host* C stack, and its guest registers live in the one
 * global `psp_cpu`. That rules out anything preemptive and makes true
 * concurrency a matter of giving each thread its own host stack and switching
 * only where the guest itself blocks.
 *
 * That is exactly what this does, with ucontext:
 *
 *   - Each thread runs on its own ucontext with a host stack it owns, plus its
 *     own saved `psp_cpu` register file.
 *   - A tiny round-robin scheduler (running on the base context) picks the next
 *     READY thread and swapcontext()s into it. A thread yields by returning to
 *     the base context.
 *   - `sceKernelWaitSema` / `WaitEventFlag` that cannot be satisfied mark the
 *     thread BLOCKED, record what it waits on, and yield; the matching Signal /
 *     Set wakes it. `sceKernelDelayThread` yields to let peers run.
 *
 * Cooperative, no preemption: a thread runs until it blocks or exits. That is
 * sufficient because PSP threads yield at exactly these firmware boundaries,
 * and it is far simpler than a timer-driven context switch. If the game spins
 * inside guest code without calling firmware, nothing here can help -- the
 * watchdog is the answer to that, not the scheduler.
 *
 * ucontext is marked deprecated on macOS but is present and works; it is the
 * only portable way to get a switchable host stack without hand-rolling one.
 */

/* macOS gates the (deprecated but functional) ucontext routines behind
 * _XOPEN_SOURCE; define it before any system header. */
#ifndef _XOPEN_SOURCE
#  define _XOPEN_SOURCE 600
#endif
#ifndef _DARWIN_C_SOURCE
#  define _DARWIN_C_SOURCE 1   /* MAP_ANON alongside _XOPEN_SOURCE */
#endif
#ifndef _DEFAULT_SOURCE
#  define _DEFAULT_SOURCE 1
#endif

#include "psprecomp/hle.h"
#include "psprecomp/dispatch.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* PES6PORT: host context switching. Android's libc has no makecontext /
 * swapcontext, so there the switch is a few lines of AArch64 assembly: push
 * the callee-saved registers, swap stack pointers, pop, return. Everywhere
 * else the (deprecated but functional) ucontext routines do the same job. */
#if defined(__ANDROID__) && defined(__aarch64__)
typedef struct { void *sp; } host_ctx;
void psp_ctx_switch(host_ctx *from, host_ctx *to);
__asm__(
    ".text\n.align 4\n.global psp_ctx_switch\n.type psp_ctx_switch,%function\n"
    "psp_ctx_switch:\n"
    "  sub sp, sp, #176\n"
    "  stp x19, x20, [sp, #0]\n  stp x21, x22, [sp, #16]\n  stp x23, x24, [sp, #32]\n"
    "  stp x25, x26, [sp, #48]\n  stp x27, x28, [sp, #64]\n  stp x29, x30, [sp, #80]\n"
    "  stp d8, d9, [sp, #96]\n  stp d10, d11, [sp, #112]\n  stp d12, d13, [sp, #128]\n"
    "  stp d14, d15, [sp, #144]\n"
    "  mov x9, sp\n  str x9, [x0]\n  ldr x9, [x1]\n  mov sp, x9\n"
    "  ldp x19, x20, [sp, #0]\n  ldp x21, x22, [sp, #16]\n  ldp x23, x24, [sp, #32]\n"
    "  ldp x25, x26, [sp, #48]\n  ldp x27, x28, [sp, #64]\n  ldp x29, x30, [sp, #80]\n"
    "  ldp d8, d9, [sp, #96]\n  ldp d10, d11, [sp, #112]\n  ldp d12, d13, [sp, #128]\n"
    "  ldp d14, d15, [sp, #144]\n"
    "  add sp, sp, #176\n  ret\n");
#define HOST_CTX_ASM 1
#else
#include <ucontext.h>
typedef ucontext_t host_ctx;
#define psp_ctx_switch(from, to) swapcontext((from), (to))
#endif
#include <sys/mman.h>
#include <time.h>

#define MAX_THREADS 128
#define MAX_SEMAS   128
#define MAX_FLAGS   128
#define MAX_CBS     64
#define UID_BASE    0x00040000u
#define TH_HOST_STACK (32u * 1024u * 1024u)

/* Host stacks come from mmap with an inaccessible guard region below them.
 * With plain malloc a runaway recursion in recompiled code silently walked off
 * the stack into whatever the allocator had placed underneath -- in practice
 * the guest RAM block -- and showed up as guest memory corruption far from its
 * cause. With a guard it is an immediate fault at the overflowing frame. */
#define TH_HOST_GUARD (64u * 1024u)
static void *host_stack_alloc(void) {
    void *m = mmap(NULL, TH_HOST_GUARD + TH_HOST_STACK, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANON, -1, 0);
    if (m == MAP_FAILED) return NULL;
    mprotect(m, TH_HOST_GUARD, PROT_NONE);
    return (char *)m + TH_HOST_GUARD;
}
static void host_stack_free(void *stack) {
    if (stack) munmap((char *)stack - TH_HOST_GUARD, TH_HOST_GUARD + TH_HOST_STACK);
}

enum { TH_DORMANT = 0, TH_READY, TH_RUNNING, TH_BLOCKED };

/* What a blocked thread is waiting on. */
enum { WAIT_NONE = 0, WAIT_SEMA, WAIT_FLAG, WAIT_THREAD, WAIT_TIME, WAIT_SLEEP };

typedef struct {
    uint32_t uid;
    char     name[32];
    uint32_t entry;
    uint32_t priority;
    uint32_t stack_size;     /* guest stack, bytes */
    uint32_t stack_base;     /* guest stack, low address */
    uint32_t stack_top;      /* initial $sp, below any copied argument block */
    uint32_t attr;
    int      state;

    uint32_t arg_len, arg_ptr;
    uint32_t exit_status;

    int      wait_kind;
    uint32_t wait_id;
    uint64_t wake_at;        /* WAIT_TIME: virtual microseconds */
    int      wakeups;        /* pending sceKernelWakeupThread count */

    psp_cpu_state ctx;       /* this thread's registers, saved between switches */
    host_ctx      uctx;      /* this thread's host stack */
    void         *host_stack;
    int           used;
} psp_thread;

typedef struct { uint32_t uid; char name[32]; int32_t count, max_count; int used; } psp_sema;
typedef struct { uint32_t uid; char name[32]; uint32_t pattern; int used; } psp_evflag;
typedef struct {
    uint32_t uid; char name[32]; uint32_t func, arg; int used;
    uint32_t owner;          /* creating thread's uid (0: any thread) */
    int      pending;        /* notifications not yet delivered */
    uint32_t notify_arg;
    int      running;
} psp_callback;
typedef struct { uint32_t uid; uint32_t handler, arg; uint64_t fire_at; int used; } psp_alarm;

#define MAX_ALARMS 16

/* ---- virtual clock (PES6PORT) ---------------------------------------------
 * Guest time is virtual and deterministic: it advances a fixed quantum per
 * context switch, a tick per clock read, and jumps straight to the next
 * deadline when every thread is waiting on time. Nothing here reads the host
 * clock, so two runs schedule identically -- which is what makes a run
 * comparable against the oracle -- and an idle game is not slowed to real
 * time during bring-up.
 *
 * A host that presents the game to a person switches to REAL TIME instead
 * (psp_threadman_set_realtime): guest time then follows the host's monotonic
 * clock, and an idle scheduler sleeps until the next deadline. A slow host
 * frame then simply takes longer in guest time too, as it would on hardware,
 * and thread delays (audio pacing, vblank waits) last what they say. */
#define TH_SWITCH_USEC 20u
static uint64_t  g_now;
static psp_alarm g_alarm[MAX_ALARMS];
static uint32_t  g_intr_stack;      /* guest stack for alarm handlers */

static int      g_rt;               /* real-time mode */
static uint64_t g_rt_guest0, g_rt_host0;

static uint64_t host_usec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
}

/* Bring guest time up to the host clock (never backwards). */
static void rt_sync(void) {
    if (!g_rt) return;
    uint64_t t = g_rt_guest0 + (host_usec() - g_rt_host0);
    if (t > g_now) g_now = t;
}

void psp_threadman_set_realtime(int on) {
    if (on && !g_rt) { g_rt_guest0 = g_now; g_rt_host0 = host_usec(); }
    g_rt = on != 0;
}

uint64_t psp_time_usec(void) {
    if (g_rt) { rt_sync(); return g_now; }
    return g_now++;
}

/* Vertical-blank sub-interrupt handlers (sceKernelRegisterSubIntrHandler on
 * interrupt 30). The scheduler calls the enabled ones once per display frame,
 * in interrupt context like an alarm. Games hang real work on this: PES6's
 * handler drives its frame counters and releases its movie player's threads. */
#define PSP_VBLANK_INT   30
#define PSP_VBLANK_USEC  16683u            /* 59.94 Hz */
#define MAX_SUBINTR      8
typedef struct { uint32_t no, handler, arg; int used, enabled; } psp_subintr;
static psp_subintr g_vblank_intr[MAX_SUBINTR];
static uint64_t    g_next_vblank = PSP_VBLANK_USEC;

int psp_subintr_register(uint32_t intr, uint32_t no, uint32_t handler, uint32_t arg) {
    if (intr != PSP_VBLANK_INT) return 0;          /* accepted, never raised */
    psp_subintr *slot = NULL;
    for (int i = 0; i < MAX_SUBINTR; i++) {
        if (g_vblank_intr[i].used && g_vblank_intr[i].no == no) { slot = &g_vblank_intr[i]; break; }
        if (!g_vblank_intr[i].used && !slot) slot = &g_vblank_intr[i];
    }
    if (!slot) return -1;
    slot->no = no; slot->handler = handler; slot->arg = arg; slot->used = 1;
    return 0;
}

/* state: 1 enable, 0 disable, -1 release. */
void psp_subintr_set(uint32_t intr, uint32_t no, int state) {
    if (intr != PSP_VBLANK_INT) return;
    for (int i = 0; i < MAX_SUBINTR; i++)
        if (g_vblank_intr[i].used && g_vblank_intr[i].no == no) {
            if (state < 0) g_vblank_intr[i].used = g_vblank_intr[i].enabled = 0;
            else g_vblank_intr[i].enabled = state;
        }
}

static psp_thread   g_thread[MAX_THREADS];
static psp_sema     g_sema[MAX_SEMAS];
static psp_evflag   g_flag[MAX_FLAGS];
static psp_callback g_cb[MAX_CBS];
static uint32_t     g_next_uid;
static psp_thread  *g_current;
static host_ctx     g_base;

void psp_threadman_reset(void) {
    for (int i = 0; i < MAX_THREADS; i++) if (g_thread[i].host_stack) host_stack_free(g_thread[i].host_stack);
    memset(g_thread, 0, sizeof g_thread);
    memset(g_sema, 0, sizeof g_sema);
    memset(g_flag, 0, sizeof g_flag);
    memset(g_cb, 0, sizeof g_cb);
    memset(g_alarm, 0, sizeof g_alarm);
    memset(g_vblank_intr, 0, sizeof g_vblank_intr);
    g_next_vblank = PSP_VBLANK_USEC;
    g_now = 0;
    g_intr_stack = 0;
    g_next_uid = UID_BASE;
    g_current = NULL;
}

void psp_threadman_init(void) { psp_threadman_reset(); }

static psp_thread *find_thread(uint32_t id) {
    for (int i = 0; i < MAX_THREADS; i++)
        if (g_thread[i].used && g_thread[i].uid == id) return &g_thread[i];
    return NULL;
}
static psp_sema *find_sema(uint32_t id) {
    for (int i = 0; i < MAX_SEMAS; i++)
        if (g_sema[i].used && g_sema[i].uid == id) return &g_sema[i];
    return NULL;
}
static psp_evflag *find_flag(uint32_t id) {
    for (int i = 0; i < MAX_FLAGS; i++)
        if (g_flag[i].used && g_flag[i].uid == id) return &g_flag[i];
    return NULL;
}

/* ---- scheduler ----------------------------------------------------------- */

static psp_thread *pick_ready(void) {
    int start = g_current ? (int)(g_current - g_thread) : 0;
    for (int k = 1; k <= MAX_THREADS; k++) {
        int i = (start + k) % MAX_THREADS;
        if (g_thread[i].used && g_thread[i].state == TH_READY) return &g_thread[i];
    }
    return NULL;
}

/* Yield the running thread to the base scheduler. Preserves the thread's
 * registers; when it is scheduled again it resumes right here with psp_cpu
 * already restored by the scheduler. */
static void yield_to_base(void) {
    psp_thread *t = g_current;
    if (t) t->ctx = psp_cpu;
    psp_ctx_switch(&t->uctx, &g_base);
}

static void wake_waiters(uint32_t id, int kind) {
    for (int i = 0; i < MAX_THREADS; i++) {
        psp_thread *t = &g_thread[i];
        if (t->used && t->state == TH_BLOCKED && t->wait_kind == kind && t->wait_id == id) {
            t->wait_kind = WAIT_NONE;
            t->wait_id = 0;
            t->state = TH_READY;
        }
    }
}

static void thread_main(void) {
    psp_thread *t = g_current;
    psp_cpu.r[PSP_REG_A0] = t->arg_len;
    psp_cpu.r[PSP_REG_A1] = t->arg_ptr;
    psp_cpu.r[PSP_REG_SP] = t->stack_top;
    psp_cpu.r[PSP_REG_RA] = 0;
    psp_dispatch(t->entry);
    t->exit_status = psp_cpu.r[PSP_REG_V0];
    t->state = TH_DORMANT;
}

#ifdef HOST_CTX_ASM
/* First entry of a thread's host context: run it, then hand the CPU back to
 * the scheduler for good (what uc_link does for ucontext). */
static void thread_boot(void) {
    thread_main();
    host_ctx dead;
    psp_ctx_switch(&dead, &g_base);
    abort();
}
#endif

/* Run one due alarm handler in "interrupt context": on the scheduler's host
 * stack, with its own guest stack and no current thread, so it cannot block
 * (WaitSema there reports a timeout). The handler's return value is the delay
 * to its next firing in microseconds, or 0 to cancel -- which is how a game
 * builds a periodic timer out of a one-shot alarm. */
static void alarm_fire(psp_alarm *a) {
    psp_cpu_state saved = psp_cpu;
    if (!g_intr_stack) g_intr_stack = psp_sysmem_alloc(0x4000, 1);
    psp_cpu.r[PSP_REG_A0] = a->arg;
    psp_cpu.r[PSP_REG_SP] = (g_intr_stack + 0x4000 - 64) & ~15u;
    psp_cpu.r[PSP_REG_RA] = 0;
    psp_dispatch(a->handler);
    uint32_t again = psp_cpu.r[PSP_REG_V0];
    psp_cpu = saved;
    if (again && a->used) a->fire_at = g_now + (again < 100 ? 100 : again);
    else                  a->used = 0;
}

/* PES6PORT: called once per virtual vblank, on the scheduler's own (host main
 * thread) stack -- where a host presents the frame, polls input and waits for
 * real time to catch up with guest time. */
static void (*g_vblank_hook)(void);
void psp_set_vblank_hook(void (*fn)(void)) { g_vblank_hook = fn; }

/* PES6PORT: firmware services that need to run without being called (the
 * network: receive, time out peers, deliver events to guest handlers) get a
 * turn here, once per virtual vblank, in "interrupt context" like an alarm. */
#define MAX_VBLANK_TASKS 16
static void (*g_vblank_task[MAX_VBLANK_TASKS])(void);
void psp_add_vblank_task(void (*fn)(void)) {
    for (int i = 0; i < MAX_VBLANK_TASKS; i++)
        if (!g_vblank_task[i] || g_vblank_task[i] == fn) { g_vblank_task[i] = fn; return; }
    /* Android already registers 4 (60 Hz, network, commentary, widescreen): a dropped task is a
     * feature silently off, so say it. */
    fprintf(stderr, "psprecomp: no room for vblank task %p (max %d)\n", (void *)fn, MAX_VBLANK_TASKS);
}

/* Wake sleepers and fire alarms whose time has come. */
static void timers_poll(void) {
    for (int i = 0; i < MAX_THREADS; i++) {
        psp_thread *t = &g_thread[i];
        if (t->used && t->state == TH_BLOCKED && t->wait_kind == WAIT_TIME && t->wake_at <= g_now) {
            t->wait_kind = WAIT_NONE;
            t->state = TH_READY;
        }
    }
    for (int i = 0; i < MAX_ALARMS; i++)
        if (g_alarm[i].used && g_alarm[i].fire_at <= g_now) alarm_fire(&g_alarm[i]);

    if (g_next_vblank <= g_now) {
        /* One call per poll however far time jumped: a handler counts frames,
         * and replaying a backlog would only make it catch up in a burst. */
        g_next_vblank = g_now - g_now % PSP_VBLANK_USEC + PSP_VBLANK_USEC;
        if (g_vblank_hook) g_vblank_hook();
        for (int i = 0; i < MAX_VBLANK_TASKS; i++) if (g_vblank_task[i]) g_vblank_task[i]();
        for (int i = 0; i < MAX_SUBINTR; i++) {
            psp_subintr *v = &g_vblank_intr[i];
            if (!v->used || !v->enabled) continue;
            psp_cpu_state saved = psp_cpu;
            if (!g_intr_stack) g_intr_stack = psp_sysmem_alloc(0x4000, 1);
            psp_cpu.r[PSP_REG_A0] = v->no;
            psp_cpu.r[PSP_REG_A1] = v->arg;
            psp_cpu.r[PSP_REG_SP] = (g_intr_stack + 0x4000 - 64) & ~15u;
            psp_cpu.r[PSP_REG_RA] = 0;
            psp_dispatch(v->handler);
            psp_cpu = saved;
        }
    }
}

/* Earliest pending deadline, if any. */
static int timers_next(uint64_t *when) {
    int have = 0;
    for (int i = 0; i < MAX_THREADS; i++) {
        psp_thread *t = &g_thread[i];
        if (!t->used || t->state != TH_BLOCKED || t->wait_kind != WAIT_TIME) continue;
        if (!have || t->wake_at < *when) { *when = t->wake_at; have = 1; }
    }
    for (int i = 0; i < MAX_ALARMS; i++) {
        if (!g_alarm[i].used) continue;
        if (!have || g_alarm[i].fire_at < *when) { *when = g_alarm[i].fire_at; have = 1; }
    }
    for (int i = 0; i < MAX_SUBINTR; i++) {
        if (!g_vblank_intr[i].used || !g_vblank_intr[i].enabled) continue;
        if (!have || g_next_vblank < *when) { *when = g_next_vblank; have = 1; }
    }
    return have;
}

/* Block the current thread for `usec` of virtual time. */
void psp_thread_sleep_usec(uint64_t usec) {
    if (!g_current) return;
    g_current->wait_kind = WAIT_TIME;
    g_current->wake_at = g_now + usec;
    g_current->state = TH_BLOCKED;
    yield_to_base();
}

/* The scheduler loop. Runs on the base context: repeatedly dispatch a READY
 * thread until none is left (all dormant) or every remaining thread is
 * blocked (deadlock). */
void psp_threadman_run(void) {
    psp_cpu_state base = psp_cpu;   /* the base/creator context: restore on exit */
    for (;;) {
        rt_sync();
        timers_poll();
        psp_thread *t = pick_ready();
        if (t) {
            uint32_t uid = t->uid;
            t->state = TH_RUNNING;
            g_current = t;
            psp_cpu = t->ctx;
            psp_ctx_switch(&g_base, &t->uctx);
            g_current = NULL;
            if (!g_rt) g_now += TH_SWITCH_USEC;
            if (t->state == TH_DORMANT) wake_waiters(uid, WAIT_THREAD);
            continue;
        }
        /* Nothing runnable: jump to the next deadline rather than spin. */
        uint64_t when;
        if (timers_next(&when)) {
            if (g_rt && when > g_now) {
                /* Everyone is waiting on time: so do we, in short naps so the
                 * vblank hook (window, input) keeps its cadence. */
                uint64_t nap = when - g_now;
                if (nap > 4000) nap = 4000;
                struct timespec ts = { 0, (long)nap * 1000L };
                nanosleep(&ts, NULL);
                continue;
            }
            if (when > g_now) g_now = when;
            continue;
        }
        int alive = 0, blocked = 0;
        for (int i = 0; i < MAX_THREADS; i++) {
            if (!g_thread[i].used || g_thread[i].state == TH_DORMANT) continue;
            alive = 1;
            if (g_thread[i].state == TH_BLOCKED) blocked = 1;
        }
        if (!alive) break;                        /* all threads finished */
        if (blocked) {
            fprintf(stderr, "psprecomp: scheduler deadlock — every live thread is blocked\n");
            break;
        }
    }
    psp_cpu = base;
}

/* One line per live thread: what it is and what it is waiting for. The first
 * question at any stall is "which thread is stuck on what", and the scheduler
 * is the only place that knows. */
void psp_threadman_dump(FILE *out) {
    static const char *ST[] = { "dormant", "ready", "running", "blocked" };
    static const char *WK[] = { "-", "sema", "evflag", "thread", "time", "sleep" };
    fprintf(out, "threads (virtual time %llu us):\n", (unsigned long long)g_now);
    for (int i = 0; i < MAX_THREADS; i++) {
        psp_thread *t = &g_thread[i];
        if (!t->used) continue;
        fprintf(out, "  0x%X %-20s entry=0x%08X prio=%-3u %-7s wait=%s", t->uid, t->name,
                t->entry, t->priority, ST[t->state], WK[t->wait_kind]);
        if (t->wait_kind == WAIT_SEMA) {
            psp_sema *s = find_sema(t->wait_id);
            fprintf(out, " 0x%X '%s' count=%d", t->wait_id, s ? s->name : "?", s ? s->count : 0);
        } else if (t->wait_kind == WAIT_FLAG) {
            psp_evflag *f = find_flag(t->wait_id);
            fprintf(out, " 0x%X '%s' pattern=0x%X", t->wait_id, f ? f->name : "?", f ? f->pattern : 0);
        } else if (t->wait_kind == WAIT_THREAD) {
            fprintf(out, " 0x%X", t->wait_id);
        }
        fprintf(out, " ra=0x%08X sp=0x%08X\n", t->ctx.r[PSP_REG_RA], t->ctx.r[PSP_REG_SP]);
    }
    for (int i = 0; i < MAX_ALARMS; i++)
        if (g_alarm[i].used)
            fprintf(out, "  alarm 0x%X handler=0x%08X due in %lld us\n", g_alarm[i].uid,
                    g_alarm[i].handler, (long long)(g_alarm[i].fire_at - g_now));
}

/* ---- threads ------------------------------------------------------------- */

static void hle_CreateThread(void) {
    psp_thread *t = NULL;
    for (int i = 0; i < MAX_THREADS; i++) if (!g_thread[i].used) { t = &g_thread[i]; break; }
    if (!t) { psp_ret(SCE_KERNEL_ERROR_NO_MEMORY); return; }

    memset(t, 0, sizeof *t);
    psp_str(psp_arg(0), t->name, sizeof t->name);
    t->entry      = psp_arg(1);
    t->priority   = psp_arg(2);
    t->stack_size = psp_arg(3);
    t->attr       = psp_arg(4);
    if (t->stack_size < 0x1000) t->stack_size = 0x1000;
    t->stack_base = psp_sysmem_alloc(t->stack_size, 1);
    if (!t->stack_base) { psp_ret(SCE_KERNEL_ERROR_NO_MEMORY); return; }

    t->uid = g_next_uid++;
    t->state = TH_DORMANT;
    t->used = 1;
    psp_ret(t->uid);
}

/* Guest stack pointer: top of the allocation, 16-byte aligned, with headroom. */

static void hle_StartThread(void) {
    psp_thread *t = find_thread(psp_arg(0));
    if (!t) { psp_ret(SCE_KERNEL_ERROR_ILLEGAL_THID); return; }

    /* The firmware copies the argument block onto the new thread's stack and
     * hands it that copy. Callers rely on it: PES6 passes the address of a
     * local, which is long gone (and reused) by the time the thread first
     * runs under a cooperative scheduler. */
    t->arg_len = psp_arg(1);
    t->arg_ptr = psp_arg(2);
    t->stack_top = (t->stack_base + t->stack_size - 64) & ~15u;
    if (t->arg_len && t->arg_ptr && t->arg_len < t->stack_size / 2) {
        uint32_t copy = (t->stack_top - t->arg_len) & ~15u;
        for (uint32_t i = 0; i < t->arg_len; i++)
            psp_write8(copy + i, psp_read8(t->arg_ptr + i));
        t->arg_ptr = copy;
        t->stack_top = copy - 16;
    }
    t->ctx = psp_cpu;                 /* inherit $gp etc. from the creator */
    t->state = TH_DORMANT;

    if (!t->host_stack) t->host_stack = host_stack_alloc();
    if (!t->host_stack) { psp_ret(SCE_KERNEL_ERROR_NO_MEMORY); return; }
#ifdef HOST_CTX_ASM
    {   /* A frame psp_ctx_switch will "return" from, into thread_boot. */
        uint64_t *sp = (uint64_t *)(((uintptr_t)t->host_stack + TH_HOST_STACK) & ~(uintptr_t)15) - 22;
        memset(sp, 0, 22 * sizeof *sp);
        sp[11] = (uint64_t)(uintptr_t)thread_boot;        /* x30 */
        t->uctx.sp = sp;
    }
#else
    getcontext(&t->uctx);
    t->uctx.uc_stack.ss_sp   = t->host_stack;
    t->uctx.uc_stack.ss_size = TH_HOST_STACK;
    t->uctx.uc_link          = &g_base;
    makecontext(&t->uctx, thread_main, 0);
#endif

    t->state = TH_READY;
    psp_ret(SCE_KERNEL_ERROR_OK);
}

static void hle_ExitThread(void) {
    uint32_t status = psp_arg(0);
    if (g_current) {
        g_current->exit_status = status;
        g_current->state = TH_DORMANT;
        yield_to_base();      /* never returns: the scheduler won't re-run us */
    }
    psp_ret(SCE_KERNEL_ERROR_OK);
}

static void hle_DeleteThread(void) {
    psp_thread *t = find_thread(psp_arg(0));
    if (!t) { psp_ret(SCE_KERNEL_ERROR_ILLEGAL_THID); return; }
    if (t->stack_base) psp_sysmem_release(t->stack_base);
    if (t->host_stack) { host_stack_free(t->host_stack); t->host_stack = NULL; }
    t->used = 0;
    psp_ret(SCE_KERNEL_ERROR_OK);
}

/* No preemption and no time, so a delay is a voluntary yield: let any peers
 * run, then continue. If none are runnable we resume immediately. */
static void hle_DelayThread(void) {
    psp_thread_sleep_usec(psp_arg(0));
    psp_ret(SCE_KERNEL_ERROR_OK);
}

static void hle_WaitThreadEnd(void) {
    psp_thread *t = find_thread(psp_arg(0));
    if (!t) { psp_ret(SCE_KERNEL_ERROR_ILLEGAL_THID); return; }
    while (t->state != TH_DORMANT) {
        if (!g_current) break;
        g_current->wait_kind = WAIT_THREAD;
        g_current->wait_id = t->uid;
        g_current->state = TH_BLOCKED;
        yield_to_base();
    }
    uint32_t out = psp_arg(1);
    if (out) psp_write32(out, t->exit_status);
    psp_ret(SCE_KERNEL_ERROR_OK);
}

static void hle_GetThreadId(void) { psp_ret(g_current ? g_current->uid : 0); }

static void hle_SuspendThread(void) {
    psp_thread *t = find_thread(psp_arg(0));
    if (!t) { psp_ret(SCE_KERNEL_ERROR_ILLEGAL_THID); return; }
    if (t->state != TH_DORMANT) t->state = TH_BLOCKED, t->wait_kind = WAIT_NONE;
    psp_ret(SCE_KERNEL_ERROR_OK);
}

static void hle_ResumeThread(void) {
    psp_thread *t = find_thread(psp_arg(0));
    if (!t) { psp_ret(SCE_KERNEL_ERROR_ILLEGAL_THID); return; }
    if (t->state == TH_BLOCKED && t->wait_kind == WAIT_NONE) t->state = TH_READY;
    psp_ret(SCE_KERNEL_ERROR_OK);
}

static void hle_ChangeThreadPriority(void) {
    psp_thread *t = find_thread(psp_arg(0));
    if (!t) { psp_ret(SCE_KERNEL_ERROR_ILLEGAL_THID); return; }
    t->priority = psp_arg(1);
    psp_ret(SCE_KERNEL_ERROR_OK);
}

static void hle_GetThreadCurrentPriority(void) {
    psp_ret(g_current ? g_current->priority : 0);
}

static void hle_ChangeCurrentThreadAttr(void) {
    if (g_current) g_current->attr = (g_current->attr & ~psp_arg(0)) | psp_arg(1);
    psp_ret(SCE_KERNEL_ERROR_OK);
}

static void hle_GetThreadStackFreeSize(void) {
    psp_thread *t = find_thread(psp_arg(0));
    psp_ret(t ? t->stack_size : (g_current ? g_current->stack_size : 0));
}

/* ---- semaphores ---------------------------------------------------------- */

static void hle_CreateSema(void) {
    psp_sema *s = NULL;
    for (int i = 0; i < MAX_SEMAS; i++) if (!g_sema[i].used) { s = &g_sema[i]; break; }
    if (!s) { psp_ret(SCE_KERNEL_ERROR_NO_MEMORY); return; }
    memset(s, 0, sizeof *s);
    psp_str(psp_arg(0), s->name, sizeof s->name);
    s->count     = (int32_t)psp_arg(2);
    s->max_count = (int32_t)psp_arg(3);
    s->uid = g_next_uid++;
    s->used = 1;
    psp_ret(s->uid);
}

static void hle_DeleteSema(void) {
    psp_sema *s = find_sema(psp_arg(0));
    if (!s) { psp_ret(SCE_KERNEL_ERROR_UNKNOWN_UID); return; }
    s->used = 0;
    psp_ret(SCE_KERNEL_ERROR_OK);
}

static void hle_SignalSema(void) {
    psp_sema *s = find_sema(psp_arg(0));
    if (!s) { psp_ret(SCE_KERNEL_ERROR_UNKNOWN_UID); return; }
    s->count += (int32_t)psp_arg(1);
    if (s->max_count > 0 && s->count > s->max_count) s->count = s->max_count;
    wake_waiters(s->uid, WAIT_SEMA);
    psp_ret(SCE_KERNEL_ERROR_OK);
}

static void hle_WaitSema(void) {
    psp_sema *s = find_sema(psp_arg(0));
    if (!s) { psp_ret(SCE_KERNEL_ERROR_UNKNOWN_UID); return; }
    int32_t need = (int32_t)psp_arg(1);
    while (s->count < need) {
        if (!g_current) {                 /* no thread context: cannot block */
            psp_ret(SCE_KERNEL_ERROR_WAIT_TIMEOUT);
            return;
        }
        g_current->wait_kind = WAIT_SEMA;
        g_current->wait_id = s->uid;
        g_current->state = TH_BLOCKED;
        yield_to_base();
        /* Resumed: whoever woke us signalled, so re-check the count. */
    }
    s->count -= need;
    psp_ret(SCE_KERNEL_ERROR_OK);
}

/* ---- event flags --------------------------------------------------------- */

#define PSP_EVENT_WAITOR   0x01
#define PSP_EVENT_WAITCLEAR 0x20

static void hle_CreateEventFlag(void) {
    psp_evflag *f = NULL;
    for (int i = 0; i < MAX_FLAGS; i++) if (!g_flag[i].used) { f = &g_flag[i]; break; }
    if (!f) { psp_ret(SCE_KERNEL_ERROR_NO_MEMORY); return; }
    memset(f, 0, sizeof *f);
    psp_str(psp_arg(0), f->name, sizeof f->name);
    f->pattern = psp_arg(2);
    f->uid = g_next_uid++;
    f->used = 1;
    psp_ret(f->uid);
}

static void hle_DeleteEventFlag(void) {
    psp_evflag *f = find_flag(psp_arg(0));
    if (!f) { psp_ret(SCE_KERNEL_ERROR_UNKNOWN_UID); return; }
    f->used = 0;
    psp_ret(SCE_KERNEL_ERROR_OK);
}

static void hle_SetEventFlag(void) {
    psp_evflag *f = find_flag(psp_arg(0));
    if (!f) { psp_ret(SCE_KERNEL_ERROR_UNKNOWN_UID); return; }
    f->pattern |= psp_arg(1);
    wake_waiters(f->uid, WAIT_FLAG);
    psp_ret(SCE_KERNEL_ERROR_OK);
}

static void hle_ClearEventFlag(void) {
    psp_evflag *f = find_flag(psp_arg(0));
    if (!f) { psp_ret(SCE_KERNEL_ERROR_UNKNOWN_UID); return; }
    /* The argument is a mask of bits to KEEP. */
    f->pattern &= psp_arg(1);
    psp_ret(SCE_KERNEL_ERROR_OK);
}

static void hle_WaitEventFlag(void) {
    psp_evflag *f = find_flag(psp_arg(0));
    if (!f) { psp_ret(SCE_KERNEL_ERROR_UNKNOWN_UID); return; }
    uint32_t bits = psp_arg(1);
    uint32_t mode = psp_arg(2);
    uint32_t out  = psp_arg(3);

    for (;;) {
        int satisfied = (mode & PSP_EVENT_WAITOR)
                      ? (f->pattern & bits) != 0
                      : (f->pattern & bits) == bits;
        if (out) psp_write32(out, f->pattern);
        if (satisfied) break;
        if (!g_current) { psp_ret(SCE_KERNEL_ERROR_WAIT_TIMEOUT); return; }
        g_current->wait_kind = WAIT_FLAG;
        g_current->wait_id = f->uid;
        g_current->state = TH_BLOCKED;
        yield_to_base();
    }
    if (mode & PSP_EVENT_WAITCLEAR) f->pattern &= ~bits;
    psp_ret(SCE_KERNEL_ERROR_OK);
}

/* ---- callbacks ----------------------------------------------------------- */

static void hle_ok_ret(void) { psp_ret(SCE_KERNEL_ERROR_OK); }

/* sceKernelSetAlarm(usec, handler, common) -> alarm uid. Fired by the
 * scheduler (alarm_fire). PES6 paces its sound thread with one: the handler
 * signals a semaphore and returns 12500 to re-arm itself. */
static void hle_SetAlarm(void) {
    for (int i = 0; i < MAX_ALARMS; i++) {
        psp_alarm *a = &g_alarm[i];
        if (a->used) continue;
        a->uid = g_next_uid++;
        a->handler = psp_arg(1);
        a->arg = psp_arg(2);
        a->fire_at = g_now + psp_arg(0);
        a->used = 1;
        psp_ret(a->uid);
        return;
    }
    psp_ret(SCE_KERNEL_ERROR_NO_MEMORY);
}

static void hle_CancelAlarm(void) {
    for (int i = 0; i < MAX_ALARMS; i++)
        if (g_alarm[i].used && g_alarm[i].uid == psp_arg(0)) { g_alarm[i].used = 0; psp_ret(0); return; }
    psp_ret(SCE_KERNEL_ERROR_UNKNOWN_UID);
}

static void hle_CreateCallback(void) {
    psp_callback *c = NULL;
    for (int i = 0; i < MAX_CBS; i++) if (!g_cb[i].used) { c = &g_cb[i]; break; }
    if (!c) { psp_ret(SCE_KERNEL_ERROR_NO_MEMORY); return; }
    memset(c, 0, sizeof *c);
    psp_str(psp_arg(0), c->name, sizeof c->name);
    c->func = psp_arg(1);
    c->arg  = psp_arg(2);
    c->uid  = g_next_uid++;
    c->owner = g_current ? g_current->uid : 0;
    c->used = 1;
    psp_ret(c->uid);
}

/* Callbacks (PES6PORT). A notification is only recorded here; the handler
 * runs later on the thread that created the callback, when that thread
 * reaches a callback-processing point (sceKernelCheckCallback or a *CB wait)
 * -- which is the firmware's contract and what games are written against.
 * PES6 learns the disc is ready through one: its loader does not start until
 * the UMD callback has reported the drive state. */
void psp_callback_notify(uint32_t uid, uint32_t arg) {
    for (int i = 0; i < MAX_CBS; i++)
        if (g_cb[i].used && g_cb[i].uid == uid) {
            g_cb[i].pending++;
            g_cb[i].notify_arg = arg;
            return;
        }
}

int psp_callbacks_run(void) {
    int ran = 0;
    if (!g_current) return 0;
    for (int i = 0; i < MAX_CBS; i++) {
        psp_callback *c = &g_cb[i];
        if (!c->used || !c->pending || c->running) continue;
        if (c->owner && c->owner != g_current->uid) continue;
        int count = c->pending;
        c->pending = 0;
        c->running = 1;
        /* Run the handler as a nested call on this thread: its own frame
         * below the interrupted one, and every register restored after. */
        psp_cpu_state saved = psp_cpu;
        psp_cpu.r[PSP_REG_A0] = (uint32_t)count;
        psp_cpu.r[PSP_REG_A1] = c->notify_arg;
        psp_cpu.r[PSP_REG_A2] = c->arg;
        psp_cpu.r[PSP_REG_SP] = (saved.r[PSP_REG_SP] - 0x200) & ~15u;
        psp_cpu.r[PSP_REG_RA] = 0;
        psp_dispatch(c->func);
        psp_cpu = saved;
        c->running = 0;
        ran = 1;
    }
    return ran;
}

/* ---- PES6PORT additions -------------------------------------------------- */

void psp_thread_yield(void) {
    if (!g_current) return;
    g_current->state = TH_READY;
    yield_to_base();
}

/* Deliver pending callbacks; also a yield point, since a loop that polls
 * here expects other threads to make progress in between. */
static void hle_CheckCallback(void) {
    int ran = psp_callbacks_run();
    psp_thread_yield();
    psp_ret((uint32_t)ran);
}

static void hle_DeleteCallback(void) {
    for (int i = 0; i < MAX_CBS; i++)
        if (g_cb[i].used && g_cb[i].uid == psp_arg(0)) { g_cb[i].used = 0; psp_ret(0); return; }
    psp_ret(SCE_KERNEL_ERROR_UNKNOWN_UID);
}

static void hle_TerminateDeleteThread(void) {
    psp_thread *t = find_thread(psp_arg(0));
    if (!t) { psp_ret(SCE_KERNEL_ERROR_ILLEGAL_THID); return; }
    if (t == g_current) { psp_ret(0x800201A2u); return; }  /* ILLEGAL_THID-ish: self */
    t->state = TH_DORMANT;
    wake_waiters(t->uid, WAIT_THREAD);
    if (t->stack_base) psp_sysmem_release(t->stack_base);
    /* The host stack may still hold the terminated thread's frames; it is never
     * resumed, so freeing it is safe. */
    if (t->host_stack) { host_stack_free(t->host_stack); t->host_stack = NULL; }
    t->used = 0;
    psp_ret(SCE_KERNEL_ERROR_OK);
}

/* sceKernelSleepThread / sceKernelWakeupThread. A wakeup that arrives while
 * the target is not asleep is counted, and the next sleep consumes it instead
 * of blocking -- without the count a wakeup racing ahead of its sleep is lost
 * and the sleeper never runs again. PES6 parks worker threads this way. */
static void hle_SleepThread(void) {
    psp_callbacks_run();
    if (g_current) {
        if (g_current->wakeups > 0) g_current->wakeups--;
        else {
            g_current->wait_kind = WAIT_SLEEP;
            g_current->state = TH_BLOCKED;
            yield_to_base();
        }
    }
    psp_ret(SCE_KERNEL_ERROR_OK);
}

static void hle_WakeupThread(void) {
    psp_thread *t = find_thread(psp_arg(0));
    if (!t) { psp_ret(SCE_KERNEL_ERROR_ILLEGAL_THID); return; }
    if (t->state == TH_BLOCKED && t->wait_kind == WAIT_SLEEP) {
        t->wait_kind = WAIT_NONE;
        t->state = TH_READY;
    } else {
        t->wakeups++;
    }
    psp_ret(SCE_KERNEL_ERROR_OK);
}

/* SceKernelSemaInfo: size, name[32], attr, initCount, currentCount, maxCount,
 * numWaitThreads. */
static void hle_ReferSemaStatus(void) {
    psp_sema *s = find_sema(psp_arg(0));
    uint32_t p = psp_arg(1);
    if (!s) { psp_ret(SCE_KERNEL_ERROR_UNKNOWN_UID); return; }
    if (p) {
        int waiting = 0;
        for (int i = 0; i < MAX_THREADS; i++)
            if (g_thread[i].used && g_thread[i].state == TH_BLOCKED &&
                g_thread[i].wait_kind == WAIT_SEMA && g_thread[i].wait_id == s->uid) waiting++;
        psp_write32(p, 56);
        for (int i = 0; i < 32; i++) psp_write8(p + 4 + i, (uint8_t)s->name[i]);
        psp_write32(p + 36, 0);
        psp_write32(p + 40, (uint32_t)s->count);
        psp_write32(p + 44, (uint32_t)s->count);
        psp_write32(p + 48, (uint32_t)s->max_count);
        psp_write32(p + 52, (uint32_t)waiting);
    }
    psp_ret(SCE_KERNEL_ERROR_OK);
}

/* SceKernelEventFlagInfo: size, name[32], attr, initPattern, currentPattern,
 * numWaitThreads. */
static void hle_ReferEventFlagStatus(void) {
    psp_evflag *f = find_flag(psp_arg(0));
    uint32_t p = psp_arg(1);
    if (!f) { psp_ret(SCE_KERNEL_ERROR_UNKNOWN_UID); return; }
    if (p) {
        psp_write32(p, 52);
        for (int i = 0; i < 32; i++) psp_write8(p + 4 + i, (uint8_t)f->name[i]);
        psp_write32(p + 36, 0);
        psp_write32(p + 40, f->pattern);
        psp_write32(p + 44, f->pattern);
        psp_write32(p + 48, 0);
    }
    psp_ret(SCE_KERNEL_ERROR_OK);
}

void psp_threadman_register(void) {
    psp_hle_register(0x6D212BAC, "ThreadManForUser", "sceKernelWaitSemaCB",              hle_WaitSema);
    psp_hle_register(0x349D6D6C, "ThreadManForUser", "sceKernelCheckCallback",           hle_CheckCallback);
    psp_hle_register(0xEDBA5844, "ThreadManForUser", "sceKernelDeleteCallback",          hle_DeleteCallback);
    psp_hle_register(0x383F7BCC, "ThreadManForUser", "sceKernelTerminateDeleteThread",   hle_TerminateDeleteThread);
    psp_hle_register(0xD59EAD2F, "ThreadManForUser", "sceKernelWakeupThread",            hle_WakeupThread);
    psp_hle_register(0x9ACE131E, "ThreadManForUser", "sceKernelSleepThread",             hle_SleepThread);
    psp_hle_register(0x82826F70, "ThreadManForUser", "sceKernelSleepThreadCB",           hle_SleepThread);
    psp_hle_register(0xBC6FEBC5, "ThreadManForUser", "sceKernelReferSemaStatus",         hle_ReferSemaStatus);
    psp_hle_register(0xA66B0120, "ThreadManForUser", "sceKernelReferEventFlagStatus",    hle_ReferEventFlagStatus);
    psp_hle_register(0x446D8DE6, "ThreadManForUser", "sceKernelCreateThread",            hle_CreateThread);
    psp_hle_register(0xF475845D, "ThreadManForUser", "sceKernelStartThread",             hle_StartThread);
    psp_hle_register(0xAA73C935, "ThreadManForUser", "sceKernelExitThread",              hle_ExitThread);
    psp_hle_register(0x9FA03CD3, "ThreadManForUser", "sceKernelDeleteThread",            hle_DeleteThread);
    psp_hle_register(0xCEADEB47, "ThreadManForUser", "sceKernelDelayThread",             hle_DelayThread);
    psp_hle_register(0x68DA9E36, "ThreadManForUser", "sceKernelDelayThreadCB",           hle_DelayThread);
    psp_hle_register(0x278C0DF5, "ThreadManForUser", "sceKernelWaitThreadEnd",           hle_WaitThreadEnd);
    psp_hle_register(0x293B45B8, "ThreadManForUser", "sceKernelGetThreadId",             hle_GetThreadId);
    psp_hle_register(0x9944F31F, "ThreadManForUser", "sceKernelSuspendThread",           hle_SuspendThread);
    psp_hle_register(0x75156E8F, "ThreadManForUser", "sceKernelResumeThread",            hle_ResumeThread);
    psp_hle_register(0x71BC9871, "ThreadManForUser", "sceKernelChangeThreadPriority",    hle_ChangeThreadPriority);
    psp_hle_register(0x94AA61EE, "ThreadManForUser", "sceKernelGetThreadCurrentPriority",hle_GetThreadCurrentPriority);
    psp_hle_register(0xEA748E31, "ThreadManForUser", "sceKernelChangeCurrentThreadAttr", hle_ChangeCurrentThreadAttr);
    psp_hle_register(0x52089CA1, "ThreadManForUser", "sceKernelGetThreadStackFreeSize",  hle_GetThreadStackFreeSize);

    psp_hle_register(0xD6DA4BA1, "ThreadManForUser", "sceKernelCreateSema",              hle_CreateSema);
    psp_hle_register(0x28B6489C, "ThreadManForUser", "sceKernelDeleteSema",              hle_DeleteSema);
    psp_hle_register(0x3F53E640, "ThreadManForUser", "sceKernelSignalSema",              hle_SignalSema);
    psp_hle_register(0x4E3A1105, "ThreadManForUser", "sceKernelWaitSema",                hle_WaitSema);

    psp_hle_register(0x55C20A00, "ThreadManForUser", "sceKernelCreateEventFlag",         hle_CreateEventFlag);
    psp_hle_register(0xEF9E4C70, "ThreadManForUser", "sceKernelDeleteEventFlag",         hle_DeleteEventFlag);
    psp_hle_register(0x1FB15A32, "ThreadManForUser", "sceKernelSetEventFlag",            hle_SetEventFlag);
    psp_hle_register(0x812346E4, "ThreadManForUser", "sceKernelClearEventFlag",          hle_ClearEventFlag);
    psp_hle_register(0x402FCF22, "ThreadManForUser", "sceKernelWaitEventFlag",           hle_WaitEventFlag);

    psp_hle_register(0xE81CAF8F, "ThreadManForUser", "sceKernelCreateCallback",          hle_CreateCallback);

    psp_hle_register(0x6652B8CA, "ThreadManForUser", "sceKernelSetAlarm",  hle_SetAlarm);
    psp_hle_register(0x7E65B999, "ThreadManForUser", "sceKernelCancelAlarm", hle_CancelAlarm);
}
