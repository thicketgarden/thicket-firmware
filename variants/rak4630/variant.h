/*
  Copyright (c) 2014-2015 Arduino LLC.  All right reserved.
  Copyright (c) 2016 Sandeep Mistry All right reserved.
  Copyright (c) 2018, Adafruit Industries (adafruit.com)

  This library is free software; you can redistribute it and/or
  modify it under the terms of the GNU Lesser General Public
  License as published by the Free Software Foundation; either
  version 2.1 of the License, or (at your option) any later version.
  This library is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
  See the GNU Lesser General Public License for more details.
  You should have received a copy of the GNU Lesser General Public
  License along with this library; if not, write to the Free Software
  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
*/

#pragma once

#ifndef _VARIANT_RAK4630_
#define _VARIANT_RAK4630_

#define RAK4630

/** Master clock frequency */
#define VARIANT_MCK (64000000ul)

#define USE_LFXO // Board uses 32khz crystal for LF
// define USE_LFRC    // Board uses RC for LF

/*----------------------------------------------------------------------------
 *        Headers
 *----------------------------------------------------------------------------*/
#include <Arduino.h>

#ifdef __cplusplus
extern "C"
{
#endif // __cplusplus

	/*
	 * WisBlock Base GPIO definitions
	 */
	static const uint8_t WB_IO1 = 17;	   // SLOT_A SLOT_B
	static const uint8_t WB_IO2 = 34;	   // SLOT_A SLOT_B
	static const uint8_t WB_IO3 = 21;	   // SLOT_C
	static const uint8_t WB_IO4 = 4;	   // SLOT_C
	static const uint8_t WB_IO5 = 9;	   // SLOT_D
	static const uint8_t WB_IO6 = 10;	   // SLOT_D
	static const uint8_t WB_SW1 = 33;	   // IO_SLOT
	static const uint8_t WB_A0 = 5;		   // IO_SLOT
	static const uint8_t WB_A1 = 31;	   // IO_SLOT
	static const uint8_t WB_I2C1_SDA = 13; // SENSOR_SLOT IO_SLOT
	static const uint8_t WB_I2C1_SCL = 14; // SENSOR_SLOT IO_SLOT
	static const uint8_t WB_I2C2_SDA = 24; // IO_SLOT
	static const uint8_t WB_I2C2_SCL = 25; // IO_SLOT
	static const uint8_t WB_SPI_CS = 26;   // IO_SLOT
	static const uint8_t WB_SPI_CLK = 3;   // IO_SLOT
	static const uint8_t WB_SPI_MISO = 29; // IO_SLOT
	static const uint8_t WB_SPI_MOSI = 30; // IO_SLOT

// Number of pins defined in PinDescription array
#define PINS_COUNT (48)
#define NUM_DIGITAL_PINS (48)
#define NUM_ANALOG_INPUTS (6)
#define NUM_ANALOG_OUTPUTS (0)

// LEDs
#define PIN_LED1 (35)
#define PIN_LED2 (36)

#define LED_BUILTIN PIN_LED1
#define LED_CONN PIN_LED2

#define LED_GREEN PIN_LED1
#define LED_BLUE PIN_LED2

#define LED_STATE_ON 1 // State when LED is litted

/*
 * Analog pins
 */
#define PIN_A0 (5)	//(3)
#define PIN_A1 (31) //(4)
#define PIN_A2 (28)
#define PIN_A3 (29)
#define PIN_A4 (30)
#define PIN_A5 (31)
#define PIN_A6 (0xff)
#define PIN_A7 (0xff)

	static const uint8_t A0 = PIN_A0;
	static const uint8_t A1 = PIN_A1;
	static const uint8_t A2 = PIN_A2;
	static const uint8_t A3 = PIN_A3;
	static const uint8_t A4 = PIN_A4;
	static const uint8_t A5 = PIN_A5;
	static const uint8_t A6 = PIN_A6;
	static const uint8_t A7 = PIN_A7;
#define ADC_RESOLUTION 14

// Other pins
#define PIN_AREF (2)
#define PIN_NFC1 (9)
#define PIN_NFC2 (10)

	static const uint8_t AREF = PIN_AREF;

/*
 * Serial interfaces
 */
// TXD1 RXD1 on Base Board
#define PIN_SERIAL1_RX (15)
#define PIN_SERIAL1_TX (16)

// TXD0 RXD0 on Base Board
#define PIN_SERIAL2_RX (19)
#define PIN_SERIAL2_TX (20)

/*
 * SPI Interfaces
 */
/* Thicket modification, 2026-08-01. Upstream (attermann/microReticulum_Firmware,
 * and identically attermann/microReticulum examples/lora_announce) declares one
 * SPI interface — the WisBlock IO-slot bus — and its LoRaInterface then calls
 * SPI.setPins() at radio start to repoint that single instance at the SX1262's
 * pins (P1.10-P1.15). That works only because the upstream examples keep no
 * state on external SPI flash; on nRF52 they fall back to microStore's
 * InternalFSFileSystem.
 *
 * Thicket keeps identity, keys and the message store on the RAK15001 external
 * flash (never the ~28 KB internal FS), which sits on the IO-slot bus. If the
 * radio repointed that bus, storage would break the moment the SX1262 came up.
 * So the radio gets its own instance: SPI stays the IO-slot bus, SPI1 is the
 * SX1262. Adafruit's nRF52 SPI library supports two (SPI=SPIM3, SPI1=SPIM2).
 */
#define SPI_INTERFACES_COUNT 2

/* SPI  — WisBlock IO slot. RAK15001 external flash (CS = WB_SPI_CS = 26). */
#define PIN_SPI_MISO (29)
#define PIN_SPI_MOSI (30)
#define PIN_SPI_SCK (3)

/* SPI1 — SX1262 LoRa radio, nRF52840 P1.11/P1.12/P1.13. CS is P1.10 (42),
 * BUSY P1.14 (46), DIO1 P1.15 (47), NRESET P1.06 (38); those are held by
 * LoRaInterface, not here, because RadioLib's Module owns them. */
#define PIN_SPI1_MISO (45)
#define PIN_SPI1_MOSI (44)
#define PIN_SPI1_SCK (43)

	static const uint8_t SS = 26;
	static const uint8_t MOSI = PIN_SPI_MOSI;
	static const uint8_t MISO = PIN_SPI_MISO;
	static const uint8_t SCK = PIN_SPI_SCK;

/*
 * Wire Interfaces
 */
#define WIRE_INTERFACES_COUNT 2

#define PIN_WIRE_SDA (13)
#define PIN_WIRE_SCL (14)
#define PIN_WIRE1_SDA (24)
#define PIN_WIRE1_SCL (25)

// QSPI Pins
#define PIN_QSPI_SCK 3	// 19
#define PIN_QSPI_CS 26	// 17
#define PIN_QSPI_IO0 30 // 20
#define PIN_QSPI_IO1 29 // 21
#define PIN_QSPI_IO2 28 // 22
#define PIN_QSPI_IO3 2	// 23

// On-board QSPI Flash
#define EXTERNAL_FLASH_DEVICES IS25LP080D
#define EXTERNAL_FLASH_USE_QSPI

#ifdef __cplusplus
}
#endif

/*----------------------------------------------------------------------------
 *        Arduino objects - C++ only
 *----------------------------------------------------------------------------*/

#endif
