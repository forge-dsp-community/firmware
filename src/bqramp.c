/*
 * Clickless EQ (port extension, issue #44).
 *
 * The TAS3108 takes a new coefficient set at once; a large change is a step in the signal. The
 * original therefore mutes the outputs for every biquad command (api_set_out_mute_change), which
 * is a short dropout while tuning. With clickless on, biquad.CH.N.calc stores the final values
 * as before, but the DSP gets a series of intermediate filters:
 *  - same type: frequency and Q (and bandwidth) geometric, gain linear in dB, steps of at most
 *    0.5 dB, 1/12 octave and a Q factor of 1.1, at most BQRAMP_MAX_STEPS steps
 *  - types that are neutral at 0 dB (EQ Q, shelves, EQ bandwidth, all-pass/bypass): the old
 *    filter goes to 0 dB, the type changes, the new one comes up from 0 dB
 *  - #92: second order passes of one family (Butterworth, Bessel, Linkwitz, Chebyshev, variable Q)
 *    fade as the variable-Q type
 *  - #92: every other type change, adding (from bypass) and removing a filter dips the channel
 *    (see dip_start below); the caller mutes as before only when that is not possible
 * Every intermediate set is a complete, stable biquad (no interpolation of coefficients).
 * A new command for a biquad that is still moving starts from where it is.
 * Steps run in the main loop at 64 Hz (Prüfung 19.11.2026; before: 8 Hz with 2 writes, 16 steps per
 * second for all channels together, eight large changes took up to 16 s). Every running fade makes
 * at most one step per pass, so each fades at the same pace; a pass stops after BQRAMP_BUDGET_MS
 * (at least one write), the rest continue in the next pass in turn. Intermediate steps are written
 * at FORGE_I2C_RAMP_KHZ without the write delay, the last one at the normal clock and delay.
 * License: CC BY-NC-SA 4.0 (derived from the Forge DSP firmware).
 */
#include <math.h>
#include <string.h>
#include "bqramp.h"
#include "boottime.h"
#include "biquad.h"
#include "dsp.h"
#include "eram.h"
#include "flavour.h"
#include "api.h"
#include "dsp_pool.h"

#define BQRAMP_SLOTS 8
#define BQRAMP_MAX_STEPS 64
#ifndef BQRAMP_BUDGET_MS
#define BQRAMP_BUDGET_MS 10  /* main loop time per 1/64 s pass; a step takes 4..12 ms (DEV unit, 19.11.2026) */
#endif
#define BQRAMP_ENABLE_ADDR 0x0F02  /* EEPROM: 0x01 = on (identity uses 0x0F00/0x0F01) */

#define STEP_GAIN 0.5            /* dB */
#define STEP_OCT (1.0 / 12.0)    /* octaves */
#define STEP_Q 1.1               /* factor */

struct segment {
	uint8_t type;
	struct bq_params from, to;
	uint8_t n;
};

struct ramp {
	uint16_t nr;      /* 0xFFFF = free */
	uint8_t nseg, seg, step;
	struct segment s[2];
	struct bq_params now;
	double rf, rq, rb;  /* per-step factors of f, Q, b in the current segment (0 = linear, see ratio) */
};

static struct ramp ramps[BQRAMP_SLOTS];
static uint8_t rr;  /* round robin */
static uint8_t enabled_cache = 0xFF;

uint8_t bqramp_enabled(void)
{
	if (enabled_cache == 0xFF)
		enabled_cache = eram_rb(BQRAMP_ENABLE_ADDR) == 0x01;
	return enabled_cache;
}

void bqramp_set_enabled(uint8_t on)
{
	enabled_cache = on ? 1 : 0;
	eram_wb(BQRAMP_ENABLE_ADDR, on ? 0x01 : 0xFF);
	if (!on)
		bqramp_cancel_all();
}

static uint8_t neutral_at_0db(uint8_t t)
{
	return t == BAS_DSP_BQTYPE_ALL_PASS || t == BAS_DSP_BQTYPE_EQ_Q_FACTOR || t == BAS_DSP_BQTYPE_BASS_SHELF ||
	       t == BAS_DSP_BQTYPE_TREBLE_SHELF || t == BAS_DSP_BQTYPE_EQ_BANDWIDTH;
}

