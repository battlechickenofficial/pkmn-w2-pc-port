/* A 2D engine layer's scanline, shared by render.c and titlemenu.c */
#pragma once
#include "internal.h"

/* A layer's line, GPU3D_W wide: screen x runs from -GPU3D_OFS to 256 + GPU3D_OFS, the parts outside 0-255 being
 * the widescreen margins */
typedef struct {
    uint16_t color[GPU3D_W];
    uint8_t prio[GPU3D_W];
    uint8_t flags[GPU3D_W];    /* bit 0 opaque, bit 1 semi-transparent OBJ, bit 2 3D pixel, bit 3 OBJ window */
    uint8_t alpha[GPU3D_W];    /* 3D / bitmap OBJ alpha, 0-31 */
} Line;
#define X0 (-GPU3D_OFS)
#define X1 (256 + GPU3D_OFS)
#define AT(x) ((x) + GPU3D_OFS)

enum { OPAQUE = 1, SEMI = 2, IS3D = 4, OBJWIN = 8 };

void title_menu_line(int y, Line *bg, Line *obj);

/* For the top-screen-only layout (topscreen.c): layers each engine leaves out (bits 0-3 BG0-BG3, 4 sprites), and engine
 * B's layers kept one by one (ARGB with alpha 255 where drawn, 0 elsewhere) while render_keep_b is set */
extern int render_hide[2], render_keep_b;
extern uint32_t render_layer_b[5][192 * 256];
/* and composed from the layers in render_ui_mask as the engine composes them, nothing where none of them is */
extern int render_ui_mask;
extern uint32_t render_ui_b[192 * 256];

/* Engine A's sprites in a rectangle drawn dy lines away (render.c), and the 3D view's scale and raise (gpu3d.c) */
typedef struct { int x0, x1, y0, y1, dy; } RenderMove;
extern RenderMove render_moves[4];
extern float gpu3d_view_scale, gpu3d_view_raise;
