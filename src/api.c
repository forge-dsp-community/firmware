/*
 * Port of libs/api (functions.bas, mute/functions.bas, mix/functions.bas) and the load
 * sequence of modc_load_preset. License: CC BY-NC-SA 4.0 (derived from the Forge DSP firmware).
 */
#include "boottime.h"
#include "log.h"
#include <math.h>
#include "api.h"
#include "factory.h"
#include "sys_init.h"
#include "bqramp.h"
#include "bas_data.h"
#include "biquad.h"
#include "board.h"
#include "cmd.h"
#include "ct7302.h"
#include "dac.h"
#include "dsp.h"
#include "eram.h"
#include "events.h"
#include "gala.h"
#include "modc.h"
#include "flavour.h"
#include "flavour_data.h"
#include "i2c.h"
#include "registers.h"
#include "store.h"
#include "tas3108.h"

#ifndef FORGE_AUDIO
#define FORGE_AUDIO 0
#endif

uint8_t api_store = 1;  /* api_init */
int16_t api_autounmute_counter;
static uint8_t api_mute_out_prio[5];  /* 1-based */
uint32_t store_mix_changed;

#define SV(base, i) storage_non_biquadvalues[(base) + (i) - 1]
#define storage_mute_out storage_non_biquadvalues[BAS_STORAGE_MUTE_OUT_BASE]
#define storage_phase    storage_non_biquadvalues[BAS_STORAGE_PHASE_BASE]
#define store_mix_position storage_non_biquadvalues[BAS_STORE_MIX_POSITION_BASE]

static uint16_t sw_get(uint16_t base, uint8_t i)
{
	uint16_t a = (uint16_t)(base + (i - 1) * 2);
	return storage_non_biquadvalues[a] | (uint16_t)storage_non_biquadvalues[a + 1] << 8;
}

static void sw_set(uint16_t base, uint8_t i, uint16_t v)
{
	uint16_t a = (uint16_t)(base + (i - 1) * 2);
	storage_non_biquadvalues[a] = v & 0xFF;
	storage_non_biquadvalues[a + 1] = v >> 8;
}

void api_init(void)
{
	/* PORT (#81): start preset kept at an automatic sysinit unless never written */
	if (factory_write() || (sysinit_active() && registers[BAS_REG_LOAD_PRESET] == 0xFF))
		reset_register(BAS_REG_LOAD_PRESET);
	api_store = 1;
}

void api_init_mute(void)
{
	for (uint8_t i = 1; i <= api_mute_out_prio_hardware; i++)
		api_mute_out_prio[i] = api_mute_out_off;
	api_autounmute_counter = BAS_API_AUTOUNMUTE_OFF;
}

static int16_t dac_mute_last = -1;  /* PORT: mask last written by the API, -1 = not yet (DAC init: all muted) */

void api_set_out_mute(void)
{
	uint8_t mute_value = storage_mute_out;
	for (uint8_t i = 1; i <= api_mute_out_prio_hardware; i++)
		mute_value |= api_mute_out_prio[i];
	dac_set_mute(mute_value);
	dac_mute_last = mute_value;
}

void api_set_out_mute_store(uint8_t v)
{
	storage_mute_out = v;
	api_set_out_mute();
	storage_flags++;
}

uint8_t api_get_out_mute_store(void) { return storage_mute_out; }

uint8_t api_get_out_mute_prio(uint8_t prio)
{
	return prio >= 1 && prio <= api_mute_out_prio_hardware ? api_mute_out_prio[prio] : 0;
}

void api_set_out_mute_prio(uint8_t prio, uint8_t v)
{
	if (prio < 1 || prio > api_mute_out_prio_hardware)   /* PORT (#80): as the getter */
		return;
	if (api_mute_out_prio[prio] != v) {
		api_mute_out_prio[prio] = v;
		api_set_out_mute();
	}
	if (prio == api_mute_out_prio_change && api_autounmute_counter < BAS_API_AUTOUNMUTE_SET)
		api_autounmute_counter = BAS_API_AUTOUNMUTE_SET;
}

void api_set_out_mute_change(void)
{
	api_set_out_mute_prio(api_mute_out_prio_change, api_mute_out_on);
	if (api_autounmute_counter < BAS_API_AUTOUNMUTE_SET)
		api_autounmute_counter = BAS_API_AUTOUNMUTE_SET;
}

void api_mainloop(void)
{
	if (api_autounmute_counter == BAS_API_AUTOUNMUTE_CALL)
		api_set_out_mute_prio(api_mute_out_prio_change, api_mute_out_off);
	if (api_autounmute_counter > BAS_API_AUTOUNMUTE_MIN)
		api_autounmute_counter--;
	if (api_autounmute_counter == BAS_API_AUTOUNMUTE_CALL)  /* after the last decr, before unmute */
		api_set_mix_reg();
}

