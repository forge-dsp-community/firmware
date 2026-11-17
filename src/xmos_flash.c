/*
 * PORT (#76): read access to the flash of the USB audio XMOS, see xmos_flash.h.
 * The original (libs/extender/usb-audio) has the same commands for its firmware update, which
 * no flavour builds (ext_usbaudio_incl_fw = 0). Here only reading so far.
 *
 *   get extender.usb.flash  -> [state,"MDID","JEDEC",SR]  state 0 ok, 1 microSD card inserted,
 *                              2 no answer (USB host at the audio port: the XMOS has the flash)
 *   get extender.usb.config -> [state,n,"HEX",mode,last]  n bytes from 0x80000 (length bytes
 *                              included), mode = EEPROM switch, last = result of the start (XCFG_*)
 *   get extender.usb.check  -> [state,crc16,"first 16 bytes"]  CRC (bas_crc16) over the XHRA
 *                              firmware area 0..40959, to compare with usb_xhra_2_1_1_0.bin
 *   set extender.usb.config=1  writes the Forge block (default, also at every start when it differs)
 *   set extender.usb.config=0  writes Mark's original block back and leaves the flash alone at start
 *
 * Forge block: Mark's block (VID/PID 20b1:3066, 500 mA, I2S left aligned, the same GPIO events)
 * with only 44.1/48/88.2/96 kHz (byte 11 = 0x33, 384K_EN off) and the strings "Forge DSP" /
 * "Forge DSP <model> - Audio", serial number = MCU id. Only the 4 KB sector at 0x80000 is erased and written, never the
 * firmware at 0, the status register (QE) is not touched. Only on PCB 4.0 (XHRA; 4.1 has an XU208
 * without this block) and only over a block that carries the XHRA marker. A broken block only
 * makes the XHRA use its defaults (up to 192 kHz, "XMOS"); the next start writes it again.
 * License: CC BY-NC-SA 4.0.
 */
#include "cmd.h"
#include "xmos_flash.h"
#ifdef __AVR__
#include "board.h"
#endif

#if defined(__AVR__) && BAS_EXT_USBAUDIO > 0
#include <avr/io.h>
#include <avr/pgmspace.h>
#include <string.h>
#include <util/delay.h>
#include "eram.h"
#include "identity.h"
#include "mcuid.h"
#include "sd.h"
#include "watchdog.h"

#define CS_LOW()  PIN_CLR(PIN_EXT_MICROSD_SPI_SS)
#define CS_HIGH() PIN_SET(PIN_EXT_MICROSD_SPI_SS)

#define CMD_RD     0x03
#define CMD_RDSR   0x05
#define CMD_RDJDID 0x9F
#define CMD_RDMDID 0x90
#define CMD_WREN   0x06
#define CMD_PP     0x02
#define CMD_SER    0x20     /* sector erase, 4 KB */
#define MFR_ISSI   0x9D
#define SR_WIP     0x01

#define EE_MODE    0x0F13   /* byte: 0 = leave the XMOS flash alone, else (0xFF blank) Forge block */

/* Mark's block, libs/extender/usb-audio/96ELN-V40-1_1_0-config.bin (98 bytes) */
static const uint8_t mark_block[] PROGMEM = {
	0x60, 0x00, 0xA5, 0x00, 0xB1, 0x20, 0x66, 0x30, 0xFA, 0x04, 0x01, 0xFF, 0, 0, 0, 0, 0, 0, 0, 0,
	0x08, 'X', 0, 'M', 0, 'O', 0, 'S', 0,
	0x28, 'x', 0, 'C', 0, 'O', 0, 'R', 0, 'E', 0, '-', 0, 'A', 0, 'U', 0, 'D', 0, 'I', 0, 'O', 0,
	' ', 0, 'H', 0, 'i', 0, '-', 0, 'R', 0, 'e', 0, 's', 0, ' ', 0, '2', 0,
	0x00,
	0x09, 0x04, 0x00, 0xD6, 0x02, 0xD6, 0x04, 0xD6, 0x06, 0xD6, 0x08, 0xE6, 0x0A, 0xE6, 0x0C, 0xE6, 0x0E,
	0xE6, 0x14, 0xE2, 0x16, 0xD2, 0x1A, 0xE0, 0x1C, 0xD0, 0xFE,
};
#define MARK_LEN   ((uint8_t)sizeof(mark_block))
#define MARK_TAIL  71       /* LangID and events follow the (empty) serial number */