/* second order high/low pass as the variable-Q type of its family (same formula, same coefficients);
 * 0 = not a second order pass */
static uint8_t as_variable_q(uint8_t t, double *q)
{
	switch (t) {
	case BAS_DSP_BQTYPE_HIGH_PASS_BUTTER_2:   *q = BAS_DSP_BQ_Q_BUTTERWORTH; return BAS_DSP_BQTYPE_HIGH_PASS_VARIABLE_Q;
	case BAS_DSP_BQTYPE_HIGH_PASS_BESSEL:     *q = BAS_DSP_BQ_Q_BESSEL;      return BAS_DSP_BQTYPE_HIGH_PASS_VARIABLE_Q;
	case BAS_DSP_BQTYPE_HIGH_PASS_LINKWITZ:   *q = BAS_DSP_BQ_Q_LINKWITZ;    return BAS_DSP_BQTYPE_HIGH_PASS_VARIABLE_Q;
	case BAS_DSP_BQTYPE_HIGH_PASS_CHEBYCHEV:  *q = BAS_DSP_BQ_Q_CHEBYCHEV;   return BAS_DSP_BQTYPE_HIGH_PASS_VARIABLE_Q;
	case BAS_DSP_BQTYPE_HIGH_PASS_VARIABLE_Q: return BAS_DSP_BQTYPE_HIGH_PASS_VARIABLE_Q;
	case BAS_DSP_BQTYPE_LOW_PASS_BUTTER_2:    *q = BAS_DSP_BQ_Q_BUTTERWORTH; return BAS_DSP_BQTYPE_LOW_PASS_VARIABLE_Q;
	case BAS_DSP_BQTYPE_LOW_PASS_BESSEL:      *q = BAS_DSP_BQ_Q_BESSEL;      return BAS_DSP_BQTYPE_LOW_PASS_VARIABLE_Q;
	case BAS_DSP_BQTYPE_LOW_PASS_LINKWITZ:    *q = BAS_DSP_BQ_Q_LINKWITZ;    return BAS_DSP_BQTYPE_LOW_PASS_VARIABLE_Q;
	case BAS_DSP_BQTYPE_LOW_PASS_CHEBYCHEV:   *q = BAS_DSP_BQ_Q_CHEBYCHEV;   return BAS_DSP_BQTYPE_LOW_PASS_VARIABLE_Q;
	case BAS_DSP_BQTYPE_LOW_PASS_VARIABLE_Q:  return BAS_DSP_BQTYPE_LOW_PASS_VARIABLE_Q;
	}
	return 0;
}

static double geo(double a, double b, double t)
{
	if (a == b)
		return a;  /* no pow for what does not change (mostly f and Q of a gain fade) */
	if (a <= 0 || b <= 0)
		return a + (b - a) * t;
	return a * pow(b / a, t);
}

static uint8_t steps(uint8_t type, const struct bq_params *a, const struct bq_params *b)
{
	/* at 0 dB these filters are flat whatever f and Q: one write, no fade */
	if (neutral_at_0db(type) && a->g == 0 && b->g == 0)
		return 1;
	double n = fabs(b->g - a->g) / STEP_GAIN;
	if (a->f > 0 && b->f > 0) {
		double o = fabs(log(b->f / a->f) / log(2.0)) / STEP_OCT;
		if (o > n) n = o;
	}
	if (a->q > 0 && b->q > 0) {
		double o = fabs(log(b->q / a->q)) / log(STEP_Q);
		if (o > n) n = o;
	}
	if (a->b > 0 && b->b > 0) {
		double o = fabs(log(b->b / a->b)) / log(STEP_Q);
		if (o > n) n = o;
	}
	n = ceil(n - 1e-9);
	if (n < 1) n = 1;
	if (n > BQRAMP_MAX_STEPS) n = BQRAMP_MAX_STEPS;
	return (uint8_t)n;
}

/* get biquad.clickless.time (Prüfung 19.11.2026): where a step spends its time, summed in RTC
 * ticks (1/1024 s; averaged over many steps the rounding evens out) */
