/* The game's 12 x 15 menu font (font.c) */
#pragma once
#include <stdint.h>
#include <wchar.h>

#define FONT_H 15
typedef void (*FontPut)(void *ctx, int x, int y, int v);     /* v: 1 letter, 2 shadow, 3 (unused by the menus) */
int font_ready(void);
int font_text(const wchar_t *s, int x, int y, FontPut put, void *ctx);   /* returns x after the text */
int font_width(const wchar_t *s);
