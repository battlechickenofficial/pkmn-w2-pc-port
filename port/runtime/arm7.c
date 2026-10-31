/* High-level stand-in for the ARM7. The ARM7 program is stock TWL SDK code (sound, touch, RTC, wireless, power);
 * instead of running it, this file answers the ARM9 the way it would. Each behaviour notes the ARM7 code it mimics. */
#include "internal.h"
#include <stdlib.h>

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

static uint32_t recv_q[16];
static int recv_head, recv_count;
static uint16_t fifo_cnt;               /* the writable bits of IPCFIFOCNT: 2, 3, 10, 14, 15 */

static void arm7_reply(uint32_t msg) {
    if (recv_count == 16) { fifo_cnt |= 0x4000; return; }
    recv_q[(recv_head + recv_count++) & 15] = msg;
}

static void pxi_reply(uint32_t tag, uint32_t data) {
    arm7_reply(tag | (data << 6));
}

/* ------------------------------------------------------------------ PXI services, by tag (NitroSDK pxi_fifo.h):
 * 3 system, 4 NVRAM, 5 RTC, 6 touch, 7 sound, 8 PM, 9 mic, 10 WM, 11 FS, 12 OS, 13 cartridge, 14 card */

/* SPI-style commands (PM, RTC, touch, mic, NVRAM) arrive as packets: bit 25 starts a command, bit 24 ends it, bits
 * 16-23 count the packets, bits 0-15 carry data; the command is the high byte of the first packet. The ARM9's
 * callbacks (PM: func_0207e928) take a reply of (command << 8) | result, 0 meaning success. */
static uint32_t spi_cmd[32], spi_args[32][16], spi_nargs[32];

/* ------------------------------------------------------------------ firmware (NVRAM) */

/* A DS firmware image with what games read from it: the header's pointer to the user settings, and two copies of
 * the user settings (nickname, birthday, language, touch screen calibration) with their CRC */
static uint8_t firmware[256 << 10];
static int firmware_built;

static uint16_t crc16(uint16_t crc, const uint8_t *p, uint32_t n) {
    while (n--) {
        crc ^= *p++;
        for (int j = 0; j < 8; j++) crc = crc & 1 ? (crc >> 1) ^ 0xa001 : crc >> 1;
    }
    return crc;
}

static void user_settings(uint8_t *u) {
    static const uint16_t name[] = {0x30d7, 0x30ec, 0x30a4, 0x30e4, 0x30fc};   /* "プレイヤー" */
    memset(u, 0, 0x100);
    u[0x00] = 5;                                        /* version */
    u[0x02] = 11;                                       /* favourite colour */
    u[0x03] = 1; u[0x04] = 1;                           /* birthday: January 1 */
    memcpy(u + 0x06, name, sizeof name);
    u[0x1a] = 5;
    /* touch calibration: two ADC points and the screen pixels they map to */
    static const uint8_t cal[12] = {0xdf, 0x02, 0x2c, 0x03, 0x20, 0x20, 0x3b, 0x0d, 0xe7, 0x0c, 0xe0, 0xa0};
    memcpy(u + 0x58, cal, sizeof cal);
    u[0x64] = 0x00;                                     /* language: Japanese */
    u[0x65] = 0xfc;
    u[0x66] = 26;                                       /* year of the settings (2026) */
    u[0x70] = 1;                                        /* update counter */
    uint16_t crc = crc16(0xffff, u, 0x70);
    memcpy(u + 0x72, &crc, 2);
}

static int firmware_dirty;

static void build_firmware(void) {
    if (firmware_built) return;
    firmware_built = 1;
    /* What the game wrote to the firmware last time, if anything */
    FILE *f = fopen("b2w2.fw", "rb");
    if (f) {
        size_t n = fread(firmware, 1, sizeof firmware, f);
        fclose(f);
        if (n == sizeof firmware) return;
    }
    memset(firmware, 0xff, sizeof firmware);
    memset(firmware, 0, 0x200);
    uint16_t us = 0x3fe00 / 8;
    memcpy(firmware + 0x20, &us, 2);
    firmware[0x1d] = 0xff;                              /* console type: DS */
    user_settings(firmware + 0x3fe00);
    user_settings(firmware + 0x3ff00);
    /* Wi-Fi connection settings 1-3 and the block after them: empty, "not set up" (0xe7 = 0xff), CRC-16 with
     * initial value 0 over 0x00-0xfd. Erased flash (all 0xff) makes the game warn that the Wi-Fi user
     * information may have been lost. */
    for (uint32_t blk = 0x3fa00; blk <= 0x3fd00; blk += 0x100) {
        uint8_t *b = firmware + blk;
        memset(b, 0, 0x100);
        b[0xe7] = 0xff;
        uint16_t crc = crc16(0, b, 0xfe);
        memcpy(b + 0xfe, &crc, 2);
    }
}

