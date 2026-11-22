/*
 * Port of the register writes of libs/dsp/tas3108 (functions.bas, cores/N/functions.bas)
 * and i2cN_wbuffer_verify (libs/interface/V40/5,6).
 * PORT: the verified writes retry at most 8 times; the original repeats until the read-back
 * matches, which hangs until the watchdog if a DSP does not answer correctly.
 * License: CC BY-NC-SA 4.0 (derived from the Forge DSP firmware, Mark Duprau).
 */
#include <string.h>
#include "bas_data.h"
#include "cmd.h"
#include "dsp.h"
#include "flavour.h"
#include "flavour_data.h"
#include "i2c.h"
#include "registers.h"
#include "tas3108.h"
#include "dsp_pool.h"
#include "boottime.h"

uint8_t dsp_core, dsp_reg;
uint16_t dsp_bq_virt_nr;
uint16_t iface_verify_error_count;

struct core { uint8_t bus, adr, rdy; };
static const struct core cores[4] = {
	{ 0, 0, 0 },
	{ 1, 0x68, BAS_TAS3108_1_RDY },
#if BAS_MODUL_TAS3108_CORES > 1
	{ 2, 0x68, BAS_TAS3108_2_RDY },
#else
	{ 0, 0, 0 },
#endif
#if BAS_MODUL_TAS3108_CORES > 2
	{ 2, 0x6A, BAS_TAS3108_3_RDY },
#else
	{ 0, 0, 0 },
#endif
};

static const struct core *cur(void)
{
	if (dsp_core < 1 || dsp_core > BAS_MODUL_TAS3108_CORES)
		return NULL;
	const struct core *c = &cores[dsp_core];
	return (hardware_dsp & (1u << c->rdy)) ? c : NULL;
}

static void put32(uint8_t at, uint32_t v)
{
	for (uint8_t i = 0; i < 4; i++)
		buffer_bytes[at + i] = (uint8_t)(v >> (8 * i));
}

static uint32_t get32(uint8_t at)
{
	uint32_t v = 0;
	for (uint8_t i = 0; i < 4; i++)
		v |= (uint32_t)buffer_bytes[at + i] << (8 * i);
	return v;
}

void dsp_calc_core(void) { dsp_core = flow_bq_core(dsp_bq_virt_nr); }  /* PORT (#47): with the spares */
void dsp_calc_reg(void) { dsp_reg = flow_bq_reg(dsp_bq_virt_nr); }

/* PORT (#66): wait after a write into the DSP as a build option (the original: dsp_writedelay = 16 ms);
 * every write is read back and repeated on a difference anyway */
#ifndef FORGE_DSP_WRITEDELAY
#define FORGE_DSP_WRITEDELAY 2   /* the original: BAS_DSP_WRITEDELAY = 16; 2 ms tested without a read-back error */
#endif
#define DSP_WRITEDELAY FORGE_DSP_WRITEDELAY

/* Prüfung 19.11.2026: intermediate clickless steps without the wait; they are read back and
 * repeated like every write, and the next step follows within 1/64 s */
#ifndef FORGE_DSP_RAMP_WRITEDELAY
#define FORGE_DSP_RAMP_WRITEDELAY 0
#endif

/* i2cN_wbuffer_verify: read back (twice at most), count differences */
static uint8_t trace_detail;  /* #94: record the first mismatch of a write once */
void dsp_trace(uint8_t core, uint8_t kind, uint8_t reg, uint8_t info);
static void wbuffer_verify(const struct core *c, uint8_t size, const uint8_t *expect)
{
	uint8_t e_count = 0;
	for (uint8_t i = 1; i <= 2; i++) {
		i2c_device_addr = c->adr;
		i2c_reg = dsp_reg;
		i2c_size = size;
		i2c_rbuffer(c->bus);
		e_count = 0;
		for (uint8_t ii = 1; ii <= size; ii++)
			if (expect[ii] != buffer_bytes[ii])
				e_count++;
		if (e_count == 0)
			break;
		delay_ms(5);
	}
	if (e_count > 0) {
		errorcount(BAS_ERRORCOUNT_FW_IFACE_VERIFY);
		iface_verify_error_count++;
		if (trace_detail) {  /* kind 6: first differing byte (reg = index, info = read), 7: count / expected, 8: bus error */
			trace_detail = 0;
			uint8_t ii = 1;
			while (ii < size && expect[ii] == buffer_bytes[ii])
				ii++;
			dsp_trace((uint8_t)(c - cores), 6, ii, buffer_bytes[ii]);
			dsp_trace((uint8_t)(c - cores), 7, e_count, expect[ii]);
			dsp_trace((uint8_t)(c - cores), 8, 0, i2c_err);
		}
	}
}

