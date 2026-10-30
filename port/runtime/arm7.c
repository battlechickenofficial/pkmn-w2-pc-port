/* High-level stand-in for the ARM7. The ARM7 program is stock TWL SDK code (sound, touch, RTC, wireless, power);
 * instead of running it, this file answers the ARM9 the way it would. Each behaviour notes the ARM7 code it mimics. */
#include "runtime.h"

/* Boot handshake through shared main RAM, as in both crt0s (ARM9 0x02004800, ARM7 0x02380000):
 *   0x02fffc24  ARM9 counter     0x02fffc26  ARM7 counter     0x02fffc28  boot stage
 * The ARM9 writes stage 1, then each side bumps its own counter until it sees the other's change. The ARM7 then
 * writes stage 4 (strh r2,[r1] in a stub it copies to its stack from 0x023805b4) and jumps to its main in WRAM;
 * the ARM9 waits for that 4 before going on. */
static void boot_handshake(void) {
    if (rd16(0x02fffc28) != 1) return;
    wr16(0x02fffc26, (uint16_t)(rd16(0x02fffc26) + 1));
    wr16(0x02fffc28, 4);
}

/* func_02079c68 syncs the two CPUs through 4 bytes at 0x02fffff0, and each CPU runs it twice, once in each role:
 *   writer (r0 = 0): clears byte 2, writes 0x80 | n to byte 0 until byte 1 echoes it or byte 2 is set, then sets
 *                    byte 3
 *   echoer (r0 = 1): copies byte 0 into byte 1 and adds up what it copied; at 0x300 it clears byte 3, sets byte 2
 *                    and waits for byte 3
 * The ARM9 writes first, so the ARM7 echoes first, then writes. */
static int sync_phase, sync_n;
static void shared_sync(void) {
    switch (sync_phase) {
    case 0:                                     /* ARM9 writes, we echo and finish at once */
        if (!(rd8(0x02fffff0) & 0x80)) return;
        wr8(0x02fffff1, rd8(0x02fffff0));
        wr8(0x02fffff2, 1);
        sync_phase = 1;
        break;
    case 1:                                     /* ARM9 sets byte 3 when it's done writing */
        if (!rd8(0x02fffff3)) return;
        wr8(0x02fffff2, 0);
        sync_phase = 2;
        /* fall through */
    case 2:                                     /* ARM9 echoes, we write until it sets byte 2 */
        if (rd8(0x02fffff2)) { wr8(0x02fffff3, 1); sync_phase = 3; return; }
        wr8(0x02fffff0, (uint8_t)(0x80 | (sync_n++ & 0xf)));
        break;
    }
}

/* PXI_IsCallbackReady (func_0207c754) tests bit <tag> of the ARM7's handler mask at 0x02fffc00 + 0x388 + 4 * proc.
 * The ARM7 sets a bit as it installs the handler for that tag; claim all of them, which commits us to answering
 * every tag's FIFO messages (see arm7_fifo_send). */
static void pxi_handlers_ready(void) {
    wr32(0x02ffff8c, 0xffffffff);
}

void arm7_tick(void) {
    boot_handshake();
    shared_sync();
    pxi_handlers_ready();
}

/* IPCSYNC (0x04000180): bits 0-3 are what the other CPU outputs, bits 8-11 our own output. In PXI init
 * (func_0207c660) the ARM9 echoes the ARM7's value back and finishes once it reads 0 after more than four changes;
 * the ARM7 answers each echo with the next value down, so the sequence keeps moving until then. */
static uint32_t arm7_sync_out;

uint16_t arm7_ipcsync_read(uint16_t stored) {
    return (uint16_t)((stored & ~0xfu) | arm7_sync_out);
}

/* IPC FIFO. Sends from the ARM9 (0x04000188) are taken by the ARM7 at once, so the send side always reads empty;
 * replies queue up for the ARM9 to read at 0x04100000. A PXI message is tag (bits 0-4), error (bit 5), data. */
#include <stdio.h>
extern FILE *rc_log;

static uint32_t recv_q[16];
static int recv_head, recv_count;
static uint16_t fifo_cnt;               /* the writable bits of IPCFIFOCNT: 2, 3, 10, 14, 15 */

static void arm7_reply(uint32_t msg) {
    if (recv_count == 16) { fifo_cnt |= 0x4000; return; }
    recv_q[(recv_head + recv_count++) & 15] = msg;
}

static void arm7_fifo_send(uint32_t msg) {
    uint32_t tag = msg & 0x1f, data = msg >> 6;
    if (rc_log) fprintf(rc_log, "pxi -> arm7: tag %2u data %07x%s\n", tag, data, msg & 0x20 ? " (err)" : "");
}

uint16_t arm7_fifocnt_read(void) {
    return (uint16_t)(fifo_cnt | 0x0001 | (recv_count == 0 ? 0x0100 : 0) | (recv_count == 16 ? 0x0200 : 0));
}

void arm7_fifocnt_write(uint16_t v) {
    if (v & 0x4000) fifo_cnt &= ~0x4000;        /* acknowledge the error flag */
    fifo_cnt = (fifo_cnt & 0x4000) | (v & 0x8404);
}

void arm7_fifo_write(uint32_t v) {
    if (fifo_cnt & 0x8000) arm7_fifo_send(v);
}

uint32_t arm7_fifo_read(void) {
    if (!recv_count) { fifo_cnt |= 0x4000; return 0; }
    uint32_t v = recv_q[recv_head];
    recv_head = (recv_head + 1) & 15;
    recv_count--;
    return v;
}

int arm7_fifo_pending(void) { return recv_count; }

void arm7_ipcsync_write(uint16_t v) {
    arm7_sync_out = (((v >> 8) & 0xf) - 1) & 0xf;
}
