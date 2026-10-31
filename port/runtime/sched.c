/* Time, interrupts and guest threads.
 *
 * Time: generated code calls rc_tick on backward branches, which moves the clock on a little; Halt and IntrWait jump
 * it to the next interrupt. Scanlines, VBlank and timers follow the clock.
 *
 * Interrupts: delivered at those points (and when the game re-enables them) the way the ARM9 BIOS does: switch to
 * IRQ mode, push r0-r3, r12, lr, call the handler at DTCM+0x3FFC, pop, return to the interrupted code.
 *
 * Threads: each guest thread runs on its own host fiber. The SDK switches threads by loading another thread's
 * registers and returning from an exception (OS_LoadContext, or the IRQ handler's thread switch at 0x01ff8ce0). After
 * every such return, rc_thread_check looks at the SDK's current-thread pointer and moves to that thread's fiber. */
#include "internal.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdlib.h>

uint64_t rc_now, rc_frames;
uint16_t rc_vcount;
static uint64_t line_end = RC_LINE_CYCLES;
static void adopt(void);

/* ------------------------------------------------------------------ timers */

static struct { uint32_t counter; uint64_t acc; uint16_t ctl; } tm[4];
static const int presc_shift[4] = {0, 6, 8, 10};

static void timer_add(int n, uint64_t ticks) {
    uint32_t reload = io16(0x100 + 4 * n), period = 0x10000 - reload;
    uint64_t c = tm[n].counter + ticks;
    if (c < 0x10000) { tm[n].counter = (uint32_t)c; return; }
    uint64_t excess = c - 0x10000, overflows = 1 + excess / period;
    tm[n].counter = reload + (uint32_t)(excess % period);
    if (tm[n].ctl & 0x40) rc_raise_irq(1u << (3 + n));
    if (n < 3 && (tm[n + 1].ctl & 0x84) == 0x84) timer_add(n + 1, overflows);
}

static void timers_advance(uint64_t dt) {
    for (int n = 0; n < 4; n++) {
        if (!(tm[n].ctl & 0x80) || (n > 0 && (tm[n].ctl & 4))) continue;
        int sh = presc_shift[tm[n].ctl & 3];
        tm[n].acc += dt;
        uint64_t ticks = tm[n].acc >> sh;
        tm[n].acc &= (1ull << sh) - 1;
        if (ticks) timer_add(n, ticks);
    }
}

uint16_t rc_timer_read(int n) { return (uint16_t)tm[n].counter; }

void rc_timer_write(int n, uint32_t off) {
    (void)off;
    uint16_t ctl = io16(0x102 + 4 * n);
    if ((ctl & 0x80) && !(tm[n].ctl & 0x80)) { tm[n].counter = io16(0x100 + 4 * n); tm[n].acc = 0; }
    tm[n].ctl = ctl;
}

/* ------------------------------------------------------------------ video timing */

void rc_dma_event(uint32_t timing);

static void scanline(void) {
    rc_vcount = (uint16_t)((rc_vcount + 1) % 263);
    uint16_t stat = io16(4) & ~7u;
    uint16_t match = (uint16_t)((stat >> 8) | ((stat & 0x80) << 1));
    if (rc_vcount >= 192 && rc_vcount < 262) stat |= 1;
    if (rc_vcount == match) { stat |= 4; if (stat & 0x20) rc_raise_irq(4); }
    if (rc_vcount < 192 && (stat & 0x10)) rc_raise_irq(2);
    set_io16(4, stat);
    set_io16(6, rc_vcount);
    if (rc_vcount < 192) rc_dma_event(2);
    if (rc_vcount == 192) {
        if (stat & 8) rc_raise_irq(1);
        rc_dma_event(1);
        gpu3d_swap();
        rc_frames++;
        arm7_vblank();
        fe_vblank();
    }
}

void rc_advance(uint64_t cycles) {
    uint64_t target = rc_now + cycles;
    while (line_end <= target) {
        timers_advance(line_end - rc_now);
        rc_now = line_end;
        line_end += RC_LINE_CYCLES;
        scanline();
    }
    timers_advance(target - rc_now);
    rc_now = target;
}

/* ------------------------------------------------------------------ interrupts */

