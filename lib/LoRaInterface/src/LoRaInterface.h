/*
 * LoRaInterface — vendored from attermann/microReticulum
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

	// Thicket addition — link quality of the most recently received LoRa frame.
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

	// Thicket addition — see last_rssi()/last_snr() above. Declared outside the
	// ARDUINO guard so the accessors compile on native builds too; only
	// written by the Arduino receive path.
	float _last_rssi    = 0.0f;
	float _last_snr     = 0.0f;
	bool  _signal_valid = false;

	// Radio parameters (RadioLib units: MHz, kHz)
	const float frequency = 915.0;   // MHz
	const float bandwidth = 125.0;   // kHz
	const int   spreading = 8;
	const int   coding    = 5;
	const int   power     = 17;      // dBm

#ifdef ARDUINO
	Module*        _module      = nullptr;
	PhysicalLayer* _radio       = nullptr;
	int            _pa_mode_pin = -1;    // V4 FEM PA mode pin; -1 = not present
#endif

};
