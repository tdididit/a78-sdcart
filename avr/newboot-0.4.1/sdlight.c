/* newboot - an AVR MMC/SD boot loader compatible with HolgerBootloader2
   Based on a heavily modified version of ChaN's FatFs library

   Copyright (C) 2011  Ingo Korb <ingo@akana.de>
   All rights reserved.

   Redistribution and use in source and binary forms, with or without
   modification, are permitted provided that the following conditions
   are met:
   1. Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
   2. Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.
   3. Neither the name of the University nor the names of its contributors
      may be used to endorse or promote products derived from this software
      without specific prior written permission.

   THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
   ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
   IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
   ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
   FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
   DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
   OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
   HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
   LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
   OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
   SUCH DAMAGE.


   sdlight.c: Minimal MMC/SD/SDHC read-only access with proper initialisation

*/

#include <avr/io.h>
#include <util/crc16.h>
#include <util/delay.h>
#include "config.h"
#include "diskio.h"

/* SD/MMC commands */
#define GO_IDLE_STATE          0x40
#define SEND_OP_COND           0x41
#define SWITCH_FUNC            0x46
#define SEND_IF_COND           0x48
#define SEND_CSD               0x49
#define SEND_CID               0x4a
#define STOP_TRANSMISSION      0x4c
#define SEND_STATUS            0x4d
#define SET_BLOCKLEN           0x50
#define READ_SINGLE_BLOCK      0x51
#define READ_MULTIPLE_BLOCK    0x52
#define WRITE_BLOCK            0x58
#define WRITE_MULTIPLE_BLOCK   0x59
#define PROGRAM_CSD            0x5b
#define SET_WRITE_PROT         0x5c
#define CLR_WRITE_PROT         0x5d
#define SEND_WRITE_PROT        0x5e
#define ERASE_WR_BLK_STAR_ADDR 0x60
#define ERASE_WR_BLK_END_ADDR  0x61
#define ERASE                  0x66
#define LOCK_UNLOCK            0x6a
#define APP_CMD                0x77
#define GEN_CMD                0x78
#define READ_OCR               0x7a
#define CRC_ON_OFF             0x7a

/* SD ACMDs */
#define SD_STATUS                 0x4d
#define SD_SEND_NUM_WR_BLOCKS     0x56
#define SD_SET_WR_BLK_ERASE_COUNT 0x57
#define SD_SEND_OP_COND           0x69
#define SD_SET_CLR_CARD_DETECT    0x6a
#define SD_SEND_SCR               0x73

/* R1 status bits */
#define STATUS_IN_IDLE          1
#define STATUS_ERASE_RESET      2
#define STATUS_ILLEGAL_COMMAND  4
#define STATUS_CRC_ERROR        8
#define STATUS_ERASE_SEQ_ERROR 16
#define STATUS_ADDRESS_ERROR   32
#define STATUS_PARAMETER_ERROR 64

/* card types */
#define CARD_MMCSD 0
#define CARD_SDHC  1

static uint8_t cardtype;

/* ---- SPI functions ---- */

static void spi_set_ss(uint8_t state) {
  if (state)
    SPI_PORT |= SPI_SS;
  else
    SPI_PORT &= ~SPI_SS;
}

static inline void spi_set_speed(uint8_t speed) {
  if (speed) {
    SPCR = 0b01010000 | _BV(SPR0); // 1MHz
  } else {
    SPCR = 0b01010000 | _BV(SPR1); // 250kHz
  }    
}

static void spi_init(void) {
  /* set up SPI I/O pins */
  SPI_PORT = (SPI_PORT & 15) | SPI_SCK | SPI_SS | SPI_MISO | ~SPI_MASK;
  SPI_DDR  = (SPI_DDR & 15) | SPI_SCK | SPI_SS | SPI_MOSI;

  /* enable and initialize SPI */
  SPSR = _BV(SPI2X);
  spi_set_speed(0);

  /* clear buffers */
  (void) SPSR;
  (void) SPDR;
}

static uint8_t spi_exchange_byte(uint8_t output) {
  SPDR = output;
  loop_until_bit_is_set(SPSR, SPIF);

  return SPDR;
}