void rc_raise_irq(uint32_t bits) { set_io32(0x214, io32(0x214) | bits); }

static uint32_t pending(void) {
    if (arm7_fifo_irq_pending()) rc_raise_irq(1u << 18);
    /* The geometry FIFO runs commands at once, so it is always empty: its "less than half full" and "empty"
     * interrupt conditions (GXSTAT bits 30-31 = 1 or 2) always hold */
    uint32_t gx = io32(0x600) >> 30;
    if (gx == 1 || gx == 2) rc_raise_irq(1u << 21);
    return io32(0x210) & io32(0x214);
}

static int in_irq;

static void bios_irq(Cpu *c, uint32_t resume) {
    uint32_t cpsr = rc_get_cpsr(c);
    rc_set_cpsr(c, (cpsr & ~0xffu) | 0x92);           /* IRQ mode, IRQs off, ARM state */
    c->spsr = cpsr;
    c->r[14] = resume + 4;
    /* BIOS: stmfd sp!, {r0-r3, r12, lr}; handler address at the top of DTCM; returns to the BIOS */
    uint32_t sp = c->r[13] - 24;
    wr32(sp, c->r[0]); wr32(sp + 4, c->r[1]); wr32(sp + 8, c->r[2]); wr32(sp + 12, c->r[3]);
    wr32(sp + 16, c->r[12]); wr32(sp + 20, c->r[14]);
    c->r[13] = sp;
    c->r[0] = rc_dtcm_base + 0x4000;
    c->r[14] = 0xffff0290;
    in_irq++;
    rc_call(c, rd32(rc_dtcm_base + 0x3ffc));
    in_irq--;
    /* BIOS: ldmfd sp!, {r0-r3, r12, lr}; subs pc, lr, #4. The SDK's thread switch may have rewritten the frame. */
    sp = c->r[13];
    c->r[0] = rd32(sp); c->r[1] = rd32(sp + 4); c->r[2] = rd32(sp + 8); c->r[3] = rd32(sp + 12);
    c->r[12] = rd32(sp + 16); c->r[14] = rd32(sp + 20);
    c->r[13] = sp + 24;
    uint32_t target = c->r[14] - 4;
    rc_set_cpsr(c, c->spsr);
    rc_thread_check(c, target);
}

void rc_irq_check(Cpu *c, uint32_t resume_pc) {
    adopt();
    if (in_irq || !(io32(0x208) & 1) || (c->ctl & 0x80) || !pending()) return;
    bios_irq(c, resume_pc);
}

void rc_halt(Cpu *c, uint32_t resume_pc) {
    for (uint32_t lines = 0; !pending(); lines++) {
        if (lines > 263u * 600) rc_fatal(c, "halted for 10 s with no interrupt enabled to wake up (IE %08x)",
                                        io32(0x210));
        rc_advance(line_end - rc_now);
    }
    rc_irq_check(c, resume_pc);
}

static uint32_t last_tick_addr, same_tick;
static uint64_t ticks;

void rc_tick(Cpu *c, uint32_t addr) {
    c->budget = 256;
    adopt();
    rc_advance(1024);                       /* 256 backward branches: about 8 ARM9 cycles a loop iteration */
    arm7_tick();
    if (++ticks % 200000 == 0)
        fprintf(stderr, "tick %llu: frame %llu, loop at %08x\n", (unsigned long long)ticks,
                (unsigned long long)rc_frames, addr);
    same_tick = addr == last_tick_addr ? same_tick + 1 : 0;
    last_tick_addr = addr;
    if (same_tick == 5000000) rc_fatal(c, "stuck in the loop at %08x", addr);
    rc_irq_check(c, addr);
}

/* Interrupts that became deliverable because the game enabled them (msr clearing I, IME/IE writes) */
void rc_irq_enabled(Cpu *c) { rc_irq_check(c, 0); }

/* ------------------------------------------------------------------ threads */

/* W2 (JP): OSi_ThreadInfo, whose word at +8 points to the current-thread variable; a thread's saved PC is at +0x40 */
#define OS_THREAD_INFO 0x0214bc10u
#define CTX_PC 0x40u

typedef struct {
    uint32_t thread;          /* guest OSThread address, 0 before the SDK's thread system starts */
    void *fiber;
    uint32_t resume_pc;       /* where this thread continues when the SDK switches back to it */
    uint32_t start_pc;
} GThread;

