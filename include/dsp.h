/*
 * DSP register access, port of libs/dsp/tas3108/functions.bas (dsp_set_*, dsp_get_*) and
 * cores/{1,2,3}/functions.bas. Globals as BASCOM: dsp_core (1..3), dsp_reg (sub address),
 * data in buffer_bytes (buffer_dword = buffer_bytes(1..4), little endian, sent backwards).
 */
#ifndef DSP_H_
#define DSP_H_
#include <stdint.h>
#include "flavour.h"
extern uint8_t dsp_core, dsp_reg;
extern uint16_t dsp_bq_virt_nr;
extern uint16_t iface_verify_error_count;

void dsp_calc_core(void);           /* dsp_core = lookup(dsp_bq_virt_nr, data_flow_dsp_bq_core) */
void dsp_calc_reg(void);            /* dsp_reg  = lookup(dsp_bq_virt_nr, data_flow_dsp_bq_reg) */
void dsp_set_biquad(const uint32_t raw[6]);  /* raw[1..5] -> buffer_dwords(1..5); skips a place that holds them (#66) */
extern uint8_t dsp_write_ramp;               /* 1 = intermediate clickless step: written at FORGE_I2C_RAMP_KHZ */
#define DSP_BQ_EXTRA_MAX 32                  /* PORT (#66): spare places of the pools at most (DSP_POOL_EXTRA) */
void dsp_bq_forget(void);                    /* the DSPs were reloaded: nothing known about their places */
extern uint16_t dsp_bq_skipped, dsp_bq_written, dsp_bq_read_same;  /* PORT (#66, #67) */
void dsp_get_biquad(uint32_t raw[6]);
void dsp_set_vol_out(int32_t vol_calc);      /* buffer_long = dsp_vol_calc, verified write */
void dsp_set_mux(uint32_t state);
uint32_t dsp_get_mux(void);
void dsp_set_delay(uint32_t value);
/* delay memory of one DSP in the samples of the protocol (delay.N): flow_max_samples counts DSP
 * words, the stored value is doubled with feature_*_delay_mode (Prüfung 19.11.2026) */
#if BAS_FEATURE_OUTPUT_DELAY_MODE != BAS_FEATURE_INPUT_DELAY_MODE && BAS_FEATURE_INPUT_DELAY > 0
#error "delay modes of inputs and outputs differ: DELAY_MEM_SAMPLES needs one per list"
#endif
#define DELAY_MEM_SAMPLES (BAS_FEATURE_OUTPUT_DELAY_MODE > 0 ? BAS_FLOW_MAX_SAMPLES / 2 : BAS_FLOW_MAX_SAMPLES)
void dsp_ack_delay(void);
void dsp_1_ack_delay(void);
void dsp_set_phase_reg(uint32_t value);      /* one output register, dsp_core/dsp_reg set */
void dsp_init_vum(void);
void dsp_get_vum(void);
void dsp_set_reg(uint8_t size);              /* PORT (#58): verified write of buffer_bytes(1..size) */
uint8_t dsp_peek(uint8_t core, uint8_t mem, uint16_t addr, uint8_t out[8]);  /* PORT (#51) */
void dsp_core_access(uint8_t core, uint8_t peek);  /* PORT (#94): gap between PEEK and write on one core */
void dsp_core_done(uint8_t core, uint8_t peek);
void dsp_trace(uint8_t core, uint8_t kind, uint8_t reg, uint8_t info);  /* PORT (#94) */
void dsp_trace_out(void);
void dsp_trace_reset(void);
#endif