/* Called at boot: the firmware leaves the user settings at 0x027ffc80 for the game */
void arm7_boot(void) {
    build_firmware();
    for (int i = 0; i < 0x70; i++) wr8(0x027ffc80 + i, firmware[0x3fe00 + i]);
}

/* NVRAM commands (NitroSDK nvram.c): 20 write enable, 21 write disable, 22 read status, 23 read, 25 page write,
 * 26 page program, 27 page erase, 28 sector erase. All start with a 24-bit address (packets 0-1). Reads (6 packets)
 * then give a 32-bit size and the RAM address; writes (5 packets) a 16-bit size and the RAM address. Read status
 * puts the status byte at an address split over three packets. */
static uint8_t nvram_status;                          /* bit 1: write enabled; bit 0 (busy) stays clear */

static void nvram_command(uint32_t cmd, const uint32_t *a, uint32_t n) {
    build_firmware();
    uint32_t addr = (a[0] & 0xff) << 16 | (a[1] & 0xffff), size, ram;
    if (n >= 6) { size = (a[2] & 0xffff) << 16 | (a[3] & 0xffff); ram = (a[4] & 0xffff) << 16 | (a[5] & 0xffff); }
    else { size = a[2] & 0xffff; ram = (a[3] & 0xffff) << 16 | (a[4] & 0xffff); }
    switch (cmd) {
    case 0x22: {
        uint32_t dst = (a[0] & 0xff) << 24 | (a[1] & 0xffff) << 8 | ((a[2] >> 8) & 0xff);
        wr8(dst, nvram_status);
        break;
    }
    case 0x20: nvram_status |= 2; break;
    case 0x21: nvram_status &= ~2; break;
    case 0x23: case 0x24:
        for (uint32_t i = 0; i < size; i++) wr8(ram + i, firmware[(addr + i) % sizeof firmware]);
        break;
    case 0x25: case 0x26:
        for (uint32_t i = 0; i < size; i++) firmware[(addr + i) % sizeof firmware] = rd8(ram + i);
        firmware_dirty = 1;
        nvram_status &= ~2;
        break;
    case 0x27: memset(firmware + (addr & ~0xffu) % sizeof firmware, 0xff, 0x100); firmware_dirty = 1; break;
    case 0x28: memset(firmware + (addr & ~0xffffu) % sizeof firmware, 0xff, 0x10000); firmware_dirty = 1; break;
    default: break;
    }
}

/* ------------------------------------------------------------------ touch screen */

static int touch_down, touch_x, touch_y;
void arm7_touch(int down, int x, int y) { touch_down = down; touch_x = x; touch_y = y; }

/* A sample as the TP callback (func_0207dcdc) reads it from 0x02ffffaa: raw x in bits 0-11, raw y in 12-23, touched
 * in bit 24, validity in 25-26 (0 = valid). Raw values come from the screen position through the firmware's
 * calibration points. */
static void touch_sample(void) {
    int x = touch_x, y = touch_y;
    uint32_t rx = (uint32_t)(0x2df + (x - 0x20) * (0xd3b - 0x2df) / (0xe0 - 0x20));
    uint32_t ry = (uint32_t)(0x32c + (y - 0x20) * (0xce7 - 0x32c) / (0xa0 - 0x20));
    uint32_t w = (rx & 0xfff) | (ry & 0xfff) << 12 | (uint32_t)(touch_down ? 1 : 0) << 24;
    wr16(0x02ffffaa, (uint16_t)w);
    wr16(0x02ffffac, (uint16_t)(w >> 16));
}

/* ------------------------------------------------------------------ clock */

static uint32_t bcd(int v) { return (uint32_t)((v / 10) << 4 | (v % 10)); }

