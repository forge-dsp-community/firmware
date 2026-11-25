/*
 * Port of libs/spdif/ct7302 (functions.bas, 20/, 22/).
 * License: CC BY-NC-SA 4.0 (derived from the Forge DSP firmware, Mark Duprau).
 */
#include "boottime.h"
#include "flavour.h"
#if BAS_MODUL_CT7302 > 0
#include "board.h"
#include "ct7302.h"
#include "eram.h"
#include "i2c.h"
#include "registers.h"
#include "store.h"

#if BAS_MODUL_CT7302_I2C == 1
#define BUS 1
#else
#define BUS 2
#endif

uint8_t ct7302_active_channel;

static void w(uint8_t reg, uint8_t v) { i2c_reg = reg; buffer_byte = v; i2c_wbyte(BUS); }
static uint8_t r(uint8_t reg) { i2c_reg = reg; i2c_rbyte(BUS); return buffer_byte; }
static void rmw(uint8_t reg, uint8_t and_mask, uint8_t or_mask)
{
	r(reg);
	buffer_byte = (uint8_t)((buffer_byte & and_mask) | or_mask);
	i2c_wbyte(BUS);  /* i2c_reg still set */
}

/* i2c_device_addr is set by the caller, as in BASCOM */
static void ct7302_init_sub(void)
{
	rmw(BAS_CT7302_REG_SRC_RATIO_FREQ, 0x80, 0x13);
	w(BAS_CT7302_REG_RXTX_CONFIG2, 0x02);           /* right volume independent */
	rmw(BAS_CT7302_REG_POWER_CONFIG, 0xFC, 0);       /* osc/xtal normal */
	rmw(BAS_CT7302_REG_TIMING_JITTER_CONFIG, 0x1F, 0);
	rmw(BAS_CT7302_REG_CONFIG1, 0xC0, 0);            /* fading normal */
	w(BAS_CT7302_REG_CDR_FREQ_COARSE, 0x23);
	w(BAS_CT7302_REG_CDR_PHASE_CS_GAIN, 0x19);
	w(BAS_CT7302_REG_CDR_PHASE_VS_GAIN, 0x1E);
	rmw(BAS_CT7302_REG_IN_PLL_CONFIG, 0xF3, 0);
	rmw(BAS_CT7302_REG_OUT_PLL_CONFIG2, 0xF3, 0);
	rmw(BAS_CT7302_REG_MISCELLANEOUS1, 0xCF, 0x02);
	w(BAS_CT7302_REG_INOUT_PLL_LOW_BAND, 0x33);
	w(BAS_CT7302_REG_SEL_DIGITAL_OSC_FREQ, 0x08);
	w(BAS_CT7302_REG_JITTER_RXTX_OSC, 0x01);
	w(BAS_CT7302_REG_PIN_DSD_IN, BAS_CT7302_REG_PIN_DSD_IN_INIT);
	w(BAS_CT7302_REG_INPUT_SRCMODE, BAS_CT7302_REG_INPUT_SRCMODE_INIT);
	w(BAS_CT7302_REG_RXTX_CONFIG1, BAS_CT7302_REG_RXTX_CONFIG1_INIT);
	w(BAS_CT7302_REG_SILENCE_DETECT, BAS_CT7302_REG_SILENCE_DETECT_INIT);
	uint8_t opt = eram_rb(ERAM_CT7302_REGISTER_OPTIONS);
	/* bit 0 ("192k for coaxial/optical spdif") has no code in the original */
	if (opt & 0x80)
		rmw(BAS_CT7302_REG_RX_STATE1, 0xFF, 0x40);   /* SPDIF RX 32 bit */
}

static uint8_t detect(uint8_t adr, uint8_t rdy)
{
	if (!(channel_open & (1u << (4 + BUS))))
		return 0;
	twi_start(BUS);
	twi_wbyte(BUS, adr);
	if (i2c_err == 0)
		hardware_other |= 1u << rdy;
	else
		errorcount((uint8_t)(rdy + BAS_ERRORCOUNT_OFFSET_OTHER));
	twi_stop(BUS);
	return 1;
}

static void spdif_init_body(void)
{
#if BAS_MODUL_CT7302_20 == 1
	detect(0x20, BAS_CT7302_20_RDY);
	if (hardware_other & (1u << BAS_CT7302_20_RDY)) {
		i2c_device_addr = 0x20;
		ct7302_init_sub();
		ct7302_active_channel = 0x00;
	}
#endif
#if BAS_MODUL_CT7302_22 == 1
	detect(0x22, BAS_CT7302_22_RDY);
	if (hardware_other & (1u << BAS_CT7302_22_RDY)) {
		i2c_device_addr = 0x22;
		ct7302_init_sub();
	}
#endif
}