/* input volume: DSP core 1, register from data_flow_dsp_input_vol_reg */
void api_set_in_vol(uint8_t ch, uint8_t v)
{
	if (ch >= 1 && ch <= 6) {
		dsp_core = 1;  /* dsp_set_vol -> dsp_1_set_vol */
		dsp_reg = BAS_LOOKUP_B(data_flow_dsp_input_vol_reg, ch);
		dsp_set_vol_out((int32_t)bas_data_word(&data_dsp_vol[v * 4]) |
		                (int32_t)bas_data_word(&data_dsp_vol[v * 4 + 2]) << 16);
	}
	if (api_store == 1) {
		SV(BAS_STORAGE_VOL_IN_BASE, ch) = v;
		storage_flags++;
	}
}

uint8_t api_get_in_vol(uint8_t ch) { return SV(BAS_STORAGE_VOL_IN_BASE, ch); }

void api_set_spdif_vol(uint8_t ch, uint8_t v)
{
	modc_loudness_touch();  /* M6: loudness follows gain.spdif when it has no event source */
#if BAS_MODUL_CT7302 > 0
	ct7302_20_set_vol(ch, v);
#elif BAS_MODUL_SRC43X2 > 0
	src43x2_set_vol(ch, v);
#endif
	if (api_store == 1) {
		SV(BAS_STORAGE_VOL_SPDIF_BASE, ch) = v;
		storage_flags++;
	}
}

uint8_t api_get_spdif_vol(uint8_t ch) { return SV(BAS_STORAGE_VOL_SPDIF_BASE, ch); }

/* PORT (#92): outputs held silent while a filter type changes (bit 0 = output 1); their level is
 * not written meanwhile (volume block, GALA) and restored by api_dip_out(ch, 0) */
static uint8_t out_dip_mask;

void api_set_out_vol_hw(uint8_t ch, uint8_t v)
{
	if (ch < 1 || ch > 8)
		return;
	if (out_dip_mask & (1u << (ch - 1)))
		return;
	v = gala_out_vol(ch, v);  /* PORT (#71): speed-dependent boost, never stored */
	dsp_core = BAS_LOOKUP_B(data_flow_dsp_output_vol_dsp, ch);
	dsp_reg = BAS_LOOKUP_B(data_flow_dsp_output_vol_reg, ch);
	dsp_set_vol_out((int32_t)bas_data_word(&data_dsp_vol[v * 4]) |
	                (int32_t)bas_data_word(&data_dsp_vol[v * 4 + 2]) << 16);  /* dsp_bq_vol_calc */
}

void api_set_out_vol(uint8_t ch, uint8_t v)
{
	if (!(modc_active_affect & (1u << BAS_ONOUTVOL)))
		api_set_out_vol_hw(ch, v);
	else
		modc_set_run(BAS_ONOUTVOL);  /* a volumen block computes the outputs */
	if (api_store == 1) {
		SV(BAS_STORAGE_VOL_OUT_BASE, ch) = v;
		storage_flags++;
	}
}

uint8_t api_get_out_vol(uint8_t ch) { return SV(BAS_STORAGE_VOL_OUT_BASE, ch); }

/* PORT (#92): the level block of an output (core 2/3, behind filters and delay) smooths every new
 * value with four first order low passes (alpha 1/128, docs/dsp-images.md): writing "off" fades the
 * output out within a few ms, writing the level back fades it in */
void api_dip_out(uint8_t ch, uint8_t on)
{
	if (ch < 1 || ch > 8)
		return;
	uint8_t bit = (uint8_t)(1u << (ch - 1));
	if (on) {
		api_set_out_vol_hw(ch, 0);  /* 0 = off */
		out_dip_mask |= bit;
	} else if (out_dip_mask & bit) {
		out_dip_mask &= (uint8_t)~bit;
		api_out_vol_refresh(bit);
	}
}

/* PORT (#71): write the outputs in mask (bit 0 = output 1) again, e.g. the GALA boost changed;
 * with a volumen block that block computes them */
void api_out_vol_refresh(uint8_t mask)
{
	if (!mask)
		return;
	if (modc_active_affect & (1u << BAS_ONOUTVOL)) {
		modc_set_run(BAS_ONOUTVOL);
		return;
	}
	for (uint8_t ch = 1; ch <= 8; ch++)
		if (mask & (1u << (ch - 1)))
			api_set_out_vol_hw(ch, api_get_out_vol(ch));
}

/* PORT (Prüfung 19.11.2026): a polarity flip is a step in the signal, a loud click on a playing
 * output. With clickless on, the outputs that change are ramped down by the DAC first (parallel
 * mute, about 0.1 s), flipped and ramped up again (0.13 s); the others keep playing. Restores
 * (api_store = 0: start, preset load) write as the original. */
static void set_phase_hw(uint8_t v);

