/* Shared between the runtime's own files; generated code only sees runtime.h. */
#pragma once
#include "runtime.h"
#include <stdio.h>

extern FILE *rc_log;
void rc_logf(const char *fmt, ...);
void rc_fatal(Cpu *c, const char *fmt, ...);
extern Cpu rc_cpu;

/* I/O register file for 0x04000000-0x0400ffff */
extern uint8_t rc_io[64 << 10];
static inline uint16_t io16(uint32_t off) { uint16_t v; memcpy(&v, rc_io + off, 2); return v; }
static inline uint32_t io32(uint32_t off) { uint32_t v; memcpy(&v, rc_io + off, 4); return v; }
static inline void set_io16(uint32_t off, uint16_t v) { memcpy(rc_io + off, &v, 2); }
static inline void set_io32(uint32_t off, uint32_t v) { memcpy(rc_io + off, &v, 4); }

/* Memory the renderer reads directly */
extern uint8_t rc_palette[2 << 10], rc_oam[2 << 10];
extern uint32_t rc_dtcm_base;
uint8_t *rc_vram_bank(int bank);                  /* A..I = 0..8 */
uint8_t *rc_vram_lcdc(uint32_t addr);             /* LCDC (0x06800000) view, for display mode 2 */
uint8_t *rc_tex_slot(int slot);                   /* 3D texture image slot 0-3, or NULL */
uint8_t *rc_texpal_slot(int slot);                /* 3D texture palette slot 0-5 (16 KB each), or NULL */
uint8_t *rc_bg_extpal(int engine, int slot);      /* BG extended palette slot 0-3 (8 KB each), or NULL */
uint8_t *rc_obj_extpal(int engine);               /* OBJ extended palette (8 KB), or NULL */
void rc_remap_vram(void);

/* Time, interrupts, threads (sched.c) */
#define RC_CLOCK 33513982u                       /* bus clock: timers and the video counter run on it */
#define RC_LINE_CYCLES 2130u
extern uint64_t rc_now;
extern uint64_t rc_frames;
extern uint16_t rc_vcount;
void rc_advance(uint64_t cycles);
void rc_raise_irq(uint32_t bits);
void rc_irq_check(Cpu *c, uint32_t resume_pc);
void rc_irq_enabled(Cpu *c);
void rc_halt(Cpu *c, uint32_t resume_pc);
void rc_thread_check(Cpu *c, uint32_t target_pc);
void rc_sched_init(void);
uint16_t rc_timer_read(int n);
void rc_timer_write(int n, uint32_t off);

/* ARM7 stand-in (arm7.c) */
void arm7_tick(void);
void arm7_boot(void);
void arm7_save_flush(void);
int arm7_save_pick_pending(void);                 /* the game started a save: which file does it go to? */
void arm7_save_pick_done(void);                   /* saves_current_path() now names it */
const char *saves_current_path(void);             /* the save chip's file (saves.c) */
extern uint64_t rc_title_menu_frame;              /* frame + 1 when the title menu last ran (runtime.c) */
enum { SCENE_TITLE_MENU, SCENE_BATTLE_COMMAND, SCENE_BATTLE_MOVES, SCENE_BATTLE_BAG, SCENE_BATTLE_PARTY,
       SCENE_FIELD_MENU, SCENE_BATTLE, SCENE_REPORT, SCENE_COUNT = 16 };
extern uint64_t rc_scene_frame[SCENE_COUNT];
int rc_scene(int scene);                          /* that screen is up (it ran in the last two frames) */
void arm7_vblank(void);
uint16_t arm7_ipcsync_read(uint16_t stored);
void arm7_ipcsync_write(uint16_t v);
uint16_t arm7_fifocnt_read(void);
void arm7_fifocnt_write(uint16_t v);
void arm7_fifo_write(uint32_t v);
uint32_t arm7_fifo_read(void);
int arm7_fifo_irq_pending(void);
void arm7_touch(int down, int x, int y);

/* Game card (card.c) */
void card_open(const char *path);
uint32_t card_chip_id(void);
void card_cmd_write(uint32_t off, uint8_t v);
void card_romctrl_write(uint32_t v);
uint8_t card_spi_write(uint16_t cnt, uint8_t v);
uint32_t card_romctrl_read(void);
int card_data_ready(void);
uint32_t card_data_read(void);
void rc_card_dma(void);

/* Frontend and graphics (frontend.c, render.c, gpu3d.c) */
void fe_init(void);
void fe_vblank(void);                             /* present the frame, pump input, keep 60 Hz */
extern uint16_t fe_keys;                          /* KEYINPUT bits, 0 = pressed */
extern uint16_t fe_extkeys;                       /* X, Y bits as the ARM7 reports them */
void render_frame(uint32_t *engine_a, uint32_t *engine_b);  /* each GPU3D_W x 192 */
void render_line(int y);
int fe_render_wanted(void);                       /* this frame is shown: a window is open or a screenshot is due */
void gpu3d_write(uint32_t off, uint32_t v);       /* geometry registers 0x04000400-0x040006ff */
uint32_t gpu3d_read(uint32_t off);
/* Widescreen: the 3D engine and the screens are drawn GPU3D_W wide, the DS's 256 pixels in the middle and
 * GPU3D_OFS more on each side (16:9 at 192 lines). */
#define GPU3D_W 342
#define GPU3D_OFS 43
void gpu3d_swap(void);                            /* at VBlank, if SWAP_BUFFERS was issued */
const uint32_t *gpu3d_framebuffer(void);          /* GPU3D_W x 192: BGR555 in bits 0-14, alpha 0-31 in 16-20 */
