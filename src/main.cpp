// Copyright (C) 2026 Thicket contributors
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// ---------------------------------------------------------------------------
// Thicket — M1 skeleton.
//
// The Reticulum stack runs ON this device: on-device identity, on-device
// encryption, messages composed and delivered over LoRa with nothing tethered.
// This file is the wiring for that claim, in the order the hardware has to come
// up, with a serial line at every step so a failure on a terminal says which
// step failed instead of going quiet.
//
// What is real here: external-flash persistence, an identity that survives a
// power cycle, the SX1262 on air, Reticulum transport, an announce, a live LXMF
// router with its delivery destination registered, and an on-device composed,
// signed and encrypted reply sent back over LoRa to whoever wrote to us.
//
// That last part is what makes the whole claim provable with nothing attached.
// The device has no screen and no buttons; the *peer's* screen is the output
// device. Send it a message from a T-Deck or a Sideband instance and a reply
// appears there, composed here. No cable, no serial console, no host.
//
// What is NOT here yet, and is marked TODO(M1) at each site: a message store,
// a UI, input, and any conformance check against Python RNS. Nothing below
// fakes those.
// ---------------------------------------------------------------------------

// Arduino.h first, then kill its abs()/round() function-like macros. Every
// libstdc++ header after this point that mentions std::abs or std::round
// (<chrono>, reached through <mutex> / MsgPack / ArduinoJson) fails to parse
// otherwise. Same collision the dependency patches in scripts/patch_deps.py
// fix inside microLXMF.
#include <Arduino.h>
#undef abs
#undef round

// Pulls the TinyUSB stack into the link — USB-CDC Serial is undefined
// without it on the Adafruit nRF52 core under PlatformIO.
#include <Adafruit_TinyUSB.h>

#include <microStore/FileSystem.h>
#include <microStore/Adapters/FlashFSFileSystem.h>

#include <microReticulum.h>

#include <LoRaInterface.h>
#include <LXMF/LXMRouter.h>
#include <LXMF/LXMessage.h>

#include <memory>
#include <string>
#include <cmath>
#include <cstdio>
#include <cstring>

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

// Where the private key lives on external flash. One file, 64 bytes: the
// Ed25519 signing key and the X25519 encryption key concatenated, exactly as
// RNS::Identity::to_file writes them. Anyone holding this file is this device.
static const char* IDENTITY_PATH = "/thicket_identity";

// microLXMF's storage root. The router takes it now; it is only used once a
// MessageStore is attached. TODO(M1): attach one, retuned — upstream's
// MAX_CONVERSATIONS=32 x MAX_MESSAGES_PER_CONVERSATION=256 is a 266,896-byte
// object and will not link on this part. 8x32 costs 9,488 B and does.
static const char* LXMF_STORAGE_PATH = "/lxmf";

// Announce app_data. What a peer running Sideband shows in its contact list.
static const char* DISPLAY_NAME = "Thicket";

// RAK15001 WisBlock Flash module: Gigadevice GD25Q16C, 2 MiB, on the IO-slot
// SPI bus with CS = WB_SPI_CS (26) = the variant's SS. The RAK15001 macro is
// microStore's, and it deliberately clamps the part to 8 MHz single-SPI rather
// than 104 MHz quad.
static const SPIFlash_Device_t FLASH_DEVICE = RAK15001;

// How often to re-announce, seconds. Reticulum's own guidance is sparing;
// this is a bring-up value chosen so the founder does not have to wait around
// with a T-Deck. TODO(M1): drive announces from user action plus a much longer
// timer once there is a UI.
static const double ANNOUNCE_INTERVAL_S = 120.0;

// Auto-reply. The device answers anything delivered to it, addressed to the
// source hash carried by the inbound message — nothing about the peer is
// compiled in, so no reflash is needed when the far end's address changes or a
// second peer appears. This is the M1 proof: it needs no screen here and no
// host, because the reply lands on the sender's screen.
//
// The pause exists because LoRaInterface::send_outgoing() blocks for the whole
// air time (~400 ms for a full frame at SF8/125 kHz) and the peer that just
// transmitted needs its own receiver back before we answer.
static const uint32_t REPLY_DELAY_MS = 1500;

// Single-LoRa-packet content budget, in bytes.
//
// microLXMF picks OPPORTUNISTIC (one packet, no link) over DIRECT (establish a
// link first) when packed_size() <= LORA_ENCRYPTED_PACKET_MDU = 159. Packed
// size is 16 (dest) + 16 (source) + 64 (signature) + msgpack payload, and the
// payload of a titleless message costs 15 bytes of framing: array header 1,
// float64 timestamp 9, empty title bin 2, content bin header 2, empty field
// map 1. 159 - 96 - 15 = 48 bytes of content.
//
// Over budget is not an error — the router just falls back to link delivery,
// which is slower and needs a path first. Under budget is worth staying.
// The reply carries no title for the same reason: a title is 1-2 more bytes of
// header plus its own text, and the peer already shows who sent it.
static const size_t REPLY_CONTENT_BUDGET = 48;

// Room to format without truncating; the budget above is a preference, not a
// limit, and a body that overruns it should be visible rather than clipped.
static const size_t BODY_BUF = 72;