void spdif_init(void)
{
	spdif_init_body();
	boot_mark(BT_SPDIF);
}

void ct7302_20_set_mux(uint8_t channel)
{
	if (!(hardware_other & (1u << BAS_CT7302_20_RDY)))
		return;
#if BAS_MODUL_CT7302_USB_AT_I2S == 1
	if (channel == 0x01)
		channel = 0x05;  /* USB (XMOS) comes in over I2S at RX5 */
#endif
	ct7302_active_channel = channel;
	i2c_device_addr = 0x20;
	rmw(BAS_CT7302_REG_INPUT_SRCMODE, 0xF8, channel);
}

void ct7302_20_set_vol(uint8_t channel, uint8_t value)
{
	if (!(hardware_other & (1u << BAS_CT7302_20_RDY)))
		return;
	i2c_device_addr = 0x20;
	w(channel == 1 ? BAS_CT7302_REG_VOL_RIGHT : BAS_CT7302_REG_VOL_LEFT, (uint8_t)(255 - value));
}

void ct7302_22_set_mux(uint8_t channel)
{
	if (!(hardware_other & (1u << BAS_CT7302_22_RDY)))
		return;
#if BAS_MODUL_CT7302_USB_AT_I2S == 1
	if (channel == 0x01)
		channel = 0x05;
#endif
	i2c_device_addr = 0x22;
	rmw(BAS_CT7302_REG_INPUT_SRCMODE, 0xF8, channel);
}

/* PORT (#82): S/PDIF output from the DSP. The CT7302 is an SRC bridge: register 4 selects ONE input,
 * which goes to all its outputs, the S/PDIF transmitter included. The second one (0x22) feeds the
 * optical port 2 when JP1 makes it an output; at its I2S IN-2 (input 7) lie DSP.BCLK/DSP.LRCLK and
 * DA.IN.4: on the 96ELN PCB 4.0 core 1 DO7/DO8 = mixer rows 3/4 (output 1/2 before the output chains of
 * core 3; measured 21.11.2026). Default: it scans the inputs for the signal events, the output then carries whichever input
 * it looks at. 1 = fixed on input 7, the scan only asks the active input. Not with the four channel
 * option (0x22 is the receiver of optical 2 then). */
#define EE_SPDIF_OUT 0x0F14   /* 1 = from the DSP, 0/0xFF = input scan */

#define SPDIF_VOL_START 0x24  /* registers 9/10 after reset: -18 dB (plus VOLUME_SHIFT_GAIN, 0 dB here) */

#define EE_SPDIF_RATE 0x0F15  /* #83: output rate in kHz (table below), 0/0xFF = reference clock */

uint8_t ct7302_spdif_out_dsp(void)
{
	return eram_rb(EE_SPDIF_OUT) == 1;
}

/* PORT (#83): SW_SEL_SRC_FREQUENCY (register 5, bits 3:0) per rate in kHz; 44/88/176 stand for 44.1/88.2/176.4 */
static const uint8_t rate_khz[] = {32, 44, 48, 88, 96, 176, 192};   /* 14 bytes, in RAM */
static const uint8_t rate_idx[] = {0x01, 0x02, 0x03, 0x05, 0x06, 0x08, 0x09};
#define RATE_N (sizeof rate_khz)

static uint8_t rate_index(uint8_t khz)  /* 0xFF = not in the table */
{
	for (uint8_t i = 0; i < RATE_N; i++)
		if (rate_khz[i] == khz)
			return rate_idx[i];
	return 0xFF;
}

uint8_t ct7302_spdif_rate(void)
{
	uint8_t v = eram_rb(EE_SPDIF_RATE);
	return rate_index(v) == 0xFF ? 0 : v;
}

/* the SRC of 0x22: with the DSP output and a rate, asynchronous to the rate of the table (mode 3);
 * else the start values (mode of REG_INPUT_SRCMODE_INIT, register 5 as ct7302_init_sub sets it).
 * The mode acts on every output of the chip, the S/PDIF transmitter and its I2S outputs. */