static GThread threads[128];
static int nthreads;
static GThread *cur;

uint32_t rc_current_thread(void) { return cur ? cur->thread : 0; }

static uint32_t guest_current(void) {
    uint32_t p = rd32(OS_THREAD_INFO + 8);
    return p ? rd32(p) : 0;
}

/* The boot fiber is whichever thread the SDK makes current first (the launcher thread). Called often, so that's
 * settled before the first switch replaces the current pointer. */
static void adopt(void) {
    if (!cur->thread) cur->thread = guest_current();
}

static void WINAPI fiber_main(void *param) {
    GThread *g = param;
    Cpu *c = &rc_cpu;
    uint32_t pc = g->start_pc;
    /* A thread function that returns goes on to whatever its lr held (OS_ExitThread) */
    for (;;) {
        c->ret_to = 0;
        rc_call(c, pc);
        pc = c->ret_to;
        if (!pc) rc_fatal(c, "thread %08x ran off its end", g->thread);
    }
}

void rc_thread_check(Cpu *c, uint32_t target) {
    uint32_t t = guest_current();
    if (!t || t == cur->thread) return;
    if (!cur->thread) { cur->thread = t; return; }    /* no switch seen before the first one: best guess */
    cur->resume_pc = rd32(cur->thread + CTX_PC) - 4;
    GThread *n = NULL;
    for (int i = 0; i < nthreads; i++) if (threads[i].thread == t) n = &threads[i];
    int restarted = 0;
    if (!n || n->resume_pc != target) {
        /* Never run, reused for a new thread, or never saved (the SDK doesn't save the idle thread, state 2): start a
         * fiber at the target. The old fiber can't be resumed any more, so it goes. */
        if (!n) {
            if (nthreads == 128) rc_fatal(c, "more than 128 guest threads");
            n = &threads[nthreads++];
        } else if (n->fiber && n != &threads[0]) {
            restarted = n->start_pc == target;
            DeleteFiber(n->fiber);
        }
        n->thread = t;
        n->start_pc = target;
        n->fiber = CreateFiberEx(64 << 10, 64 << 20, 0, fiber_main, n);
        if (!n->fiber) rc_fatal(c, "CreateFiber failed");
        if (!restarted) rc_logf("thread %08x starts at %08x\n", t, target);
    }
    if (getenv("B2W2_LOG_THREADS") && (rc_get_user_reg(c, 13) >= 0x02fe3f80u || n->start_pc == target && !restarted))
        rc_logf("switch %08x -> %08x at %08x (frame %llu) sp %08x cpsr %08x user sp %08x\n", cur->thread, t, target,
                (unsigned long long)rc_frames, c->r[13], rc_get_cpsr(c), rc_get_user_reg(c, 13));
    cur = n;
    SwitchToFiber(n->fiber);
}

/* Host watchdog: reports progress every 2 s, so hangs without loops (no ticks) still show where they are */
volatile uint32_t rc_last_call;
const char *rc_last_overlay(void);
static DWORD WINAPI watchdog(void *p) {
    (void)p;
    uint64_t last = 0;
    for (int n = 0;; n++) {
        Sleep(100);
        if (getenv("B2W2_WATCHDOG")[0] == '2')
            fprintf(stderr, "  sample: lr %08x r0 %08x sp %08x last call %08x\n", rc_cpu.r[14], rc_cpu.r[0], rc_cpu.r[13],
                    rc_last_call);
        if (n % 20) continue;
        fprintf(stderr, "[watchdog] frame %llu (+%llu), ticks %llu, last loop %08x, last call %08x (%s), thread %08x\n",
                (unsigned long long)rc_frames, (unsigned long long)(rc_frames - last), (unsigned long long)ticks,
                last_tick_addr, rc_last_call, rc_last_overlay(), cur ? cur->thread : 0);
        last = rc_frames;
    }
}

void rc_sched_init(void) {
    if (getenv("B2W2_WATCHDOG")) CreateThread(NULL, 0, watchdog, NULL, 0, NULL);
    threads[0].fiber = ConvertThreadToFiber(NULL);
    nthreads = 1;
    cur = &threads[0];
}