// Optional timed send — OFF unless both flags are compiled in.
//
//   -DTHICKET_AUTOSEND_INTERVAL_S=60
//   -DTHICKET_AUTOSEND_DEST=<32 hex chars, the peer's lxmf.delivery hash>
//
// This is the fallback for the case where auto-reply misbehaves and the founder
// needs to know whether the transmit path works at all. It is deliberately not
// the default: a stock build transmits announces and replies, and never
// unsolicited traffic. It also requires knowing the peer's destination hash at
// build time, which is exactly the reflash-after-you-find-out problem that
// auto-reply exists to avoid.
#if defined(THICKET_AUTOSEND_INTERVAL_S) && !defined(THICKET_AUTOSEND_DEST)
#error "THICKET_AUTOSEND_INTERVAL_S set without THICKET_AUTOSEND_DEST=<32 hex chars>"
#endif
#if defined(THICKET_AUTOSEND_DEST) && !defined(THICKET_AUTOSEND_INTERVAL_S)
#error "THICKET_AUTOSEND_DEST set without THICKET_AUTOSEND_INTERVAL_S=<seconds>"
#endif

// -D on the command line delivers a bare token, not a string literal.
#define THICKET_STR_(x) #x
#define THICKET_STR(x) THICKET_STR_(x)

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

static microStore::FileSystem g_filesystem;
static RNS::Reticulum g_reticulum({RNS::Type::NONE});
static RNS::Interface g_lora_interface({RNS::Type::NONE});
static RNS::Identity g_identity({RNS::Type::NONE});

// sizeof(LXMF::LXMRouter) is 33,656 bytes. That is 14% of the 237,568 B SRAM
// region and it is not wanted in .bss on top of the TLSF pool, so it is built
// on the heap once the identity exists.
static std::unique_ptr<LXMF::LXMRouter> g_router;

// Observer pointer to the same object g_lora_interface owns. RNS::Interface
// wraps an InterfaceImpl* in a shared_ptr and exposes no way back down to the
// concrete type, and last_rssi()/last_snr() are LoRaInterface's, not
// InterfaceImpl's. Both are file-scope globals with program lifetime, so this
// never dangles; it must not be deleted here.
static LoRaInterface* g_lora = nullptr;

static bool g_stack_up = false;
static double g_last_announce = 0.0;

// Counts every message this device has composed since boot — replies and, if
// compiled in, timed sends. It is the first field of the body precisely so a
// missing number on the peer's screen means a dropped packet, unambiguously.
static uint32_t g_send_counter = 0;

// One pending reply, not a queue. If a second message arrives before the first
// reply is composed, the newer source wins and one reply goes out: this is a
// bring-up diagnostic, and answering a flood packet-for-packet on a shared
// 125 kHz channel is antisocial. The router's own outbound pool (16) is the
// backstop.
struct ReplyRequest {
	bool     armed = false;
	uint32_t due_ms = 0;
	RNS::Bytes to;
	bool     signal_valid = false;
	float    rssi = 0.0f;
	float    snr = 0.0f;
};
static ReplyRequest g_reply;

#ifdef THICKET_AUTOSEND_INTERVAL_S
static RNS::Bytes g_autosend_dest;
static bool   g_autosend_ok = false;
static double g_last_autosend = 0.0;
#endif

// ---------------------------------------------------------------------------
// Serial diagnostics
//
// Deliberately printf-free at the call sites and deliberately not RNS::Log:
// these lines have to survive a failure inside the RNS stack, including one
// that happens before RNS is initialised at all.
// ---------------------------------------------------------------------------

static void step(int n, const char* what) {
	Serial.print("[");
	Serial.print(n);
	Serial.print("/6] ");
	Serial.println(what);
}

static void ok(const char* what) {
	Serial.print("      ok   : ");
	Serial.println(what);
}

static void info(const char* key, const char* value) {
	Serial.print("      ");
	Serial.print(key);
	Serial.print(": ");
	Serial.println(value);
}

static void info_u32(const char* key, uint32_t value) {
	Serial.print("      ");
	Serial.print(key);
	Serial.print(": ");
	Serial.println(value);
}

// ---------------------------------------------------------------------------
// Silicon identity and debug-port protection state.
//
// Printed at boot because it cannot be inferred and needs no debugger to read.
//
// nRF52840 revision 3 (build codes Fx0) carries Nordic's improved hardware
// APPROTECT and SHIPS LOCKED FROM THE FACTORY. Earlier revisions ship open.
// UICR.APPROTECT at 0x10001208: 0x00 = Enabled, 0x5A = HwDisabled.
//   https://devzone.nordicsemi.com/nordic/nordic-blog/b/blog/posts/
//     working-with-the-nrf52-series-improved-approtect
//
// Firmware built against MDK >= 8.45.0 without ENABLE_APPROTECT reads that
// register at startup and writes APPROTECT.DISABLE, i.e. it unlocks the part
// on every boot so a developer can attach a debugger. Our BSP does NOT do
// this: APPROTECT appears only in the register headers of
// framework-arduinoadafruitnrf52, never in system_nrf52840.c. So whatever
// state the module arrives in is the state it stays in.
//
// Why we care: docs/open-questions.md Q9 considers holding an encryption
// "pepper" in the MCU's internal flash so that dumping the external SPI flash
// yields ciphertext and no key. That is only worth anything if the debug port
// cannot simply be used to read internal flash back out.
// ---------------------------------------------------------------------------
// Set by report_silicon_and_approtect(). False means the debug port is open,
// or open-by-default, on this particular chip.
//
// This exists to be GATED ON, not merely printed. Anything that stores a secret
// in internal flash (docs/open-questions.md Q9, the "pepper") must refuse when
// this is false: a secret on a chip whose SWD is open is worse than no secret,
// because it looks protected and is not.
static bool g_debug_port_protected = false;

