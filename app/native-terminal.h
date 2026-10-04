/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PS5_AI_NATIVE_TERMINAL_H
#define PS5_AI_NATIVE_TERMINAL_H
int ps5_terminal_prepare(void);
int ps5_terminal_reserve_stdio(void);
int ps5_terminal_adopt(int control_fd);
int ps5_terminal_attach(unsigned columns, unsigned rows);
void ps5_terminal_resize(unsigned columns, unsigned rows);
void ps5_terminal_detach(void);
int ps5_terminal_wait(void);
#endif