/* write buffer_bytes(1..size) and verify, as dsp_N_set_vol / dsp_N_set_biquad; 1 = verified */
/* PORT (#94): device test 22.11.2026: PEEKs (register 0x06/0x07, level display) and coefficient writes
 * on the same TAS3108 right after each other stopped DSP core 1 now and then (dips on sum A with the
 * S/PDIF level shown: hung after 5 and 9 dips; 30 dips without PEEK: none). Between a PEEK and a
 * write (either way) on one core at least DSP_PEEK_GAP_MS pass; writes after writes and PEEKs after
 * PEEKs are not slowed down. */
#define DSP_PEEK_GAP_MS 2
#ifndef DSP_PEEK_WRITE_MS
#ifdef __AVR__
#define DSP_PEEK_WRITE_MS 2
#else
#define DSP_PEEK_WRITE_MS 2
#endif
#endif
/* PORT (#94): with the 2 ms gap core 1 still hung now and then (GUI-like test: 1 in 5 rounds of adding
 * an EQ to sum A and dragging its gain). No PEEK on a core while it is being written to: none within
 * DSP_PEEK_QUIET_MS after its last write (the level display stands still meanwhile). */
#ifndef DSP_PEEK_QUIET_MS
#ifdef __AVR__
#define DSP_PEEK_QUIET_MS 250
#else
#define DSP_PEEK_QUIET_MS 0   /* host tests: the clock of the harness does not run between commands */
#endif
#endif
static uint8_t gap_last[BAS_MODUL_TAS3108_CORES + 1];   /* 1 = last access a PEEK, 2 = a write */
static uint16_t gap_t[BAS_MODUL_TAS3108_CORES + 1];

/* PORT (#94): trace of the last DSP accesses, `get system.dsp.trace` = [[ms, core, kind, reg, info], ...]
 * oldest first. kind: 1 verified write (info = attempts, 0 = failed after all), 2 plain write,
 * 3 mixer row, 4 PEEK, 5 wait between PEEK and write (info = ms). After a failed write it records
 * DSP_TRACE_AFTER more and then stops until `set system.dsp.trace=0`. */
#define DSP_TRACE_N 32
#define DSP_TRACE_AFTER 16
static struct { uint16_t ms; uint8_t core, kind, reg, info; } trace[DSP_TRACE_N];
static uint8_t trace_pos, trace_left = 0xFF;

void dsp_trace(uint8_t core, uint8_t kind, uint8_t reg, uint8_t info)
{
	if (trace_left == 0)
		return;
	trace[trace_pos].ms = BOOT_RAW_TO_MS(boot_clock_raw());
	trace[trace_pos].core = core;
	trace[trace_pos].kind = kind;
	trace[trace_pos].reg = reg;
	trace[trace_pos].info = info;
	trace_pos = (uint8_t)((trace_pos + 1) % DSP_TRACE_N);
	if (trace_left != 0xFF)
		trace_left--;
	else if (kind == 6)
		trace_left = DSP_TRACE_AFTER;  /* from the first read-back mismatch on */
}

void dsp_trace_reset(void)
{
	memset(trace, 0, sizeof trace);
	trace_pos = 0;
	trace_left = 0xFF;
}

