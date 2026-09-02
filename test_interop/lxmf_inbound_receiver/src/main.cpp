// Thicket interop scenario 2: LXMF DELIVERY INBOUND.
//
// Copyright (C) 2026 Thicket contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The gap this closes
// -------------------
// microLXMF has a conformance bridge that passes a large Python-to-C++ matrix,
// and docs/parity-matrix.md cites it as `lxmf-conformance`. But that bridge
// builds microLXMF against torlando-tech/microReticulum @ 6054f6ba -- a fork of
// a different upstream base than the attermann 0.5.0-8 tree Thicket pins. It is
// good evidence about the LXMF layer. It isn't evidence about the LXMF layer
// running on OUR RNS layer, which is what we ship.
//
// This binary is microLXMF's LXMRouter on the microReticulum SHA in our
// platformio.ini, receiving a message from the Python LXMF reference and
// asserting the decoded fields against values fixed on both sides.
//
// NOT a cold test, and deliberately so. LXMF signature validation needs the
// source identity, so the Python peer must announce its lxmf.delivery
// destination first -- that's the reference flow, not a shortcut. Scenario 1
// (cold_inbound) is where coldness is tested.
//
// Exit 0 iff a message arrived and EVERY asserted field matched.

#include <microStore/FileSystem.h>
#include <microStore/Adapters/UniversalFileSystem.h>
#include <UDPInterface.h>

#include <microReticulum.h>

#include <LXMF/LXMRouter.h>
#include <LXMF/LXMessage.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <string>

// ---------------------------------------------------------------------------
// The contract. Every one of these is duplicated verbatim in
// python/lxmf_inbound_sender.py; if the two drift the scenario fails, which is
// the intended behaviour.
// ---------------------------------------------------------------------------
static const char* EXPECT_TITLE   = "thicket-interop-title";
static const char* EXPECT_CONTENT = "thicket interop: LXMF delivery inbound, "
                                    "originated by the Python reference";
// Pinned rather than "now", so the assertion is exact instead of a window.
static const double EXPECT_TIMESTAMP = 1750000000.5;
// LXMF fields are a msgpack map. microLXMF stores keys and values as the RAW
// msgpack byte spans (LXMessage.cpp captures them via Unpacker::indices), so
// what we assert here is the wire encoding the reference produced, not a
// decoded value. Key 6 is a positive fixint, so one byte, 0x06.
static const uint8_t EXPECT_FIELD_KEY = 0x06;
static const char* EXPECT_FIELD_VALUE = "thicket-interop-field-value";

static RNS::Reticulum reticulum({RNS::Type::NONE});
static RNS::Interface udp_interface(RNS::Type::NONE);

static volatile bool message_seen = false;
static int failures = 0;
static int checks   = 0;

static void check(bool ok, const char* what, const std::string& got,
                  const std::string& want) {
	++checks;
	if (ok) {
		printf("[cpp]   OK   %-18s %s\n", what, got.c_str());
	}
	else {
		++failures;
		printf("[cpp]   FAIL %-18s\n", what);
		printf("[cpp]          got:  %s\n", got.c_str());
		printf("[cpp]          want: %s\n", want.c_str());
	}
}

static std::string as_text(const RNS::Bytes& b) {
	if (!b) return std::string();
	return std::string((const char*)b.data(), b.size());
}

// msgpack bin format for a payload shorter than 256 bytes: 0xc4, length, data.
static RNS::Bytes msgpack_bin(const char* s) {
	const size_t n = strlen(s);
	RNS::Bytes out;
	out.append((uint8_t)0xc4);
	out.append((uint8_t)(n & 0xff));
	out.append((const uint8_t*)s, n);
	return out;
}

