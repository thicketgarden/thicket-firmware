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
// Thicket — bring-up skeleton.
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
// What is NOT here yet, and is marked TODO(bring-up) at each site: a message store,
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

#ifdef THICKET_RAM_PROBE
#include <malloc.h>
#endif
#include <microStore/FileSystem.h>
#ifdef THICKET_PATH_INDEX_PROBE
#include <unordered_map>
#include <vector>
#endif
#include <microStore/Adapters/FlashFSFileSystem.h>
#ifdef THICKET_NO_PERSISTENCE
#ifdef THICKET_INTERNAL_FS
#include <microStore/Adapters/InternalFSFileSystem.h>
#else
#include <microStore/Adapters/NoopFileSystem.h>
#endif
#endif

#include <microReticulum.h>

#include <LoRaInterface.h>
#include <LXMF/LXMRouter.h>
#include <LXMF/LXMessage.h>
// Attached only where a filesystem actually exists. The no-flash bring-up env
// mounts a no-op filesystem whose writes are accepted and discarded, so a store
// there would fail every save and teach us nothing.
#if !defined(THICKET_NO_PERSISTENCE) || defined(THICKET_INTERNAL_FS)
#define THICKET_HAVE_STORE 1
#include <LXMF/MessageStore.h>
// Encryption at rest for everything the store persists. Included with the
// store, not separately: there is no build in which we want one without the
// other.
#include <encrypted_store.h>
#endif
#ifdef THICKET_RAM_PROBE
#include <LXMF/MessageStore.h>
#endif

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
// MessageStore is attached.
//
// ⚠ The old note here said upstream's 32 x 256 pool "will not link on this
// part" and that was the stated reason for having no store. That reason is
// gone: MessageStore.h now takes LXMF_MAX_CONVERSATIONS and
// LXMF_MAX_MESSAGES_PER_CONVERSATION from -D, and platformio.ini sets 8 x 32.
//
// Measured, by instantiating one and building:
//   8 x 32   RAM 36,304 B, flash 448,660 B   links
//   32 x 256                                 ld returns 1
// So attaching a store costs +10,656 B of RAM and +12,400 B of flash.
//
// It is still not attached, and now for a different and better reason: the
// first boot on real hardware should have as few moving parts as possible, so
// that when something fails the failure is unambiguous. Attach it once the
// board has come up clean once. Doing so also turns the storage questions
// (retention, flash cost per message, LittleFS block granularity) from things
// to research into things to measure.
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
// with a T-Deck. TODO(bring-up): drive announces from user action plus a much longer
// timer once there is a UI.
static const double ANNOUNCE_INTERVAL_S = 120.0;

// Auto-reply. The device answers anything delivered to it, addressed to the
// source hash carried by the inbound message — nothing about the peer is
// compiled in, so no reflash is needed when the far end's address changes or a
// second peer appears. This is the bring-up proof: it needs no screen here and no
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
#ifdef THICKET_HAVE_STORE
// Application-owned: LXMRouter never references a MessageStore, so saving is
// ours to do at the two points where a message exists.
static LXMF::MessageStore* g_store = nullptr;  // points at static storage; see bring-up
static uint32_t g_saved_in = 0;
static uint32_t g_saved_out = 0;
static uint32_t g_save_failures = 0;

// Report what the store holds. Counts rather than contents: this is a
// bring-up console, and the interesting question at this stage is whether
// persistence is happening at all, not what was said.
static void report_store(const char* when) {
	if (!g_store) return;
	auto convs = g_store->get_conversations();
	Serial.print("  store  : ");
	Serial.print(when);
	Serial.print(" - conversations=");
	Serial.print((uint32_t)convs.size());
	Serial.print(" saved_in=");
	Serial.print(g_saved_in);
	Serial.print(" saved_out=");
	Serial.print(g_saved_out);
	if (g_save_failures) {
		Serial.print(" FAILURES=");
		Serial.print(g_save_failures);
	}
	Serial.println();
}

static void store_message(const LXMF::LXMessage& m, bool inbound) {
	if (!g_store) return;
	if (g_store->save_message(m)) {
		if (inbound) g_saved_in++; else g_saved_out++;
	}
	else {
		// Loud, because a store that silently fails to save is
		// indistinguishable from one that is working until someone looks for
		// a message that is not there.
		g_save_failures++;
		Serial.print("STORE: save FAILED for ");
		Serial.println(m.hash().toHex().c_str());
	}
}
#endif

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