static void report_silicon_and_approtect() {
	const uint32_t part    = NRF_FICR->INFO.PART;
	const uint32_t variant = NRF_FICR->INFO.VARIANT;   // ASCII, e.g. 'AAF0'
	const uint32_t approt  = NRF_UICR->APPROTECT;

	char var_s[5] = {
		(char)((variant >> 24) & 0xFF), (char)((variant >> 16) & 0xFF),
		(char)((variant >>  8) & 0xFF), (char)( variant        & 0xFF), 0
	};
	// Third character of the variant code is the revision letter; 'F' and
	// later indicate the hardware-APPROTECT parts on nRF52840.
	const bool hw_approtect_part = (var_s[2] >= 'F' && var_s[2] <= 'Z');

	char line[96];
	snprintf(line, sizeof(line), "part=0x%05lX variant=%s%s",
	         (unsigned long)part, var_s,
	         hw_approtect_part ? " (rev3+/Fx0, hardware APPROTECT)" : "");
	info("silicon", line);

	// ERASEALL over CTRL-AP is ALWAYS available and cannot be disabled on any
	// build code. An attacker with the device can therefore always wipe it to
	// factory state. That is destruction, not disclosure: it costs us the data
	// and gives them nothing. Stated here so nobody reads it later as a hole in
	// the scheme, and so nobody tries to "fix" it.
	const uint8_t approt_b = (uint8_t)(approt & 0xFF);
	const char* meaning;
	if (approt == 0xFFFFFFFFul)   meaning = "erased (0xFFFFFFFF)";
	else if (approt_b == 0x5A)    meaning = "HwDisabled - debug port OPEN";
	else if (approt_b == 0x00)    meaning = "Enabled - debug port LOCKED";
	else                          meaning = "unrecognised value";
	snprintf(line, sizeof(line), "0x%08lX  %s", (unsigned long)approt, meaning);
	info("UICR.APPROTECT", line);

	// The two build-code families default in OPPOSITE directions, so an erased
	// UICR means different things and neither is guessable from the hex value.
	// Product Specification, Debug and trace / access port protection:
	//   Dxx and earlier - hardware only, DISABLED by default
	//   Fxx and later   - hardware + software, ENABLED by default
	if (hw_approtect_part) {
		if (approt == 0xFFFFFFFFul || (approt & 0xFF) != 0x5A) {
			info("APPROTECT", "LOCKED. Fxx+ defaults enabled; opening it needs "
			                  "UICR=HwDisabled AND firmware writing "
			                  "APPROTECT.DISABLE. We never write it.");
		}
	}
	else {
		// The dangerous case, and the quiet one.
		if (approt == 0xFFFFFFFFul || (approt & 0xFF) == 0x5A) {
			info("APPROTECT", "*** DEBUG PORT OPEN *** Dxx-or-earlier part "
			                  "defaults DISABLED. Internal flash is readable "
			                  "over SWD with no attack required.");
		}
		else {
			info("APPROTECT", "enabled, but hardware-only on a Dxx-or-earlier "
			                  "part: defeated by the 2020 DEC1 voltage glitch.");
		}
	}

	// Only Fxx+ silicon with the software half never disabled is protected.
	// Hardware-only protection on an older part does not count: the 2020 DEC1
	// glitch defeats it for about $5.
	g_debug_port_protected = hw_approtect_part && (approt_b != 0x5A);

	// A STABLE, MACHINE-READABLE line. A flasher can reconnect after DFU, read
	// this, and tell the user in the browser what their particular chip is —
	// which matters because tasks/T16 invites anyone with a RAK4631 to flash
	// this, and their silicon is not ours. Serial prose warns only whoever is
	// already staring at a terminal.
	//
	// Format is contractual. Parsers depend on it; change it deliberately.
	char m[80];
	snprintf(m, sizeof(m), "THICKET-SEC v1 variant=%s approtect=%s", var_s,
	         g_debug_port_protected ? "protected" : "OPEN");
	Serial.println(m);
}

static void fail(const char* what) {
	Serial.print("      FAIL : ");
	Serial.println(what);
}

// ---------------------------------------------------------------------------
// Message composition
//
// Everything below runs on the device. The content is built here, the message
// is signed here with the private key on external flash, and microReticulum
// encrypts it to the recipient here. Nothing is templated by a host.
// ---------------------------------------------------------------------------

