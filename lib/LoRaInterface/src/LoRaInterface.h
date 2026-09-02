/*
 * LoRaInterface: vendored from attermann/microReticulum
 * examples/common/lora_interface/ at commit
 * 40fa628809d57140180c1c833559ab96fec992c1.
 *
 * Copyright (c) 2026 Chad Attermann
 * Licensed under the Apache License, Version 2.0. See ./LICENSE.
 *
 * Thicket modifications are listed in lib/LoRaInterface/README.md. In this file:
 * the last-frame RSSI/SNR accessors below.
 */

#pragma once

#include <microReticulum/Interface.h>
#include <microReticulum/Bytes.h>
#include <microReticulum/Type.h>

#ifdef ARDUINO
#include <SPI.h>
#include <RadioLib.h>
#endif

#include <stdint.h>

class LoRaInterface : public RNS::InterfaceImpl {

// THICKET: optional interrupt-driven receive, off unless -DTHICKET_LORA_ISR.
//
// Upstream polls checkIrq() every loop() and says so in a comment. That is a
// choice, not a constraint microReticulum imposes -- so this is a change to our
// own vendored copy plus a wake, not a negotiation with upstream's cooperative
// design.
//
// ⚠ On the RAK4631 EVERY SX1262 interrupt routes to DIO1, transmit-done
// included. So the ISR is a *wake*, never a diagnosis: it sets a flag, and
// loop() still asks the radio what actually happened. Treating the interrupt
// as "a packet arrived" would read the FIFO after our own transmissions.
public:
	// Called from ISR context. Keep it to setting a flag or giving a task
	// notification -- nothing that allocates, logs, or touches SPI.
	static void set_wake_hook(void (*hook)());

public:
	//z def get_address_for_if(name):
	//z def get_broadcast_for_if(name):

public:
	//p def __init__(self, owner, name, device=None, bindip=None, bindport=None, forwardip=None, forwardport=None):
	LoRaInterface(const char* name = "LoRaInterface");
	virtual ~LoRaInterface();

	virtual bool start();
	virtual void stop();
	virtual void loop();

	//virtual inline std::string toString() const { return "LoRaInterface[" + name() + "]"; }

	// Thicket addition: link quality of the most recently received LoRa frame.
	//
	// RadioLib's getRSSI()/getSNR() are only meaningful immediately after
	// readData() and are overwritten by the next packet, so loop() latches them
	// there. These are the *frame* figures, not per-RNS-packet: a frame split
	// across two LoRa transmissions reports the second half, and an application
	// that reads them one main-loop iteration after delivery may be reading a
	// later, unrelated frame. Diagnostics, not telemetry.
	inline float last_rssi() const { return _last_rssi; }   // dBm
	inline float last_snr() const { return _last_snr; }     // dB
	inline bool signal_valid() const { return _signal_valid; }

private:
	virtual bool send_outgoing(const RNS::Bytes& data);
	void on_incoming(const RNS::Bytes& data);

public:
	// Split-packet protocol constants
	static constexpr uint8_t HEADER_SPLIT     = 0x08;  // bit 3: split-packet flag
	static constexpr uint8_t HEADER_SEQ_MASK  = 0x07;  // bits 2:0: sequence number
	static constexpr uint8_t SEQ_UNSET        = 0xFF;  // sentinel: no split in progress
	static constexpr int     LORA_MAX_PAYLOAD = 254;   // 255 - 1 header byte

private:
	//uint8_t buffer[Type::Reticulum::MTU] = {0};
	const uint8_t message_count = 0;
	RNS::Bytes buffer;

	uint8_t _rx_seq     = SEQ_UNSET;  // sequence of split RX in progress
	uint8_t _tx_seq_ctr = 0;          // rolling TX split sequence counter

	// Thicket addition: see last_rssi()/last_snr() above. Declared outside the
	// ARDUINO guard so the accessors compile on native builds too; only
	// written by the Arduino receive path.
	float _last_rssi    = 0.0f;
	float _last_snr     = 0.0f;
	bool  _signal_valid = false;

	// Radio parameters (RadioLib units: MHz, kHz)
	//
	// 914.875 is the US Reticulum community frequency, NOT a round 915.0.
	// Being one channel width (125 kHz) off leaves the passbands barely
	// overlapping and both ends mutually deaf. It presents as a dead radio:
	// announces go out, nothing is heard, and no error is raised anywhere.
	//
	// frequency, bandwidth and spreading factor must match a peer EXACTLY.
	// Coding rate and TX power do not (docs/lora-parameters.md).
	const float frequency = 914.875; // MHz
	const float bandwidth = 125.0;   // kHz
	const int   spreading = 8;
	const int   coding    = 5;
	const int   power     = 17;      // dBm

#ifdef ARDUINO
	Module*        _module      = nullptr;
	PhysicalLayer* _radio       = nullptr;

	// THICKET: set by the DIO1 ISR, consumed by loop(). volatile because it
	// crosses an interrupt boundary; sig_atomic_t would be equivalent here.
	static volatile bool _irq_pending;
	static void (*_wake_hook)();
	static void _isr();
	int            _pa_mode_pin = -1;    // V4 FEM PA mode pin; -1 = not present
#endif

};
