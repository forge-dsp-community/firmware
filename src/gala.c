/*
 * GALA (port extension, issue #71): speed-dependent volume and loudness, see gala.h.
 *
 * Speed: from the command (gala_set_kmh, falls to 0 km/h after the timeout) or from pulses on a
 * ground input, counted by its pin interrupt (any edge rate the inputs pass; the 512-Hz polling
 * of the rotary encoder would end near 250 Hz). Pulses of the last second (8 bins of 1/8 s)
 * give the speed: kmh = pulses * 3600 / pulses_km.
 * Boost: above start_kmh, vol_slope (0.1 dB per 10 km/h) up to vol_max, the same for the bass;
 * it follows the target at `ramp` (0.1 dB/s). The volume is applied in 0.5-dB steps (the gain
 * scale) with 0.1 dB hysteresis around the step boundary, so a steady speed near a boundary
 * does not toggle. Applied only to outputs with a volume above 0 (0 = off stays off), and to the
 * level of the sums A/B (api_set_sum_vol) when chosen.
 * License: CC BY-NC-SA 4.0 (derived from the Forge DSP firmware).
 */
#include "gala.h"
#include "api.h"
#include "events.h"
#include "modc.h"
#include "board.h"

static uint8_t owner;                /* MODC block 1..32, 0 = none */
static struct gala_cfg cfg;
static uint16_t cmd_kmh;
static uint16_t cmd_age;             /* passes since the last command value */
static uint16_t kmh;
static uint16_t bins[GALA_RATE_HZ];
static uint8_t bin_i;
static volatile uint16_t pulses;     /* pin interrupt */
static int16_t vol_h, bass_h;        /* boost now, 0.01 dB */
static uint8_t vol_steps;            /* applied volume boost, 0.5 dB steps */
static uint8_t applied_mask;
static uint8_t applied_sums;         /* sums A/B (bit 0/1) */
static int16_t bass_sent;

#ifdef __AVR__
#include <avr/interrupt.h>
#include <avr/io.h>

static PORT_t *irq_port;
static uint8_t irq_bp, irq_ctrl;    /* pin, its PINnCTRL before */

ISR(PORTA_INT0_vect) { pulses++; }
ISR(PORTF_INT0_vect) { pulses++; }

static void pin_irq(uint8_t src)
{
	if (irq_port) {  /* release the previous pin */
		irq_port->INTCTRL &= (uint8_t)~PORT_INT0LVL_gm;
		irq_port->INT0MASK = 0;
		(&irq_port->PIN0CTRL)[irq_bp] = irq_ctrl;
		irq_port = 0;
	}
	PORT_t *p = 0;
	uint8_t bp = 0;
	switch (src) {
	case 1: p = &PIN_QUAD_DECODING_1_PORT; bp = PIN_QUAD_DECODING_1_bp; break;
	case 2: p = &PIN_QUAD_DECODING_2_PORT; bp = PIN_QUAD_DECODING_2_bp; break;
	case 3: p = &PIN_QUAD_DECODING_3_PORT; bp = PIN_QUAD_DECODING_3_bp; break;
	case 4: p = &PIN_GROUND_SWITCH_1_PORT; bp = PIN_GROUND_SWITCH_1_bp; break;
	case 5: p = &PIN_GROUND_SWITCH_2_PORT; bp = PIN_GROUND_SWITCH_2_bp; break;
	case 6: p = &PIN_GROUND_SWITCH_3_PORT; bp = PIN_GROUND_SWITCH_3_bp; break;
	}
	if (!p)
		return;
	irq_port = p;
	irq_bp = bp;
	irq_ctrl = (&p->PIN0CTRL)[bp];
	(&p->PIN0CTRL)[bp] = (uint8_t)((irq_ctrl & ~PORT_ISC_gm) | PORT_ISC_FALLING_gc);  /* one edge per pulse */
	p->INT0MASK = (uint8_t)(1u << bp);
	p->INTFLAGS = PORT_INT0IF_bm;
	p->INTCTRL = (uint8_t)((p->INTCTRL & ~PORT_INT0LVL_gm) | PORT_INT0LVL_LO_gc);
}

static uint16_t take_pulses(void)
{
	uint8_t s = SREG;
	cli();
	uint16_t n = pulses;
	pulses = 0;
	SREG = s;
	return n;
}
#else
static void pin_irq(uint8_t src) { (void)src; }
static uint16_t take_pulses(void) { uint16_t n = pulses; pulses = 0; return n; }
#endif

void gala_pulses_add(uint16_t n) { pulses = (uint16_t)(pulses + n); }

static void apply_volume(void)
{
	uint8_t mask = owner ? cfg.out_mask : 0;
	uint8_t touch = (uint8_t)(mask | applied_mask);
	applied_mask = mask;
	applied_sums = owner ? (uint8_t)(cfg.sum_mask & 3) : 0;
	api_out_vol_refresh(touch);
	api_sum_vol_refresh();  /* writes only when the factor of a sum changed */
}

