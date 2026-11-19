/* avr-gcc 16.2.1 (pinned toolchain, also without the lower-subreg patch?): wrong code at -Os and -O2.
 *   avr-gcc -mmcu=atxmega256a3u -Os -S miscompile-bit-pairs.c
 * rot_bitwise(): the term ((v >> 7) & 1) << 1 is dropped (no "bst rX,7" in the output), so bit 7 of v
 * never reaches bit 1 of the result. -O1 and x86 are right. Found as forge-dsp-community/firmware#84:
 * phase.out.8 had no effect on the device (es9018_set_phase, cs4385_set_phase).
 * rot_masks(): the same mapping with masks, as the firmware does now (dac_rotate_pairs). */
#include <stdint.h>

uint8_t rot_bitwise(uint8_t v)
{
	uint8_t b = 0;
	b |= (uint8_t)(((v >> 0) & 1) << 6) | (uint8_t)(((v >> 1) & 1) << 7);
	b |= (uint8_t)(((v >> 2) & 1) << 4) | (uint8_t)(((v >> 3) & 1) << 5);
	b |= (uint8_t)(((v >> 4) & 1) << 2) | (uint8_t)(((v >> 5) & 1) << 3);
	b |= (uint8_t)(((v >> 6) & 1) << 0) | (uint8_t)(((v >> 7) & 1) << 1);
	return b;
}

uint8_t rot_masks(uint8_t v)
{
	return (uint8_t)((v & 0x03) << 6 | (v & 0x0C) << 2 | (v & 0x30) >> 2 | (v & 0xC0) >> 6);
}
