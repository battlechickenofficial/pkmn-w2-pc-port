/* Game card bus (0x040001a0-0x040001af, data at 0x04100010), serving reads from the ROM file. The game reads its
 * file system and overlays with its own card code, so only the commands it sends after boot are needed:
 *   B7 aaaaaaaa  read data from ROM offset a      B8  read chip ID
 * Command bytes go in 0x040001a8 (first byte lowest); writing ROMCTRL (0x040001a4) with bit 31 starts a transfer of
 * the block size in bits 24-26. Data words appear one at a time with bit 23 set until the block is done. */
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>

extern FILE *rc_log;
void rc_card_dma(void);
void rc_raise_irq(uint32_t bits);

static FILE *rom;
static uint64_t rom_size;
static uint32_t chip_id = 0x0000ffc2;     /* also left at 0x027ff800 / 0x027ffc00 by boot */

static uint8_t buf[0x4000];
static uint32_t buf_len, buf_pos;
static uint32_t romctrl;
static uint8_t cmd[8];

void card_open(const char *path) {
    rom = fopen(path, "rb");
    if (!rom) { perror(path); exit(1); }
    _fseeki64(rom, 0, SEEK_END);
    rom_size = (uint64_t)_ftelli64(rom);
}

uint32_t card_chip_id(void) { return chip_id; }

/* Read the ROM file directly (the runtime's own use: the game's font for the save-file screens) */
int card_rom_read(uint64_t addr, void *dst, uint32_t len) {
    if (!rom || addr + len > rom_size) return 0;
    _fseeki64(rom, (int64_t)addr, SEEK_SET);
    return fread(dst, 1, len, rom) == len;
}

static void read_rom(uint64_t addr, uint32_t len) {
    /* Retail cards redirect reads of the secure area (below 0x8000) */
    if (addr < 0x8000) addr = 0x8000 + (addr & 0x1ff);
    memset(buf, 0xff, len);
    if (addr < rom_size) {
        _fseeki64(rom, (int64_t)addr, SEEK_SET);
        fread(buf, 1, (size_t)(len < rom_size - addr ? len : rom_size - addr), rom);
    }
}

void card_cmd_write(uint32_t off, uint8_t v) { cmd[off & 7] = v; }

void card_romctrl_write(uint32_t v) {
    romctrl = (romctrl & 0x00800000) | (v & ~0x00800000u);
    if (!(v & 0x80000000u)) return;
    uint32_t bs = (v >> 24) & 7;
    uint32_t len = bs == 0 ? 0 : bs == 7 ? 4 : 0x100u << bs;
    switch (cmd[0]) {
    case 0xb7: {
        uint32_t addr = (uint32_t)cmd[1] << 24 | cmd[2] << 16 | cmd[3] << 8 | cmd[4];
        read_rom(addr, len);
        break;
    }
    case 0xb8:
        for (uint32_t i = 0; i < len; i += 4) memcpy(buf + i, &chip_id, 4);
        break;
    default:
        if (rc_log) fprintf(rc_log, "card: command %02x %02x %02x %02x %02x %02x %02x %02x not handled\n", cmd[0], cmd[1],
                            cmd[2], cmd[3], cmd[4], cmd[5], cmd[6], cmd[7]);
        memset(buf, 0xff, len);
    }
    buf_len = len;
    buf_pos = 0;
    if (len) romctrl |= 0x00800000; else romctrl &= ~0x80000000u;
    rc_card_dma();
}

uint32_t card_romctrl_read(void) { return romctrl; }

int card_data_ready(void) { return buf_pos < buf_len; }

uint32_t card_data_read(void) {
    uint32_t v = 0xffffffff;
    if (buf_pos < buf_len) {
        memcpy(&v, buf + buf_pos, 4);
        buf_pos += 4;
    }
    if (buf_pos >= buf_len && (romctrl & 0x80000000u)) {
        romctrl &= ~0x80800000u;
        rc_raise_irq(1u << 19);              /* card transfer complete */
    }
    return v;
}

/* The card's SPI bus (0x040001a0 control, 0x040001a2 data). On the ARM9 only the infrared chip of the Black 2 /
 * White 2 card is asked anything: before a battle gives experience (overlay 338) it sends command 08h, which a genuine
 * card's chip answers with its ID, AAh; anything else and no experience is given. Bit 6 of the control holds the
 * chip selected for the next byte, so a byte written while it was clear ends the transfer. */
static int spi_busy;
static uint8_t spi_cmd;

uint8_t card_spi_write(uint16_t cnt, uint8_t v) {
    if (!spi_busy) spi_cmd = v;
    spi_busy = (cnt & 0x40) != 0;
    return spi_cmd == 0x08 ? 0xaa : 0xff;
}
