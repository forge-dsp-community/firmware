/*
 * PORT (#76): read access to the flash of the USB audio XMOS, see xmos_flash.h.
 * The original (libs/extender/usb-audio) has the same commands for its firmware update, which
 * no flavour builds (ext_usbaudio_incl_fw = 0). Here only reading so far.
 *
 *   get extender.usb.flash  -> [state,"MDID","JEDEC",SR]  state 0 ok, 1 microSD card inserted,
 *                              2 no answer (USB host at the audio port: the XMOS has the flash)
 *   get extender.usb.config -> [state,n,"HEX"]  n bytes from 0x80000 (length bytes included)
 * License: CC BY-NC-SA 4.0.
 */
#include "cmd.h"
#include "xmos_flash.h"
#ifdef __AVR__
#include "board.h"
#endif

#if defined(__AVR__) && BAS_EXT_USBAUDIO > 0
#include <avr/io.h>
#include "sd.h"
#include "watchdog.h"

#define CS_LOW()  PIN_CLR(PIN_EXT_MICROSD_SPI_SS)
#define CS_HIGH() PIN_SET(PIN_EXT_MICROSD_SPI_SS)

#define CMD_RD     0x03
#define CMD_RDSR   0x05
#define CMD_RDJDID 0x9F
#define CMD_RDMDID 0x90
#define MFR_ISSI   0x9D

static uint8_t spi_ctrl;

static uint8_t xfer(uint8_t b)
{
	SPIE.DATA = b;
	while (!(SPIE.STATUS & SPI_IF_bm))
		;
	return SPIE.DATA;
}

static void mdid(uint8_t id[2])
{
	CS_LOW();
	xfer(CMD_RDMDID);
	xfer(0); xfer(0); xfer(0);
	id[0] = xfer(0xFF);
	id[1] = xfer(0xFF);
	CS_HIGH();
}

uint8_t xmos_flash_open(void)
{
	if (sd_card_present())
		return XMOS_FLASH_CARD;      /* the bus switch gives SPIE to the card */
	spi_ctrl = SPIE.CTRL;
	SPIE.CTRL = SPI_ENABLE_bm | SPI_MASTER_bm | SPI_MODE_0_gc | SPI_PRESCALER_DIV16_gc;  /* 2 MHz */
	CS_HIGH();
	xfer(0xFF);
	uint8_t id[2];
	mdid(id);
	if (id[0] != MFR_ISSI) {
		xmos_flash_close();
		return XMOS_FLASH_NONE;
	}
	return XMOS_FLASH_OK;
}

void xmos_flash_close(void)
{
	CS_HIGH();
	SPIE.CTRL = spi_ctrl;
}

void xmos_flash_ids(uint8_t md[2], uint8_t jedec[3], uint8_t *sr)
{
	mdid(md);
	CS_LOW();
	xfer(CMD_RDJDID);
	for (uint8_t i = 0; i < 3; i++)
		jedec[i] = xfer(0xFF);
	CS_HIGH();
	CS_LOW();
	xfer(CMD_RDSR);
	*sr = xfer(0xFF);
	CS_HIGH();
}

void xmos_flash_read(uint32_t addr, uint8_t *buf, uint8_t n)
{
	CS_LOW();
	xfer(CMD_RD);
	xfer((uint8_t)(addr >> 16));
	xfer((uint8_t)(addr >> 8));
	xfer((uint8_t)addr);
	while (n--)
		*buf++ = xfer(0xFF);
	CS_HIGH();
}

static void out_hex2(uint8_t b)
{
	static const char h[] = "0123456789ABCDEF";
	out_chr(h[b >> 4]);
	out_chr(h[b & 15]);
}

static void out_hexs(const uint8_t *b, uint8_t n)
{
	out_chr('"');
	for (uint8_t i = 0; i < n; i++)
		out_hex2(b[i]);
	out_chr('"');
}

void cmd_get_extender_usb_flash(void)
{
	uint8_t md[2] = {0, 0}, jedec[3] = {0, 0, 0}, sr = 0;
	uint8_t st = xmos_flash_open();
	if (st == XMOS_FLASH_OK) {
		xmos_flash_ids(md, jedec, &sr);
		xmos_flash_close();
	}
	cmd_answer_header();
	out_chr('[');
	out_u16(st);
	out_chr(',');
	out_hexs(md, 2);
	out_chr(',');
	out_hexs(jedec, 3);
	out_chr(',');
	out_u16(sr);
	out_chr(']');
	out_crlf();
}

void cmd_get_extender_usb_config(void)
{
	uint8_t b[XMOS_FLASH_CONFIG_MAX];
	uint8_t n = 0;
	uint8_t st = xmos_flash_open();
	if (st == XMOS_FLASH_OK) {
		xmos_flash_read(XMOS_FLASH_CONFIG, b, 2);
		uint16_t len = (uint16_t)(b[0] | (uint16_t)b[1] << 8);   /* length without the two bytes */
		n = len + 2 > XMOS_FLASH_CONFIG_MAX || len == 0 ? XMOS_FLASH_CONFIG_MAX : (uint8_t)(len + 2);
		watchdog_reset();
		xmos_flash_read(XMOS_FLASH_CONFIG, b, n);
		xmos_flash_close();
	}
	cmd_answer_header();
	out_chr('[');
	out_u16(st);
	out_chr(',');
	out_u16(n);
	out_chr(',');
	out_hexs(b, n);
	out_chr(']');
	out_crlf();
}
#else
void cmd_get_extender_usb_flash(void) { cmd_error_unknown(); }
void cmd_get_extender_usb_config(void) { cmd_error_unknown(); }
#endif