// Formats one line of diagnostics, small enough to fit a single LoRa packet and
// short enough to read off a T-Deck without scrolling. Shape:
//
//     #7 up812s r-96 s+8.2 id3f9ac114
//
// counter, uptime in seconds, RSSI in dBm, SNR in dB, and the first four bytes
// of this device's identity hash — the same hash the boot banner prints, so a
// peer can confirm which physical board answered without a cable.
//
// Deliberately no %f: the float formatter is a chunk of newlib we would be
// linking for one number, and fixed-point tenths from an integer is exact.
static size_t build_diag_body(char* out, size_t out_size,
                              bool signal_valid, float rssi, float snr) {
	char rssi_buf[8] = "?";
	char snr_buf[10] = "?";

	if (signal_valid) {
		// Clamped to what an SX1262 can physically report, so the body length
		// is bounded no matter what RadioLib hands back after a bad read.
		// -Wformat-truncation is right to care: an unbounded number here could
		// push the message past the single-packet budget.
		long r = lroundf(rssi);
		if (r < -200) { r = -200; } else if (r > 20) { r = 20; }
		snprintf(rssi_buf, sizeof(rssi_buf), "%ld", r);

		long tenths = lroundf(snr * 10.0f);
		if (tenths < -300) { tenths = -300; } else if (tenths > 300) { tenths = 300; }
		long mag = (tenths < 0) ? -tenths : tenths;
		snprintf(snr_buf, sizeof(snr_buf), "%c%ld.%ld",
		         (tenths < 0) ? '-' : '+', mag / 10, mag % 10);
	}

	// toHex() on a 32-byte hash is 64 chars; the first 8 are 4 bytes, which is
	// plenty to disambiguate one board on a bench and cheap on the wire.
	std::string id = g_identity.hash().toHex();
	if (id.size() > 8) { id.resize(8); }

	int n = snprintf(out, out_size, "#%lu up%lus r%s s%s id%s",
	                 (unsigned long)g_send_counter,
	                 (unsigned long)(millis() / 1000UL),
	                 rssi_buf, snr_buf, id.c_str());
	return (n < 0) ? 0 : (size_t)n;
}

// Compose, sign, encrypt and queue one LXMF message. No MessageStore is
// involved and none is needed: LXMRouter takes the message by reference, packs
// it, and copies it into its own fixed outbound pool. Persistence of sent
// messages is a separate feature, not a precondition for sending one.
static bool send_lxmf(const RNS::Bytes& destination_hash,
                      const char* body, const char* why) {
	if (!g_router) { return false; }

	// 16 bytes is RNS's truncated destination hash. A wrong length here means
	// the source hash or the build flag is malformed, and quietly transmitting
	// to a garbage address is worse than refusing.
	if (destination_hash.size() != 16) {
		Serial.print("SEND refused (");
		Serial.print(why);
		Serial.print("): destination hash is ");
		Serial.print((uint32_t)destination_hash.size());
		Serial.println(" bytes, expected 16");
		return false;
	}

	size_t len = strlen(body);

	Serial.println();
	Serial.println("--- LXMF message send ---");
	Serial.print("  reason : ");
	Serial.println(why);
	Serial.print("  to     : ");
	Serial.println(destination_hash.toHex().c_str());
	Serial.print("  body   : ");
	Serial.println(body);
	Serial.print("  length : ");
	Serial.print((uint32_t)len);
	Serial.print(" B (single-packet content budget ");
	Serial.print((uint32_t)REPLY_CONTENT_BUDGET);
	Serial.println(" B)");
	if (len > REPLY_CONTENT_BUDGET) {
		Serial.println("  note   : over budget, router will need a link (DIRECT), not one packet");
	}

	try {
		// The destination is passed as NONE and the hash set afterwards: we
		// know the peer's address but not necessarily its public key yet, and
		// RNS::Destination cannot be built without an Identity. The router
		// resolves the identity at send time via Identity::recall(), and asks
		// Transport for a path if it cannot.
		//
		// The source, by contrast, must be a real Destination — LXMessage::pack
		// signs with it and throws if it is absent. delivery_destination() is
		// ours, SINGLE, backed by the identity loaded from external flash.
		//
		// Heap, not stack, and not by preference: sizeof(LXMF::LXMessage) is
		// 800 bytes (measured at the pinned commit — 16 FieldEntry slots of two
		// RNS::Bytes each dominate it), and the Adafruit nRF52 core runs loop()
		// on a FreeRTOS task with LOOP_STACK_SZ = 256*4 words = 4,096 bytes
		// total. A 20%-of-stack local in front of pack(), which nests msgpack
		// and SHA-256 and Ed25519, is not a margin worth having. The router
		// copies the message into its own pool anyway, so this lives exactly as
		// long as the call.
		std::unique_ptr<LXMF::LXMessage> message(new LXMF::LXMessage(
			RNS::Destination(RNS::Type::NONE),
			g_router->delivery_destination(),
			RNS::Bytes((const uint8_t*)body, len),
			RNS::Bytes(),                          // no title — see REPLY_CONTENT_BUDGET
			LXMF::Type::Message::OPPORTUNISTIC
		));
		if (!message) {
			Serial.println("  FAILED : out of heap composing message");
			Serial.println("-------------------------");
			return false;
		}
		message->destination_hash(destination_hash);

		// OPPORTUNISTIC and not DIRECT on purpose. DIRECT establishes an RNS
		// link first, which is several round trips before any payload moves; on
		// a half-duplex 125 kHz channel that is the most fragile thing we could
		// ask of an unproven radio. One encrypted packet either arrives or does
		// not, and the answer is legible either way.
		g_router->handle_outbound(*message);
		++g_send_counter;

		Serial.println("  queued : yes (process_outbound will send it)");
		Serial.println("-------------------------");
		return true;

	} catch (const std::exception& e) {
		// pack() throws if the source destination cannot sign. Catching here
		// keeps a composition failure from unwinding through the main loop and
		// taking the radio down with it.
		Serial.print("  FAILED : ");
		Serial.println(e.what());
		Serial.println("-------------------------");
		return false;
	}
}

