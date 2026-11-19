/*
 * GALA (port extension, issue #71): speed-dependent volume and loudness. The MODC block of type
 * GALA (modc.c) configures it; the speed comes from a command (set system.kmh, USB/RS485) or from
 * pulses on one of the ground inputs (speed signal of the car, pulses per km set in the block).
 * The boost is added to the output volumes (api_set_out_vol_hw), to the level of the sums A/B
 * (api_set_sum_vol, mixer rows C 1/2) and to the bass shelf of the loudness block, never stored.
 * License: CC BY-NC-SA 4.0 (derived from the Forge DSP firmware).
 */
#ifndef GALA_H_
#define GALA_H_
#include <stdint.h>

#define GALA_RATE_HZ 8
#define GALA_KMH_MAX 300
/* source: 0 = command, 1..3 = ground input Imp 1..3 (PIN_QUAD_DECODING_1..3),
 * 4..6 = ground input switch 1..3 (PIN_GROUND_SWITCH_1..3) */
#define GALA_SRC_COMMAND 0
#define GALA_SRC_MAX 6
#define GALA_SRC_NONE 0xFF  /* no GALA block on */

struct gala_cfg {
	uint8_t out_mask;     /* outputs 1..8 (bit 0 = output 1) that get the volume boost */
	uint8_t source;       /* GALA_SRC_* */
	uint8_t start_kmh;    /* no boost up to this speed */
	uint8_t vol_slope;    /* volume boost per 10 km/h above start, 0.1 dB */
	uint8_t vol_max;      /* volume boost limit, 0.5 dB steps */
	uint8_t bass_slope;   /* loudness bass boost per 10 km/h above start, 0.1 dB (0 = none) */
	uint8_t bass_max;     /* bass boost limit, 0.5 dB steps */
	uint8_t ramp;         /* change rate, 0.1 dB/s */
	uint16_t pulses_km;   /* pulses per km of the speed signal */
	uint8_t timeout;      /* command source: no value for this long -> 0 km/h, 0.1 s */
	uint8_t sum_mask;     /* sums A/B (bit 0 = A = chain 35, bit 1 = B = 36) that get the volume boost */
};

void gala_config(uint8_t block, const struct gala_cfg *c);  /* block 1..32; c = NULL: block off */
void gala_mainloop(void);               /* GALA_RATE_HZ */
uint8_t gala_set_kmh(uint16_t kmh);     /* command source; 0 = out of range */
uint16_t gala_kmh(void);                /* current speed (0 without block or after the timeout) */
uint8_t gala_source(void);              /* source of the active block, GALA_SRC_NONE = none */
uint8_t gala_vol_tenths(void);          /* volume boost now applied, 0.1 dB */
int16_t gala_bass_hundredths(void);     /* bass boost for the loudness block, 0.01 dB */
uint8_t gala_out_vol(uint8_t ch, uint8_t v);  /* output volume ch 1..8 with the boost */
uint8_t gala_sum_vol(uint8_t s, uint8_t v);   /* level of sum s 1..2 (A/B) with the boost */
void gala_pulses_add(uint16_t n);       /* tests: pulses as from the pin interrupt */
#endif
