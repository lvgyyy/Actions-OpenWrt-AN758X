/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef POE_MANAGER_H
#define POE_MANAGER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define POE_MAX_PORTS 8
#define POE_EVENTS 9
#define POE_INPUT_MIN_MV 44000
#define POE_INPUT_MAX_MV 57000

struct poe_io;
struct poe_board;

struct poe_policy {
	unsigned int budget_mw;
	uint8_t mask;
	bool class4plus;
	bool debug;
};

struct poe_status {
	uint8_t modes[2];
	uint8_t detect;
	uint8_t classify;
	uint8_t disconnect;
	uint8_t class4plus;
	unsigned int budget_raw;
	uint8_t summary;
	uint8_t powered;
	uint8_t good;
	uint8_t events[POE_EVENTS];
	uint8_t port_state[POE_MAX_PORTS];
	unsigned int input_mv;
	/* RTL8238B exposes no input-voltage register; supply monitoring is skipped. */
	bool input_valid;
	unsigned int current_ma[POE_MAX_PORTS];
	unsigned int current_raw[POE_MAX_PORTS];
	unsigned int voltage_mv[POE_MAX_PORTS];
	unsigned int power_mw[POE_MAX_PORTS];
};

/* Chip backends. Every backend owns its register map, initialization order
 * and the meaning of the shared status fields above.
 */
struct poe_chip {
	int (*initialize)(struct poe_io *io, const struct poe_board *board,
			  const struct poe_policy *policy);
	int (*set_policy)(struct poe_io *io, const struct poe_board *board,
			  const struct poe_policy *old, const struct poe_policy *policy);
	int (*disable)(struct poe_io *io, const struct poe_board *board);
	int (*verify_policy)(struct poe_io *io, const struct poe_board *board,
			     const struct poe_policy *policy);
	int (*check_policy_status)(struct poe_io *io, const struct poe_board *board,
				   const struct poe_policy *policy,
				   const struct poe_status *status);
	int (*read_status)(struct poe_io *io, const struct poe_board *board,
			   struct poe_status *status);
	int (*poll_status)(struct poe_io *io, const struct poe_board *board,
			   struct poe_status *status);
	/* True when the chip enforces budget_mw itself. Otherwise the daemon only
	 * reports a measured total above the configured budget.
	 */
	bool hardware_budget;
	/* True when voltage_mv/current_ma carry calibrated units. Otherwise only
	 * power_mw is defined and the raw counts stay in the debug log.
	 */
	bool analog_scaled;
};

struct poe_board {
	const char *compatible;
	const char *chip;
	const char *i2c_node; /* device-tree path of the I2C controller */
	unsigned int address; /* first I2C address; RTL8238B also uses address + 1 */
	unsigned int addresses; /* number of consecutive I2C addresses */
	unsigned int channels;
	unsigned int ports;
	unsigned int max_budget_mw;
	bool reset_gpio; /* board wires the shared PoE reset line */
	uint8_t port_map[POE_MAX_PORTS]; /* port index (LAN1 first) -> one-based PSE channel */
	const struct poe_chip *chip_ops;
};

struct poe_io {
	void *ctx;
	int (*read)(void *ctx, unsigned int address, uint8_t reg, uint8_t *value);
	int (*write)(void *ctx, unsigned int address, uint8_t reg, uint8_t value);
	/* Chunked firmware download. Returns 0 or a negative errno. */
	int (*write_bulk)(void *ctx, unsigned int address, uint8_t reg,
			  const uint8_t *data, size_t len);
	void (*delay)(void *ctx, unsigned int ms);
	const char *stage;
	int failed_reg; /* -1 when the failing stage has not accessed a register. */
	const char *operation;
	uint8_t expected;
	uint8_t actual;
	/* Preserve consumed events until a complete sample can be delivered. */
	uint8_t pending_events[POE_EVENTS];
};

extern const struct poe_board poe_boards[3];
extern const struct poe_chip poe_chip_tmi7608;
extern const struct poe_chip poe_chip_rtl8238b;

uint8_t poe_board_mask(const struct poe_board *board);
int poe_validate(const struct poe_board *board, const struct poe_policy *policy);
int poe_initialize(struct poe_io *io, const struct poe_board *board,
		   const struct poe_policy *policy);
int poe_set_policy(struct poe_io *io, const struct poe_board *board,
		   const struct poe_policy *old, const struct poe_policy *policy);
int poe_disable(struct poe_io *io, const struct poe_board *board);
int poe_verify_policy(struct poe_io *io, const struct poe_board *board,
		      const struct poe_policy *policy);
int poe_check_policy_status(struct poe_io *io, const struct poe_board *board,
			    const struct poe_policy *policy, const struct poe_status *status);
int poe_read_status(struct poe_io *io, const struct poe_board *board,
		    struct poe_status *status);
int poe_poll_status(struct poe_io *io, const struct poe_board *board,
		    struct poe_status *status);

#endif
