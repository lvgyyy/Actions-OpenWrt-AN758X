/* SPDX-License-Identifier: GPL-2.0-only */
#include "poe.h"

#include <errno.h>
#include <stddef.h>
#include <string.h>

/* Mode/reset/event semantics: TMI7604R V1.3 and TMI7608R V0.3, pp.14-23.
 * Board wiring, ADC packing and undocumented setup values: RP01/RP02 pse_ctl.
 * These are board-specific settings, not a generic TMI register specification.
 */
#define POE_I2C_NODE "/sys/firmware/devicetree/base/soc@0/i2c@78b5000"
/* MT7987B exposes a single I2C controller (i2c0 @ 0x11003000) in mt7987.dtsi.
 * Confirm both strings against the board DTS before shipping.
 */
#define RTL8238B_I2C_NODE "/sys/firmware/devicetree/base/soc/i2c@11003000"
#define RTL8238B_COMPATIBLE "beecon,beeconmini2"

const struct poe_board poe_boards[3] = {
	{ "xiaomi,be3600-pro-wired-p5", "TMI7604R", POE_I2C_NODE, 0x50, 1, 4, 4, 60000,
	  true, { 2, 1, 4, 3 }, &poe_chip_tmi7608 },
	{ "xiaomi,be3600-pro-wired-p8", "TMI7608R", POE_I2C_NODE, 0x70, 1, 8, 7, 105000,
	  true, { 2, 1, 4, 3, 6, 5, 8 }, &poe_chip_tmi7608 },
	/* Placeholder compatible: replace once the board DTS lands.
	 * Mapping follows pse_src/rtl-poe.c (7 ports, reversed): LAN1..LAN7 use
	 * one-based PSE channels 7..1, so one-based channel 8 stays unused. The
	 * stock web UI instead suggests 8 channels (WAN + LAN2..LAN8); confirm
	 * on hardware which mapping is real before shipping this row.
	 */
	{ RTL8238B_COMPATIBLE, "RTL8238B", RTL8238B_I2C_NODE, 0x20, 2, 8, 7, 90000,
	  false, { 7, 6, 5, 4, 3, 2, 1 }, &poe_chip_rtl8238b },
};

static int read_reg(struct poe_io *io, const struct poe_board *board, uint8_t reg,
		    uint8_t *value)
{
	io->failed_reg = reg;
	io->operation = "read";
	return io->read(io->ctx, board->address, reg, value);
}

static int write_reg(struct poe_io *io, const struct poe_board *board, uint8_t reg,
		     uint8_t value)
{
	io->failed_reg = reg;
	io->operation = "write";
	return io->write(io->ctx, board->address, reg, value);
}

static int verify_value(struct poe_io *io, uint8_t reg, uint8_t expected, uint8_t value)
{
	if (value == expected)
		return 0;
	io->failed_reg = reg;
	io->operation = "verify";
	io->expected = expected;
	io->actual = value;
	return -EIO;
}

static int verify_reg(struct poe_io *io, const struct poe_board *board, uint8_t reg,
		      uint8_t expected)
{
	uint8_t value;
	int ret = read_reg(io, board, reg, &value);

	return ret ? ret : verify_value(io, reg, expected, value);
}

static int write_verify(struct poe_io *io, const struct poe_board *board, uint8_t reg,
			uint8_t value)
{
	int ret = write_reg(io, board, reg, value);

	return ret ? ret : verify_reg(io, board, reg, value);
}

static int read_word(struct poe_io *io, const struct poe_board *board, uint8_t reg,
		     unsigned int *value)
{
	uint8_t lo, hi, check;
	int ret;
	unsigned int attempt;

	/* Retry if the high byte changes during the factory two-byte ADC read.
	 * This detects rollover; the PDFs do not specify an atomic sample latch.
	 */
	for (attempt = 0; attempt < 3; attempt++) {
		ret = read_reg(io, board, reg + 1, &hi);
		if (ret)
			return ret;
		ret = read_reg(io, board, reg, &lo);
		if (ret)
			return ret;
		ret = read_reg(io, board, reg + 1, &check);
		if (ret)
			return ret;
		if (hi == check) {
			*value = lo | ((unsigned int)hi << 8);
			return 0;
		}
	}
	return -EAGAIN;
}

static unsigned int voltage_mv(unsigned int raw)
{
	/* 124.5 mV/LSB (datasheet); factory word packing has five fraction bits. */
	return raw * 1245U / 320U;
}