void dsp_trace_out(void)
{
	cmd_answer_header();
	out_chr('[');
	uint8_t first = 1;
	for (uint8_t k = 0; k < DSP_TRACE_N; k++) {
		uint8_t i = (uint8_t)((trace_pos + k) % DSP_TRACE_N);
		if (!trace[i].kind)
			continue;
		if (!first)
			out_chr(',');
		first = 0;
		out_chr('['); out_u16(trace[i].ms); out_chr(','); out_u16(trace[i].core); out_chr(',');
		out_u16(trace[i].kind); out_chr(','); out_u16(trace[i].reg); out_chr(','); out_u16(trace[i].info); out_chr(']');
	}
	out_chr(']');
	out_crlf();
}

void dsp_core_access(uint8_t core, uint8_t peek)
{
	if (core < 1 || core > BAS_MODUL_TAS3108_CORES)
		return;
	uint8_t kind = peek ? 1 : 2;
	if (gap_last[core] && gap_last[core] != kind) {
		/* PEEK -> write: DSP_PEEK_WRITE_MS (the TAS3108's 8051 may still be busy with the PEEK) */
		uint16_t gap = kind == 2 ? DSP_PEEK_WRITE_MS : DSP_PEEK_GAP_MS;
		uint16_t ms = BOOT_RAW_TO_MS((uint16_t)(boot_clock_raw() - gap_t[core]));
		if (ms < gap + 1) {
			delay_ms((uint16_t)(gap + 1 - ms));  /* +1: the clock counts in whole ms */
			dsp_trace(core, 5, 0, (uint8_t)(gap + 1 - ms > 255 ? 255 : gap + 1 - ms));
		}
	}
}

void dsp_core_done(uint8_t core, uint8_t peek)
{
	if (core < 1 || core > BAS_MODUL_TAS3108_CORES)
		return;
	gap_last[core] = peek ? 1 : 2;
	gap_t[core] = boot_clock_raw();
}

static uint8_t verified_write_(const struct core *c, uint8_t size);
uint8_t dsp_write_ramp;  /* Prüfung 19.11.2026: set by bqramp for intermediate steps (I2C_RAMP clock) */
static uint8_t verified_write(const struct core *c, uint8_t size)
{
	uint8_t core = (uint8_t)(c - cores);
	dsp_core_access(core, 0);
	i2c_speed(c->bus, dsp_write_ramp ? I2C_RAMP : 1);   /* PORT (#66): read back and repeated, so at the fast clock */
	uint8_t ok = verified_write_(c, size);
	i2c_speed(c->bus, 0);
	dsp_core_done(core, 0);
	return ok;
}
static uint8_t verified_write_(const struct core *c, uint8_t size)
{
	uint8_t copy[BAS_DSP_REG_SIZE_BIQUAD + 1];
	memcpy(copy, buffer_bytes, (size_t)size + 1);  /* iface_verify_copy2verify */
	trace_detail = 1;
	for (uint8_t attempt = 0; attempt < 8; attempt++) {
		uint16_t ec = iface_verify_error_count;
		(void)attempt;
		i2c_device_addr = c->adr;
		i2c_reg = dsp_reg;
		i2c_size = size;
		i2c_wbuffer(c->bus);
		/* #94: core 1 (the longest program) answered the read-back of an intermediate step written
		 * without the wait with NACK; after the bus clear it stopped. The wait stays there. */
		delay_ms(dsp_write_ramp && c != &cores[1] ? FORGE_DSP_RAMP_WRITEDELAY : DSP_WRITEDELAY);
#if BAS_FEATURE_DATA_VERIFY > 0
		wbuffer_verify(c, size, copy);
		if (ec == iface_verify_error_count) {
			dsp_trace((uint8_t)(c - cores), 1, dsp_reg, (uint8_t)(attempt + 1));
			return 1;
		}
		i2c_reset(c->bus);
		memcpy(buffer_bytes, copy, (size_t)size + 1);  /* iface_verify_copy2buffer */
#else
		(void)ec;
		return 1;
#endif
	}
	dsp_trace((uint8_t)(c - cores), 1, dsp_reg, 0);
	return 0;
}

static void plain_write(const struct core *c, uint8_t reg, uint8_t size, uint8_t wait)
{
	dsp_core_access((uint8_t)(c - cores), 0);
	i2c_device_addr = c->adr;
	i2c_reg = reg;
	i2c_size = size;
	i2c_wbuffer(c->bus);
	dsp_core_done((uint8_t)(c - cores), 0);
	dsp_trace((uint8_t)(c - cores), 2, reg, 1);
	if (wait)
		delay_ms(DSP_WRITEDELAY);
}