// ---------------------------------------------------------------------------
// Bring-up steps
// ---------------------------------------------------------------------------

// Step 1 — external SPI flash and microStore.
//
// This runs before the radio, not after, for two reasons. Reticulum's transport
// reads and writes persisted state from Reticulum::start() onward, so the
// filesystem has to be registered by then. And if this step is going to fail,
// it should fail while the radio is still cold.
//
// FlashFSFileSystem, never UniversalFileSystem: on nRF52 microStore aliases
// UniversalFileSystem to InternalFSFileSystem, which is the ~28 KB internal
// filesystem. That is too small for a message store, and worse, an nRF52840
// internal-flash erase takes ~85 ms with interrupts disabled, which times out
// RadioLib's SPI transactions to the SX1262. Persisting a path during an
// announce rebroadcast would break the radio, not just storage.
static bool bringup_storage() {
	step(1, "External SPI flash + microStore");
	info("device", "RAK15001 (GD25Q16C, 2 MiB) on SPI / IO slot");
	info_u32("cs pin", SS);

	g_filesystem = microStore::FileSystem{
		microStore::Adapters::FlashFSFileSystem(&FLASH_DEVICE)
	};

	if (!g_filesystem) {
		fail("could not construct FlashFSFileSystem");
		return false;
	}

	// init() formats on first boot if the LittleFS superblock is absent.
	if (!g_filesystem.init()) {
		fail("filesystem init failed - is the RAK15001 seated in the IO slot?");
		return false;
	}

	RNS::Utilities::OS::register_filesystem(g_filesystem);

	info_u32("storage size (B)", (uint32_t)g_filesystem.storageSize());
	info_u32("storage free (B)", (uint32_t)g_filesystem.storageAvailable());
	ok("external flash mounted, registered with RNS");
	return true;
}

// Step 2 — identity, created once and loaded forever after.
//
// This is the half of M1's done-condition that a demo cannot fake: the hash the
// founder's T-Deck sees must be the same hash after a power cycle. First boot
// generates and writes; every later boot reads.
static bool bringup_identity() {
	step(2, "Identity (create or load)");

	if (RNS::Utilities::OS::file_exists(IDENTITY_PATH)) {
		g_identity = RNS::Identity::from_file(IDENTITY_PATH);
		if (g_identity) {
			info("source", "loaded from external flash");
			ok("identity restored across reboot");
		} else {
			// The file is there and unreadable. Do not silently mint a new
			// identity over the top: that would change the device's address
			// without anyone noticing, which is exactly the failure a user
			// cannot diagnose. Stop and say so.
			fail("identity file present but unreadable - refusing to overwrite");
			info("path", IDENTITY_PATH);
			return false;
		}
	} else {
		g_identity = RNS::Identity();  // generates Ed25519 + X25519 keypairs
		if (!g_identity) {
			fail("key generation failed");
			return false;
		}
		if (!g_identity.to_file(IDENTITY_PATH)) {
			fail("could not persist identity to external flash");
			return false;
		}
		info("source", "generated and written (first boot)");
		ok("identity created and persisted");
	}

	info("identity hash", g_identity.hash().toHex().c_str());
	return true;
}

// Step 3 — SX1262 through the vendored LoRaInterface, 915 MHz US.
//
// Register before start: Transport has to know the interface exists before it
// can be handed a frame, and start() is what puts the radio into continuous
// receive.
static bool bringup_radio() {
	step(3, "SX1262 radio (LoRaInterface)");
	info("band", "914.875 MHz, BW 125 kHz, SF8, CR4:5, +17 dBm");
	info("spi", "SPI1 (radio) - SPI stays on the flash bus");

	// Keep a typed handle before the Interface wrapper swallows it — see
	// g_lora. RNS::Interface takes ownership via shared_ptr; this is an
	// observer, never deleted here.
	g_lora = new LoRaInterface();
	g_lora_interface = g_lora;

	// MODE_GATEWAY, matching upstream's own example: this interface is a
	// full participant, not a leaf or an access point.
	// TODO(M1): revisit against A26's always-listening idle contract once
	// there is a power measurement to argue with.
	g_lora_interface.mode(RNS::Type::Interface::MODE_GATEWAY);
	RNS::Transport::register_interface(g_lora_interface);

	if (!g_lora_interface.start()) {
		fail("radio did not start");
		info("check", "TCXO voltage (THICKET_SX1262_TCXO_VOLTAGE), SPI1 wiring, antenna");
		return false;
	}

	ok("SX1262 online, continuous receive");
	return true;
}

