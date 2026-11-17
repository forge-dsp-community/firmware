/*
 * Sysinit and factory reset (#81).
 *
 * The original runs a factory reset ("sysinit") after a blank or foreign internal EEPROM, a
 * firmware with another version string, system.reinit or system.hardware.memory.eram: the modules
 * write their defaults (internal EEPROM, preset EEPROM erased, preset 0 copied to all presets),
 * then startup_state = done and a reset.
 *
 * The port distinguishes two cases:
 *  - automatic sysinit (blank or foreign EEPROM, startup_state = sysinit): nothing is erased. Every
 *    module writes its defaults only into internal EEPROM areas that are blank (all 0xFF) and
 *    keeps what is there; the tuning in the AT24C512 stays. At the end of the start
 *    startup_state = done, so the following starts load the stored values. (Up to 2.6.1 the port
 *    kept startup_state at sysinit for good: every start cleared the output names and ignored
 *    the stored MODC, event start values, IR setup and the start preset.)
 *  - factory reset (system.reinit / system.hardware.memory.eram, startup_state = factory, or
 *    FORGE_FACTORY_RESET=1 at an automatic sysinit): ALL settings to the defaults as in the
 *    original, AT24C512 erased, and the port's own areas from 0x0C00 erased (0xFF = not set for
 *    every port module). Kept: the calibrations (main clock fine tune, supply calibration,
 *    VCC calibration) unless blank, and the identity (model/PCB). The command prints the
 *    calibration values before the reset.
 */
#ifndef FACTORY_H_
#define FACTORY_H_
#include <stdint.h>
#include "flavour.h"
#include "registers.h"
#ifndef FORGE_FACTORY_RESET
#define FORGE_FACTORY_RESET 0
#endif
/* PORT (#81): a factory reset was requested; outside the values of the original (85 done,
 * 255 sysinit), so the original firmware would just start normally with it */
#define PORT_STARTUP_STATE_FACTORY 0xF0

extern uint8_t factory_reset_enabled;  /* FORGE_FACTORY_RESET: automatic sysinit = factory reset */
extern uint8_t factory_reset_now;      /* this start runs a requested factory reset */
static inline uint8_t sysinit_active(void)
{
	return registers[BAS_REG_STARTUP_STATE] == BAS_STARTUP_STATE_SYSINIT;
}
/* full factory reset now: every module writes its defaults, the AT24C512 is erased */
static inline uint8_t factory_write(void)
{
	return sysinit_active() && (factory_reset_enabled || factory_reset_now);
}
/* the internal EEPROM area [a, a+n) has never been written (all 0xFF) */
uint8_t eram_blank(uint16_t a, uint16_t n);
/* the module writes its defaults into [a, a+n): factory reset, or sysinit and the area blank */
static inline uint8_t eram_init_area(uint16_t a, uint16_t n)
{
	return sysinit_active() && (factory_write() || eram_blank(a, n));
}
/* request a factory reset at the next start (the caller resets) */
void factory_request(void);
/* factory reset: the port's own areas from 0x0C00 to 0xFF, except calibrations and identity */
void factory_erase_port_areas(void);
/* " - ..." line with the calibrations a factory reset keeps (to note them down beforehand) */
void factory_print_calibration(void);
#endif