uint8_t poe_board_mask(const struct poe_board *board)
{
	uint8_t mask = 0;
	unsigned int port;

	for (port = 0; port < board->ports; port++)
		mask |= 1U << (board->port_map[port] - 1);
	return mask;
}

int poe_validate(const struct poe_board *board, const struct poe_policy *policy)
{
	if (policy->budget_mw < 1000 || policy->budget_mw > board->max_budget_mw ||
	    (policy->mask & ~poe_board_mask(board)))
		return -EINVAL;
	return 0;
}

/* ------------------------------------------------------------------ */
/* TMI7604R / TMI7608R backend                                        */
/* ------------------------------------------------------------------ */

static uint8_t poe_port_modes(uint8_t automatic, uint8_t semi, unsigned int group)
{
	uint8_t result = 0;
	unsigned int i, bit;

	for (i = 0; i < 4; i++) {
		bit = 1U << (group * 4 + i);
		/* 00=shutdown, 10=semiautomatic, 11=automatic; never manual. */
		if (automatic & bit)
			result |= 3U << (2 * i);
		else if (semi & bit)
			result |= 2U << (2 * i);
	}
	return result;
}

static int poe_set_modes(struct poe_io *io, const struct poe_board *board,
			 uint8_t automatic, uint8_t semi)
{
	unsigned int group;
	int ret;

	/* The four-channel part documents only R1Fh. */
	for (group = 0; group < board->channels / 4; group++) {
		ret = write_verify(io, board, 0x1f + group,
				   poe_port_modes(automatic, semi, group));
		if (ret)
			return ret;
	}
	return 0;
}

static unsigned int poe_budget_value(unsigned int budget_mw)
{
	/* Factory compatibility: trunc((budget_mw / 53 integer division) / 1.956).
	 * A nominal-53-V current threshold, not a calibrated wattmeter limit.
	 */
	return (budget_mw / 53U) * 1000U / 1956U;
}

static int poe_set_budget(struct poe_io *io, const struct poe_board *board,
			  unsigned int budget_mw)
{
	unsigned int value = poe_budget_value(budget_mw);
	int ret = write_verify(io, board, 0x77, value & 0xff);

	return ret ? ret : write_verify(io, board, 0x78, value >> 8);
}

static int poe_check_supply(struct poe_io *io, const struct poe_board *board)
{
	unsigned int attempt, raw, mv;
	int ret;

	io->stage = "supply-check";
	for (attempt = 0; attempt < 20; attempt++) {
		ret = read_word(io, board, 0x89, &raw);
		if (ret)
			return ret;
		mv = voltage_mv(raw);
		if (mv >= POE_INPUT_MIN_MV && mv <= POE_INPUT_MAX_MV)
			return 0;
		io->delay(io->ctx, 100);
	}
	/* No 12/24 V mode or factory 45 V display clamp on these boards. */
	return -ERANGE;
}

static int poe_confirm_off(struct poe_io *io, const struct poe_board *board, uint8_t mask)
{
	unsigned int attempt;
	uint8_t powered, good;
	int ret;

	io->stage = "shutdown-confirm";
	for (attempt = 0; attempt < 20; attempt++) {
		ret = read_reg(io, board, 0x1c, &powered);
		if (ret)
			return ret;
		ret = read_reg(io, board, 0x1d, &good);
		if (ret)
			return ret;
		if (!((powered | good) & mask))
			return 0;
		io->delay(io->ctx, 100);
	}
	return -ETIMEDOUT;
}

static int poe_chip_disable(struct poe_io *io, const struct poe_board *board)
{
	int ret, first = 0;
	unsigned int group;
	uint8_t expected = 0, actual = 0;
	int first_reg = -1;
	const char *operation = NULL;

	io->stage = "shutdown-modes";
	/* Try every mode group despite bus errors, preserving the first error. */
	for (group = 0; group < board->channels / 4; group++) {
		ret = write_verify(io, board, 0x1f + group, 0);
		if (ret && !first) {
			first = ret;
			first_reg = io->failed_reg;
			operation = io->operation;
			expected = io->expected;
			actual = io->actual;
		}
	}
	if (first) {
		io->failed_reg = first_reg;
		io->operation = operation;
		io->expected = expected;
		io->actual = actual;
		return first;
	}
	return poe_confirm_off(io, board, (1U << board->channels) - 1);
}

