/*
 * PORT (#81): sysinit without a lasting sysinit state, factory reset of all settings that keeps
 * the calibrations. See factory.h.
 * License: CC BY-NC-SA 4.0 (derived from the Forge DSP firmware, Mark Duprau).
 */
#include "factory.h"
#include "identity.h"
#include "cmd.h"
#include "eram.h"
#include "registers.h"
#include "supply.h"

void watchdog_kick(void);

uint8_t factory_reset_now;

uint8_t eram_blank(uint16_t a, uint16_t n)
{
	for (uint16_t i = 0; i < n; i++)
		if (eram_rb((uint16_t)(a + i)) != 0xFF)
			return 0;
	return 1;
}

void factory_request(void)
{
	set_register(BAS_REG_STARTUP_STATE, PORT_STARTUP_STATE_FACTORY);
}

void factory_print_calibration(void)
{
	int16_t c[4];
	supply_get_cal(c);
	out_str(" - factory reset keeps the calibration (note it down):");
#ifdef ERAM_SI5338_CUSTOM_MCLK_FINE_TUNE
	out_str(" mainclock.fine=");
	out_i16((int16_t)eram_rw(ERAM_SI5338_CUSTOM_MCLK_FINE_TUNE));
#endif
	out_str(" system.supply.cal=[");
	for (uint8_t i = 0; i < 4; i++) {
		if (i)
			out_chr(',');
		out_i16(c[i]);
	}
	out_str("] system.supply.vcc=");
	out_u16(supply_vcc_gain());
	out_crlf();
}

/* port areas of the internal EEPROM (0x0C00 .. end): erased at a factory reset except the
 * calibrations and the identity. Every port module reads 0xFF as "not set" (its default). */
#define PORT_AREA_FROM 0x0C00u
#define PORT_AREA_TO   0x1000u
static uint8_t keep(uint16_t a)
{
	return a == 0x0F00 || a == 0x0F01      /* identity.c: magic, index (model/PCB) */
	    || (a >= 0x0F04 && a <= 0x0F0B)    /* supply.c EE_CAL: raw1, mV1, raw2, mV2 */
	    || a == 0x0F0F || a == 0x0F10      /* supply.c EE_VCCCAL */
	    || (a >= IDENTITY_SERIAL_ADDR && a <= IDENTITY_SERIAL_ADDR + IDENTITY_SERIAL_LEN);  /* production number */
}

void factory_erase_port_areas(void)
{
	for (uint16_t a = PORT_AREA_FROM; a < PORT_AREA_TO; a++) {
		if (!keep(a))
			eram_wb(a, 0xFF);   /* only bytes that differ are written */
		if ((a & 0x3F) == 0)
			watchdog_kick();
	}
}