/* The RTC callback (func_0207ca2c) decodes BCD from 0x02fffde8 (year bits 0-7, month 8-12, day 16-21, weekday
 * 24-26) and 0x02fffdec (hour 0-5, minute 8-14, second 16-22). Reply: command << 8 | result, 0 = success. */
#include <time.h>
static void rtc_command(uint32_t data) {
    time_t now = time(NULL);
    struct tm t;
    localtime_s(&t, &now);
    wr32(0x02fffde8, bcd(t.tm_year % 100) | bcd(t.tm_mon + 1) << 8 | bcd(t.tm_mday) << 16 | (uint32_t)t.tm_wday << 24);
    wr32(0x02fffdec, bcd(t.tm_hour) | bcd(t.tm_min) << 8 | bcd(t.tm_sec) << 16);
    pxi_reply(5, data & 0x7f00);
}

static void spi_packet(uint32_t tag, uint32_t data) {
    if (data & (1u << 25)) { spi_cmd[tag] = (data >> 8) & 0x7f; spi_nargs[tag] = 0; }
    if (spi_nargs[tag] < 16) spi_args[tag][spi_nargs[tag]++] = data;
    if (!(data & (1u << 24))) return;
    if (tag == 4) nvram_command(spi_cmd[tag], spi_args[tag], spi_nargs[tag]);
    if (tag == 6 && spi_cmd[tag] == 0) touch_sample();
    /* The touch callback only takes replies framed like the requests: start and end bits set */
    pxi_reply(tag, 3u << 24 | spi_cmd[tag] << 8);
}

/* Tag 11: the SDK's CARD library has the ARM7 drive the save chip. The ARM9 sends a command (with bit 5 set), and
 * for command 0 (init) the address of a shared argument block:
 *   +0 result  +4 type  +8 id  +12 src  +16 dest  +20 len  +24.. chip spec
 * The ARM7 does the work, writes the result (0 = success) and answers on tag 11 with bit 5 set; the ARM9's callback
 * (func_0206eff0) then wakes the waiting thread. The save chip is a file next to the executable. */
#define SAVE_SIZE (512u << 10)
static uint8_t save[SAVE_SIZE];
static int save_loaded, save_dirty;
static uint32_t card_arg, card_expect_arg;

static void save_load(void) {
    if (save_loaded) return;
    save_loaded = 1;
    memset(save, 0xff, sizeof save);
    FILE *f = fopen("b2w2.sav", "rb");
    if (f) { fread(save, 1, sizeof save, f); fclose(f); }
}

void arm7_save_flush(void) {
    if (firmware_dirty) {
        FILE *fw = fopen("b2w2.fw", "wb");
        if (fw) { fwrite(firmware, 1, sizeof firmware, fw); fclose(fw); }
        firmware_dirty = 0;
    }
    if (!save_dirty) return;
    FILE *f = fopen("b2w2.sav", "wb");
    if (f) { fwrite(save, 1, sizeof save, f); fclose(f); }
    save_dirty = 0;
}

static void card_command(uint32_t cmd) {
    uint32_t a = card_arg, result = 0;
    uint32_t src = rd32(a + 12), dst = rd32(a + 16), len = rd32(a + 20);
    save_load();
    switch (cmd) {
    case 6:                                             /* read backup: save[src] -> RAM[dst] */
        for (uint32_t i = 0; i < len; i++) wr8(dst + i, save[(src + i) % SAVE_SIZE]);
        break;
    case 5: case 7: case 8:                             /* write / program backup: RAM[src] -> save[dst] */
        for (uint32_t i = 0; i < len; i++) save[(dst + i) % SAVE_SIZE] = rd8(src + i);
        save_dirty = 1;
        break;
    case 9:                                             /* verify */
        for (uint32_t i = 0; i < len; i++)
            if (save[(dst + i) % SAVE_SIZE] != rd8(src + i)) result = 3;
        break;
    case 10: case 11: case 12: case 15: {               /* erase page / sector / chip / subsector */
        uint32_t n = cmd == 12 ? SAVE_SIZE : len;
        for (uint32_t i = 0; i < n; i++) save[(dst + i) % SAVE_SIZE] = 0xff;
        save_dirty = 1;
        break;
    }
    case 13: wr32(a + 24 + 4 * 13, 0); break;           /* read status */
    default: break;                                     /* init, ack, identify, read ID, write status */
    }
    if (a) wr32(a, result);
    arm7_reply(11 | 0x20 | (cmd << 6));
}

