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
// power cycle, the SX1262 on air, Reticulum transport, an announce, and a live
// LXMF router with its delivery destination registered.
//
// What is NOT here yet, and is marked TODO(M1) at each site: sending a message,
// a message store, a UI, input, and any conformance check against Python RNS.
// Nothing below fakes those.
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

#include <memory>
#include <string>

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

static bool g_stack_up = false;
static double g_last_announce = 0.0;

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

static void fail(const char* what) {
	Serial.print("      FAIL : ");
	Serial.println(what);
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
	info("band", "915.0 MHz, BW 125 kHz, SF8, CR4:5, +17 dBm");
	info("spi", "SPI1 (radio) - SPI stays on the flash bus");

	g_lora_interface = new LoRaInterface();

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
		// TODO(M1): this is the receive path's only consumer. It prints and
		// drops. It needs a MessageStore (retuned to 8x32) behind it and,
		// later, the UI and the A9 alert ladder.
		Serial.println();
		Serial.println("--- LXMF message received ---");
		Serial.print("  from   : ");
		Serial.println(message.source_hash().toHex().c_str());
		Serial.print("  title  : ");
		Serial.println(message.title().toString().c_str());
		Serial.print("  content: ");
		Serial.println(message.content().toString().c_str());
		Serial.println("-----------------------------");
	});

	g_router->register_failed_callback([](LXMF::LXMessage& message) {
		Serial.print("LXMF: delivery failed for ");
		Serial.println(message.hash().toHex().c_str());
	});

	// TODO(M1): no MessageStore is attached. microLXMF's LXMRouter does not
	// reference MessageStore at all - it is application-owned - so the class is
	// dropped from the image entirely until we instantiate one. When we do, its
	// pool constants must be overridden first: they are `static constexpr` in
	// MessageStore.h with no #ifndef guard, and at upstream's 32x256 the object
	// is 266,896 bytes and the link fails outright. Guarding them is a six-line
	// upstream patch and should be our first contribution to microLXMF.

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

	// TODO(M1): compose and send. The path is
	// LXMF::LXMessage(destination, source, content, title, method) then
	// router->handle_outbound(msg); it needs a destination to send to, which
	// needs an announce list, which needs Transport path-table enumeration -
	// upstream is mid-migration to a microStore-backed path table with no
	// enumeration API, so path_table() returns empty. That is M2's first wall.

	// TODO(M1): no sleep. A26's always-listening contract does not mean a busy
	// loop; it means the receiver stays powered while the CPU idles. This spins
	// the M4 at 64 MHz and will not meet any battery target.
}

// No _write() retarget here on purpose. microReticulum's log macros go through
// printf, and upstream's own examples define _write to push that at Serial —
// but the Adafruit nRF52 core already retargets printf to Serial (CDC) in
// cores/nRF5/main.cpp under CFG_LOGGER 0, which is the default. Defining our
// own is a duplicate-symbol link error, not an improvement.