static uint32_t t_steps, t_interp, t_coeff, t_write;

void bqramp_time(uint32_t out[4])
{
	out[0] = t_steps;
	uint32_t n = t_steps ? t_steps : 1;
	out[1] = (uint32_t)((uint64_t)t_interp * 1000000u / 1024u / n);  /* us per step */
	out[2] = (uint32_t)((uint64_t)t_coeff * 1000000u / 1024u / n);
	out[3] = (uint32_t)((uint64_t)t_write * 1000000u / 1024u / n);
}

void bqramp_time_reset(void) { t_steps = t_interp = t_coeff = t_write = 0; }

#if FORGE_BQRAMP_TRACE
/* diagnostics build (#85): the coefficients of the first fade after `set biquad.clickless.trace=1`,
 * to compare them with the native build: [flags, A2, A1, B2, B1, B0], flags bit 0 = float, bit 1 = last */
#define TRACE_N 32
static uint32_t trace_raw[TRACE_N][5];
static uint8_t trace_flags[TRACE_N], trace_n, trace_armed;
static uint16_t trace_nr;
void bqramp_trace_arm(void) { trace_armed = 1; trace_n = 0; trace_nr = 0xFFFF; }
uint8_t bqramp_trace_get(uint8_t i, uint32_t out[5], uint8_t *flags, uint16_t *nr)
{
	if (i >= trace_n)
		return 0;
	for (uint8_t k = 0; k < 5; k++)
		out[k] = trace_raw[i][k];
	*flags = trace_flags[i];
	*nr = trace_nr;
	return 1;
}
static void trace(uint16_t nr, const uint32_t raw[6], uint8_t flags)
{
	if (!trace_armed)
		return;
	if (trace_nr == 0xFFFF)
		trace_nr = nr;
	if (nr != trace_nr || trace_n >= TRACE_N)
		return;
	for (uint8_t k = 0; k < 5; k++)
		trace_raw[trace_n][k] = raw[k + 1];
	trace_flags[trace_n++] = flags;
	if (flags & 2)
		trace_armed = 0;
}
#else
#define trace(nr, raw, flags) ((void)0)
#endif

static void write_bq(uint16_t nr, const struct bq_params *p, uint8_t intermediate)
{
	uint16_t t0 = boot_clock_raw();
	uint32_t raw[6];
	uint8_t fast = 0;
	if (p->type == BAS_DSP_BQTYPE_ALL_PASS) {
		dsp_bq_default(raw);
	} else if (intermediate && dsp_bq_calc_fast(p->type, p->f, p->g, p->q, p->b, raw)) {
		fast = 1;   /* float, see biquad.c */
	} else {
		dsp_bq_cache_type = p->type;
		dsp_bq_cache_values[BAS_DSP_BQ_CACHE_VALUE_F] = p->f;
		dsp_bq_cache_values[BAS_DSP_BQ_CACHE_VALUE_Q] = p->q;
		dsp_bq_cache_values[BAS_DSP_BQ_CACHE_VALUE_GAIN] = p->g;
		dsp_bq_cache_values[BAS_DSP_BQ_CACHE_VALUE_B] = p->b;
		dsp_bq_calc_type(p->type);
		dsp_bq_calcto5coeff();
		dsp_bq_convert2raw(raw);
	}
	(void)fast;
	trace(nr, raw, (uint8_t)(fast | (intermediate ? 0 : 2)));
	uint16_t t1 = boot_clock_raw();
	dsp_bq_virt_nr = nr;
	dsp_calc_core();
	dsp_calc_reg();
	dsp_set_biquad(raw);
	t_coeff += (uint16_t)(t1 - t0);
	t_write += (uint16_t)(boot_clock_raw() - t1);
	t_steps++;
}

static struct ramp *find(uint16_t nr)
{
	for (uint8_t i = 0; i < BQRAMP_SLOTS; i++)
		if (ramps[i].nr == nr && ramps[i].nseg)
			return &ramps[i];
	return 0;
}