// Step 4 — Reticulum.
static bool bringup_reticulum() {
	step(4, "Reticulum");

	g_reticulum = RNS::Reticulum();

	// A handheld is a leaf, not a router: it must not rebroadcast other
	// people's traffic on battery. Relay (a separate product) is what carries
	// transport_enabled(true).
	//
	// ⚠ This line is load-bearing in a way it does not look. Upstream
	// microReticulum initialises its path table, known-destinations and packet
	// hashlist stores ONLY inside `if (Reticulum::transport_enabled())`
	// (Transport.cpp:380). Against stock upstream, this correct setting makes
	// Identity::remember() fail on every inbound announce, so the device
	// announces, looks healthy, and can never address a peer — all four of
	// upstream's own test_interop scenarios fail on it.
	//
	// We build against our fork's `thicket/integration`, which moves those store
	// initialisations out of the gate. Do NOT re-point lib_deps at upstream
	// without carrying that fix, and do not "simplify" this to
	// transport_enabled(true) to make symptoms go away — that would turn a
	// battery-powered handheld into a router. See tasks/T19.
	g_reticulum.transport_enabled(false);
	g_reticulum.probe_destination_enabled(true);
	g_reticulum.start();

	if (!g_reticulum) {
		fail("Reticulum did not start");
		return false;
	}

	ok("transport running (leaf mode, not routing)");
	return true;
}

// Step 5 — the LXMF messenger layer.
//
// Constructing the router is what creates and registers the
// <identity>/lxmf/delivery destination, which is the address a Sideband peer
// actually sends to. It is linked and live from here on: inbound messages will
// reach the delivery callback below. Outbound composition is not wired.
static bool bringup_lxmf() {
	step(5, "LXMF router (microLXMF)");

	g_router.reset(new LXMF::LXMRouter(g_identity, LXMF_STORAGE_PATH, false));
	if (!g_router) {
		fail("router allocation failed - out of heap");
		return false;
	}

	g_router->set_display_name(DISPLAY_NAME);

	g_router->register_delivery_callback([](LXMF::LXMessage& message) {
		// Link quality of the last LoRa frame the interface saw. This is a
		// close approximation of "how well did THIS message arrive", not a
		// guarantee: the delivery callback fires from process_inbound(), one
		// or more main-loop iterations after the frame was read, and a split
		// payload reports its second half. Good enough to tell a peer across
		// the room from a peer across a valley; not a calibrated measurement.
		bool  sig  = (g_lora != nullptr) && g_lora->signal_valid();
		float rssi = sig ? g_lora->last_rssi() : 0.0f;
		float snr  = sig ? g_lora->last_snr()  : 0.0f;

		// The tethered case. Untethered is the proof, but when there IS a
		// terminal attached this is the whole receive path in one block.
		Serial.println();
		Serial.println("--- LXMF message received ---");
		Serial.print("  from   : ");
		Serial.println(message.source_hash().toHex().c_str());
		Serial.print("  length : ");
		Serial.print((uint32_t)message.content().size());
		Serial.println(" B content");
		Serial.print("  rssi   : ");
		if (sig) { Serial.print(rssi); Serial.println(" dBm"); } else { Serial.println("unavailable"); }
		Serial.print("  snr    : ");
		if (sig) { Serial.print(snr);  Serial.println(" dB");  } else { Serial.println("unavailable"); }
		Serial.print("  signed : ");
		Serial.println(message.signature_validated() ? "yes" : "NO - source identity not known yet");
		Serial.print("  title  : ");
		Serial.println(message.title().toString().c_str());
		Serial.print("  content: ");
		Serial.println(message.content().toString().c_str());
		Serial.println("-----------------------------");

		// Arm the reply. Deliberately does NOT send from inside this callback:
		// we are being called from LXMRouter::process_inbound(), inside its
		// try block, with the inbound queue head still live. handle_outbound()
		// packs and can throw, and a throw here would be caught by the
		// router's own handler and reported as a receive failure. Latch the
		// address, send from loop() where the failure is legible.
		if (message.source_hash().size() != 16) {
			Serial.println("REPLY skipped: source hash malformed");
			return;
		}
		if (message.source_hash() == g_router->delivery_destination().hash()) {
			// Our own message, arriving back at us. Replying would loop.
			Serial.println("REPLY skipped: source is this device");
			return;
		}

		g_reply.armed        = true;
		g_reply.due_ms       = millis() + REPLY_DELAY_MS;
		g_reply.to           = message.source_hash();
		g_reply.signal_valid = sig;
		g_reply.rssi         = rssi;
		g_reply.snr          = snr;

		Serial.print("REPLY armed for ");
		Serial.print(g_reply.to.toHex().c_str());
		Serial.print(" in ");
		Serial.print(REPLY_DELAY_MS);
		Serial.println(" ms");

		// TODO(M1): this is still the receive path's only consumer beyond the
		// reply. Nothing is retained. It needs a MessageStore (retuned to 8x32)
		// behind it and, later, the UI and the A9 alert ladder.
	});

	g_router->register_sent_callback([](LXMF::LXMessage& message) {
		Serial.print("LXMF: sent ");
		Serial.println(message.hash().toHex().c_str());
	});

	g_router->register_delivered_callback([](LXMF::LXMessage& message) {
		// A proof came back: the peer decrypted it and acknowledged. This is
		// the strongest signal the untethered loop actually closed, and it is
		// the one line worth watching for on a terminal.
		Serial.print("LXMF: DELIVERED (proof received) ");
		Serial.println(message.hash().toHex().c_str());
	});

	g_router->register_failed_callback([](LXMF::LXMessage& message) {
		Serial.print("LXMF: delivery failed for ");
		Serial.println(message.hash().toHex().c_str());
		Serial.println("      (no path to peer, or peer never announced - check both announce)");
	});

	// TODO(M1): no MessageStore is attached, and sending does not need one.
	// Verified against microLXMF at the pinned commit: LXMRouter never
	// references MessageStore - it is application-owned - and handle_outbound()
	// packs the message and copies it into the router's own fixed
	// _pending_outbound_pool (16 slots). Construct, send, discard is the whole
	// lifecycle; persistence of sent and received messages is a separate
	// feature, not a precondition for either direction.
	//
	// When we do attach one, its pool constants must be overridden first: they
	// are `static constexpr` in MessageStore.h with no #ifndef guard, and at
	// upstream's 32x256 the object is 266,896 bytes and the link fails
	// outright. Guarding them is a six-line upstream patch and should be our
	// first contribution to microLXMF.

	// TODO(M1): no stamps. Sideband ships lxmf_require_stamps = False and
	// announces a nil inbound stamp cost, Python LXMF skips the whole stamp
	// block when the cost is None, and microLXMF hard-codes packNil() for its
	// own announced cost, so nothing on either side allocates LXStamper's
	// 750 KiB workblock at M1's done-condition. If a peer ever demands a stamp
	// this device cannot pay it; the fix is the streaming SHA-256 rewrite, not
	// more heap.

	info("delivery dest", g_router->delivery_destination().hash().toHex().c_str());
	ok("LXMF router live, delivery destination registered");
	return true;
}

