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


   main.c: The actual flash updater

*/

#include <stdio.h>
#include <string.h>
#include <avr/boot.h>
#include <avr/interrupt.h>
#include <avr/pgmspace.h>
#include <avr/power.h>
#include <avr/wdt.h>
#include <util/delay.h>
#include <util/crc16.h>
#include "config.h"
#include "ff.h"

#ifdef __AVR_ATmega1284P__
/* fix an issue with the avr-libc from Debian lenny */
#  undef  SPM_PAGESIZE
#  define SPM_PAGESIZE 256
#endif

/* create alias to the correct progmem read function */
#if BINARY_LENGTH >= 65536
#  define flash_read_word(x) pgm_read_word_far(x)
#else
#  define flash_read_word(x) pgm_read_word(x)
#endif

#define min(a,b) ((a)<(b)?(a):(b))

typedef struct {
  uint32_t device_id;
  uint16_t version;
  uint16_t crc;
} bootinfo_t;

static FATFS fat;
static DIR dh;
static FILINFO finfo;
static FRESULT fr;
static FIL fd;
static bootinfo_t file_bi;
static uint8_t databuffer[512];

static void flash_file(void) {
  uint32_t address;
  uint16_t remain = BINARY_LENGTH/512;
  uint8_t  i,j;
  uint16_t *ptr;

  /* reopen file to reset offset */
  l_openfile(&fat, &finfo, &fd);

  address = 0;
  while (remain--) {

    /* toggle green LED */
    set_green_led(remain & 1);

    /* read sector */
    if (f_read(&fd, databuffer, 512) != FR_OK)
      break;

    /* flash sector */
    ptr = (uint16_t *)databuffer;

    for (i=0; i < 512 / SPM_PAGESIZE; i++) {
      /* erase page */
      boot_page_erase(address);
      boot_spm_busy_wait();

      /* copy new contents */
      for (j=0; j<SPM_PAGESIZE/2; j++)
        boot_page_fill(address + j*2, *ptr++);

      /* write page */
      boot_page_write(address);
      boot_spm_busy_wait();

      address += SPM_PAGESIZE;
    }
  }

  boot_rww_enable();
}

static uint8_t validate_file(void) {
  uint8_t  *ptr;
  uint16_t i,crc;
  uint16_t remain;

  /* open file, can't fail */
  l_openfile(&fat, &finfo, &fd);

  /* calculate CRC */
  remain = BINARY_LENGTH/512;
  crc = 0xffff;
  while (remain) {
    if (f_read(&fd, databuffer, 512) != FR_OK)
      return 0;

    remain--;
    ptr = databuffer;

    for (i=0; i<512; i++)
      crc = _crc_ccitt_update(crc, *ptr++);
  }

  if (crc != 0)
    return 0;

  /* copy bootinfo from buffer*/
  memcpy(&file_bi, databuffer+512-sizeof(bootinfo_t), sizeof(bootinfo_t));

  /* check bootinfo contents */
  if (file_bi.device_id != BOOTLOADER_DEVID) {
    return 0;
  }

  /* dev mode */
  if (file_bi.version == 0 &&
      file_bi.crc != flash_read_word(BINARY_LENGTH - sizeof(bootinfo_t)
                                     + offsetof(bootinfo_t, crc))) {
    return 1;
  }

  /* check version */
  uint16_t flashversion = flash_read_word(BINARY_LENGTH - sizeof(bootinfo_t)
                                          + offsetof(bootinfo_t, version));

  if (flashversion == 0xffff ||
      file_bi.version > flashversion) {
    return 1;
  }

  return 0;
}

static void try_update(void) {
  set_green_led(1);

  /* mount file system */
  fr = f_mount(0, &fat);
  if (fr != FR_OK) {
    set_green_led(0);
    return;
  }

  l_openroot(&fat, &dh);

  while ((f_readdir(&dh, &finfo) == FR_OK) && finfo.fname[0] != 0) {
    if (finfo.fsize == BINARY_LENGTH) {
      /* candidate file found - validate and flash if valid */
      if (validate_file()) {
        flash_file();
        break;
      }
    }
  }

  set_green_led(0);
}

static void __attribute__((noreturn)) (*start_app)(void) = 0;

static void try_start_app(void) {
  uint16_t crc = 0xffff;
  uint16_t addr;

  /* check in-flash CRC */
#if BINARY_LENGTH < 64*1024
  for (addr=0; addr<BINARY_LENGTH; addr++)
    crc = _crc_ccitt_update(crc, pgm_read_byte(addr));
#elif BINARY_LENGTH < 128*1024
  /* slightly smaller and faster than a 32 bit loop counter */
  addr = 0;
  do {
    crc = _crc_ccitt_update(crc, pgm_read_byte(addr));
  } while (++addr != 0);

  for (addr=0; addr < BINARY_LENGTH - 65536; addr++)
    crc = _crc_ccitt_update(crc, pgm_read_byte_far(65536UL + addr));
#else
#  error FIXME: Devices larger than 128K not supported yet
#endif

  if (crc == 0) {
    /* deinitialize hardware */
    leds_deinit();
    SPCR     = 0;
    SPSR     = 0;
    SPI_PORT = 0;
    SPI_DDR  = 0;

    /* start app */
    start_app();
  } else {
    /* blink red LED for two seconds if application is corrupted */
    for (uint8_t i=0; i<10;i++) {
      set_red_led(0);
      _delay_ms(100);
      set_red_led(1);
      _delay_ms(100);
    }
    return;
  }
}

/* Make sure the watchdog is disabled as soon as possible */
void disable_watchdog(void) \
      __attribute__((naked)) \
      __attribute__((section(".init3")));
void disable_watchdog(void) {
  wdt_disable();
}

/* backup SPM instruction in case someone wants to write a bootloader-updater */
void __attribute((naked,section(".flash_end"))) spm_instruction(void) {
  asm volatile("spm\n"
               "ret\n");
}

#if __GNUC__ > 4 || (__GNUC__ == 4 && __GNUC_MINOR__ > 1)
int main(void) __attribute__((OS_main));
#endif
int main(void) {
  /* disable JTAG */
  asm volatile("in  r24, %0\n"
               "ori r24, 0x80\n"
               "out %0, r24\n"
               "out %0, r24\n"
               :
               : "I" (_SFR_IO_ADDR(MCUCR))
               : "r24"
               );

  leds_init();
  set_red_led(1);
  set_green_led(0);

  while (1) {
    try_update();
    try_start_app();
//     set_red_led(0);
//      _delay_ms(100);
//      set_red_led(1);
//      _delay_ms(100);
  }
}