/* --- dip (#92) ----------------------------------------------------------------------------------
 * The TAS3108 takes the five coefficients of a biquad one by one while the block is written (device
 * test 22.11.2026: adding a high pass at 10 Hz or an EQ at 100 Hz/Q 4 from bypass clicked loudly,
 * removing them and a low pass at 22 kHz did not; with direct form I that fits only an order with
 * A1 before A2). For some samples the DSP then computes a mix of old and new coefficients: from
 * bypass to a filter with poles that mix has a pole outside the unit circle or a huge gain at low
 * frequencies, although both filters sound alike. No order of small steps avoids that at full level
 * in useful time (simulation: residue -25 dBFS, accepted EQ fades -39 dBFS).
 * So a type change (and adding/removing) dips the channel: its level goes to 0 behind the chain
 * (output: level block, smoothed by the DSP itself; sum 35/36: its chain in the mixer rows), the
 * biquad goes to bypass (resets its state), then through the flat filter b = a = 1 + t (a_new - 1)
 * (u = 1 - t falling by DIP_R per write) to the new denominator, the numerator is blended over
 * DIP_BLEND writes to the new one, the last write is the exact filter; then the level comes back.
 * Simulated with all plausible orders and a ringing EQ before it: residue below -56 dBFS. The dip
 * lasts about 0.2 s; several biquads of one channel (an order change, a group) share it. */
#define DIP_SLOTS 8
#define DIP_CHS 4
#define DIP_R 0.6
#define DIP_BLEND 16
#define DIP_DEG_MAX 40
#define DIP_WAIT_DOWN 3  /* passes (1/64 s) after "off": the DSP smooths with 4 x alpha 1/128 */
#define DIP_WAIT_UP 1    /* passes after the last write before the level comes back */
#define DIP_PER_PASS 6   /* writes per pass (about 1.5 ms each), also within BQRAMP_BUDGET_MS */

struct dip {
	uint16_t nr;     /* 0xFFFF = free */
	uint8_t ch;
	uint8_t phase;   /* 0 bypass, 1 flat filter, 2 numerator blend */
	uint8_t k;
	double a1, a2, b0, b1, b2;  /* target in the DSP form (A negated) */
	double uend;
	struct bq_params to;
};
static struct dip dips[DIP_SLOTS];
static struct { uint8_t ch, state, wait; } dipch[DIP_CHS];  /* state 0 free, 1 going down, 2 silent, 3 coming back */
static uint8_t dip_init_done;

static void dip_init(void)
{
	if (dip_init_done)
		return;
	for (uint8_t i = 0; i < DIP_SLOTS; i++)
		dips[i].nr = 0xFFFF;
	dip_init_done = 1;
}

static uint8_t channel_of(uint16_t nr)  /* biquad place -> channel (1..8 outputs, 35/36 sums), 0 = none */
{
	for (uint8_t ch = 1; ch <= BAS_FLOW_MAX_CHANNELS; ch++) {
		uint8_t lim = flow_bq_limit(ch);
		uint16_t off = flow_bq_offset(ch);
		if (lim && nr > off && nr <= off + lim)
			return ch;
	}
	return 0;
}

static uint8_t dippable(uint8_t ch) { return (ch >= 1 && ch <= 8) || ch == 35 || ch == 36; }

static void dip_level(uint8_t ch, uint8_t on)
{
	if (ch >= 1 && ch <= 8)
		api_dip_out(ch, on);
	else
		api_dip_chain((uint8_t)(ch - 34), on);
}

static struct dip *dip_find(uint16_t nr)
{
	dip_init();
	for (uint8_t i = 0; i < DIP_SLOTS; i++)
		if (dips[i].nr == nr)
			return &dips[i];
	return 0;
}