// Step 6 — announce.
static void announce(const char* why) {
	if (!g_router) { return; }
	Serial.print("ANNOUNCE (");
	Serial.print(why);
	Serial.println(")");
	g_router->announce();
	g_last_announce = RNS::Utilities::OS::time();
}

// Timed send, compiled out entirely unless both flags are given.
//
// Parsed and validated at boot rather than at first send, because a typo in a
// -D reaching the radio as a mangled address is the kind of failure that looks
// like broken RF. Bytes::assignHex() does not validate: it maps any character
// through arithmetic and produces garbage silently, so the check is ours.
#ifdef THICKET_AUTOSEND_INTERVAL_S
static void bringup_autosend() {
	const char* hex = THICKET_STR(THICKET_AUTOSEND_DEST);
	size_t n = strlen(hex);

	Serial.println();
	Serial.println("TIMED SEND is compiled in (this is not the default build)");
	info("dest hex", hex);
	info_u32("interval (s)", (uint32_t)(THICKET_AUTOSEND_INTERVAL_S));

	if (n != 32) {
		fail("THICKET_AUTOSEND_DEST must be exactly 32 hex characters (16-byte dest hash)");
		info_u32("got length", (uint32_t)n);
		return;
	}
	for (size_t i = 0; i < n; ++i) {
		char c = hex[i];
		bool is_hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
		if (!is_hex) {
			fail("THICKET_AUTOSEND_DEST contains a non-hex character");
			return;
		}
	}

	g_autosend_dest.assignHex(hex);
	if (g_autosend_dest.size() != 16) {
		fail("THICKET_AUTOSEND_DEST did not decode to 16 bytes");
		return;
	}

	g_autosend_ok = true;
	g_last_autosend = RNS::Utilities::OS::time();
	ok("timed send armed");
}
#endif

// ---------------------------------------------------------------------------

static void print_addresses() {
	Serial.println();
	Serial.println("=========================================================");
	Serial.println(" THICKET IS ON AIR");
	Serial.println();
	Serial.println(" Compare these against what your Sideband peer sees.");
	Serial.print("   identity hash        : ");
	Serial.println(g_identity.hash().toHex().c_str());
	Serial.print("   lxmf.delivery dest   : ");
	Serial.println(g_router->delivery_destination().hash().toHex().c_str());
	Serial.print("   display name         : ");
	Serial.println(DISPLAY_NAME);
	Serial.println();
	Serial.println(" The identity hash must be identical after a power cycle.");
	Serial.println(" If it is not, external-flash persistence is broken and");
	Serial.println(" nothing else in this build can be trusted.");
	Serial.println();
	Serial.println(" Send a message to the lxmf.delivery address above and this");
	Serial.println(" device will answer it. The reply is composed, signed and");
	Serial.println(" encrypted here; the address it goes to is learned from the");
	Serial.println(" message, never compiled in. Nothing needs to be attached.");
	Serial.println("=========================================================");
	Serial.println();
}

