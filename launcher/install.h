/* SPDX-License-Identifier: GPL-3.0-or-later
 * Adapted from saawant12/orbit-store-ps5-source launcher.
 */
#ifndef PS5_AI_INSTALL_H
#define PS5_AI_INSTALL_H
/* Written only by startup; copy the result before publishing it to the API. */
const char *ps5_ai_launcher_last_error(void);
const char *ps5_ai_launcher_registration_method(void);
void ps5_ai_launcher_record_error(const char *step, int system_error, int platform_error);
int ps5_ai_launcher_ensure(int state_fd);
/* Injectable platform operations permit filesystem tests without a console. */
int ps5_ai_launcher_ensure_at(int state_fd, const char *user_parent, int (*prepare)(void),
                             int (*register_title)(void));
#endif