static void dip_target(struct dip *d, const struct bq_params *to)
{
	d->to = *to;
	d->phase = 0;
	d->k = 0;
	if (to->type == BAS_DSP_BQTYPE_ALL_PASS) {
		d->a1 = d->a2 = d->b1 = d->b2 = 0;
		d->b0 = 1;
	} else {
		dsp_bq_cache_type = to->type;
		dsp_bq_cache_values[BAS_DSP_BQ_CACHE_VALUE_F] = to->f;
		dsp_bq_cache_values[BAS_DSP_BQ_CACHE_VALUE_Q] = to->q;
		dsp_bq_cache_values[BAS_DSP_BQ_CACHE_VALUE_GAIN] = to->g;
		dsp_bq_cache_values[BAS_DSP_BQ_CACHE_VALUE_B] = to->b;
		dsp_bq_calc_type(to->type);
		dsp_bq_calcto5coeff();
		d->a1 = dsp_bq_cache_values[BAS_DSP_BQ_CACHE_A1];
		d->a2 = dsp_bq_cache_values[BAS_DSP_BQ_CACHE_A2];
		d->b0 = dsp_bq_cache_values[BAS_DSP_BQ_CACHE_B0];
		d->b1 = dsp_bq_cache_values[BAS_DSP_BQ_CACHE_B1];
		d->b2 = dsp_bq_cache_values[BAS_DSP_BQ_CACHE_B2];
	}
	/* the flat filter is close enough when u is below |a(z)| at DC and at fs/2 (a = 1 - A1 z^-1 - A2 z^-2) */
	double dc = fabs(1.0 - d->a1 - d->a2), ny = fabs(1.0 + d->a1 - d->a2);
	d->uend = (dc < ny ? dc : ny) / 2;
	if (d->uend < 1e-7)
		d->uend = 1e-7;
}

static void dip_write(uint16_t nr, double b0, double b1, double b2, double a1, double a2)
{
	uint32_t raw[6];
	raw[1] = dsp_math_to523(a2);
	raw[2] = dsp_math_to523(a1);
	raw[3] = dsp_math_to523(b2);
	raw[4] = dsp_math_to523(b1);
	raw[5] = dsp_math_to523(b0);
	dsp_bq_virt_nr = nr;
	dsp_calc_core();
	dsp_calc_reg();
	dsp_write_ramp = 1;
	dsp_set_biquad(raw);
	dsp_write_ramp = 0;
	t_steps++;
}

/* one write of a dip; -> 1 when the biquad is done */
static uint8_t dip_step(struct dip *d)
{
	if (d->phase == 0) {  /* bypass: the DF1 state of this biquad is the input again */
		uint32_t raw[6];
		dsp_bq_default(raw);
		dsp_bq_virt_nr = d->nr;
		dsp_calc_core();
		dsp_calc_reg();
		dsp_set_biquad(raw);
		if (d->to.type == BAS_DSP_BQTYPE_ALL_PASS)
			return 1;
		d->phase = 1;
		d->k = 0;
		return 0;
	}
	if (d->phase == 1) {  /* b = a = 1 - t A1 z^-1 - t A2 z^-2 (DSP form: A = t A, B = -t A): exactly flat */
		d->k++;
		double u = pow(DIP_R, d->k), t = u <= d->uend || d->k >= DIP_DEG_MAX ? 1.0 : 1.0 - u;
		dip_write(d->nr, 1.0, -t * d->a1, -t * d->a2, t * d->a1, t * d->a2);
		if (t == 1.0) {
			d->phase = 2;
			d->k = 0;
		}
		return 0;
	}
	d->k++;  /* numerator from a to the new b, the denominator stays */
	if (d->k >= DIP_BLEND) {
		write_bq(d->nr, &d->to, 0);  /* exact, as a direct calc */
		return 1;
	}
	double l = (double)d->k / DIP_BLEND;
	dip_write(d->nr, (1 - l) + l * d->b0, (1 - l) * -d->a1 + l * d->b1, (1 - l) * -d->a2 + l * d->b2, d->a1, d->a2);
	return 0;
}

static uint8_t dip_users(uint8_t ch)
{
	uint8_t n = 0;
	for (uint8_t i = 0; i < DIP_SLOTS; i++)
		if (dips[i].nr != 0xFFFF && dips[i].ch == ch)
			n++;
	return n;
}