void api_set_phase(uint8_t v)
{
	uint8_t flip = (uint8_t)(v ^ storage_phase);
	/* only when the DAC state is known (written by api_set_out_mute since its init) */
	uint8_t soft = api_store == 1 && flip && bqramp_enabled() && dac_mute_last >= 0;
	uint8_t m = soft ? (uint8_t)dac_mute_last : 0;
	if (soft && (m & flip) != flip)
		dac_set_mute((uint8_t)(m | flip));
	else
		soft = 0;
	set_phase_hw(v);
	if (soft)
		dac_set_mute(m);
	if (api_store == 1) {
		storage_phase = v;
		storage_flags++;
	}
}

static void set_phase_hw(uint8_t v)
{
#if BAS_MODUL_TAS3108_OUTPUT_PHASE > 0
	/* dsp_set_phase: per output a DSP register, bit = inverted */
	for (uint8_t i = 1; i <= BAS_FLOW_MAX_OUT_CHANNEL; i++) {
		dsp_core = BAS_LOOKUP_B(data_flow_dsp_output_inv_dsp, i);
		dsp_reg = BAS_LOOKUP_B(data_flow_dsp_output_inv_reg, i);
		dsp_set_phase_reg((v >> (i - 1)) & 1 ? BAS_FLOW_PHASE_ON_VALUE : BAS_FLOW_PHASE_OFF_VALUE);
	}
#else
	dac_set_phase(v);
#endif
}

uint8_t api_get_phase(void) { return storage_phase; }

/* delays: two channels per DSP register (odd channel = high word) */
static void set_delay_pair(uint16_t base, uint8_t ch, uint16_t v, const uint8_t *tab_dsp, const uint8_t *tab_reg)
{
	uint16_t w1, w2;
	if (ch % 2 == 1) {
		w1 = sw_get(base, (uint8_t)(ch + 1));
		w2 = v;
	} else {
		w1 = v;
		w2 = sw_get(base, (uint8_t)(ch - 1));
	}
	dsp_core = BAS_LOOKUP_B(tab_dsp, ch);
	dsp_reg = BAS_LOOKUP_B(tab_reg, ch);
	dsp_set_delay((uint32_t)w1 | (uint32_t)w2 << 16);  /* buffer_words(1), buffer_words(2) */
}

void api_set_delay(uint8_t ch, uint16_t v)
{
	set_delay_pair(BAS_STORAGE_DELAY_BASE, ch, v, data_flow_pair_delay_dsp, data_flow_pair_delay_reg);
	if (api_store == 1) {
		sw_set(BAS_STORAGE_DELAY_BASE, ch, v);
		storage_flags++;
	}
}

uint16_t api_get_delay(uint8_t ch) { return sw_get(BAS_STORAGE_DELAY_BASE, ch); }

#if BAS_FEATURE_INPUT_DELAY > 0
void api_set_delay_in(uint8_t ch, uint16_t v)
{
	set_delay_pair(BAS_STORAGE_DELAY_IN_BASE, ch, v, data_flow_pair_delay_in_dsp, data_flow_pair_delay_in_reg);
	if (api_store == 1) {
		sw_set(BAS_STORAGE_DELAY_IN_BASE, ch, v);
		storage_flags++;
	}
}

uint16_t api_get_delay_in(uint8_t ch) { return sw_get(BAS_STORAGE_DELAY_IN_BASE, ch); }
#else
void api_set_delay_in(uint8_t ch, uint16_t v) { (void)ch; (void)v; }
uint16_t api_get_delay_in(uint8_t ch) { (void)ch; return 0; }
#endif

uint8_t api_get_mix_position(void) { return store_mix_position; }

void api_set_mix_position(uint8_t pos)
{
	if (store_mix_position != pos) {
		store_mix_position = pos;
		store_mix_changed = BAS_FLOW_MIX_MAX_CHANGED;
		api_set_out_mute_change();
		static const uint16_t ev[cmd_req_words_max] = {0x0104, 0x6D69, 0x706F, 0x0000, 0x0000};
		cmd_req_value[0] = (char)('0' + pos % 10);  /* positions 1..4 */
		cmd_req_value[1] = 0;
		cmd_load_header(ev);
		cmd_sendevent();
	}
}

/* --- mono per output pair (PORT extension #14, MODC type 0x6A) ---------------------------------
 * The input mixer (mixer.P.C.G) has one row per DSP channel: C 1/2 = 35/36, C 3..10 = outputs
 * 1..8 (odd = left), G = source (5 pairs, even = left, odd = right; GUI page INMI21). Mono for
 * output pair p makes the rows of channels 2p-1 and 2p equal when the coefficients are written:
 * 1 stereo, 2 right on both, 3 left on both, 4 sum (L+R)/2. The stored tuning is not changed. */
static uint8_t mix_mono[BAS_FLOW_MAX_OUT_PAIRS + 1];
#define MIX_IN_PAIRS 3

static uint8_t mix_b(uint8_t c, uint8_t g)  /* storage index of mixer.P.c.g, 0 = none, 0xFF = mux */
{
	return BAS_LOOKUP_B(data_flow_mix_cmd2array, (c - 1) * BAS_FLOW_MIX_MAX_CHANNELS + g);
}

