/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef POE_MANAGER_LOG_SINK_H
#define POE_MANAGER_LOG_SINK_H

#include <stdbool.h>

int poe_log_init(bool debug);
int poe_log_clear(void);
void poe_log_set_debug(bool debug);
void poe_log_close(void);
#ifdef syslog
#define poe_log_message syslog
#endif

void poe_log_message(int priority, const char *format, ...);

#endif
