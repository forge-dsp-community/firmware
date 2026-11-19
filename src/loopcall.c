/*
 * Port of libs/loopcall/functions.bas.
 * License: CC BY-NC-SA 4.0 (derived from the Forge DSP firmware).
 */
#include "level.h"
#include "gala.h"
#include "supply.h"
#include "bqramp.h"
#include "loopcall.h"
#include "api.h"
#include "board.h"
#include "cmd.h"
#include "events.h"
#include "modc.h"
#include "ir.h"
#include "registers.h"
#include "store.h"
#include "tick.h"
#include "log.h"
#include "boottime.h"

void sys_init_mainloop(void);
void events_vumeter_mainloop(void);
void watchdog_kick(void);

#define B(n) (bits & (1u << (n)))

/* PORT (#72): longest run of each task of the main loop since the start (or the last reset),
 * get system.loop. The main loop answers commands only between tasks: a long task delays every
 * answer. Ids in the order of loop_names. */
enum { LT_CMD, LT_IR, LT_LEVEL, LT_RAMP, LT_ADC32, LT_SUPPLY_SAMPLE, LT_MODC, LT_ADC16, LT_SUPPLY, LT_SYSINIT,
       LT_OVERFLOW, LT_API, LT_BQRAMP, LT_STORE, LT_LOG, LT_SPDIF, LT_VUMETER, LT_ADC41, LT_PASS, LT_N };
uint16_t loop_max_ms[LT_N];
uint16_t loop_slow;  /* passes longer than 100 ms */
#define T(id, call) do { uint16_t t0_ = boot_clock_raw(); call; uint16_t d_ = BOOT_RAW_TO_MS(boot_clock_raw() - t0_); \
	if (d_ > loop_max_ms[id]) loop_max_ms[id] = d_; } while (0)

void loopcall(uint16_t bits)
{
	uint16_t pass0 = boot_clock_raw();
	events_poll_pins();  /* PORT: pin interrupts of the original */
	if (B(timer_tce0_div_256)) {
		T(LT_CMD, cmd_check4buffer());
		T(LT_IR, irreceiver_mainloop());
	}
	if (B(timer_tce0_div_64)) {
		T(LT_LEVEL, level_mainloop());    /* PORT (#51): input level for the loudness block */
		T(LT_RAMP, api_mix_ramp_mainloop());  /* PORT (#50): clickless mixer changes */
		T(LT_BQRAMP, bqramp_mainloop());  /* PORT (#44): clickless EQ steps, 64 Hz since Prüfung 19.11.2026 (was 8 Hz) */
	}
	if (B(timer_tce0_div_32)) {
		T(LT_ADC32, events_adc_mainloop_32());
		T(LT_SUPPLY_SAMPLE, supply_sample());     /* PORT (#46) */
		T(LT_MODC, ctrl_modc_mainloop());
	}
	if (B(timer_tce0_div_16)) {
		T(LT_ADC16, events_adc_mainloop_16());
		T(LT_SUPPLY, supply_mainloop());   /* PORT (#46) */
		T(LT_SYSINIT, sys_init_mainloop());
		T(LT_OVERFLOW, events_overflow_mainloop());
	}
	if (B(timer_tce0_div_8)) {
		T(LT_API, (api_mainloop(), gala_mainloop()));  /* PORT (#71): GALA at 8 Hz, timed with api */
		T(LT_STORE, store_mainloop());
		T(LT_LOG, log_mainloop());     /* PORT (#60): binary log to the microSD */
#if BAS_MODUL_CS8416 > 0
		/* V30-96LN: spdif events at 1/8 s */
		if (PIN_READ(PIN_OPTION_SPDIF_4CH) == 0x00)
			T(LT_SPDIF, events_spdif_mainloop());
#endif
	}
	if (B(timer_tce0_div_2)) {
		if (error_count > 0)
			PIN_TGL(PIN_LED_RED);
	}
	if (B(timer_tce0_div_1)) {
		T(LT_VUMETER, events_vumeter_mainloop());
#if BAS_MODUL_CS8416 == 0
		if (PIN_READ(PIN_OPTION_SPDIF_4CH) == 0x00)
			T(LT_SPDIF, events_spdif_mainloop());
#endif
		watchdog_kick();
	}
	if (B(timer_tce0_div_41))
		T(LT_ADC41, events_adc_mainloop_41());
	uint16_t pass = BOOT_RAW_TO_MS(boot_clock_raw() - pass0);
	if (pass > loop_max_ms[LT_PASS])
		loop_max_ms[LT_PASS] = pass;
	if (pass > 100 && loop_slow < 0xFFFF)
		loop_slow++;
}