/* Tag 7: sound. The ARM9 sends the address of a linked list of commands (NitroSDK SNDCommand: next, id, arg[4]);
 * snd.c plays them. This side counts finished lists in SNDSharedWork.finishCommandTag (+0), whose address comes with
 * command 29 (SHARED_WORK), and answers driver-info reads. */
static uint32_t snd_shared;
void snd_command(uint32_t id, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3);
void snd_set_shared(uint32_t a);

/* An alarm set up by SETUP_ALARM went off: the ARM9's callback (func_0207d898) takes the alarm number in the low
 * byte and its id in the next */
void arm7_reply_sound_alarm(int no, int id) { arm7_reply(7 | (uint32_t)(no | id << 8) << 6); }

static void sound_commands(uint32_t list) {
    for (int guard = 0; list && guard < 4096; guard++) {
        uint32_t next = rd32(list), id = rd32(list + 4), arg0 = rd32(list + 8);
        static int logged[64];
        if (getenv("B2W2_LOG_SND") && id < 64 && logged[id]++ < 6)
            rc_logf("snd cmd %2u: %08x %08x %08x %08x (frame %llu)\n", id, arg0, rd32(list + 12), rd32(list + 16),
                    rd32(list + 20), (unsigned long long)rc_frames);
        snd_command(id, arg0, rd32(list + 12), rd32(list + 16), rd32(list + 20));
        switch (id) {
        case 29: snd_shared = arg0; snd_set_shared(arg0); break;    /* SHARED_WORK */
        case 33:                                                 /* READ_DRIVER_INFO: all channels and players idle */
            for (uint32_t i = 0; i < 0x400; i += 4) wr32(arg0 + i, 0);
            break;
        default: break;
        }
        list = next;
    }
    if (snd_shared) wr32(snd_shared, rd32(snd_shared) + 1);
}

/* Tag 10: wireless (WM). The ARM9 sends the address of a request whose first halfword is the API ID (the sender
 * around 0x020806cc). The ARM7 answers with the address of a callback record {u16 api ID, u16 error code, ...} in its
 * own buffer, which the WM work area (pointer at 0x0214dc98, +4) gives at +0x10; func_0208080c dispatches it. There is
 * no wireless here: every request fails with WM_ERRCODE_WM_DISABLE (4), as on a console with wireless turned off. */
static void wm_request(uint32_t req) {
    uint32_t work = rd32(0x0214dc98) ? rd32(rd32(0x0214dc98) + 4) : 0;
    uint32_t buf = work ? rd32(work + 0x10) : 0;
    if (!buf) return;
    for (uint32_t i = 0; i < 0x40; i += 4) wr32(buf + i, 0);
    wr16(buf, rd16(req));
    wr16(buf + 2, 4);
    pxi_reply(10, buf);
}

static void arm7_fifo_send(uint32_t msg) {
    uint32_t tag = msg & 0x1f, data = msg >> 6;
    static uint8_t seen[32][128];               /* log each tag/command once */
    static int log_all = -1;
    if (log_all < 0) log_all = getenv("B2W2_LOG_PXI") != NULL;
    if (log_all || (!seen[tag][(data >> 8) & 0x7f] && (seen[tag][(data >> 8) & 0x7f] = 1)))
        rc_logf("pxi -> arm7: tag %2u data %07x%s\n", tag, data, msg & 0x20 ? " (err)" : "");
    switch (tag) {
    case 4: case 6: case 8: case 9:
        spi_packet(tag, data);
        break;
    case 5:
        rtc_command(data);
        break;
    case 7:
        sound_commands(data);
        break;
    case 10:
        wm_request(data);
        break;
    case 11:
        if (card_expect_arg) { card_arg = data; card_expect_arg = 0; card_command(0); }
        else if (data == 0) card_expect_arg = 1;
        else card_command(data);
        break;
    default:
        break;
    }
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

int arm7_fifo_irq_pending(void) { return (fifo_cnt & 0x0400) && recv_count; }

void arm7_vblank(void) {
    /* The ARM7's VBlank handler counts frames in shared RAM (HW_VBLANK_COUNT_BUF); games pace themselves on it */
    wr32(0x02fffc3c, rd32(0x02fffc3c) + 1);
    if (rc_frames % 60 == 0) arm7_save_flush();
}