void gala_config(uint8_t block, const struct gala_cfg *c)
{
	if (!c) {
		if (block != owner)
			return;  /* another block holds GALA */
		owner = 0;
		pin_irq(0);
		vol_h = bass_h = 0;
		vol_steps = 0;
		kmh = 0;
		apply_volume();
		modc_loudness_touch();
		bass_sent = 0;
		return;
	}
	if (owner && block > owner)
		return;  /* the first GALA block counts */
	uint8_t src_changed = !owner || block != owner || cfg.source != c->source;
	uint8_t mask_changed = !owner || cfg.out_mask != c->out_mask || cfg.sum_mask != c->sum_mask;
	owner = block;
	cfg = *c;
	if (src_changed) {
		pin_irq(cfg.source);
		take_pulses();
		for (uint8_t i = 0; i < GALA_RATE_HZ; i++)
			bins[i] = 0;
	}
	if (mask_changed)
		apply_volume();
}

uint8_t gala_set_kmh(uint16_t v)
{
	if (v > GALA_KMH_MAX)
		return 0;
	cmd_kmh = v;
	cmd_age = 0;
	return 1;
}

static int16_t target(uint8_t slope, uint8_t max_steps)
{
	if (kmh <= cfg.start_kmh)
		return 0;
	int32_t t = (int32_t)(kmh - cfg.start_kmh) * slope;  /* 0.1 dB per 10 km/h = 0.01 dB per km/h */
	int32_t lim = (int32_t)max_steps * 50;
	return (int16_t)(t > lim ? lim : t);
}

static int16_t follow(int16_t cur, int16_t to, int16_t step)
{
	if (to > cur + step) return (int16_t)(cur + step);
	if (to < cur - step) return (int16_t)(cur - step);
	return to;
}

void gala_mainloop(void)
{
	if (!owner) {
		take_pulses();
		return;
	}
	/* speed */
	if (cfg.source == GALA_SRC_COMMAND) {
		uint16_t lim = (uint16_t)cfg.timeout * GALA_RATE_HZ / 10;
		if (cmd_age < lim)
			cmd_age++;
		kmh = cmd_age < lim ? cmd_kmh : 0;
	} else {
		bins[bin_i] = take_pulses();
		bin_i = (uint8_t)((bin_i + 1) % GALA_RATE_HZ);
		uint32_t n = 0;
		for (uint8_t i = 0; i < GALA_RATE_HZ; i++)
			n += bins[i];
		uint32_t v = cfg.pulses_km ? n * 3600UL / cfg.pulses_km : 0;
		kmh = (uint16_t)(v > GALA_KMH_MAX ? GALA_KMH_MAX : v);
	}
	event_set_value((uint8_t)(BAS_EVENTS_FIELD_MODC_VALUE + owner), (uint8_t)(kmh > 255 ? 255 : kmh));
	/* boost */
	int16_t step = (int16_t)((cfg.ramp * 10 + GALA_RATE_HZ / 2) / GALA_RATE_HZ);
	if (step < 1)
		step = 1;
	vol_h = follow(vol_h, target(cfg.vol_slope, cfg.vol_max), step);
	bass_h = follow(bass_h, cfg.bass_slope ? target(cfg.bass_slope, cfg.bass_max) : 0, step);
	int16_t edge = (int16_t)(vol_steps * 50);
	if (vol_h > edge + 35 || vol_h < edge - 35) {  /* half a step plus 0.1 dB away */
		uint8_t s = (uint8_t)((vol_h + 25) / 50);
		if (s != vol_steps) {
			vol_steps = s;
			apply_volume();
		}
	}
	if (bass_h / 10 != bass_sent / 10) {  /* the loudness block works on a 0.1 dB grid */
		bass_sent = bass_h;
		modc_loudness_touch();
	}
}

uint16_t gala_kmh(void) { return owner ? kmh : 0; }
uint8_t gala_source(void) { return owner ? cfg.source : GALA_SRC_NONE; }
uint8_t gala_vol_tenths(void) { return applied_mask || applied_sums ? (uint8_t)(vol_steps * 5) : 0; }  /* what acts */
int16_t gala_bass_hundredths(void) { return owner ? bass_h : 0; }

uint8_t gala_out_vol(uint8_t ch, uint8_t v)
{
	if (!vol_steps || v == 0 || ch < 1 || ch > 8 || !(applied_mask & (1u << (ch - 1))))
		return v;
	uint16_t b = (uint16_t)(v + vol_steps);
	return (uint8_t)(b > 255 ? 255 : b);
}

uint8_t gala_sum_vol(uint8_t s, uint8_t v)
{
	if (!vol_steps || v == 0 || s < 1 || s > 2 || !(applied_sums & (1u << (s - 1))))
		return v;
	uint16_t b = (uint16_t)(v + vol_steps);
	return (uint8_t)(b > 255 ? 255 : b);
}
