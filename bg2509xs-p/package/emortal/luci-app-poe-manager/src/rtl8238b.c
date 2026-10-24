/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * RTL8238B PSE backend.
 *
 * Register map, I2C framing, firmware download order and the power formula
 * were recovered from the vendor binaries (rtl8373n.ko / usr/bin/rtlgsw,
 * csdp_pse_8238x.c). See pse_src/RTL8238B_POE_寄存器映射与协议.md.
 *
 * Topology: one RTL8238B presented as two I2C devices. Device 0 (0x20) drives
 * channels 0-3, device 1 (0x21) drives channels 4-7. Port control, telemetry
 * and firmware state all live at the same register offsets on both devices.
 */
#include "poe.h"
#include "log_sink.h"
#include "rtl8238b_fw.h"

#include <errno.h>
#include <stddef.h>
#include <string.h>
/* Included after log_sink.h so that a syslog() macro can never redirect
 * poe_log_message() away from the PoE log file.
 */
#include <syslog.h>

#define RTL8238B_DEVICES 2

#define REG_GLOBAL	0x01	/* global enable/init, written as 0x00 */
#define REG_PORT_CTRL	0x12	/* 2 bit per channel: 0b00=off, 0b11=on */
#define REG_FW_CTRL	0x1a	/* download control, written as 0x3f */
#define REG_VOLT_LO	0x30	/* + 4 * (channel % 4) */
#define REG_VOLT_HI	0x31
#define REG_TEMP	0x32
#define REG_CURRENT	0x33
#define REG_DEVICE_ID	0xe8	/* selects the firmware image */
#define REG_FW_STATUS	0xec	/* bit5=firmware loaded, bit2=download mode */
#define REG_FW_DATA	0xf4	/* firmware stream command byte */

#define FW_STATUS_LOADED	0x20
#define FW_STATUS_DOWNLOAD	0x04

#define PORT_CTRL_ON	0x3	/* vendor init writes 0xff = all four on */
#define PORT_CTRL_OFF	0x0

struct rtl8238b_channel {
	unsigned int device; /* 0 or 1, added to board->address */
	unsigned int offset; /* channel % 4 */
};

static struct rtl8238b_channel channel_location(unsigned int channel)
{
	struct rtl8238b_channel location = { channel / 4, channel % 4 };

	return location;
}

static unsigned int channel_address(const struct poe_board *board, unsigned int device)
{
	return board->address + device;
}

static int read_reg(struct poe_io *io, const struct poe_board *board, unsigned int device,
		    uint8_t reg, uint8_t *value)
{
	io->failed_reg = reg;
	io->operation = "read";
	return io->read(io->ctx, channel_address(board, device), reg, value);
}