void api_set_mix_mono(uint8_t pair, uint8_t mode)
{
	if (pair < 1 || pair > BAS_FLOW_MAX_OUT_PAIRS)
		return;
	if (mode < 1 || mode > 4)
		mode = 1;
	if (mix_mono[pair] == mode || (mix_mono[pair] <= 1 && mode == 1))
		return;
	mix_mono[pair] = mode;
	if (bqramp_enabled()) {  /* PORT (#50): ramp the rows instead of muting */
		api_mix_ramp_all();
		return;
	}
	api_set_out_mute_change();  /* the mixer is written with the auto unmute, as a mixer change */
	store_mix_changed = BAS_FLOW_MIX_MAX_CHANGED;
}

uint8_t api_get_mix_mono(uint8_t pair)
{
	return pair >= 1 && pair <= BAS_FLOW_MAX_OUT_PAIRS && mix_mono[pair] > 1 ? mix_mono[pair] : 1;
}

/* value written for storage index b of the active position (with the mono overlay) */
static uint8_t mix_value_mono(uint16_t frame, uint8_t b)
{
	for (uint8_t c = 3; c <= BAS_FLOW_MIX_MAX_CHANNELS; c++) {
		uint8_t p = (uint8_t)((c - 1) / 2), mode = p <= BAS_FLOW_MAX_OUT_PAIRS ? mix_mono[p] : 0;
		if (mode <= 1)
			continue;
		for (uint8_t g = 1; g <= BAS_FLOW_MIX_MAX_GAINS; g++) {
			if (mix_b(c, g) != b)
				continue;
			uint8_t bl = mix_b((uint8_t)(2 * p + 1), g), br = mix_b((uint8_t)(2 * p + 2), g);
			uint8_t vl = bl && bl != 0xFF ? SV(BAS_STORAGE_MIX_BASE, frame + bl) : 0;
			uint8_t vr = br && br != 0xFF ? SV(BAS_STORAGE_MIX_BASE, frame + br) : 0;
			return mode == 2 ? vr : mode == 3 ? vl : (uint8_t)((vl + vr + 1) / 2);
		}
	}
	return SV(BAS_STORAGE_MIX_BASE, frame + b);
}

/* --- analog input gain (PORT extension #16, MODC type 0x6D "Analoger Mischer") ---------------
 * Weights the analog input pairs 1..3 (GUI "Kanal 1..3" = G 1/2, 3/4, 5/6) with 0..100 % on top
 * of the stored mixer, in all rows. Like mono an overlay: the tuning is not changed. */
static uint8_t mix_in_pct[MIX_IN_PAIRS + 1] = {100, 100, 100, 100};
static uint8_t mix_in_any;  /* some pair below 100 % */

void api_set_mix_input_gain(uint8_t pair, uint8_t pct)
{
	if (pair < 1 || pair > MIX_IN_PAIRS)
		return;
	if (pct > 100)
		pct = 100;
	if (mix_in_pct[pair] == pct)
		return;
	mix_in_pct[pair] = pct;
	mix_in_any = 0;
	for (uint8_t i = 1; i <= MIX_IN_PAIRS; i++)
		if (mix_in_pct[i] < 100)
			mix_in_any = 1;
	if (bqramp_enabled()) {  /* (#50): ramp instead of mute */
		api_mix_ramp_all();
		return;
	}
	api_set_out_mute_change();
	store_mix_changed = BAS_FLOW_MIX_MAX_CHANGED;
}

uint8_t api_get_mix_input_gain(uint8_t pair)
{
	return pair >= 1 && pair <= MIX_IN_PAIRS ? mix_in_pct[pair] : 100;
}

static uint8_t mix_value(uint16_t frame, uint8_t b)
{
	uint8_t v = mix_value_mono(frame, b);
	if (!mix_in_any || v == 0)
		return v;
	for (uint8_t c = 1; c <= BAS_FLOW_MIX_MAX_CHANNELS; c++)
		for (uint8_t g = 1; g <= 2 * MIX_IN_PAIRS; g++)
			if (mix_b(c, g) == b)
				return (uint8_t)((v * mix_in_pct[(g + 1) / 2] + 50) / 100);
	return v;
}

uint8_t api_mix_value(uint8_t c, uint8_t g)  /* effective value of mixer.<active>.c.g */
{
	uint8_t b = mix_b(c, g);
	if (b == 0 || b == 0xFF || store_mix_position < 1 || store_mix_position > BAS_FLOW_MIX_MAX_POSITIONS)
		return 0;
	return mix_value((uint16_t)((store_mix_position - 1) * BAS_FLOW_MIX_MAX_POS_FRAME), b);
}

static uint32_t mix_ramp_rows;
static void mix_write_row(uint8_t i, const double val[4]);

