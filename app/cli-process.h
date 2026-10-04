/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PS5_AI_CLI_PROCESS_H
#define PS5_AI_CLI_PROCESS_H
int ps5_cli_prepare(void);
int ps5_cli_attach(unsigned columns, unsigned rows);
void ps5_cli_resize(unsigned columns, unsigned rows);
void ps5_cli_detach(void);
int ps5_cli_restart(void);
void ps5_cli_poll(void);
#endif