/* PORT (#58): buffer_bytes(1..size) to dsp_core/dsp_reg, verified as the biquads */
void dsp_set_reg(uint8_t size)
{
	const struct core *c = cur();
	if (c && size <= BAS_DSP_REG_SIZE_BIQUAD)
		verified_write(c, size);
}

/* PORT (#66): what each biquad place of the DSPs holds, as CRC-16 of its coefficients (0 = not known).
 * A place that already holds the coefficients is not written again: a preset change rewrites all
 * places, most of them the same (empty in both presets), and each write costs dsp_writedelay
 * (16 ms) plus the read back. Starts empty (RAM after every reset); every DSP restart is a reset
 * of the MCU. Indexed by the virtual biquad number (dsp_bq_virt_nr), which all callers set. */
#define BQ_PLACES (BAS_FLOW_MAX_DSP_BIQUADS + DSP_BQ_EXTRA_MAX + 1)  /* the pools' spares included */
static uint16_t bq_held[BQ_PLACES];
uint16_t dsp_bq_skipped, dsp_bq_written, dsp_bq_read_same;  /* since the start (diagnostics, tests) */
void dsp_bq_forget(void) { memset(bq_held, 0, sizeof bq_held); }

void dsp_set_biquad(const uint32_t raw[6])
{
	const struct core *c = cur();
	if (!c)
		return;
	for (uint8_t i = 1; i <= 5; i++)
		put32((uint8_t)((i - 1) * 4 + 1), raw[i]);  /* buffer_dwords(i) */
	uint16_t crc = (uint16_t)(bas_crc16(&buffer_bytes[1], BAS_DSP_REG_SIZE_BIQUAD) | 1u);  /* never 0 */
	uint16_t n = dsp_bq_virt_nr;
	if (n < BQ_PLACES && bq_held[n] == crc) {
		dsp_bq_skipped++;
		return;
	}
	if (n < BQ_PLACES && bq_held[n] == 0) {
		/* PORT (#67): not known yet (after the start): read the place first, at FORGE_I2C_CHECK_KHZ.
		 * Reading is a third of a verified write; most places of the first preset hold already
		 * what the program put there. A wrong read only means the place is written. */
		uint8_t want[BAS_DSP_REG_SIZE_BIQUAD + 1];
		memcpy(want, buffer_bytes, sizeof want);
		i2c_speed(c->bus, I2C_CHECK);
		i2c_device_addr = c->adr;
		i2c_reg = dsp_reg;
		i2c_size = BAS_DSP_REG_SIZE_BIQUAD;
		i2c_rbuffer(c->bus);
		i2c_speed(c->bus, 0);
		uint8_t same = i2c_err == 0 && memcmp(&buffer_bytes[1], &want[1], BAS_DSP_REG_SIZE_BIQUAD) == 0;
		memcpy(buffer_bytes, want, sizeof want);
		if (same) {
			bq_held[n] = crc;
			dsp_bq_read_same++;
			return;
		}
	}
	dsp_bq_written++;
	uint8_t ok = verified_write(c, BAS_DSP_REG_SIZE_BIQUAD);
	if (n < BQ_PLACES)
		bq_held[n] = ok ? crc : 0;
}

void dsp_get_biquad(uint32_t raw[6])
{
	const struct core *c = cur();
	if (!c)
		return;
	i2c_device_addr = c->adr;
	i2c_reg = dsp_reg;
	i2c_size = BAS_DSP_REG_SIZE_BIQUAD;
	i2c_rbuffer(c->bus);
	for (uint8_t i = 1; i <= 5; i++)
		raw[i] = get32((uint8_t)((i - 1) * 4 + 1));
}

void dsp_set_vol_out(int32_t vol_calc)
{
	const struct core *c = cur();
	if (!c)
		return;
	put32(1, (uint32_t)vol_calc);  /* buffer_long */
	verified_write(c, BAS_DSP_REG_SIZE_VOL);
}