static int write_reg(struct poe_io *io, const struct poe_board *board, unsigned int device,
		     uint8_t reg, uint8_t value)
{
	io->failed_reg = reg;
	io->operation = "write";
	return io->write(io->ctx, channel_address(board, device), reg, value);
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

/* Pack the per-channel 2-bit control fields of one device into 0x12. */
static uint8_t port_ctrl_byte(uint8_t mask, unsigned int device)
{
	uint8_t value = 0;
	unsigned int i;

	for (i = 0; i < 4; i++)
		if (mask & (1U << (device * 4 + i)))
			value |= PORT_CTRL_ON << (2 * i);
	return value;
}

/* ------------------------------------------------------------------ */
/* Firmware                                                           */
/* ------------------------------------------------------------------ */

/* RTL8238B keeps its firmware in SRAM: it is lost on every power cycle and
 * must be reloaded before the telemetry registers report anything. Both I2C
 * devices carry their own image, selected by their own device ID.
 */
static int download_firmware(struct poe_io *io, const struct poe_board *board)
{
	uint8_t ids[RTL8238B_DEVICES];
	unsigned int i;
	uint8_t status;
	int ret;

	io->stage = "firmware-check";
	for (i = 0; i < RTL8238B_DEVICES; i++) {
		ret = read_reg(io, board, i, REG_FW_STATUS, &status);
		if (ret)
			return ret;
		if (!(status & FW_STATUS_LOADED))
			break;
	}
	if (i == RTL8238B_DEVICES)
		return 0; /* Already resident: a warm restart kept SRAM. */

	io->stage = "firmware-id";
	for (i = 0; i < RTL8238B_DEVICES; i++) {
		ret = read_reg(io, board, i, REG_DEVICE_ID, &ids[i]);
		if (ret)
			return ret;
	}
	io->stage = "firmware-mode";
	for (i = 0; i < RTL8238B_DEVICES; i++) {
		ret = write_reg(io, board, i, REG_FW_STATUS, 0x03);
		if (ret)
			return ret;
	}
	for (i = 0; i < RTL8238B_DEVICES; i++) {
		ret = read_reg(io, board, i, REG_FW_STATUS, &status);
		if (ret)
			return ret;
		if (status & FW_STATUS_DOWNLOAD)
			continue;
		ret = write_reg(io, board, i, REG_FW_CTRL, 0x3f);
		if (ret)
			return ret;
		/* 10 ms: rtlgsw delay_ms(10); the ko probe loops 10 times over
		 * __const_udelay(0x418958), which is udelay(1000) to within 0.03%
		 * (arm64: __udelay(us) = __const_udelay(us * 0x10c7)).
		 */
		io->delay(io->ctx, 10);
		ret = write_reg(io, board, i, REG_FW_STATUS, 0x03);
		if (ret)
			return ret;
	}
	io->stage = "firmware-download";
	for (i = 0; i < RTL8238B_DEVICES; i++) {
		const uint8_t *image;
		size_t length;

		/* Device ID 2 selects the small image, everything else the big one.
		 * The vendor code compares against 2 and never validates further.
		 */
		if (ids[i] == 2) {
			image = rtl8238b_fw_small;
			length = sizeof(rtl8238b_fw_small);
		} else {
			image = rtl8238b_fw_big;
			length = sizeof(rtl8238b_fw_big);
		}
		io->failed_reg = REG_FW_DATA;
		io->operation = "write-bulk";
		ret = io->write_bulk(io->ctx, channel_address(board, i), REG_FW_DATA,
				     image, length);
		if (ret)
			return ret;
		/* 10 ms, same vendor value as above (rtlgsw delay_ms(10); ko probe
		 * 10 x __const_udelay(0x418958)).
		 */
		io->delay(io->ctx, 10);
	}
	io->stage = "firmware-verify";
	for (i = 0; i < RTL8238B_DEVICES; i++) {
		ret = read_reg(io, board, i, REG_FW_STATUS, &status);
		if (ret)
			return ret;
		if (!(status & FW_STATUS_LOADED))
			return verify_value(io, REG_FW_STATUS, FW_STATUS_LOADED, status);
	}
	return 0;
}

/* ------------------------------------------------------------------ */
/* Port control                                                       */
/* ------------------------------------------------------------------ */

static int write_port_mask(struct poe_io *io, const struct poe_board *board, uint8_t mask)
{
	unsigned int device;
	int ret;

	for (device = 0; device < RTL8238B_DEVICES; device++) {
		ret = write_reg(io, board, device, REG_PORT_CTRL,
				port_ctrl_byte(mask, device));
		if (ret)
			return ret;
	}
	return 0;
}

/* RTL8238B reports no powered/power-good pair. The only observable end of a
 * shutdown is the port control field itself.
 */
static int confirm_off(struct poe_io *io, const struct poe_board *board, uint8_t mask)
{
	unsigned int attempt, device;
	uint8_t value;
	int ret;

	io->stage = "shutdown-confirm";
	for (attempt = 0; attempt < 20; attempt++) {
		uint8_t on = 0;

		for (device = 0; device < RTL8238B_DEVICES; device++) {
			ret = read_reg(io, board, device, REG_PORT_CTRL, &value);
			if (ret)
				return ret;
			on |= value & port_ctrl_byte(mask, device);
		}
		if (!on)
			return 0;
		io->delay(io->ctx, 100);
	}
	return -ETIMEDOUT;
}

static int read_port_ctrl(struct poe_io *io, const struct poe_board *board,
			  uint8_t ctrl[RTL8238B_DEVICES])
{
	unsigned int device;
	int ret;

	for (device = 0; device < RTL8238B_DEVICES; device++) {
		ret = read_reg(io, board, device, REG_PORT_CTRL, &ctrl[device]);
		if (ret)
			return ret;
	}
	return 0;
}

/* ------------------------------------------------------------------ */
/* Chip operations                                                    */
/* ------------------------------------------------------------------ */

static int rtl8238b_initialize(struct poe_io *io, const struct poe_board *board,
			       const struct poe_policy *policy)
{
	unsigned int device;
	int ret = poe_validate(board, policy);

	io->stage = "validate";
	io->failed_reg = -1;
	if (ret)
		return ret;
	ret = download_firmware(io, board);
	if (ret)
		return ret;
	/* Vendor probe order, per device: global init, all outputs off, wait,
	 * then all outputs on. The per-port policy is applied right after.
	 */
	io->stage = "factory-setup";
	for (device = 0; device < RTL8238B_DEVICES; device++) {
		ret = write_reg(io, board, device, REG_GLOBAL, 0x00);
		if (ret)
			return ret;
		ret = write_reg(io, board, device, REG_PORT_CTRL, 0x00);
		if (ret)
			return ret;
		/* 10 ms, same vendor value as the firmware delays (rtlgsw
		 * delay_ms(10) between the two 0x12 writes; ko probe 10 x
		 * __const_udelay(0x418958)).
		 */
		io->delay(io->ctx, 10);
		ret = write_reg(io, board, device, REG_PORT_CTRL, 0xff);
		if (ret)
			return ret;
	}
	io->stage = "automatic-mode";
	ret = write_port_mask(io, board, policy->mask);
	if (ret)
		return ret;
	return poe_verify_policy(io, board, policy);
}

static int rtl8238b_set_policy(struct poe_io *io, const struct poe_board *board,
			       const struct poe_policy *old, const struct poe_policy *policy)
{
	int ret = poe_validate(board, policy);

	io->stage = "validate";
	io->failed_reg = -1;
	if (ret)
		return ret;
	/* No budget or classification register is known, so the mask is the only
	 * hardware setting that can change. Rewriting it never disturbs channels
	 * that keep their previous state.
	 */
	ret = write_port_mask(io, board, policy->mask);
	if (ret)
		return ret;
	if (old->mask & ~policy->mask) {
		ret = confirm_off(io, board, old->mask & ~policy->mask);
		if (ret)
			return ret;
	}
	return poe_verify_policy(io, board, policy);
}

static int rtl8238b_disable(struct poe_io *io, const struct poe_board *board)
{
	uint8_t mask = (1U << board->channels) - 1;
	int ret;

	io->stage = "shutdown-modes";
	ret = write_port_mask(io, board, 0);
	if (ret)
		return ret;
	return confirm_off(io, board, mask);
}

static int rtl8238b_verify_policy(struct poe_io *io, const struct poe_board *board,
				  const struct poe_policy *policy)
{
	uint8_t ctrl[RTL8238B_DEVICES];
	unsigned int device;
	int ret;

	io->stage = "configuration-monitor";
	ret = read_port_ctrl(io, board, ctrl);
	if (ret)
		return ret;
	for (device = 0; device < RTL8238B_DEVICES; device++) {
		ret = verify_value(io, REG_PORT_CTRL, port_ctrl_byte(policy->mask, device),
				   ctrl[device]);
		if (ret)
			return ret;
	}
	return 0;
}

static int rtl8238b_check_policy_status(struct poe_io *io, const struct poe_board *board,
					const struct poe_policy *policy,
					const struct poe_status *status)
{
	unsigned int device;
	int ret;

	/* Detect a reset or a lost configuration from the collected snapshot. */
	io->stage = "configuration-monitor";
	for (device = 0; device < board->channels / 4; device++) {
		ret = verify_value(io, REG_PORT_CTRL, port_ctrl_byte(policy->mask, device),
				   status->modes[device]);
		if (ret)
			return ret;
	}
	return 0;
}

/* power_mW = ((volt_word * 250) >> 13) * ((current >> 1) & 0x3f), byte-for-byte
 * identical to the vendor power loop. volt_word and the 6-bit current are raw
 * ADC counts: only the product is known to be milliwatts.
 */
static unsigned int port_power_mw(unsigned int volt_word, uint8_t current)
{
	return ((volt_word * 250U) >> 13) * ((current >> 1) & 0x3f);
}

static int rtl8238b_read_status(struct poe_io *io, const struct poe_board *board,
				struct poe_status *status)
{
	struct poe_status sample = { 0 };
	uint8_t ctrl[RTL8238B_DEVICES];
	unsigned int port, channel, device, bit;
	int ret;

	io->stage = "status";
	ret = read_port_ctrl(io, board, ctrl);
	if (ret)
		return ret;
	for (device = 0; device < RTL8238B_DEVICES; device++)
		sample.modes[device] = ctrl[device];

	for (port = 0; port < board->ports; port++) {
		struct rtl8238b_channel location;
		uint8_t volt_lo = 0, volt_hi = 0, current = 0;
		unsigned int volt_word, power;

		channel = board->port_map[port] - 1;
		bit = 1U << channel;
		location = channel_location(channel);
		ret = read_reg(io, board, location.device,
			       REG_VOLT_LO + 4 * location.offset, &volt_lo);
		if (ret)
			return ret;
		ret = read_reg(io, board, location.device,
			       REG_VOLT_HI + 4 * location.offset, &volt_hi);
		if (ret)
			return ret;
		ret = read_reg(io, board, location.device,
			       REG_CURRENT + 4 * location.offset, &current);
		if (ret)
			return ret;
		volt_word = volt_lo | ((unsigned int)volt_hi << 8);
		power = port_power_mw(volt_word, current);
		/* Only the mW product is defined by the vendor code. The voltage and
		 * current scales are undocumented, so publish no mV/mA figures and
		 * keep the raw counts for debug output instead.
		 */
		sample.voltage_mv[port] = 0;
		sample.current_ma[port] = 0;
		sample.current_raw[port] = (current >> 1) & 0x3f;
		sample.power_mw[port] = power;
		poe_log_message(LOG_DEBUG, "channel=%u volt-word=%u current-byte=0x%02x "
				"power-mw=%u", channel, volt_word, current, power);
		sample.port_state[port] = (ctrl[location.device] >> (2 * location.offset)) & 3;
		if (sample.port_state[port] == PORT_CTRL_ON && power) {
			sample.powered |= bit;
			sample.good |= bit;
		}
		/* The chip performs detection and classification autonomously and
		 * does not expose the result. Report the enabled channels so the
		 * log reader can tell "waiting for a device" from "disabled".
		 */
		if (sample.port_state[port] == PORT_CTRL_ON)
			sample.detect |= bit;
	}
	sample.classify = sample.detect;
	/* No input-voltage, budget, class or event register is known.
	 * Leaving input_valid clear disables supply monitoring in the daemon.
	 */
	sample.input_valid = false;
	*status = sample;
	return 0;
}

static int rtl8238b_poll_status(struct poe_io *io, const struct poe_board *board,
				struct poe_status *status)
{
	/* No clear-on-read event latches are known on this chip. */
	return rtl8238b_read_status(io, board, status);
}

const struct poe_chip poe_chip_rtl8238b = {
	.initialize = rtl8238b_initialize,
	.set_policy = rtl8238b_set_policy,
	.disable = rtl8238b_disable,
	.verify_policy = rtl8238b_verify_policy,
	.check_policy_status = rtl8238b_check_policy_status,
	.read_status = rtl8238b_read_status,
	.poll_status = rtl8238b_poll_status,
	.hardware_budget = false,
	.analog_scaled = false,
};