/* --- level of the sums A/B (PORT, #71 follow-up): the sums before the chains 35/36 (mixer rows
 * C 1/2) have no level register in the DSP. Their coefficients (all sources G) are scaled on top
 * of the stored mixer, like the overlays above. Level in the scale of gain.out (223 = 0 dB,
 * 0.5 dB per step, 0 = off), set by a volumen block, plus the GALA boost. Never stored, written
 * at once without a mute, like an output level. */
#define SUM_VOL_0DB 223
static uint8_t sum_vol[3] = {0, SUM_VOL_0DB, SUM_VOL_0DB};
static float sum_f[3] = {1.0f, 1.0f, 1.0f};  /* factor now in the coefficients */

static uint8_t mix_sum_of(uint8_t b)  /* 1/2: storage index b is a coefficient of sum A/B */
{
	for (uint8_t s = 1; s <= 2; s++)
		for (uint8_t g = 1; g <= BAS_FLOW_MIX_MAX_GAINS; g++)
			if (mix_b(s, g) == b)
				return s;
	return 0;
}

/* PORT (#92): the chains of the sums (35/36) have no level of their own behind them; their outputs
 * enter the mixer rows of the outputs as sources G9/G10. A filter type change on a sum fades those
 * coefficients out (chain_f) and in again. */
#define CHAIN_G0 (BAS_FLOW_MIX_MAX_GAINS - 2)  /* G9 = chain of sum A, G10 = sum B */
static float chain_f[3] = {1.0f, 1.0f, 1.0f};

static uint8_t mix_chain_of(uint8_t b)  /* 1/2: storage index b takes the chain of sum A/B */
{
	for (uint8_t c = 3; c <= BAS_FLOW_MIX_MAX_CHANNELS; c++)
		for (uint8_t s = 1; s <= 2; s++)
			if (mix_b(c, (uint8_t)(CHAIN_G0 + s)) == b)
				return s;
	return 0;
}

static double mix_coef(uint16_t frame, uint8_t b)  /* coefficient written for storage index b */
{
	double d = mix_value(frame, b) / 100.0;
	if (d != 0 && (chain_f[1] != 1.0f || chain_f[2] != 1.0f)) {
		uint8_t s = mix_chain_of(b);
		if (s)
			d *= chain_f[s];
	}
	if (d != 0 && (sum_f[1] != 1.0f || sum_f[2] != 1.0f)) {
		uint8_t s = mix_sum_of(b);
		if (s)
			d *= sum_f[s];
		if (d > 15.99)
			d = 15.99;  /* 5.23 */
	}
	return d;
}

static float sum_factor(uint8_t s)
{
	uint8_t v = gala_sum_vol(s, sum_vol[s]);
	if (v == SUM_VOL_0DB)
		return 1.0f;
	return v ? (float)pow(10.0, ((int16_t)v - SUM_VOL_0DB) / 40.0) : 0.0f;
}

void api_sum_vol_refresh(void)
{
	float f1 = sum_factor(1), f2 = sum_factor(2);
	if (f1 == sum_f[1] && f2 == sum_f[2])
		return;
	sum_f[1] = f1;
	sum_f[2] = f2;
	if (!(hardware_dsp & (1u << BAS_TAS3108_1_RDY)) ||
	    store_mix_position < 1 || store_mix_position > BAS_FLOW_MIX_MAX_POSITIONS)
		return;  /* api_set_mix_reg writes them with the factor */
	uint16_t frame = (uint16_t)((store_mix_position - 1) * BAS_FLOW_MIX_MAX_POS_FRAME);
	for (uint8_t i = 1; i <= BAS_FLOW_MIX_MAX_REG; i++) {
		if (mix_ramp_rows & (1UL << i))
			continue;  /* a running ramp goes to the new value */
		double val[4];
		uint8_t hit = 0;
		for (uint8_t ii = 1; ii <= 4; ii++) {
			uint8_t b = (uint8_t)((i - 1) * 4 + ii);
			if (mix_sum_of(b))
				hit = 1;
			val[ii - 1] = mix_coef(frame, b);
		}
		if (hit)
			mix_write_row(i, val);
	}
}

/* PORT (#92): fade the chain of sum s (1/2) out (on) or in, DIP_CHAIN_STEPS writes of the rows that
 * take it, at once (about 0.5 ms per row and step); a running mixer ramp on such a row goes on to the
 * new value. #93: 24 linear steps were heard as a slight digital distortion (the DSP does not smooth
 * mixer coefficients): 64 steps along a raised cosine, soft at both ends. */