static int poe_enable_detection(struct poe_io *io, const struct poe_board *board,
				uint8_t retained, uint8_t mask)
{
	int ret;

	/* Stage new ports in semi mode: detection cannot power them on. */
	io->stage = "detection-prepare";
	ret = poe_set_modes(io, board, retained, mask & ~retained);
	if (ret)
		return ret;
	ret = write_verify(io, board, 0x23, mask);
	if (ret)
		return ret;
	ret = write_verify(io, board, 0x22, mask);
	if (ret)
		return ret;
	/* Automatic mode powers only valid PDs. Do not issue PWR_ON (R28h). */
	io->stage = "automatic-mode";
	return poe_set_modes(io, board, mask, 0);
}

static int poe_chip_initialize(struct poe_io *io, const struct poe_board *board,
			       const struct poe_policy *policy)
{
	static const uint8_t setup[][2] = {
		{ 0x01, 0xe7 }, { 0x54, 0xe7 }, { 0x32, 0xff }, { 0x76, 0x23 },
	};
	unsigned int i;
	int ret = poe_validate(board, policy);

	io->stage = "validate";
	io->failed_reg = -1;
	if (ret)
		return ret;
	/* Reset pin straps can select auto mode. Quiesce before programming. */
	ret = poe_chip_disable(io, board);
	if (ret)
		return ret;
	io->stage = "clear-interrupts";
	ret = write_reg(io, board, 0x2b, 0x80); /* INT_CLR, not RESET_IC (bit 4). */
	if (ret)
		return ret;
	io->stage = "factory-setup";
	for (i = 0; i < sizeof(setup) / sizeof(setup[0]); i++) {
		uint8_t value = setup[i][1];

		/* R32h only retains implemented channels: P5=0x0f, P8=0xff. */
		if (setup[i][0] == 0x32)
			value &= (1U << board->channels) - 1;
		ret = write_verify(io, board, setup[i][0], value);
		if (ret)
			return ret;
	}
	/* Factory channel-pair settings: P5 retains only R2Eh/R2Fh;
	 * P8 also implements R30h/R31h. Do not access absent P5 pairs.
	 */
	for (i = 0; i < board->channels / 2; i++) {
		ret = write_verify(io, board, 0x2e + i, 0x33);
		if (ret)
			return ret;
	}
	io->stage = "budget";
	ret = poe_set_budget(io, board, policy->budget_mw);
	if (ret)
		return ret;
	ret = write_verify(io, board, 0x88, 0x3a);
	if (ret)
		return ret;
	io->stage = "protection";
	/* Retain automatic detection; Class4+ extends classification, not PWR_ON. */
	ret = write_verify(io, board, 0x2d, policy->class4plus ? poe_board_mask(board) : 0);
	if (ret)
		return ret;
	ret = write_verify(io, board, 0x21, poe_board_mask(board)); /* DC disconnect */
	if (ret)
		return ret;
	ret = poe_check_supply(io, board);
	if (ret)
		return ret;
	ret = poe_enable_detection(io, board, 0, policy->mask);
	if (ret)
		return ret;
	return poe_verify_policy(io, board, policy);
}

static int poe_chip_set_policy(struct poe_io *io, const struct poe_board *board,
			       const struct poe_policy *old, const struct poe_policy *policy)
{
	uint8_t retained = old->mask & policy->mask;
	int ret = poe_validate(board, policy);

	io->stage = "validate";
	io->failed_reg = -1;
	if (ret)
		return ret;
	if (old->budget_mw == policy->budget_mw && old->mask == policy->mask &&
	    old->class4plus == policy->class4plus)
		return policy->mask ? 0 : poe_confirm_off(io, board, (1U << board->channels) - 1);
	if (old->budget_mw != policy->budget_mw || old->class4plus != policy->class4plus) {
		/* Update two budget bytes with outputs off, never at a transient limit. */
		ret = poe_chip_disable(io, board);
		if (ret)
			return ret;
		io->stage = "budget";
		ret = poe_set_budget(io, board, policy->budget_mw);
		if (ret)
			return ret;
		io->stage = "protection";
		ret = write_verify(io, board, 0x2d,
				   policy->class4plus ? poe_board_mask(board) : 0);
		if (ret)
			return ret;
		retained = 0;
	}
	if (policy->mask & ~retained) {
		ret = poe_check_supply(io, board);
		if (ret)
			return ret;
	}
	ret = poe_enable_detection(io, board, retained, policy->mask);
	if (ret)
		return ret;
	if (!policy->mask || (old->mask & ~policy->mask)) {
		ret = poe_confirm_off(io, board, !policy->mask ? (1U << board->channels) - 1 :
				      old->mask & ~policy->mask);
		if (ret)
			return ret;
	}
	return poe_verify_policy(io, board, policy);
}