void dsp_set_mux(uint32_t state)
{
	const struct core *c = cur();
	if (!c)
		return;
	put32(1, state);
	plain_write(c, dsp_reg, BAS_DSP_REG_SIZE_MUX, 0);
}

uint32_t dsp_get_mux(void)
{
	const struct core *c = cur();
	if (!c)
		return 0;
	i2c_device_addr = c->adr;
	i2c_reg = dsp_reg;
	i2c_size = BAS_DSP_REG_SIZE_MUX;
	i2c_rbuffer(c->bus);
	return get32(1);
}

void dsp_set_delay(uint32_t value)
{
	const struct core *c = cur();
	if (!c)
		return;
	put32(1, value);
	plain_write(c, dsp_reg, BAS_DSP_REG_SIZE_DELAY, 1);
}

static void ack_delay(uint8_t n)
{
	const struct core *c = &cores[n];
	if (!(hardware_dsp & (1u << c->rdy)))
		return;
	put32(1, 1);
	plain_write(c, BAS_DSP_SUB_DELAY_ACK, BAS_DSP_REG_SIZE_DELAY, 1);
}

void dsp_ack_delay(void)
{
	/* every core, as dsp_ack_delay */
	for (uint8_t n = 1; n <= BAS_MODUL_TAS3108_CORES; n++)
		ack_delay(n);
}

void dsp_1_ack_delay(void) { ack_delay(1); }  /* input delays live in core 1 */

void dsp_set_phase_reg(uint32_t value)
{
	const struct core *c = cur();
	if (!c)
		return;
	put32(1, value);
	plain_write(c, dsp_reg, BAS_DSP_REG_SIZE_PHASE, 1);
}

void dsp_init_vum(void)
{
	dsp_core = 1;  /* dsp_init_vum calls dsp_1_init_vum */
	const struct core *c = cur();
	if (!c)
		return;
	plain_write(c, dsp_reg, BAS_DSP_REG_SIZE_VUM, 0);
}

void dsp_get_vum(void)
{
	dsp_core = 1;
	const struct core *c = cur();
	if (!c)
		return;
	i2c_device_addr = c->adr;
	i2c_reg = dsp_reg;
	i2c_size = BAS_DSP_REG_SIZE_VUM;
	i2c_rbuffer(c->bus);
}

/* PORT (#51): TAS3108 PEEK (SLES152B 7.4): register 0x06 = [unused, memory, addr high, addr low]
 * (normal bit order, checked on the device), then 0x07 returns 8 bytes, LSB first in
 * buffer_bytes(1). mem: 1 = coefficients, 2 = data. -> 0 if the core is not present. */
uint8_t dsp_peek(uint8_t core, uint8_t mem, uint16_t addr, uint8_t out[8])
{
	if (core < 1 || core > BAS_MODUL_TAS3108_CORES)
		return 0;
	const struct core *c = &cores[core];
	if (!(hardware_dsp & (1u << c->rdy)))
		return 0;
	if (DSP_PEEK_QUIET_MS && gap_last[core] == 2 && BOOT_RAW_TO_MS((uint16_t)(boot_clock_raw() - gap_t[core])) < DSP_PEEK_QUIET_MS)
		return 0;  /* #94: the core is being written to */
	dsp_core_access(core, 1);  /* #94 */
	buffer_bytes[4] = 0;  /* sent backwards: [4] first */
	buffer_bytes[3] = mem;
	buffer_bytes[2] = (uint8_t)(addr >> 8);
	buffer_bytes[1] = (uint8_t)addr;
	i2c_device_addr = c->adr;
	i2c_reg = 0x06;
	i2c_size = 4;
	i2c_wbuffer(c->bus);
	i2c_device_addr = c->adr;
	i2c_reg = 0x07;
	i2c_size = 8;
	i2c_rbuffer(c->bus);
	dsp_core_done(core, 1);
	dsp_trace(core, 4, (uint8_t)addr, 1);
	for (uint8_t i = 0; i < 8; i++)
		out[i] = buffer_bytes[i + 1];
	return 1;
}