/* a type change of biquad nr on channel ch: 1 = the dip takes it, 0 = not possible (mute) */
static uint8_t dip_start(uint16_t nr, const struct bq_params *to)
{
	struct dip *d = dip_find(nr);
	if (!d) {
		uint8_t ch = channel_of(nr);
		if (!dippable(ch))
			return 0;
		uint8_t c = 0xFF;
		for (uint8_t i = 0; i < DIP_CHS; i++)
			if (dipch[i].state && dipch[i].ch == ch)
				c = i;
		for (uint8_t i = 0; i < DIP_CHS && c == 0xFF; i++)
			if (!dipch[i].state)
				c = i;
		for (uint8_t i = 0; i < DIP_SLOTS && !d; i++)
			if (dips[i].nr == 0xFFFF)
				d = &dips[i];
		if (!d || c == 0xFF)
			return 0;
		d->nr = nr;
		d->ch = ch;
		if (!dipch[c].state) {
			dipch[c].ch = ch;
			dipch[c].state = 1;
			dipch[c].wait = DIP_WAIT_DOWN;
			dip_level(ch, 1);
		}
	}
	dip_target(d, to);  /* a new command while it runs: on to the new filter (still silent) */
	return 1;
}

static void dip_mainloop(uint16_t t0)
{
	dip_init();
	for (uint8_t c = 0; c < DIP_CHS; c++) {
		uint8_t ch = dipch[c].ch, users = dip_users(ch);
		switch (dipch[c].state) {
		case 1:
			if (dipch[c].wait-- <= 1)
				dipch[c].state = 2;
			break;
		case 2:
			if (!users) {
				dipch[c].state = 3;
				dipch[c].wait = DIP_WAIT_UP;
			}
			break;
		case 3:
			if (users)
				dipch[c].state = 2;
			else if (dipch[c].wait-- <= 1) {
				dip_level(ch, 0);
				dipch[c].state = 0;
			}
			break;
		}
	}
	uint8_t done = 0;
	for (uint8_t i = 0; i < DIP_SLOTS; i++) {
		struct dip *d = &dips[i];
		if (d->nr == 0xFFFF)
			continue;
		uint8_t silent = 0;
		for (uint8_t c = 0; c < DIP_CHS; c++)
			if (dipch[c].state == 2 && dipch[c].ch == d->ch)
				silent = 1;
		while (silent && d->nr != 0xFFFF) {
			if (dip_step(d))
				d->nr = 0xFFFF;
			if (++done >= DIP_PER_PASS || BOOT_RAW_TO_MS((uint16_t)(boot_clock_raw() - t0)) >= BQRAMP_BUDGET_MS)
				return;
		}
	}
}

static void dip_cancel(uint16_t nr)
{
	struct dip *d = dip_find(nr);
	if (d)
		d->nr = 0xFFFF;  /* the channel comes back when no biquad of it is left */
}

void bqramp_cancel(uint16_t nr)
{
	struct ramp *r = find(nr);
	if (r)
		r->nseg = 0;
	dip_cancel(nr);
}

void bqramp_cancel_all(void)
{
	for (uint8_t i = 0; i < BQRAMP_SLOTS; i++)
		ramps[i].nseg = 0;
	dip_init();
	for (uint8_t i = 0; i < DIP_SLOTS; i++)
		dips[i].nr = 0xFFFF;  /* the channels come back in the next passes */
}

uint8_t bqramp_active(void)
{
	uint8_t n = 0;
	for (uint8_t i = 0; i < BQRAMP_SLOTS; i++)
		if (ramps[i].nseg)
			n++;
	dip_init();
	for (uint8_t i = 0; i < DIP_SLOTS; i++)
		if (dips[i].nr != 0xFFFF)
			n++;
	return n;
}