static void on_delivery(LXMF::LXMessage& message) {
	message_seen = true;
	printf("[cpp] LXMF message delivered, asserting decoded fields\n");

	check(message.signature_validated(), "signature",
	      message.signature_validated() ? "validated" : "NOT validated",
	      "validated");

	check(as_text(message.title()) == EXPECT_TITLE, "title",
	      as_text(message.title()), EXPECT_TITLE);

	check(as_text(message.content()) == EXPECT_CONTENT, "content",
	      as_text(message.content()), EXPECT_CONTENT);

	// The reference packs the timestamp as an IEEE-754 double, so this should
	// be exact; compare with a tolerance far below one tick anyway so a
	// genuine round-trip loss is still caught.
	const double ts = message.timestamp();
	char tsbuf[64], tswant[64];
	snprintf(tsbuf,  sizeof(tsbuf),  "%.6f", ts);
	snprintf(tswant, sizeof(tswant), "%.6f", EXPECT_TIMESTAMP);
	check(fabs(ts - EXPECT_TIMESTAMP) < 1e-6, "timestamp", tsbuf, tswant);

	char cntbuf[32];
	snprintf(cntbuf, sizeof(cntbuf), "%lu", (unsigned long)message.fields_count());
	check(message.fields_count() == 1, "fields_count", cntbuf, "1");

	RNS::Bytes key;
	key.append(EXPECT_FIELD_KEY);
	const RNS::Bytes* value = message.fields_get(key);
	if (value == nullptr) {
		++checks; ++failures;
		printf("[cpp]   FAIL %-18s\n", "field[0x06]");
		printf("[cpp]          got:  absent\n");
		printf("[cpp]          want: %s\n",
		       msgpack_bin(EXPECT_FIELD_VALUE).toHex().c_str());
		// Dump whatever did arrive, so a wire-format surprise is diagnosable
		// rather than just red.
		for (size_t i = 0; i < message.fields_count(); ++i) {
			const LXMF::FieldEntry* f = message.field_at(i);
			if (f) {
				printf("[cpp]          present key=%s value=%s\n",
				       f->key.toHex().c_str(), f->value.toHex().c_str());
			}
		}
	}
	else {
		const RNS::Bytes want = msgpack_bin(EXPECT_FIELD_VALUE);
		check(*value == want, "field[0x06]", value->toHex(), want.toHex());
	}

	check(message.source_hash().size() == 16, "source_hash len",
	      std::to_string(message.source_hash().size()), "16");

	printf("[cpp] source_hash    %s\n", message.source_hash().toHex().c_str());
	printf("[cpp] message hash   %s\n", message.hash().toHex().c_str());
	fflush(stdout);
}

int main() {
	printf("[cpp] lxmf_inbound_receiver starting\n");

	microStore::FileSystem filesystem{microStore::Adapters::UniversalFileSystem()};
	filesystem.init();
	RNS::Utilities::OS::register_filesystem(filesystem);

	udp_interface = new UDPInterface();
	udp_interface.mode(RNS::Type::Interface::MODE_GATEWAY);
	RNS::Transport::register_interface(udp_interface);
	udp_interface.start();

	reticulum = RNS::Reticulum();
	reticulum.transport_enabled(false);
	reticulum.start();

	RNS::Identity identity;
	LXMF::LXMRouter router(identity, "", false);
	router.set_display_name("thicket-interop");
	router.register_delivery_callback(on_delivery);

	printf("[cpp] lxmf.delivery destination %s\n",
	       router.delivery_destination().hash().toHex().c_str());
	router.announce();
	fflush(stdout);

	double TIMEOUT_S = 40.0;
	if (const char* env = getenv("THICKET_INTEROP_TIMEOUT_S")) {
		const double v = atof(env);
		if (v > 0.0) TIMEOUT_S = v;
	}
	const double LINGER_S = 3.0;   // let the delivery proof leave.
	const double start    = RNS::Utilities::OS::time();
	double last_announce  = start;
	double seen_at        = 0.0;

	while (true) {
		reticulum.loop();
		router.process_inbound();
		router.process_outbound();

		const double now = RNS::Utilities::OS::time();
		if (message_seen && seen_at == 0.0) seen_at = now;
		if (seen_at != 0.0 && now - seen_at >= LINGER_S) break;

		if (now - start > TIMEOUT_S) {
			printf("[cpp] TIMEOUT: no LXMF message was delivered\n");
			break;
		}
		if (now - last_announce >= 2.0 && !message_seen) {
			router.announce();
			last_announce = now;
		}
		RNS::Utilities::OS::sleep(0.01);
	}

	RNS::Transport::deregister_interface(udp_interface);

	int rc = 0;
	if (!message_seen) {
		printf("[cpp] FAIL: no LXMF message arrived\n");
		rc = 1;
	}
	else if (failures > 0) {
		printf("[cpp] FAIL: %d of %d field assertions failed\n", failures, checks);
		rc = 1;
	}
	else {
		printf("[cpp] SUCCESS all %d LXMF field assertions passed\n", checks);
	}
	printf("[cpp] exit code %d\n", rc);
	return rc;
}