// Loud on purpose. Everything this prints is a reason not to trust the run
// that follows it.
static void warn(const char* what) {
	Serial.print("      WARN : ");
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
// Why we care: we are considering holding an encryption
// "pepper" in the MCU's internal flash so that dumping the external SPI flash
// yields ciphertext and no key. That is only worth anything if the debug port
// cannot simply be used to read internal flash back out.
// ---------------------------------------------------------------------------
// Set by report_silicon_and_approtect(). False means the debug port is open,
// or open-by-default, on this particular chip.
//
// This exists to be GATED ON, not merely printed. Anything that stores a secret
// in internal flash (the "pepper" described above) must refuse when
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
	// which matters because we intend to invite anyone with a RAK4631 to flash
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
#ifdef THICKET_HAVE_STORE
		// Save after handle_outbound, not before: the router packs the message
		// and that is when its hash is final. Saving earlier would store a
		// record keyed on a hash the peer never sees.
		store_message(*message, false);
		report_store("after outbound");
#endif
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
// filesystem. That is far too small for a message store, and capacity alone
// settles the choice.
//
// A second argument has been made upstream that an internal-flash erase masks
// interrupts long enough to time out RadioLib's SPI to the SX1262. We have not
// measured that and it is not our figure. Treat it as unverified; the capacity
// argument does not depend on it.
static bool bringup_storage() {
	step(1, "External SPI flash + microStore");

#ifdef THICKET_NO_PERSISTENCE
	// Bring-up crutch. No external flash is mounted, so nothing survives a
	// reboot: the identity is regenerated every boot and the device's address
	// changes with it. This exists to reach the radio on a board with no
	// RAK15001 in the IO slot, and for nothing else.
	//
	// It cannot prove the half of the done-condition that matters, which is
	// that the hash a peer sees is the same after a power cycle. A green run
	// under this flag is evidence about the radio and evidence about nothing
	// else.
	warn("PERSISTENCE DISABLED at build time (THICKET_NO_PERSISTENCE)");
	info("effect", "identity is regenerated every boot; address changes");
	info("proves", "radio only, never persistence");

	// Reticulum needs somewhere to put its path table, known destinations and
	// packet hashlist. Registering nothing threw std::runtime_error out of
	// Reticulum(); registering a no-op filesystem got past that but made
	// Identity::remember() fail, because remember writes and then reads back.
	// A store that forgets produces the same symptom as a store that was never
	// initialised: the node announces, looks healthy, and can never reach a peer.
#ifdef THICKET_INTERNAL_FS
	// ⚠ The internal filesystem is far too small for a message store, which is
	// why nothing ships on it. There is also an unverified claim that its erases
	// stall the radio -- an interrupt hazard we have recorded but not measured.
	// This env exists only to reach a round trip without a RAK15001.
	g_filesystem = microStore::FileSystem{ microStore::Adapters::InternalFSFileSystem() };
	if (!g_filesystem) {
		fail("could not construct InternalFSFileSystem");
		return false;
	}
	// A full-but-valid filesystem never trips init()'s reformat-on-fail, so
	// there is no way back from one except this. Build with
	// -DTHICKET_FORMAT_FS once, boot, then build without it.
	//
	// Earned the hard way on 2026-08-05: the ~28 KB internal filesystem filled
	// up, RNS began clearing its own path and known-destination stores at every
	// boot just to run, and eventually the erase-and-clear during init stopped
	// completing and the device reset there in a loop. Recovering it needed a
	// double-tap into the bootloader and a build that mounts nothing.
#ifdef THICKET_FORMAT_FS
	warn("FORMATTING the internal filesystem - every stored thing is gone");
	if (!g_filesystem.format()) {
		fail("format failed");
		return false;
	}
	ok("internal filesystem formatted");
#endif
	if (!g_filesystem.init()) {
		fail("internal filesystem init failed");
		return false;
	}
	warn("using INTERNAL flash (~28 KB) - erases can stall the radio");
	info("filesystem", "internal (small, and on the interrupt-blocking path)");
#else
	g_filesystem = microStore::FileSystem{ microStore::Adapters::NoopFileSystem() };
	if (!g_filesystem) {
		fail("could not construct NoopFileSystem");
		return false;
	}
	warn("no-op filesystem: remember() will fail and no path will be stored");
	info("filesystem", "no-op (writes accepted and discarded)");
#endif
	RNS::Utilities::OS::register_filesystem(g_filesystem);
	return true;
#else
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
#endif
}

// Step 2 — identity, created once and loaded forever after.
//
// This is the half of the bring-up done-condition that a demo cannot fake: the
// hash the paired T-Deck sees must be the same after a power cycle. First boot
// generates and writes; every later boot reads.
static bool bringup_identity() {
	step(2, "Identity (create or load)");

#ifdef THICKET_NO_PERSISTENCE
	// No filesystem is registered, so file_exists would be answering about a
	// store that is not there. Generate and keep it in RAM.
	g_identity = RNS::Identity();
	if (!g_identity) {
		fail("key generation failed");
		return false;
	}
	warn("identity is EPHEMERAL - this address dies at the next reset");
	info("identity hash", g_identity.hash().toHex().c_str());
	return true;
#else
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
#endif
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
	// TODO(bring-up): revisit against the always-listening idle contract once
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

	// A handheld runs with transport disabled: it must not rebroadcast other
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
	// battery-powered handheld into a router.
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
#ifdef THICKET_RAM_PROBE
	// Straddle the router allocation. Whether its 34 KB lands in the TLSF pool
	// or in the system heap decides which budget the message store competes
	// with later, and the two have very different amounts left.
	info_u32("pool free BEFORE router (B)",
	         (uint32_t)RNS::Utilities::Memory::heap_pool_free());
	info_u32("heap in use BEFORE router (B)", (uint32_t)mallinfo().uordblks);
#endif

	g_router.reset(new LXMF::LXMRouter(g_identity, LXMF_STORAGE_PATH, false));
	if (!g_router) {
		fail("router allocation failed - out of heap");
		return false;
	}

	g_router->set_display_name(DISPLAY_NAME);

#ifdef THICKET_HAVE_STORE
	// The store is independent of the router: LXMRouter never references one.
	// Constructed here rather than earlier so a failure reports against the
	// messaging step, which is what it belongs to.
#ifdef THICKET_RAM_PROBE
	// Straddle the allocation, because the obvious reasoning about it is wrong.
	// The store's index is a fixed array member rather than a heap container,
	// which suggests it costs system heap and not the RNS pool. That is true of
	// the INDEX. It is not true of the OBJECT: anything new'd goes wherever
	// RNS_DEFAULT_ALLOCATOR points, which is the pool.
	const uint32_t pool_before = (uint32_t)RNS::Utilities::Memory::heap_pool_free();
	const uint32_t heap_before = (uint32_t)mallinfo().uordblks;
#endif
	// ⚠ NOT `new`. RNS_DEFAULT_ALLOCATOR redirects global operator new into
	// the RNS pool, so a new'd store costs 37,388 B of the 98,304 B pool --
	// measured, and it took the pool from 49% to 88% full. A function-local
	// static is constructed on first use and lives in .bss instead, which
	// comes out of the same SRAM but NOT out of the pool, leaving the pool for
	// the per-packet working memory it exists to serve.
	//
	// Static storage is what makes "the store does not cost pool" true. It was
	// assumed to be true before it was measured, and it was not.
	static LXMF::MessageStore store_storage(LXMF_STORAGE_PATH);
	g_store = &store_storage;
#ifdef THICKET_RAM_PROBE
	info_u32("store cost from RNS pool (B)",
	         pool_before - (uint32_t)RNS::Utilities::Memory::heap_pool_free());
	info_u32("store cost from system heap (B)",
	         (uint32_t)mallinfo().uordblks - heap_before);
#endif
	if (!g_store) {
		fail("could not construct MessageStore");
		return false;
	}

	// Encrypt everything the store persists, keyed from this device's own
	// identity. The store holds no key and knows no cipher: it calls these two
	// and reports decoded sizes, so its write-then-verify-readback checks still
	// compare like with like.
	//
	// Captured by value on purpose. g_identity is reassigned during bring-up
	// and a reference would be left pointing at a stale object; the codec must
	// hold the identity these messages are actually keyed to.
	{
		const RNS::Identity id = g_identity;
		g_store->set_codec(
			[id](const RNS::Bytes& in, RNS::Bytes& out) {
				return encstore_encrypt(id, in.data(), in.size(), out);
			},
			[id](const RNS::Bytes& in, RNS::Bytes& out) {
				return encstore_decrypt(id, in.data(), in.size(), out);
			});
	}
	if (!g_store->has_codec()) {
		fail("message store is not encrypted - refusing to store in the clear");
		return false;
	}
	// Worth stating plainly, because the guarantee is narrower than
	// "encrypted at rest" sounds. The identity itself is a plaintext file on
	// the same flash, so whoever images the whole device reads it and derives
	// these keys from it. What this buys is a message store that is useless on
	// its own, and crypto-erase: destroying the identity makes every stored
	// message unreadable at once, with no wipe.
	ok("message store encrypted (AES-256-CTR + HMAC, keyed from identity)");
	info("store at-rest limit", "identity is stored in the clear; erasing it "
	                            "renders all stored messages unreadable");
	info_u32("store per-message overhead (B)", (uint32_t)ENCSTORE_FILE_OVERHEAD);

	info_u32("store capacity (conversations)",
	         (uint32_t)LXMF::MAX_CONVERSATIONS);
	info_u32("store capacity (msgs/conversation)",
	         (uint32_t)LXMF::MAX_MESSAGES_PER_CONVERSATION);
	info_u32("store hot tier (msgs/conversation)",
	         (uint32_t)LXMF::HOT_MESSAGES_PER_CONVERSATION);
	// No archive filesystem is set, so the cull deletes rather than moves. That
	// is deliberate: the device is allowed to forget silently rather than carry
	// a patch to announce it. An archive tier would need a second filesystem
	// this board does not have.
	info("store archive", g_store->has_archive() ? "yes" : "none - cull deletes (ruled)");

	// Prove the store can actually store, here, on this filesystem, before
	// claiming it is attached.
	//
	// This exists because the device spent an afternoon reporting "message
	// store attached" while every save failed. The internal-flash bring-up
	// filesystem had 128 bytes free -- RNS was already clearing its own path
	// and known-destination stores just to keep running -- and nothing on the
	// happy path looked. Three inbound messages were answered correctly and
	// none was kept; the only symptom was a FAILURES counter nobody read.
	//
	// Deliberately NOT routed through LXMessage. Packing needs a real
	// destination and can fail for reasons that have nothing to do with
	// storage, which would make this probe report the wrong thing. What can
	// fail silently is narrower and is exactly what is checked: the codec on
	// this silicon, and whether this filesystem can hold one message-sized
	// file. Static free-space arithmetic would not catch a codec bug, and a
	// host codec test would not catch a full disk.
	{
		// Sized like a real stored message: the JSON carries the packed
		// message as hex, so a short "hi" still lands in the hundreds of bytes.
		uint8_t probe[512];
		for (size_t i = 0; i < sizeof(probe); ++i) {
			probe[i] = (uint8_t)(i * 31 + 7);
		}
		// Filesystem root, not the store's directory. MessageStore derives its
		// own layout internally -- the real failure earlier named `/m/...`, not
		// the base path this firmware passes in -- so writing "inside" it means
		// guessing at a directory that may not exist, and a probe that fails
		// because it guessed wrong is worse than no probe. Root always exists,
		// and the two things being tested (can this filesystem hold a
		// message-sized file, does the codec survive this silicon) do not care
		// which directory they happen in.
		const char* probe_path = "/.thicket_selftest";
		const char* why = nullptr;

		RNS::Bytes sealed;
		RNS::Bytes read_back;
		RNS::Bytes opened;

		if (!encstore_encrypt(g_identity, probe, sizeof(probe), sealed)) {
			why = "codec could not encrypt - out of memory?";
		}
		else if (RNS::Utilities::OS::write_file(probe_path, sealed) != sealed.size()) {
			// Deliberately does NOT say "full". The first version of this
			// asserted a full disk and printed it next to 26 KB free, because
			// the probe was writing to a directory that did not exist. Report
			// what is known -- the write did not complete -- and print the
			// free space beside it so the reader draws their own conclusion.
			why = "could not write a message-sized file (see free space below)";
		}
		else if (RNS::Utilities::OS::read_file(probe_path, read_back) != sealed.size()) {
			why = "wrote the file but could not read it back";
		}
		else if (!encstore_decrypt(g_identity, read_back.data(),
		                           read_back.size(), opened)) {
			// The failure encryption introduces, and it is silent until a
			// reload: everything looks healthy right up to the first read.
			why = "stored file failed authentication - decode path is broken";
		}
		else if (opened.size() != sizeof(probe) ||
		         memcmp(opened.data(), probe, sizeof(probe)) != 0) {
			why = "round trip returned different bytes - codec is not lossless";
		}
		RNS::Utilities::OS::remove_file(probe_path);

		if (why != nullptr) {
			info_u32("filesystem free (B)",
			         (uint32_t)g_filesystem.storageAvailable());
			info_u32("one sealed message needs (B)", (uint32_t)(512 + ENCSTORE_FILE_OVERHEAD));
			fail(why);
			fail("message store cannot store - refusing to report it attached");
			return false;
		}
		ok("store round trip verified on this filesystem (encrypt, write, read, decrypt)");
	}
	report_store("at startup");
	ok("message store attached");
#else
	info("store", "not attached - this env has no real filesystem");
#endif

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
#ifdef THICKET_HAVE_STORE
		// Save what arrived. Done here rather than deeper in the router
		// because the router does not own a store -- this is the only place
		// an inbound message exists as an object we can persist.
		store_message(message, true);
		report_store("after inbound");
#endif
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

		// Someone we are actually corresponding with. Pin their path so an
		// announce flood from strangers cannot evict it: the path table is
		// bounded, and eviction is by age, which on a leaf node is a poor
		// proxy for importance - a correspondent that announces rarely is
		// more evictable than a stranger that announces constantly.
		//
		// Pinning is soft: the table still holds at its cap exactly, so this
		// cannot grow memory and needs no failure path here. When the device
		// grows a real contact list this call moves there; until then an
		// inbound message is the only evidence of a conversation we have.
#if RNS_PINNED_DESTINATIONS
		RNS::Transport::pin_destination(message.source_hash());
#endif

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

		// TODO(bring-up): this is still the receive path's only consumer beyond the
		// reply. Nothing is retained. It needs a MessageStore (retuned to 8x32)
		// behind it and, later, the UI and the alert ladder.
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

	// TODO(bring-up): no MessageStore is attached, and sending does not need one.
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

	// TODO(bring-up): no stamps. Sideband ships lxmf_require_stamps = False and
	// announces a nil inbound stamp cost, Python LXMF skips the whole stamp
	// block when the cost is None, and microLXMF hard-codes packNil() for its
	// own announced cost, so nothing on either side allocates LXStamper's
	// 750 KiB workblock at the bring-up done-condition. If a peer ever demands a stamp
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

#ifdef THICKET_PATH_INDEX_PROBE
// Diagnostic, not product code. Compiled out unless THICKET_PATH_INDEX_PROBE is
// defined, and it drives a private store in its own directory so Transport's
// real path table is never touched.
//
// It answers two things a source read could only estimate:
//
//   A. What one path record actually costs in the RNS container pool. Computed
//      from ARM sizeof values plus TLSF overhead this came to ~64 B; that is
//      arithmetic and wants confirming against a real allocator on silicon.
//   B. Whether microStore's record cap bounds the index at all. The cap only
//      applies if something calls set_max_recs(). Left at its default of 0 it
//      disables both count eviction and the threshold compaction that reclaims
//      expired records — so max_recs=0 and max_recs=100 should differ visibly.
// Mirrors microStore BasicFileStore's IndexMap exactly: same key type, same
// value, same allocator, and a hash that is not noexcept - which is what makes
// libstdc++ pick the hash-caching node. Reproducing the type rather than
// driving a real store keeps the measurement independent of storage hardware,
// which matters on a board with no external flash fitted.
struct ProbeIndexValue {
	uint32_t segment;
	uint32_t offset;
	uint32_t timestamp;
	uint32_t ttl;
};

struct ProbeVectorHash {
	template <typename T>
	size_t operator()(const T& v) const {
		uint32_t h = 2166136261u;
		for (uint8_t b : v) { h ^= b; h *= 16777619u; }
		return h;
	}
};

using ProbeKey  = std::vector<uint8_t, RNS::Utilities::Memory::ContainerAllocator<uint8_t>>;
using ProbePair = std::pair<const ProbeKey, ProbeIndexValue>;
using ProbeIndex = std::unordered_map<
	ProbeKey, ProbeIndexValue, ProbeVectorHash, std::equal_to<ProbeKey>,
	RNS::Utilities::Memory::ContainerAllocator<ProbePair>>;

static uint32_t probe_pool_used() {
	return (uint32_t)RNS::Utilities::Memory::container_allocator_info.alloc_size;
}

// Keys are spread rather than sequential so bucket distribution resembles real
// destination hashes; a dense counter would flatter the hash map.
// Keys are spread rather than sequential so bucket distribution resembles real
// 16-byte destination hashes; a dense counter would flatter the hash map.
static void probe_fill(ProbeIndex& index, uint32_t from, uint32_t to) {
	for (uint32_t i = from; i < to; i++) {
		const uint32_t h = i * 2654435761u;
		ProbeKey key;
		key.reserve(16);
		for (uint8_t b = 0; b < 16; b++) {
			key.push_back((uint8_t)(h >> ((b % 4) * 8)) ^ (uint8_t)(b * 31u + i));
		}
		ProbeIndexValue v{ 0, i, i, 0 };
		index.emplace(std::move(key), v);
	}
}

static void probe_path_index() {
	Serial.println();
	Serial.println("--- path index probe (diagnostic) ---");

	// Deliberately touches no filesystem, so it still reports on a board with
	// no external flash fitted and after a failed storage bring-up.
	//
	// Marginal cost per record, measured against the real TLSF pool.
	//
	// Only the index is measured. The record payload lives in a flash segment
	// and costs no SRAM, so this is the figure that decides how many paths fit
	// on this part. The cap-enforcement half of the experiment needs a working
	// store and therefore a filesystem; it belongs on the host target, where it
	// runs without hardware at all.
	{
		ProbeIndex index;
		const uint32_t base = probe_pool_used();
		uint32_t last_n = 0;
		uint32_t last_used = base;
		const uint32_t steps[] = { 50, 100, 150, 200, 250 };
		for (uint32_t s = 0; s < sizeof(steps) / sizeof(steps[0]); s++) {
			const uint32_t n = steps[s];
			probe_fill(index, last_n, n);
			const uint32_t used = probe_pool_used();
			Serial.print("      records=");
			Serial.print((uint32_t)index.size());
			Serial.print("  buckets=");
			Serial.print((uint32_t)index.bucket_count());
			Serial.print("  pool_delta=");
			Serial.print(used - base);
			Serial.print(" B  marginal=");
			Serial.print((double)(used - last_used) / (double)(n - last_n), 1);
			Serial.println(" B/record");
			last_n = n;
			last_used = used;
		}
		Serial.print("      mean over ");
		Serial.print(last_n);
		Serial.print(" records: ");
		Serial.print((double)(last_used - base) / (double)last_n, 1);
		Serial.println(" B/record");
	}
	// index destroyed here; report the pool to confirm it all came back.
	info_u32("pool in use after teardown (B)", probe_pool_used());
	ok("probe complete");
	Serial.println();
}
#endif

#ifdef THICKET_RAM_PROBE
// Turns the RAM budget from link-time arithmetic into measured fact.
//
// Three things here cannot be obtained any other way than on silicon:
//   - newlib's mallinfo, which is the only honest answer to "how much of the
//     .heap section is actually spent" once the TLSF pool and the LXMF router
//     have been allocated out of it;
//   - uxTaskGetStackHighWaterMark, which reports the deepest the loop task's
//     stack has ever been. A stack overflow here presents as a random hard
//     fault, never as a budget line, so the margin is worth knowing exactly;
//   - sizeof on this compiler for this ABI. Host builds are 64-bit and every
//     pointer in these structures is twice the width there.
static void report_ram(const char* when) {
	Serial.println();
	Serial.print("--- RAM probe (");
	Serial.print(when);
	Serial.println(") ---");

	struct mallinfo mi = mallinfo();
	info_u32("heap arena (B)", (uint32_t)mi.arena);
	info_u32("heap in use (B)", (uint32_t)mi.uordblks);
	info_u32("heap free in arena (B)", (uint32_t)mi.fordblks);

	info_u32("RNS pool size (B)",
	         (uint32_t)RNS::Utilities::Memory::heap_pool_size());
	info_u32("RNS pool free (B)",
	         (uint32_t)RNS::Utilities::Memory::heap_pool_free());
	info_u32("RNS pool frag (%)",
	         (uint32_t)RNS::Utilities::Memory::heap_pool_fragmented());

	// Free words remaining at the shallowest point this task has ever reached.
	// Multiply by 4 for bytes on this core.
	// Name the task explicitly. Since the work moved off loop(), a stack figure
	// with no task attached to it is ambiguous and easy to misread as the other
	// one -- they differ by 4 KB of ceiling.
	const uint32_t free_words = (uint32_t)uxTaskGetStackHighWaterMark(NULL);
	info("stack reported for task", pcTaskGetName(NULL));
	info_u32("task stack free low-water (B)", free_words * 4);

	info_u32("sizeof LXMRouter", (uint32_t)sizeof(LXMF::LXMRouter));
	info_u32("sizeof LXMessage", (uint32_t)sizeof(LXMF::LXMessage));
	info_u32("sizeof MessageStore", (uint32_t)sizeof(LXMF::MessageStore));
	info_u32("sizeof RNS::Packet", (uint32_t)sizeof(RNS::Packet));
	info_u32("sizeof RNS::Bytes", (uint32_t)sizeof(RNS::Bytes));
	info_u32("sizeof RNS::Identity", (uint32_t)sizeof(RNS::Identity));
	info_u32("sizeof RNS::Destination", (uint32_t)sizeof(RNS::Destination));
	Serial.println();
}
#endif

#ifdef THICKET_LOWRAM_PROBE
// Is the 24,576 B below RAM ORIGIN actually free?
//
// The linker reserves 0x20000000-0x20006000 for a SoftDevice on every build,
// including the no-BLE one that has no SoftDevice on the part at all. Moving
// RAM ORIGIN down recovers 24,568 B -- twice the framebuffer -- but relocating
// the image is a brick-class risk on a board nobody can press RESET on.
//
// This asks the question without moving anything: write a pattern across the
// window, read it back, and check it again later. If it survives, nothing else
// owns that memory. It runs AFTER bring-up on purpose, so that if writing there
// does upset something, the useful output has already been printed and USB is
// already up -- which is what keeps the board reflashable.
//
// The bottom 256 B are left alone: the MBR keeps its parameter page at
// 0x20000000, which is why the relocation target is 0x20000008 and not 0.
#define LOWRAM_START 0x20000100UL
#define LOWRAM_END   0x20006000UL

static uint32_t lowram_check(void) {
	uint32_t bad = 0;
	for (uint32_t a = LOWRAM_START; a < LOWRAM_END; a += 4) {
		if (*(volatile uint32_t*)a != (a ^ 0xA5A5A5A5UL)) bad++;
	}
	return bad;
}

static void probe_low_ram(void) {
	Serial.println();
	Serial.println("--- low-RAM probe (SoftDevice window) ---");
	info_u32("window start", LOWRAM_START);
	info_u32("window end", LOWRAM_END);
	info_u32("window size (B)", LOWRAM_END - LOWRAM_START);

	for (uint32_t a = LOWRAM_START; a < LOWRAM_END; a += 4) {
		*(volatile uint32_t*)a = (a ^ 0xA5A5A5A5UL);
	}
	info_u32("mismatches immediately after write", lowram_check());
	Serial.println("      (0 = the window is writable and reads back)");
	Serial.println();
}
#endif



// Stack for the work task, in FreeRTOS words. 2048 words = 8,192 bytes.
// Defined here because setup() creates the task; the reasoning is at
// thicket_task() below.
#define THICKET_TASK_STACK_WORDS 2048
static void thicket_task(void* arg);

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
	Serial.println(" Thicket firmware - bring-up skeleton");
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
	// TODO(bring-up): make this a build flag once bring-up is done.
	// Raise with -DTHICKET_LOG_DEBUG when a link establishes and then nothing
	// happens, which is the shape of an inbound DIRECT/Resource failure: at
	// INFO the log goes silent after "Incoming link established" and tells you
	// nothing about why.
#ifdef THICKET_LOG_DEBUG
	RNS::loglevel(RNS::LOG_DEBUG);
#else
	RNS::loglevel(RNS::LOG_INFO);
#endif

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

	// Hand the work to a task with a stack that fits it. See thicket_task().
	{
		TaskHandle_t h = NULL;
		const BaseType_t rc = xTaskCreate(thicket_task, "thicket",
		                                  THICKET_TASK_STACK_WORDS, NULL,
		                                  TASK_PRIO_LOW, &h);
		if (rc != pdPASS) {
			// Nothing runs if this fails, so say so rather than idling silently
			// with a green LED claiming success.
			g_stack_up = false;
			fail("could not create the work task (out of heap?)");
		}
		else {
			ok("work task started, 8192 B stack");
		}
	}
#ifdef THICKET_RAM_PROBE
	report_ram("after bring-up");
#endif
#ifdef THICKET_LOWRAM_PROBE
	probe_low_ram();
#endif
}

// The body that used to be loop(). It now runs in a task we create ourselves,
// for the stack -- see thicket_task() below.
static void thicket_work() {
#ifdef THICKET_PATH_INDEX_PROBE
	// Repeat the probe a few times after boot rather than once. A host that has
	// just flashed the board cannot re-attach before bring-up finishes, so a
	// single boot-time run is unobservable in practice. Bounded at three runs
	// because each one erases and rewrites the ring.
	// On demand, from any serial input. A host that has just flashed the board
	// cannot attach before bring-up finishes, and USB CDC discards writes made
	// while no host is listening - so a boot-time or timer-driven run prints
	// into a void and is never seen. Triggering on input is the only scheme
	// that reliably produces output on an attached terminal.
	if (Serial.available()) {
		while (Serial.available()) { (void)Serial.read(); }
		probe_path_index();
	}
#endif
#ifdef THICKET_RAM_PROBE
	// The stack low-water mark only means something once the device has done
	// the deep work -- receiving, decrypting, composing. Re-reading it on
	// demand after the board has been running is the measurement that matters;
	// the one taken at the end of setup() is only a floor.
	if (Serial.available()) {
		while (Serial.available()) { (void)Serial.read(); }
		report_ram("on demand");
	}
#endif
#ifdef THICKET_LOWRAM_PROBE
	// Writable is necessary but not sufficient: the question is whether
	// anything ELSE writes there while the radio, USB and filesystem are all
	// running. Re-check on demand, after the device has been up a while.
	if (Serial.available()) {
		while (Serial.available()) { (void)Serial.read(); }
		info_u32("low-RAM mismatches now", lowram_check());
		info_u32("uptime (ms)", (uint32_t)millis());
	}
#endif
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
	// remains the next substantial piece of work.
	if (g_reply.armed && (int32_t)(millis() - g_reply.due_ms) >= 0) {
		g_reply.armed = false;

		char body[BODY_BUF];
		build_diag_body(body, sizeof(body),
		                g_reply.signal_valid, g_reply.rssi, g_reply.snr);
		send_lxmf(g_reply.to, body, "auto-reply");
#ifdef THICKET_RAM_PROBE
		// Report here rather than on demand. This is the one moment the deep
		// path has just run -- inbound decrypt, proof, compose, sign, pack --
		// so it is the only stack figure worth having, and waiting for a serial
		// poke to ask for it has proved unreliable on this build.
		report_ram("after inbound + auto-reply");
#endif
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

	// TODO(bring-up): no sleep. The always-listening contract does not mean a busy
	// loop; it means the receiver stays powered while the CPU idles. This spins
	// the M4 at 64 MHz and will not meet any battery target.
}

// Why this work does not run in loop().
//
// The Adafruit core runs loop() in a FreeRTOS task whose stack it fixes at
// LOOP_STACK_SZ = 256*4 -- FreeRTOS counts words, so 4,096 bytes -- defined
// unconditionally in the core's own main.cpp with no #ifndef. No build flag can
// reach it.
//
// 4 KB is not enough for what this loop does. Measured on the board, the loop
// task had been 2,280 B deep with 1,816 B spare, but that only covers paths that
// had actually run, and the device had done nothing but announce and idle.
// -fstack-usage on the linked image shows single frames on the INBOUND MESSAGE
// path that nothing had exercised: LXMRouter::on_packet 1,056 B,
// RNS::doLog(va_list) 1,032 B, handle_direct_proof 1,040 B,
// static_proof_callback 1,088 B. on_packet plus doLog alone is 2,088 B of 4,096,
// before any Transport, Fernet or msgpack frame in between. Receiving a message,
// proving it and logging about it is the ordinary job of this device.
//
// A stack overflow here is a hard fault at a random address with no diagnostic.
// It would present as "the board locks up when people message it", which is the
// most expensive kind of bug to attribute. Upstream's own compression code says
// it assumes "nRF52 task stacks (8 KB+)".
//
// So: our own task at 8,192 B. Priced on the board at 8,288 B including the TCB.
// FreeRTOS here is heap_3, so pvPortMalloc is plain malloc and this comes from
// the system heap -- the budget with ~130 KB spare -- never from the RNS pool.
static void thicket_task(void* arg) {
	(void)arg;
	for (;;) {
		thicket_work();
		// Yield to anything of equal or lower priority. Nothing here may block:
		// there is no RX ISR, so receive latency is bounded by how often
		// Transport::loop() is called.
		taskYIELD();
	}
}

void loop() {
	// Deliberately empty of work. The core's loop task keeps its 4 KB stack and
	// this must stay shallow. Sleeping here yields the CPU to the task above
	// rather than spinning; it is not a power strategy, and the TODO about
	// actually idling the Cortex-M4 still stands in thicket_work().
	delay(1000);
}

// No _write() retarget here on purpose. microReticulum's log macros go through
// printf, and upstream's own examples define _write to push that at Serial —
// but the Adafruit nRF52 core already retargets printf to Serial (CDC) in
// cores/nRF5/main.cpp under CFG_LOGGER 0, which is the default. Defining our
// own is a duplicate-symbol link error, not an improvement.