#define DIP_CHAIN_STEPS 64
void api_dip_chain(uint8_t s, uint8_t on)
{
	if (s < 1 || s > 2 || !(hardware_dsp & (1u << BAS_TAS3108_1_RDY)) ||
	    store_mix_position < 1 || store_mix_position > BAS_FLOW_MIX_MAX_POSITIONS) {
		if (s >= 1 && s <= 2)
			chain_f[s] = on ? 0.0f : 1.0f;
		return;
	}
	uint16_t frame = (uint16_t)((store_mix_position - 1) * BAS_FLOW_MIX_MAX_POS_FRAME);
	uint32_t rows = 0;
	for (uint8_t i = 1; i <= BAS_FLOW_MIX_MAX_REG; i++)
		for (uint8_t ii = 1; ii <= 4; ii++) {
			uint8_t b = (uint8_t)((i - 1) * 4 + ii);
			if (mix_chain_of(b) == s && mix_value(frame, b) != 0)
				rows |= 1UL << i;
		}
	for (uint8_t k = 1; k <= DIP_CHAIN_STEPS; k++) {
		float f = 0.5f - 0.5f * (float)cos(3.14159265 * k / DIP_CHAIN_STEPS);  /* 0 .. 1, raised cosine */
		chain_f[s] = on ? 1.0f - f : f;
		for (uint8_t i = 1; i <= BAS_FLOW_MIX_MAX_REG; i++) {
			if (!(rows & (1UL << i)) || (mix_ramp_rows & (1UL << i)))
				continue;
			double val[4];
			for (uint8_t ii = 1; ii <= 4; ii++)
				val[ii - 1] = mix_coef(frame, (uint8_t)((i - 1) * 4 + ii));
			mix_write_row(i, val);
		}
	}
}

void api_set_sum_vol(uint8_t s, uint8_t v)
{
	if (s < 1 || s > 2 || sum_vol[s] == v)
		return;
	sum_vol[s] = v;
	api_sum_vol_refresh();
}

uint8_t api_get_sum_vol(uint8_t s) { return s >= 1 && s <= 2 ? sum_vol[s] : SUM_VOL_0DB; }

/* --- mixer ramp (PORT #50): with clickless on a mixer change does not mute; the changed rows
 * go to their new coefficients in steps of at most MIX_STEP (linear), MIX_ROWS rows per call of
 * api_mix_ramp_mainloop (64 Hz). mix_cur holds the coefficients last written for the active
 * position (word b = (row - 1) * 4 + k, 1-based). */
#define MIX_STEP 0.0125  /* 80 steps for 0..1 (1.25 s), ~0.1 dB near full gain: no audible steps */
#define MIX_ROWS 2
static float mix_cur[BAS_FLOW_MIX_MAX_REG * 4 + 1];
static uint32_t mix_cur_valid;
static uint8_t mix_rr;

static void mix_write_row(uint8_t i, const double val[4])
{
	for (uint8_t ii = 1; ii <= 4; ii++) {
		uint32_t v = dsp_math_to523(val[ii - 1]);
		for (uint8_t k = 0; k < 4; k++)
			buffer_bytes[(ii - 1) * 4 + 1 + k] = (uint8_t)(v >> (8 * k));
		mix_cur[(i - 1) * 4 + ii] = (float)val[ii - 1];
	}
	mix_cur_valid |= 1UL << i;
	i2c_device_addr = 0x68;  /* dsp_1_slave_adr_w */
	i2c_reg = BAS_LOOKUP_B(data_flow_mix_dsp_reg, i);
	i2c_size = BAS_FLOW_MIX_MAX_REG_SIZE;
	i2c_wbuffer(1);
}

void api_mix_ramp_request(uint8_t row)
{
	if (row >= 1 && row <= BAS_FLOW_MIX_MAX_REG)
		mix_ramp_rows |= 1UL << row;
}

void api_mix_ramp_all(void)
{
	for (uint8_t i = 1; i <= BAS_FLOW_MIX_MAX_REG; i++)
		mix_ramp_rows |= 1UL << i;
}

void api_mix_ramp_mainloop(void)
{
	if (!mix_ramp_rows || !(hardware_dsp & (1u << BAS_TAS3108_1_RDY)))
		return;
	if (store_mix_position < 1 || store_mix_position > BAS_FLOW_MIX_MAX_POSITIONS) {
		mix_ramp_rows = 0;
		return;
	}
	uint16_t frame = (uint16_t)((store_mix_position - 1) * BAS_FLOW_MIX_MAX_POS_FRAME);
	for (uint8_t n = 0, k = 0; n < MIX_ROWS && k < BAS_FLOW_MIX_MAX_REG; k++) {
		mix_rr = (uint8_t)(mix_rr % BAS_FLOW_MIX_MAX_REG + 1);
		uint8_t i = mix_rr;
		if (!(mix_ramp_rows & (1UL << i)))
			continue;
		n++;
		double val[4];
		uint8_t done = 1;
		for (uint8_t ii = 1; ii <= 4; ii++) {
			double target = mix_coef(frame, (uint8_t)((i - 1) * 4 + ii));
			double cur = mix_cur[(i - 1) * 4 + ii];
			if (!(mix_cur_valid & (1UL << i)) || fabs(target - cur) <= MIX_STEP + 1e-6) {
				val[ii - 1] = target;
			} else {
				val[ii - 1] = cur + (target > cur ? MIX_STEP : -MIX_STEP);
				done = 0;
			}
		}
		mix_write_row(i, val);
		if (done)
			mix_ramp_rows &= ~(1UL << i);
	}
}