uint8_t bqramp_start(uint16_t nr, const struct bq_params *stored, const struct bq_params *to)
{
	if (dip_find(nr))
		return dip_start(nr, to);  /* still silent: straight to the new filter */
	struct ramp *r = find(nr);
	struct bq_params from = r ? r->now : *stored;
	struct segment s[2];
	uint8_t nseg = 0;
	if (from.type == to->type && to->type == DSP_BQTYPE_PHASE_ANGLE && (from.b < 180) != (to->b < 180)) {
		if (r)  /* PORT (#48): first <-> second order is no continuous path */
			r->nseg = 0;
		return dip_start(nr, to);
	}
	struct bq_params vf = from, vt = *to;
	uint8_t fam_f = as_variable_q(from.type, &vf.q), fam_t = as_variable_q(to->type, &vt.q);
	if (from.type == to->type) {
		if (to->type == BAS_DSP_BQTYPE_ALL_PASS)
			return r ? (r->nseg = 0, 1) : 1;  /* bypass stays bypass: nothing to write */
		s[nseg++] = (struct segment){to->type, from, *to, 0};
	} else if (fam_f && fam_f == fam_t) {  /* #92: e.g. Butterworth -> Linkwitz: f and Q in one go */
		vf.type = vt.type = fam_t;
		s[nseg++] = (struct segment){fam_t, vf, vt, 0};
	} else {  /* #92: type change, adding, removing: dip */
		if (r)
			r->nseg = 0;
		return dip_start(nr, to);
	}
	for (uint8_t i = 0; i < nseg; i++)
		s[i].n = s[i].type == BAS_DSP_BQTYPE_ALL_PASS ? 1 : steps(s[i].type, &s[i].from, &s[i].to);
	if (!r) {
		for (uint8_t i = 0; i < BQRAMP_SLOTS && !r; i++)
			if (!ramps[i].nseg)
				r = &ramps[i];
		if (!r)
			return 0;  /* all slots busy: write directly (with mute) */
	}
	r->nr = nr;
	memcpy(r->s, s, sizeof s);
	r->nseg = nseg;
	r->seg = 0;
	r->step = 0;
	r->now = from;
	return 1;
}

/* geometric fade as a factor per step: one pow per segment instead of one per step and value
 * (a step of a frequency fade spent about 3 ms in pow, DEV unit 19.11.2026). 0 = not geometric
 * (a value <= 0): linear via geo. The last step writes the exact end values anyway. */
static double ratio(double a, double b, uint8_t n)
{
	if (a == b)
		return 1.0;
	if (a <= 0 || b <= 0)
		return 0.0;
	return pow(b / a, 1.0 / n);
}

static double next(double base, double r, double a, double b, double t)
{
	return r > 0 ? base * r : geo(a, b, t);
}

static void advance(struct ramp *r)
{
	uint16_t t0 = boot_clock_raw();
	struct segment *s = &r->s[r->seg];
	if (r->step == 0) {
		r->rf = ratio(s->from.f, s->to.f, s->n);
		r->rq = ratio(s->from.q, s->to.q, s->n);
		r->rb = ratio(s->from.b, s->to.b, s->n);
	}
	r->step++;
	double t = (double)r->step / s->n;
	const struct bq_params *base = r->step == 1 ? &s->from : &r->now;
	struct bq_params p;
	p.type = s->type;
	p.f = next(base->f, r->rf, s->from.f, s->to.f, t);
	p.q = next(base->q, r->rq, s->from.q, s->to.q, t);
	p.b = next(base->b, r->rb, s->from.b, s->to.b, t);
	p.g = (s->from.g + (s->to.g - s->from.g) * t);
	uint8_t last = r->step >= s->n && r->seg + 1 >= r->nseg;
	if (r->step >= s->n)
		p = s->to, p.type = s->type;  /* exact end values */
	t_interp += (uint16_t)(boot_clock_raw() - t0);
	dsp_write_ramp = !last;   /* intermediate steps at FORGE_I2C_RAMP_KHZ */
	write_bq(r->nr, &p, !last);
	dsp_write_ramp = 0;
	r->now = p;
	if (r->step >= s->n) {
		r->seg++;
		r->step = 0;
		if (r->seg >= r->nseg)
			r->nseg = 0;
	}
}

void bqramp_mainloop(void)
{
	uint16_t t0 = boot_clock_raw();
	dip_mainloop(t0);
	uint8_t first = rr;  /* every fade at most once per pass */
	for (uint8_t k = 0; k < BQRAMP_SLOTS; k++) {
		uint8_t i = (uint8_t)((first + k) % BQRAMP_SLOTS);
		struct ramp *r = &ramps[i];
		if (!r->nseg)
			continue;
		advance(r);
		rr = (uint8_t)((i + 1) % BQRAMP_SLOTS);  /* next pass starts behind the last one served */
		if (BOOT_RAW_TO_MS((uint16_t)(boot_clock_raw() - t0)) >= BQRAMP_BUDGET_MS)
			break;
	}
}
