#pragma once

/* Special key codes pushed into the keyboard buffer for non-ASCII keys */
#define KEY_LEFT  0x01
#define KEY_RIGHT 0x02
#define KEY_UP    0x03
#define KEY_DOWN  0x04

void keyboard_init(void);
char keyboard_getchar(void);    /* returns 0 if buffer is empty */