void api_set_mix_reg(void)
{
	if (!(hardware_dsp & (1u << BAS_TAS3108_1_RDY)))
		return;
	/* PORT: an unstored preset has position 0xFF; BASCOM then reads far beyond storage_mix
	 * and writes RAM contents as mixer coefficients. Skip invalid positions. */
	if (store_mix_position < 1 || store_mix_position > BAS_FLOW_MIX_MAX_POSITIONS)
		return;
	uint16_t count = (uint16_t)((store_mix_position - 1) * BAS_FLOW_MIX_MAX_POS_FRAME);
	for (uint8_t i = 1; i <= BAS_FLOW_MIX_MAX_REG; i++) {
		if (store_mix_changed & (1UL << i)) {
			double val[4];
			for (uint8_t ii = 1; ii <= 4; ii++) {
				count++;
				/* cvt_long = storage_mix(count); PORT: with the mono overlay (#14) */
				uint16_t frame = (uint16_t)((store_mix_position - 1) * BAS_FLOW_MIX_MAX_POS_FRAME);
				val[ii - 1] = mix_coef(frame, (uint8_t)(count - frame));  /* PORT: with the sum level */
			}
			mix_write_row(i, val);
			mix_ramp_rows &= ~(1UL << i);  /* written directly (with the mute) */
		} else {
			count += 4;
		}
	}
	uint8_t mux = SV(BAS_STORAGE_MUX_BASE, store_mix_position);
#if BAS_MODUL_CT7302 > 0
	/* PORT: index checked; BASCOM reads beyond the table for invalid stored values */
	uint8_t ch = mux < modul_ct7302_tab_set_mux_ITEMS ? BAS_LOOKUP_B(modul_ct7302_tab_set_mux, mux) : 0xFF;
	if (ch < 0xFF)
		ct7302_20_set_mux(ch);
	if (PIN_READ(PIN_OPTION_SPDIF_4CH) == 0x01)
		ct7302_22_set_mux(0x03);  /* four channel mode: RX3 optical 2 */
#elif BAS_MODUL_SRC43X2 > 0
	uint8_t ch = mux < modul_src43x2_tab_set_mux_ITEMS ? BAS_LOOKUP_B(modul_src43x2_tab_set_mux, mux) : 0xFF;
	if (ch < 0xFF)
		src43x2_set_mux(ch);
#endif
	store_mix_changed = 0;
}

void api_restore_mix(void) { store_mix_changed = BAS_FLOW_MIX_MAX_CHANGED; }  /* done by api_mainloop */

/* --- input selection (issue #23): api_set/get/restore_in_input are declared in the original
 * without an implementation. The input is the mux of the active mixer position (the mux table
 * maps it to the S/PDIF receiver input). Events 45..48 (SOURCE.1..4.MUX in events.txt, never set
 * in the original) carry the mux of each position. --- */
#if BAS_MODUL_CT7302 > 0
#define MUX_TAB modul_ct7302_tab_set_mux
#define MUX_ITEMS modul_ct7302_tab_set_mux_ITEMS
#elif BAS_MODUL_SRC43X2 > 0
#define MUX_TAB modul_src43x2_tab_set_mux
#define MUX_ITEMS modul_src43x2_tab_set_mux_ITEMS
#endif

uint8_t api_in_input_valid(uint8_t v)
{
#ifdef MUX_TAB
	return v >= 1 && v < MUX_ITEMS && BAS_LOOKUP_B(MUX_TAB, v) != 0xFF;
#else
	(void)v;
	return 0;
#endif
}

uint8_t api_in_input_max(void)
{
	uint8_t n = 0;
	for (uint8_t v = 1; v < 16; v++)
		if (api_in_input_valid(v))
			n = v;
	return n;
}

void api_publish_in_input(void)
{
	for (uint8_t pos = 1; pos <= BAS_STORE_MUX_COUNT && pos <= 4; pos++)
		event_set_value((uint8_t)(BAS_EVENTS_FIELD_SOURCE_MUX + pos), SV(BAS_STORAGE_MUX_BASE, pos));
}

uint8_t api_get_in_input(void)
{
	uint8_t pos = store_mix_position;
	return pos >= 1 && pos <= BAS_STORE_MUX_COUNT ? SV(BAS_STORAGE_MUX_BASE, pos) : 0;
}

/* 0 = refused (invalid input or no valid mixer position) */
uint8_t api_set_in_input(uint8_t v)
{
	uint8_t pos = store_mix_position;
	if (!api_in_input_valid(v) || pos < 1 || pos > BAS_STORE_MUX_COUNT)
		return 0;
	if (SV(BAS_STORAGE_MUX_BASE, pos) != v) {
		api_set_out_mute_change();  /* the mux is set by api_set_mix_reg with the auto unmute */
		SV(BAS_STORAGE_MUX_BASE, pos) = v;
		if (api_store == 1)
			storage_flags++;
	}
	api_publish_in_input();
	return 1;
}