static void spi_exchange_long(uint32_t *value) {
  uint8_t *data = (uint8_t *)value + 3;
  uint8_t i;

  for (i=0; i<4; i++) {
    *data = spi_exchange_byte(*data);
    data--;
  }
}

/* ---- SD functions ---- */

static uint8_t send_command(uint8_t cmd, uint32_t parameter, uint8_t crc) {
  uint16_t loops = ~0;
  uint8_t  res;

  spi_set_ss(0);
  spi_exchange_byte(cmd);
  spi_exchange_long(&parameter);
  spi_exchange_byte(crc);

  do {
    res = spi_exchange_byte(0xff);
  } while ((res & 0x80) && --loops != 0);

  return res;
}

/* deselect card and send 8 clocks */
static void deselect_card(void) {
  spi_set_ss(1);
  spi_exchange_byte(0xff);
  spi_exchange_byte(0xff);
  spi_exchange_byte(0xff);
}

DSTATUS disk_initialize(void) {
  uint32_t parameter;
  uint16_t tries = 3;
  uint8_t  i,res;

  spi_init();
 retry:
  cardtype = CARD_MMCSD;

  /* send 80 clocks with SS high */
  spi_set_ss(1);
  for (i=0; i<10; i++)
    spi_exchange_byte(0xff);

  /* switch card to idle state */
  res = send_command(GO_IDLE_STATE, 0, 0x95);
  deselect_card();
  if (res & 0x80)
    return STA_NOINIT;

  if (res != 1) {
    if (--tries)
      goto retry;
    else
      return STA_NOINIT;
  }

  /* send interface conditions (required for SDHC) */
  res = send_command(SEND_IF_COND, 0b000110101010, 0x87);
  if (res == 1) {
    /* read answer and discard */
    parameter = 0;
    spi_exchange_long(&parameter);
  }

  deselect_card();

  /* tell SD/SDHC cards to initialize */
  tries = 65535;
  do {
    /* send APP_CMD */
    res = send_command(APP_CMD, 0, 0xff);
    deselect_card();
    if (res != 1)
      goto not_sd;

    /* send SD_SEND_OP_COND */
    res = send_command(SD_SEND_OP_COND, 1L<<30, 0xff);
    deselect_card();
  } while (res == 1 && --tries > 0);

  /* failure just means that the card isn't SDHC */
  if (res != 0)
    goto not_sd;

  /* send READ_OCR to detect SDHC cards */
  res = send_command(READ_OCR, 0, 0xff);

  if (res <= 1) {
    parameter = 0;
    spi_exchange_long(&parameter);

    if (parameter & 0x40000000)
      cardtype = CARD_SDHC;
  }

  deselect_card();

 not_sd:
  /* tell MMC cards to initialize (SD ignores this) */
  tries = 65535;
  do {
    res = send_command(SEND_OP_COND, 1L<<30, 0xff);
    deselect_card();
  } while (res != 0 && --tries > 0);

  if (res != 0)
    return STA_NOINIT;

  /* set block size to 512 */
  res = send_command(SET_BLOCKLEN, 512, 0xff);
  deselect_card();
  if (res != 0)
    return STA_NOINIT;

  spi_set_speed(1);

  return 0;
}

DRESULT disk_read(BYTE *buffer, DWORD sector) {
  uint8_t res;
  uint16_t i;

  /* convert sector number to byte offset for non-SDHC cards */
  if (cardtype == CARD_MMCSD)
    sector <<= 9;

  /* send read command */
  res = send_command(READ_SINGLE_BLOCK, sector, 0xff);

  if (res != 0) {
    deselect_card();
    return RES_ERROR;
  }

  /* wait for data token */
  // FIXME: Timeout?
  do {
    res = spi_exchange_byte(0xff);
  } while (res != 0xfe);

  /* transfer data */
  SPDR = 0xff;
  for (i=0; i<512; i++) {
    uint8_t tmp;

    loop_until_bit_is_set(SPSR, SPIF);
    tmp = SPDR;
    SPDR = 0xff;
    *buffer++ = tmp;
  }
  loop_until_bit_is_set(SPSR, SPIF);

  /* drop CRC */
  (void) SPDR;
  spi_exchange_byte(0xff);

  deselect_card();
  return RES_OK;
}