uint8_t xmos_flash_last;   /* XCFG_* of the start */

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

static uint8_t sr(void)
{
	CS_LOW();
	xfer(CMD_RDSR);
	uint8_t v = xfer(0xFF);
	CS_HIGH();
	return v;
}

static void wren(void)
{
	CS_LOW();
	xfer(CMD_WREN);
	CS_HIGH();
}

static uint8_t wait_idle(uint16_t ms)
{
	for (uint16_t i = 0; i < ms; i++) {
		if (!(sr() & SR_WIP))
			return 1;
		_delay_ms(1);
		watchdog_reset();
	}
	return 0;
}

static void addr24(uint32_t a)
{
	xfer((uint8_t)(a >> 16));
	xfer((uint8_t)(a >> 8));
	xfer((uint8_t)a);
}

/* erase the config sector, program n bytes (one page), read back into r */
static uint8_t write_block(const uint8_t *b, uint8_t n, uint8_t *r)
{
	wren();
	CS_LOW();
	xfer(CMD_SER);
	addr24(XMOS_FLASH_CONFIG);
	CS_HIGH();
	if (!wait_idle(1000))      /* IS25LQ016B: sector erase typ. 70 ms, max 300 ms */
		return 0;
	wren();
	CS_LOW();
	xfer(CMD_PP);
	addr24(XMOS_FLASH_CONFIG);
	for (uint8_t i = 0; i < n; i++)
		xfer(b[i]);
	CS_HIGH();
	if (!wait_idle(20))        /* page program max 0.8 ms */
		return 0;
	xmos_flash_read(XMOS_FLASH_CONFIG, r, n);
	return memcmp(r, b, n) == 0;
}

static uint8_t put_str(uint8_t *b, uint8_t n, const char *s)
{
	uint8_t l = 0;
	for (; s[l]; l++) {
		b[n + 1 + 2 * l] = (uint8_t)s[l];
		b[n + 2 + 2 * l] = 0;
	}
	b[n] = (uint8_t)(2 * l);
	return (uint8_t)(n + 1 + 2 * l);
}

/* the Forge block in b, returns its length */
static uint8_t forge_block(uint8_t *b)
{
	for (uint8_t i = 0; i < 20; i++)
		b[i] = pgm_read_byte(&mark_block[i]);
	b[9] = 0x00;               /* no 384K_EN */
	b[11] = 0x33;              /* 48, 96, 44.1, 88.2 kHz */
	char name[40] = "Forge DSP ";
	const char *m = identity_model();
	strncat(name, strcmp(m, "96ELN3") ? m : "96ELN-3", 16);
	strcat(name, " - Audio");
	uint8_t n = put_str(b, 20, "Forge DSP");
	n = put_str(b, n, name);
	/* serial number: the MCU id of system.key.info (lot number, wafer, coordinates), so a host
	 * tells several Forges apart; anything not alphanumeric becomes '0' */
	char sn[sizeof mcuid_string];
	uint8_t k = 0;
	for (; mcuid_string[k] && k < sizeof sn - 1; k++) {
		char c = mcuid_string[k];
		sn[k] = ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) ? c : '0';
	}
	sn[k] = 0;
	n = put_str(b, n, sn);
	for (uint8_t i = MARK_TAIL; i < MARK_LEN; i++)
		b[n++] = pgm_read_byte(&mark_block[i]);
	b[0] = (uint8_t)(n - 2);
	b[1] = 0;
	return n;
}

