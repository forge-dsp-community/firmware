/*
 * Device identity (#31): one build serves several identical variants that only differ in
 * the identity strings (V40/V41, 96ELN/CORED8, see docs/single-firmware.md). The identity
 * (model + PCB) is chosen from the list of the build and kept in the EEPROM (0xF00, unused by
 * the original layout); the flasher copies it from the firmware that was on the device.
 */
#ifndef IDENTITY_H_
#define IDENTITY_H_
#include <stdint.h>
const char *identity_model(void);   /* model_id: 96ELN, CORED8, ... */
const char *identity_board(void);   /* board_version: 4.0, 4.1, ... */
uint8_t identity_count(void);
const char *identity_name(uint8_t i, char buf[16]);  /* "MODEL-PCB" */
uint8_t identity_set(const char *model_pcb);         /* 0 = not allowed for this build */

/* Production number ("Fertigungsnummer") of the device: exactly 8 digits (20000023, 00000042). The original
 * firmware 1.10.x prints it in system.key.info (FAG-MODEL-PCB-MCUID-<number>-<date>-MD); it is
 * stored AES encrypted in the EEPROM of DSP core 1 (bus 1, 0xA0, from 0x7EFF) with a key bound to
 * the MCU. The port does not decrypt it: the flasher copies it from key.info, or it is set by hand
 * (set system.number). Kept at 0x0F40 (magic 0x53, 8 ASCII digits), also over a factory reset. */
#define IDENTITY_SERIAL_ADDR 0x0F40
#define IDENTITY_SERIAL_LEN 8
uint8_t identity_serial(char buf[IDENTITY_SERIAL_LEN + 1]);  /* 1 = set; buf "" when not */
uint8_t identity_serial_set(const char *digits);            /* "0" or "" clears; 0 = not valid */
#endif
