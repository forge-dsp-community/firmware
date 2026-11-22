/*
 * Port of the "checks evaluate" step at the end of systeminit.bas: if the peripherals found
 * on the I2C buses (hardware_dsp, hardware_other) differ from the variant's chk_hardware_*,
 * the outputs stay muted with the hardware priority (api_mute_out_prio_hardware).
 * PORT fix (#32): V30-96ELN expects the clock generator (bit 11), which that build does not
 * have (modul_si5338 = 0), and not the ES9018 (bit 2) and CT7302 #20 (bit 3) it detects; the
 * original therefore always muted. The port expects 0x007D there (V40-96ELN without Si5338).
 * PORT fix (#45, #47): if DSP cores are still missing or report an error state, restart once (software reset), as the
 * original does in effect through its watchdog (it waits for the program load without a
 * limit). An EEPROM flag allows only one restart in a row, so a missing DSP cannot loop.
 * License: CC BY-NC-SA 4.0 (derived from the Forge DSP firmware).
 */
#include "hwcheck.h"
#include "boottime.h"
#include "level.h"
#include "dsp.h"
#include <string.h>
#include "variant.h"
#include "api.h"
#include "flavour.h"
#include "store.h"
#include "tas3108.h"
#include "eram.h"
#include "log.h"

#define EE_DSP_RESTART 0x0F11  /* byte: 1 = the last start restarted for missing DSP cores */
uint8_t hw_dsp_restarted;
void mcu_reset(void);  /* power.c: software reset */

uint16_t hw_check_expected_other(void)
{
#if BAS_MODUL_SI5338 == 0 && (BAS_CHK_HARDWARE_OTHER == 0x0871)
	return 0x007D;  /* V30-96ELN, see above */
#else
	if (variant_v30())
		return V30_EXPECTED_OTHER;  /* PORT (#69): the build for all 96ELN on a PCB 3.0 board */
	return BAS_CHK_HARDWARE_OTHER;
#endif
}

uint16_t hw_check_expected_dsp(void)
{
	return BAS_CHK_HARDWARE_DSP;
}

uint8_t hw_check_evaluate(void)
{
	uint8_t bad = 0;
	if (hardware_dsp != hw_check_expected_dsp())
		bad = 1;
	if (hardware_other != hw_check_expected_other())
		bad = 1;
	if (bad) {
		api_set_out_mute_prio(api_mute_out_prio_hardware, api_mute_out_on);
		log_event(LOG_ERROR, LOGM_SYS, LOGE_SYS_HW_MISMATCH, hardware_dsp, hardware_other);  /* PORT (#60) */
	}
	return bad;
}

void hw_check_dsp_restart(void)
{
	uint8_t f = eram_rb(EE_DSP_RESTART);
	hw_dsp_restarted = f == 1;
	/* PORT (#47): a core that answers but reports an error state did not load its program (seen
	 * once on the device right after its EEPROM had been rewritten); a restart loads it */
	if ((hardware_dsp == hw_check_expected_dsp() && !dsp_state_error) || f == 1) {
		eram_wb(EE_DSP_RESTART, 0xFF);  /* found, or already restarted once: go on */
		return;
	}
	eram_wb(EE_DSP_RESTART, 1);
	log_event(LOG_ERROR, LOGM_SYS, LOGE_SYS_DSP_RESTART, hardware_dsp, dsp_state_error);  /* PORT (#60) */
	log_mainloop();   /* the queue would be lost with the reset: write it now (mounts the card) */
	dsp_boot_keep();  /* PORT diagnostics (#57): get system.hardware.dsp.2 after the restart */
	mcu_reset();
}

/* PORT (#94): watch of DSP core 1. Once a second the raw words of the analog inputs 1-4 in its data
 * memory are read (PEEK); the A/D converters keep their lowest bits moving even in silence. If all
 * four stay exactly the same for WATCH_SAME checks in a row, the core has stopped (seen in the
 * tests of #94: no sound until power off). Then the event is logged and the MCU restarts, which
 * resets and loads all DSPs and the preset again, as hw_check_dsp_restart. Not within the first
 * WATCH_MIN_UPTIME_MS after a start, so at most once a minute. Skipped while the core is being
 * written to (dsp_peek returns 0 then) and in the host build. */
#define WATCH_EVERY 64             /* calls at 64 Hz = 1 s */
#define WATCH_SAME 3
#define WATCH_MIN_UPTIME_MS 60000UL
static uint8_t watch_cnt, watch_same, watch_valid;
static uint8_t watch_prev[4][6];
uint8_t hw_dsp_watch_trips;  /* diagnostics: times it would have restarted (host build) */

void hw_check_dsp_watch(void)
{
	if (++watch_cnt < WATCH_EVERY)
		return;
	watch_cnt = 0;
	if (!level_inputs_known())
		return;
	static const uint8_t addr[4] = {4, 5, 2, 3};  /* inputs 1..4, as level.c vu_addr */
	uint8_t cur[4][6], same = 1, b[8];
	for (uint8_t i = 0; i < 4; i++) {
		if (!dsp_peek(1, 2, addr[i], b))
			return;  /* not present or being written to: next time */
		memcpy(cur[i], b, 6);
		if (memcmp(cur[i], watch_prev[i], 6))
			same = 0;
	}
	memcpy(watch_prev, cur, sizeof cur);
	if (!watch_valid) {
		watch_valid = 1;
		return;
	}
	watch_same = same ? (uint8_t)(watch_same + 1) : 0;
	if (watch_same < WATCH_SAME || boot_uptime_ms() < WATCH_MIN_UPTIME_MS)
		return;
	watch_same = 0;
	hw_dsp_watch_trips++;
#ifdef __AVR__
	log_event(LOG_ERROR, LOGM_SYS, LOGE_SYS_DSP_RESTART, 0xF1, 1);  /* p1 0xF1: by the watch */
	log_mainloop();
	mcu_reset();
#endif
}