void api_restore_in_input(void)
{
	api_restore_mix();  /* sets the mux of the active position with the mixer registers */
	api_publish_in_input();
}

static void led_gruen_gelb_wait(uint8_t a)
{
	PIN_CLR(PIN_LED_GREEN);
	PIN_SET(PIN_LED_YELLOW);
	for (uint16_t i = 1; i <= a; i++) {  /* 16 bit: a = 255 must not wrap */
		PIN_TGL(PIN_LED_GREEN);
		PIN_TGL(PIN_LED_YELLOW);
		delay_ms(250);
	}
	PIN_SET(PIN_LED_GREEN);
	PIN_CLR(PIN_LED_YELLOW);
}

/* modc.32 sets 1 / 3: wait before and after remote out (250 ms steps) */
void api_startup_delay(void)
{
	if (events_field_value[BAS_EVENTS_FIELD_REMOTEOUT] == BAS_EVENTS_STATE_FALSE &&
	    events_field_value[BAS_EVENTS_FIELD_SYSTEM_SHUTDOWN] == BAS_EVENTS_STATE_FALSE) {
		uint8_t a = modc_sets[BAS_MAX_MODC];                 /* modc_set1(max_modc) */
		if (a > 0)
			led_gruen_gelb_wait(a);
#if FORGE_AUDIO
		PIN_SET(PIN_CPU_POWER_CTRL_OUT);  /* remote out: amplifiers on */
#endif
		boot_mark(BT_REMOTE_ON);
		event_set_value(BAS_EVENTS_FIELD_REMOTEOUT, BAS_EVENTS_STATE_TRUE);
		a = modc_sets[2 * BAS_MAX_MODC + BAS_MAX_MODC];      /* modc_set3(max_modc) */
		if (a > 0)
			led_gruen_gelb_wait(a);
	}
	PIN_CLR(PIN_LED_YELLOW);
}

void api_preset_load(uint8_t preset)
{
	if (preset == store_preset)
		return;
	if (preset >= BAS_MODUL_MAX_PRESET)  /* PORT: an invalid register value would index beyond the address tables */
		return;
	event_set_value(BAS_EVENTS_FIELD_SYSTEM_PRESET, preset);
	api_set_out_mute_change();
	store_preset = preset;
	store_load_preset();
	api_store = 0;
	for (uint8_t ch = 1; ch <= BAS_FLOW_MAX_IN_CHANNEL; ch++)
		api_set_in_vol(ch, api_get_in_vol(ch));
	for (uint8_t ch = 1; ch <= BAS_FLOW_MAX_SPDIF_CHANNEL; ch++)
		api_set_spdif_vol(ch, api_get_spdif_vol(ch));
	api_set_phase(api_get_phase());
#if BAS_FEATURE_INPUT_DELAY > 0
	for (uint8_t ch = 1; ch <= BAS_STORE_DELAY_IN_COUNT; ch++)
		api_set_delay_in(ch, api_get_delay_in(ch));
	dsp_ack_delay();
#endif
	for (uint8_t ch = 1; ch <= BAS_STORE_DELAY_COUNT; ch++)
		api_set_delay(ch, api_get_delay(ch));
	dsp_ack_delay();
	api_restore_in_input();  /* api_restore_mix + events 45..48 */
	modc_loudness_invalidate();  /* the preset load rewrote all biquads (M6) */
	bqramp_cancel_all();         /* and ends running clickless fades (#44) */
	for (uint8_t ch = 1; ch <= BAS_FLOW_MAX_OUT_CHANNEL; ch++)
		api_set_out_vol(ch, api_get_out_vol(ch));
	api_store = 1;
	boot_mark(BT_PRESET);
	api_startup_delay();  /* "first unmute must check for remote.out delay" */
	api_set_out_mute();   /* api_restore_out_mute */
	boot_mark(BT_UNMUTED);
	sys_init_preset_loaded();  /* PORT (#40): start commands after the first preset load */
	log_event(LOG_INFO, LOGM_PRESET, LOGE_PRESET_LOADED, store_preset, 0);  /* PORT (#60) */
	static const uint16_t ev[cmd_req_words_max] = {0x0104, 0x7072, 0x6C6F, 0x0000, 0x0000};
	uint8_t i = 0, v = store_preset;
	if (v >= 100) cmd_req_value[i++] = (char)('0' + v / 100);
	if (v >= 10) cmd_req_value[i++] = (char)('0' + v / 10 % 10);
	cmd_req_value[i++] = (char)('0' + v % 10);
	cmd_req_value[i] = 0;
	cmd_load_header(ev);
	cmd_sendevent();
}