static void apply_rate(void)
{
	uint8_t idx = ct7302_spdif_out_dsp() ? rate_index(ct7302_spdif_rate()) : 0xFF;
	i2c_device_addr = 0x22;
	if (idx != 0xFF) {
		rmw(BAS_CT7302_REG_INPUT_SRCMODE, 0x8F, 0x30);
		rmw(BAS_CT7302_REG_SRC_RATIO_FREQ, 0xF0, idx);
	} else {
		rmw(BAS_CT7302_REG_INPUT_SRCMODE, 0x8F, BAS_CT7302_REG_INPUT_SRCMODE_INIT & 0x70);
		rmw(BAS_CT7302_REG_SRC_RATIO_FREQ, 0x80, 0x13);
	}
}

uint8_t ct7302_set_spdif_rate(uint8_t khz)
{
	if (khz != 0 && rate_index(khz) == 0xFF)
		return 0;
	if (!(hardware_other & (1u << BAS_CT7302_22_RDY)))
		return 0;
	eram_wb(EE_SPDIF_RATE, khz ? khz : 0xFF);
	apply_rate();
	return 1;
}

uint8_t ct7302_set_spdif_out(uint8_t dsp)
{
	if (!(hardware_other & (1u << BAS_CT7302_22_RDY)))
		return 0;
	if (dsp && PIN_READ(PIN_OPTION_SPDIF_4CH) == 0x01)
		return 0;
	eram_wb(EE_SPDIF_OUT, dsp ? 1 : 0xFF);
	i2c_device_addr = 0x22;
	if (dsp)
		rmw(BAS_CT7302_REG_INPUT_SRCMODE, 0xF8, 0x07);   /* I2S IN-2 = DA.IN.4 */
	/* PORT (#82): ct7302_init_sub clears VOLUME_SHIFT_GAIN (+18 dB -> +0 dB, as the original); with the
	 * start value 0x24 of registers 9/10 (-18 dB) the S/PDIF output was 18 dB below the DSP. With the
	 * DSP output 0 dB (0x00), with the input scan the start value as before. */
	w(BAS_CT7302_REG_VOL_LEFT, dsp ? 0x00 : SPDIF_VOL_START);
	w(BAS_CT7302_REG_VOL_RIGHT, dsp ? 0x00 : SPDIF_VOL_START);
	apply_rate();   /* #83: the chosen rate only with the DSP output; back to the start values without */
	return 1;
}

/* ct7302_get_form_state: 1 = valid PCM (no silence/error flags) */
static uint8_t form_state(uint8_t adr, uint8_t rdy)
{
	if (!(hardware_other & (1u << rdy)))
		return 0;
	i2c_device_addr = adr;
	return (r(BAS_CT7302_REG_PCM_SILENCE) & 0xE0) == 0x00;
}

uint8_t ct7302_20_get_form_state(void) { return form_state(0x20, BAS_CT7302_20_RDY); }
uint8_t ct7302_22_get_form_state(void) { return form_state(0x22, BAS_CT7302_22_RDY); }

/* PORT (#95): lock of the S/PDIF receiver of (20) on its active input: REG_STATUS_8E bit 0
 * CTRL_SPDIF_CDR_LOCK_IN and bit 3 STATUS_BLOCK_LOCKED (datasheet V0.11, p. 69). Locked = 0x0F on the
 * 96ELN PCB 4.0 (coax 48 kHz), 0x84/0x85 while lost. 0x89 bit 7 (input PLL) stayed set for a while after
 * the loss, 0x7A only tells silence, not lock. Inputs 0-4 only (5-7 are I2S). */
#define REG_STATUS_8E 0x8E
#define LOCK_BITS     0x09
#define EE_SPDIF_HOLD 0x0F16  /* #95: 0 = scan always (original), 1/0xFF = hold the scan while locked */

uint8_t ct7302_20_spdif_locked(void)
{
	if (!(hardware_other & (1u << BAS_CT7302_20_RDY)) || ct7302_active_channel > 0x04)
		return 0;
	i2c_device_addr = 0x20;
	return (r(REG_STATUS_8E) & LOCK_BITS) == LOCK_BITS;
}

uint8_t ct7302_spdif_hold(void)
{
	return eram_rb(EE_SPDIF_HOLD) != 0;
}

uint8_t ct7302_set_spdif_hold(uint8_t on)
{
	if (!(hardware_other & (1u << BAS_CT7302_22_RDY)))
		return 0;
	eram_wb(EE_SPDIF_HOLD, on ? 1 : 0);
	return 1;
}

uint8_t ct7302_20_get_mux(void)
{
	i2c_device_addr = 0x20;
	uint8_t v = r(BAS_CT7302_REG_INPUT_SRCMODE) & 0x07;
#if BAS_MODUL_CT7302_USB_AT_I2S == 1
	if (v == 0x05)
		v = 0x01;
#endif
	return v;
}
#endif
