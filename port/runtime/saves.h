/* Save files and the list that picks one (saves.c). */
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>

void saves_init(void);
const char *saves_current_name(void);
const char *saves_current_path(void);
void saves_set_current(const char *name);
void saves_picture_path(char *out, const char *name);

/* The picker, drawn on the game's screens in the game's style and moved with the game's buttons. Mode 0 picks a
 * file to play, mode 1 the file a save goes to. */
enum { PICK_LOAD, PICK_SAVE };
enum { PK_UP, PK_DOWN, PK_PAGE_UP, PK_PAGE_DOWN, PK_A, PK_B };
void picker_open(int mode);
int picker_active(void);
void picker_key(int button);
void picker_render(uint32_t *top, uint32_t *bottom);  /* each GPU3D_W x 192, the DS's 256 columns in the middle */
int picker_result(char name[64]);                 /* 0 still open, 1 chosen (name), -1 cancelled */
void picker_close(void);