static int poe_chip_verify_policy(struct poe_io *io, const struct poe_board *board,
				  const struct poe_policy *policy)
{
	unsigned int group, value = poe_budget_value(policy->budget_mw);
	int ret;

	io->stage = "configuration-monitor";
	for (group = 0; group < board->channels / 4; group++) {
		ret = verify_reg(io, board, 0x1f + group,
				 poe_port_modes(policy->mask, 0, group));
		if (ret)
			return ret;
	}
	ret = verify_reg(io, board, 0x77, value & 0xff);
	if (ret)
		return ret;
	ret = verify_reg(io, board, 0x78, value >> 8);
	if (ret)
		return ret;
	ret = verify_reg(io, board, 0x21, poe_board_mask(board));
	if (ret)
		return ret;
	ret = verify_reg(io, board, 0x22, policy->mask);
	if (ret)
		return ret;
	ret = verify_reg(io, board, 0x23, policy->mask);
	if (ret)
		return ret;
	return verify_reg(io, board, 0x2d, policy->class4plus ? poe_board_mask(board) : 0);
}

static int poe_chip_check_policy_status(struct poe_io *io, const struct poe_board *board,
					const struct poe_policy *policy,
					const struct poe_status *status)
{
	unsigned int group, value = poe_budget_value(policy->budget_mw);
	int ret;

	/* Detect reset/lost configuration from the already collected snapshot.
	 * Do not silently continue under strap defaults or re-enable outputs.
	 */
	io->stage = "configuration-monitor";
	for (group = 0; group < board->channels / 4; group++) {
		ret = verify_value(io, 0x1f + group, poe_port_modes(policy->mask, 0, group),
				   status->modes[group]);
		if (ret)
			return ret;
	}
	ret = verify_value(io, 0x21, poe_board_mask(board), status->disconnect);
	if (ret)
		return ret;
	ret = verify_value(io, 0x22, policy->mask, status->detect);
	if (ret)
		return ret;
	ret = verify_value(io, 0x23, policy->mask, status->classify);
	if (ret)
		return ret;
	ret = verify_value(io, 0x2d, policy->class4plus ? poe_board_mask(board) : 0,
			   status->class4plus);
	if (ret)
		return ret;
	ret = verify_value(io, 0x77, value & 0xff, status->budget_raw & 0xff);
	if (ret)
		return ret;
	return verify_value(io, 0x78, value >> 8, status->budget_raw >> 8);
}

static int poe_read_snapshot(struct poe_io *io, const struct poe_board *board,
			     struct poe_status *status, bool read_events)
{
	struct poe_status sample = { 0 };
	unsigned int raw, port, channel, i;
	int ret;