void setup() {
	Serial.begin(115200);

	// Wait for a terminal, but not forever: this firmware has to boot on
	// battery with nothing attached. Five seconds is enough to catch the
	// banner when a laptop is plugged in.
	uint32_t waited = 0;
	while (!Serial && waited < 5000) { delay(50); waited += 50; }

	pinMode(LED_GREEN, OUTPUT);
	digitalWrite(LED_GREEN, LOW);

	Serial.println();
	Serial.println("=========================================================");
	Serial.println(" Thicket firmware - M1 skeleton");
	Serial.println(" RAK4631 (nRF52840 + SX1262), 915 MHz US");
	Serial.println("=========================================================");
	info_u32("RNS heap pool (B)", (uint32_t)RNS_HEAP_POOL_BUFFER_SIZE);
	report_silicon_and_approtect();
#ifdef THICKET_NO_BLE
	info("build", "no-BLE (SoftDevice dropped, 966,656 B app region)");
#else
	info("build", "SoftDevice S140 (815,104 B app region)");
#endif
	Serial.println();

	// INFO is the right default here: TRACE is what upstream's examples use and
	// it is both enormous and slow enough to change LoRa timing.
	// TODO(M1): make this a build flag once bring-up is done.
	RNS::loglevel(RNS::LOG_INFO);

	// Each step reports its own failure and we stop at the first one. A stack
	// that is half up is worse than one that is plainly down, because it looks
	// like a radio problem.
	if (!bringup_storage())   { return; }
	if (!bringup_identity())  { return; }
	if (!bringup_radio())     { return; }
	if (!bringup_reticulum()) { return; }
	if (!bringup_lxmf())      { return; }

	step(6, "Announce");
	announce("boot");

#ifdef THICKET_AUTOSEND_INTERVAL_S
	bringup_autosend();
#endif

	print_addresses();
	g_stack_up = true;
	digitalWrite(LED_GREEN, HIGH);
}

void loop() {
	if (!g_stack_up) {
		// Bring-up failed. Blink fast and keep the serial port alive so the
		// failure above stays readable instead of scrolling past a reboot loop.
		static uint32_t last = 0;
		static bool on = false;
		if (millis() - last >= 120) {
			last = millis();
			on = !on;
			digitalWrite(LED_GREEN, on ? HIGH : LOW);
		}
		return;
	}

	// Drives Transport jobs and, through it, LoRaInterface::loop(), which polls
	// the SX1262's RX_DONE IRQ. There is no ISR: receive latency is bounded by
	// how often this is called, so nothing in this loop may block.
	g_reticulum.loop();

	g_router->process_outbound();
	g_router->process_inbound();

	if ((RNS::Utilities::OS::time() - g_last_announce) > ANNOUNCE_INTERVAL_S) {
		announce("timer");
	}

	// Auto-reply. The address came off the wire, so this closes the loop with
	// nothing compiled in and nothing attached.
	//
	// Note what this sidesteps: sending to a peer we have NOT heard from still
	// needs a way to pick a destination, and there is no way to enumerate known
	// destinations - upstream is mid-migration to a microStore-backed path
	// table with no enumeration API, so path_table() returns empty. Answering
	// is possible because the source hash arrives with the message. Initiating
	// remains M2's first wall.
	if (g_reply.armed && (int32_t)(millis() - g_reply.due_ms) >= 0) {
		g_reply.armed = false;

		char body[BODY_BUF];
		build_diag_body(body, sizeof(body),
		                g_reply.signal_valid, g_reply.rssi, g_reply.snr);
		send_lxmf(g_reply.to, body, "auto-reply");
	}

#ifdef THICKET_AUTOSEND_INTERVAL_S
	// Timed send. Only reachable when both build flags were given AND the
	// destination parsed; a stock build never compiles this block at all.
	if (g_autosend_ok &&
	    (RNS::Utilities::OS::time() - g_last_autosend) > (double)(THICKET_AUTOSEND_INTERVAL_S)) {
		g_last_autosend = RNS::Utilities::OS::time();

		// No inbound frame to attribute the signal figures to, so report the
		// interface's most recent frame if it has ever seen one. On a silent
		// channel these read "?" and that is the honest answer.
		bool  sig  = (g_lora != nullptr) && g_lora->signal_valid();
		float rssi = sig ? g_lora->last_rssi() : 0.0f;
		float snr  = sig ? g_lora->last_snr()  : 0.0f;

		char body[BODY_BUF];
		build_diag_body(body, sizeof(body), sig, rssi, snr);
		send_lxmf(g_autosend_dest, body, "timed send");
	}
#endif

	// TODO(M1): no sleep. A26's always-listening contract does not mean a busy
	// loop; it means the receiver stays powered while the CPU idles. This spins
	// the M4 at 64 MHz and will not meet any battery target.
}

// No _write() retarget here on purpose. microReticulum's log macros go through
// printf, and upstream's own examples define _write to push that at Serial —
// but the Adafruit nRF52 core already retargets printf to Serial (CDC) in
// cores/nRF5/main.cpp under CFG_LOGGER 0, which is the default. Defining our
// own is a duplicate-symbol link error, not an improvement.
