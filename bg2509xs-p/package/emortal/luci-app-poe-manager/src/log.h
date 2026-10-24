/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef POE_MANAGER_LOG_H
#define POE_MANAGER_LOG_H

#include "poe.h"

struct poe_port_log {
	bool initialized;
	bool detection;
	bool classification;
	bool classification_event;
	bool waiting;
	uint8_t faults;
};

struct poe_log {
	struct poe_status previous;
	struct poe_port_log ports[POE_MAX_PORTS];
	uint8_t supply_events;
	uint8_t detection_events;
	bool initialized;
	bool debug;
};

void poe_log_status(const struct poe_board *board, struct poe_log *log,
		    const struct poe_status *now, bool debug);
void poe_log_ports_off(const struct poe_board *board, struct poe_log *log,
		       uint8_t mask, const char *reason);
void poe_log_policy(const struct poe_board *board, const struct poe_policy *policy,
		    bool enabled, const char *action);

#endif