/* bring the flash to the block of mode (1 Forge, 0 Mark); XCFG_* */
static uint8_t apply(uint8_t mode)
{
	uint8_t want[XMOS_FLASH_CONFIG_MAX], have[XMOS_FLASH_CONFIG_MAX];
	uint8_t n;
	if (mode) {
		n = forge_block(want);
	} else {
		n = MARK_LEN;
		for (uint8_t i = 0; i < n; i++)
			want[i] = pgm_read_byte(&mark_block[i]);
	}
	xmos_flash_read(XMOS_FLASH_CONFIG, have, n);
	if (have[2] != 0xA5 || have[3] != 0x00)
		return XCFG_FOREIGN;   /* no XHRA block there: not ours to write */
	if (memcmp(have, want, n) == 0)
		return XCFG_SAME;
	return write_block(want, n, have) ? XCFG_WRITTEN : XCFG_ERROR;
}

static uint8_t board_xhra(void) { return strcmp(identity_board(), "4.0") == 0; }

void xmos_flash_update(void)
{
	xmos_flash_last = XCFG_OFF;
	if (eram_rb(EE_MODE) == 0)
		return;
	xmos_flash_last = XCFG_BOARD;
	if (!board_xhra())
		return;
	uint8_t st = xmos_flash_open();
	if (st != XMOS_FLASH_OK) {
		xmos_flash_last = st == XMOS_FLASH_CARD ? XCFG_CARD : XCFG_NONE;
		return;
	}
	xmos_flash_last = apply(1);
	xmos_flash_close();
}

void cmd_set_extender_usb_config(void)
{
	int16_t v = cmd_reg_var_integers[1];
	if (v < 0 || v > 1) {
		cmd_error_cmd_oor();
		return;
	}
	if (!board_xhra()) {
		cmd_error_unknown_para();
		return;
	}
	uint8_t st = xmos_flash_open();
	if (st != XMOS_FLASH_OK) {
		cmd_error_unknown_para();  /* card inserted or host at the USB audio port */
		return;
	}
	uint8_t r = apply((uint8_t)v);
	xmos_flash_close();
	if (r == XCFG_SAME || r == XCFG_WRITTEN) {
		eram_wb(EE_MODE, (uint8_t)v);
		cmd_answer_ok();
	} else {
		cmd_error_chksum();
	}
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

void cmd_get_extender_usb_check(void)
{
	uint8_t b[XMOS_FLASH_CONFIG_MAX], head[16];
	uint16_t crc = 0;
	uint8_t st = xmos_flash_open();
	if (st == XMOS_FLASH_OK) {
		for (uint32_t a = 0; a < 40960UL; a += sizeof b) {
			xmos_flash_read(a, b, sizeof b);
			if (a == 0)
				memcpy(head, b, sizeof head);
			crc = bas_crc16_more(crc, b, sizeof b);
			watchdog_reset();
		}
		xmos_flash_close();
	} else {
		memset(head, 0, sizeof head);
	}
	cmd_answer_header();
	out_chr('[');
	out_u16(st);
	out_chr(',');
	out_u16(crc);
	out_chr(',');
	out_hexs(head, sizeof head);
	out_chr(']');
	out_crlf();
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
		/* PORT (#80): 32 bit, an erased block (0xFFFF) wrapped to 1 with the 16-bit int of avr-gcc */
		n = (uint32_t)len + 2 > XMOS_FLASH_CONFIG_MAX || len == 0 ? XMOS_FLASH_CONFIG_MAX : (uint8_t)(len + 2);
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
	out_chr(',');
	out_u16(eram_rb(EE_MODE) == 0 ? 0 : 1);
	out_chr(',');
	out_u16(xmos_flash_last);
	out_chr(']');
	out_crlf();
}
#else
void xmos_flash_update(void) {}
void cmd_set_extender_usb_config(void) { cmd_error_unknown(); }
void cmd_get_extender_usb_check(void) { cmd_error_unknown(); }
void cmd_get_extender_usb_flash(void) { cmd_error_unknown(); }
void cmd_get_extender_usb_config(void) { cmd_error_unknown(); }
#endif
