#ifndef BQRAMP_H
#define BQRAMP_H
#include <stdint.h>
/*
 * Clickless EQ (port extension, issue #44): biquad changes are faded in over intermediate
 * filters instead of muting the outputs. Off by default (behaviour of the original).
 */
struct bq_params {
	uint8_t type;  /* effective type (all-pass when the active flag is off) */
	double f;      /* Hz */
	double g;      /* dB */
	double q;
	double b;
};

uint8_t bqramp_enabled(void);
void bqramp_set_enabled(uint8_t on);           /* stored in the EEPROM */
/* start a ramp of biquad nr (0-based, store_bq_nr) from the filter now in the DSP to `to`.
 * `stored` = the parameters stored before this change (used when no ramp is running).
 * Returns 0 when the change cannot be faded (type change other than via 0 dB): the caller
 * mutes and writes directly. */
uint8_t bqramp_start(uint16_t nr, const struct bq_params *stored, const struct bq_params *to);
void bqramp_cancel(uint16_t nr);
void bqramp_cancel_all(void);                  /* preset load rewrote all biquads */
uint8_t bqramp_active(void);                   /* number of running ramps */
void bqramp_mainloop(void);                    /* one step of up to BQRAMP_WRITES ramps */
void bqramp_time(uint32_t out[4]);             /* [steps, us interpolation, us coefficients, us write] per step */
void bqramp_time_reset(void);
#ifndef FORGE_BQRAMP_TRACE
#define FORGE_BQRAMP_TRACE 0
#endif
#if FORGE_BQRAMP_TRACE   /* diagnostics build (#85) */
void bqramp_trace_arm(void);
uint8_t bqramp_trace_get(uint8_t i, uint32_t out[5], uint8_t *flags, uint16_t *nr);
#endif
#endif
