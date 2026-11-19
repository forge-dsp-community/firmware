/* DACs (libs/da): ES90x8Pro (96ELN-3), ES9018 (96ELN, CORED8, V30-96ELN), CS4385 (V30-96LN) */
#ifndef DAC_H_
#define DAC_H_
#include <stdint.h>
#include "flavour.h"
void dac_init(void);                 /* libs/da/init.bas of the fitted DAC */
void dac_set_mute(uint8_t mask);     /* api_set_out_mute: the fitted DAC's set_mute */
void dac_set_phase(uint8_t v);       /* api_set_phase on DAC-phase flavours (ES9018, CS4385) */

/* es9018_rotate_to_byte / cs4385_rotate_to_byte: the bit pairs in reverse order (bit 0/1 -> 6/7, 2/3 -> 4/5,
 * 4/5 -> 2/3, 6/7 -> 0/1). PORT (#84): with masks, not bit by bit. avr-gcc 16.2.1 at -Os/-O2 dropped the
 * term ((v >> 7) & 1) << 1 of the bitwise form, so output 8 never changed polarity on the device
 * (tools/toolchain/tests/miscompile-bit-pairs.c). */
static inline uint8_t dac_rotate_pairs(uint8_t v)
{
	return (uint8_t)((v & 0x03) << 6 | (v & 0x0C) << 2 | (v & 0x30) >> 2 | (v & 0xC0) >> 6);
}
#if BAS_MODUL_CS4385 > 0
extern uint8_t cs4385_reg, cs4385_value;
void cs4385_set_reg(void);
void cs4385_set_mute(uint8_t mask);
void cs4385_set_phase(uint8_t v);
#endif
/* bit n = channel n+1 muted (volume 0xFF), else 0 dB; 100/130 ms ramp per channel */
void es90x8pro_set_mute(uint8_t mask);
void es9018_set_mute(uint8_t mask);
void es9018_set_phase(uint8_t v);
#endif
