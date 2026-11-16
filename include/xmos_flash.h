/*
 * PORT (#76): the QSPI flash (IS25LQ016B) of the USB audio XMOS (XHRA-2HPA, V40 USB board) through
 * SPIE. Bus switches on the USB board (IC103/IC104, enable CARD.SWITCH AND NOT XMOS.3V3) give the
 * flash to the MCU only while no microSD card is inserted and no USB host powers the XMOS.
 * XHRA configuration block at 0x80000 (datasheet X008627, appendix A.2).
 * License: CC BY-NC-SA 4.0.
 */
#ifndef XMOS_FLASH_H_
#define XMOS_FLASH_H_
#include <stdint.h>

#define XMOS_FLASH_CONFIG 0x80000UL
#define XMOS_FLASH_CONFIG_MAX 160    /* bytes of the block handled (Forge block: 146) */

enum { XMOS_FLASH_OK = 0, XMOS_FLASH_CARD = 1, XMOS_FLASH_NONE = 2 };

uint8_t xmos_flash_open(void);       /* XMOS_FLASH_*: reachable (manufacturer ID answers) */
void xmos_flash_close(void);
void xmos_flash_ids(uint8_t mdid[2], uint8_t jedec[3], uint8_t *sr);
void xmos_flash_read(uint32_t addr, uint8_t *buf, uint8_t n);

/* result of the start (get extender.usb.config, last value) */
enum { XCFG_OFF = 0, XCFG_BOARD = 1, XCFG_CARD = 2, XCFG_NONE = 3, XCFG_FOREIGN = 4,
       XCFG_SAME = 5, XCFG_WRITTEN = 6, XCFG_ERROR = 7 };
void xmos_flash_update(void);            /* at start: Forge block unless switched off */
void cmd_set_extender_usb_config(void);  /* set extender.usb.config=0|1 */
void cmd_get_extender_usb_flash(void);   /* get extender.usb.flash */
void cmd_get_extender_usb_config(void);  /* get extender.usb.config */
void cmd_get_extender_usb_check(void);   /* get extender.usb.check: CRC of the firmware area */
#endif