	io->stage = "status";
	for (i = 0; i < board->channels / 4; i++) {
		ret = read_reg(io, board, 0x1f + i, &sample.modes[i]);
		if (ret)
			return ret;
	}
	ret = read_reg(io, board, 0x21, &sample.disconnect);
	if (ret)
		return ret;
	ret = read_reg(io, board, 0x22, &sample.detect);
	if (ret)
		return ret;
	ret = read_reg(io, board, 0x23, &sample.classify);
	if (ret)
		return ret;
	ret = read_word(io, board, 0x77, &sample.budget_raw);
	if (ret)
		return ret;
	ret = read_reg(io, board, 0x2d, &sample.class4plus);
	if (ret)
		return ret;
	ret = read_reg(io, board, 0x00, &sample.summary);
	if (ret)
		return ret;
	ret = read_reg(io, board, 0x1c, &sample.powered);
	if (ret)
		return ret;
	ret = read_reg(io, board, 0x1d, &sample.good);
	if (ret)
		return ret;
	/* Queries peek at the even, non-clearing aliases. The daemon already
	 * collected the odd aliases and must deliver its pending events instead.
	 */
	if (read_events)
		for (i = 0; i < POE_EVENTS; i++) {
			ret = read_reg(io, board, 0x02 + 2 * i, &sample.events[i]);
			if (ret)
				return ret;
		}
	ret = read_word(io, board, 0x89, &raw);
	if (ret)
		return ret;
	sample.input_mv = voltage_mv(raw);
	sample.input_valid = true;
	for (port = 0; port < board->ports; port++) {
		channel = board->port_map[port] - 1;
		ret = read_reg(io, board, 0x14 + channel, &sample.port_state[port]);
		if (ret)
			return ret;
		ret = read_word(io, board, 0x33 + 4 * channel, &raw);
		if (ret)
			return ret;
		/* Preserve raw ADC: Class4+ uses twice the af/at current scale.
		 * The supplied PDFs do not define the active-range status encoding;
		 * an enabled Class4+ bit alone does not identify the detected PD class.
		 */
		sample.current_raw[port] = raw;
		sample.current_ma[port] = raw * 1956U / 16000U;
		ret = read_word(io, board, 0x35 + 4 * channel, &raw);
		if (ret)
			return ret;
		sample.voltage_mv[port] = voltage_mv(raw);
		sample.power_mw[port] = (unsigned long long)sample.voltage_mv[port] *
					sample.current_ma[port] / 1000;
	}
	/* Never publish a partly updated snapshot after an I2C failure. */
	*status = sample;
	return 0;
}

static int poe_chip_read_status(struct poe_io *io, const struct poe_board *board,
				struct poe_status *status)
{
	return poe_read_snapshot(io, board, status, true);
}

static int poe_chip_poll_status(struct poe_io *io, const struct poe_board *board,
				struct poe_status *status)
{
	struct poe_status sample;
	unsigned int i;
	uint8_t event;
	int ret;

	/* Only the owning daemon consumes the clear-on-read aliases. Never
	 * retry these reads in the transport: a failed transfer may have cleared
	 * the latch already. Keep every successful read across later failures.
	 */
	io->stage = "events";
	for (i = 0; i < POE_EVENTS; i++) {
		ret = read_reg(io, board, 0x03 + 2 * i, &event);
		if (ret)
			return ret;
		io->pending_events[i] |= event;
	}
	ret = poe_read_snapshot(io, board, &sample, false);
	if (ret)
		return ret;
	memcpy(sample.events, io->pending_events, sizeof(sample.events));
	memset(io->pending_events, 0, sizeof(io->pending_events));
	*status = sample;
	return 0;
}

/* Shared backend for both TMI7604R (P5, 4 channels) and TMI7608R (P8, 8
 * channels); the register interface is identical across the two parts.
 */
const struct poe_chip poe_chip_tmi7608 = {
	.initialize = poe_chip_initialize,
	.set_policy = poe_chip_set_policy,
	.disable = poe_chip_disable,
	.verify_policy = poe_chip_verify_policy,
	.check_policy_status = poe_chip_check_policy_status,
	.read_status = poe_chip_read_status,
	.poll_status = poe_chip_poll_status,
	.hardware_budget = true,
	.analog_scaled = true,
};

/* ------------------------------------------------------------------ */
/* Dispatch to the board's chip backend                               */
/* ------------------------------------------------------------------ */

int poe_initialize(struct poe_io *io, const struct poe_board *board,
		   const struct poe_policy *policy)
{
	return board->chip_ops->initialize(io, board, policy);
}

int poe_set_policy(struct poe_io *io, const struct poe_board *board,
		   const struct poe_policy *old, const struct poe_policy *policy)
{
	return board->chip_ops->set_policy(io, board, old, policy);
}

int poe_disable(struct poe_io *io, const struct poe_board *board)
{
	return board->chip_ops->disable(io, board);
}

int poe_verify_policy(struct poe_io *io, const struct poe_board *board,
		      const struct poe_policy *policy)
{
	return board->chip_ops->verify_policy(io, board, policy);
}

int poe_check_policy_status(struct poe_io *io, const struct poe_board *board,
			    const struct poe_policy *policy, const struct poe_status *status)
{
	return board->chip_ops->check_policy_status(io, board, policy, status);
}

int poe_read_status(struct poe_io *io, const struct poe_board *board,
		    struct poe_status *status)
{
	return board->chip_ops->read_status(io, board, status);
}

int poe_poll_status(struct poe_io *io, const struct poe_board *board,
		    struct poe_status *status)
{
	return board->chip_ops->poll_status(io, board, status);
}
