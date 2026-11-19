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
 *  - any other type change cannot be faded: the caller mutes as before
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

static void write_bq(uint16_t nr, const struct bq_params *p, uint8_t intermediate)
{
	uint16_t t0 = boot_clock_raw();
	uint32_t raw[6];
	if (p->type == BAS_DSP_BQTYPE_ALL_PASS) {
		dsp_bq_default(raw);
	} else if (intermediate && dsp_bq_calc_fast(p->type, p->f, p->g, p->q, p->b, raw)) {
		/* float, see biquad.c */
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

void bqramp_cancel(uint16_t nr)
{
	struct ramp *r = find(nr);
	if (r)
		r->nseg = 0;
}

void bqramp_cancel_all(void)
{
	for (uint8_t i = 0; i < BQRAMP_SLOTS; i++)
		ramps[i].nseg = 0;
}

uint8_t bqramp_active(void)
{
	uint8_t n = 0;
	for (uint8_t i = 0; i < BQRAMP_SLOTS; i++)
		if (ramps[i].nseg)
			n++;
	return n;
}

uint8_t bqramp_start(uint16_t nr, const struct bq_params *stored, const struct bq_params *to)
{
	struct ramp *r = find(nr);
	struct bq_params from = r ? r->now : *stored;
	struct segment s[2];
	uint8_t nseg = 0;
	if (from.type == to->type && to->type == DSP_BQTYPE_PHASE_ANGLE && (from.b < 180) != (to->b < 180)) {
		if (r)  /* PORT (#48): first <-> second order is no continuous path */
			r->nseg = 0;
		return 0;
	}
	if (from.type == to->type) {
		if (to->type == BAS_DSP_BQTYPE_ALL_PASS)
			return r ? (r->nseg = 0, 1) : 1;  /* bypass stays bypass: nothing to write */
		s[nseg++] = (struct segment){to->type, from, *to, 0};
	} else if (neutral_at_0db(from.type) && neutral_at_0db(to->type)) {
		if (from.type != BAS_DSP_BQTYPE_ALL_PASS && from.g != 0) {  /* old filter down to 0 dB (unless it is there) */
			struct bq_params flat = from;
			flat.g = 0;
			s[nseg++] = (struct segment){from.type, from, flat, 0};
		}
		struct bq_params start = *to;
		start.g = 0;
		if (to->type == BAS_DSP_BQTYPE_ALL_PASS)
			s[nseg++] = (struct segment){to->type, start, start, 0};  /* one write: bypass */
		else
			s[nseg++] = (struct segment){to->type, start, *to, 0};
	} else {
		if (r)
			r->nseg = 0;
		return 0;
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
