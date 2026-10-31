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
