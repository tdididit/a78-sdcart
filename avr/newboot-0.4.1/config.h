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


   config.h: configuration options

*/

#ifndef CONFIG_H
#define CONFIG_H

#include "autoconf.h"

#if CONFIG_HARDWARE_VARIANT==1
/* ---------- Hardware configuration: Example ---------- */
/* This is a commented example for most of the available options    */
/* in case someone wants to build Yet Another[tm] hardware variant. */
/* Some of the values are chosen randomly, so this variant is not   */
/* expected to compile successfully.                                */

/*** SD card support ***/
/* SD Card supply voltage - choose the one appropiate to your board */
/* #  define SD_SUPPLY_VOLTAGE (1L<<15)  / * 2.7V - 2.8V */
/* #  define SD_SUPPLY_VOLTAGE (1L<<16)  / * 2.8V - 2.9V */
/* #  define SD_SUPPLY_VOLTAGE (1L<<17)  / * 2.9V - 3.0V */
#  define SD_SUPPLY_VOLTAGE (1L<<18)  /* 3.0V - 3.1V */
/* #  define SD_SUPPLY_VOLTAGE (1L<<19)  / * 3.1V - 3.2V */
/* #  define SD_SUPPLY_VOLTAGE (1L<<20)  / * 3.2V - 3.3V */
/* #  define SD_SUPPLY_VOLTAGE (1L<<21)  / * 3.3V - 3.4V */
/* #  define SD_SUPPLY_VOLTAGE (1L<<22)  / * 3.4V - 3.5V */
/* #  define SD_SUPPLY_VOLTAGE (1L<<23)  / * 3.5V - 3.6V */


/*** LEDs ***/
/* Please don't build single-LED hardware anymore... */

/* Initialize ports for all LEDs */
static inline void leds_init(void) {
  DDRC |= _BV(0);
  DDRC |= _BV(1);
}

static inline void leds_deinit(void) {
  DDRC  = 0;
  PORTC = 0;
}

/* green LED */
static inline __attribute__((always_inline)) void set_green_led(uint8_t state) {
  if (state)
    PORTC |= _BV(0);
  else
    PORTC &= ~_BV(0);
}

/* red LED */
static inline __attribute__((always_inline)) void set_red_led(uint8_t state) {
  if (state)
    PORTC |= _BV(1);
  else
    PORTC &= ~_BV(1);
}


/*** Bootloader info ***/
/* Default bootloader device ID if CONFIG_BOOT_DEVID is not set */
#define BOOTLOADER_DEVID 0xdeadbeef


/* Pre-configurated hardware variants */

#elif CONFIG_HARDWARE_VARIANT==2
/* ---------- Hardware configuration: A78-SDCART ---------- */
#  define SD_SUPPLY_VOLTAGE     (1L<<18)

static inline void leds_init(void) {
  DDRB |= _BV(0);
  DDRB |= _BV(1);
}

static inline void leds_deinit(void) {
  DDRB &= ~_BV(0);
  DDRB &= ~_BV(1);

  PORTB &= ~_BV(0);
  PORTB &= ~_BV(1);
}

static inline __attribute__((always_inline)) void set_green_led(uint8_t state) {
  if (state)
    PORTB |= _BV(1);
  else
    PORTB &= ~_BV(1);
}

static inline __attribute__((always_inline)) void set_red_led(uint8_t state) {
  if (state)
    PORTB |= _BV(0);
  else
    PORTB &= ~_BV(0);
}

#  define BOOTLOADER_DEVID 0x54444921


#else
#  error "CONFIG_HARDWARE_VARIANT is unset or set to an unknown value."
#endif


/* ---------------- End of user-configurable options ---------------- */

#ifdef CONFIG_BOOT_DEVID
#  undef  BOOTLOADER_DEVID
#  define BOOTLOADER_DEVID CONFIG_BOOT_DEVID
#endif

#if defined __AVR_ATmega644__  \
 || defined __AVR_ATmega644P__  \
 || defined __AVR_ATmega1284P__

#  define SPI_PORT   PORTB
#  define SPI_DDR    DDRB
#  define SPI_SS     _BV(4)
#  define SPI_MOSI   _BV(5)
#  define SPI_MISO   _BV(6)
#  define SPI_SCK    _BV(7)

#elif defined __AVR_ATmega128__ || defined __AVR_ATmega1281__

#  define SPI_PORT  PORTB
#  define SPI_DDR   DDRB
#  define SPI_SS    _BV(PB0)
#  define SPI_SCK   _BV(PB1)
#  define SPI_MOSI  _BV(PB2)
#  define SPI_MISO  _BV(PB3)

#else
#  error Unknown chip!
#endif

#define SPI_MASK (SPI_SS | SPI_MOSI | SPI_MISO | SPI_SCK)

#endif
