// Thicket interop scenario: WIRE ORACLE (emitter half).
//
// Copyright (C) 2026 Thicket contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The gap this closes
// -------------------
// The other scenarios prove that a message arrives. That's a weaker statement
// than it looks: a field can be encoded wrongly and still round-trip, because
// both ends are the same implementation making the same mistake. An end-to-end
// pass reports "delivered" either way, and the first symptom of a wrong header
// field is a peer that silently ignores our traffic.
//
// This binary packs a set of packets through microReticulum's real pack() path
// and prints each one as hex with the field values it was built from. The
// Python half decodes the same bytes with the reference implementation's own
// RNS.Packet and compares field by field, so a disagreement is reported as the
// field that differs rather than as a message that failed to arrive.
//
// No network and no second process: the packets are never sent.
//
// Writes one JSON object to the path given as argv[1]. The stack logs to
// stdout, so the report needs a stream of its own.

#include <microStore/FileSystem.h>
#include <microStore/Adapters/UniversalFileSystem.h>

#include <microReticulum/Identity.h>
#include <microReticulum/Destination.h>
#include <microReticulum/Packet.h>
#include <microReticulum/Reticulum.h>
#include <microReticulum/Transport.h>
#include <microReticulum/Type.h>

#include <stdio.h>
#include <string>
#include <vector>

namespace {

const char* APP_NAME = "thicket";
const char* ASPECT   = "oracle";

struct Case {
	std::string name;
	std::string hex;
	int header_type;       const char* header_type_name;
	int packet_type;       const char* packet_type_name;
	int context;
	int transport_type;    const char* transport_type_name;
	int destination_type;  const char* destination_type_name;
	std::string destination_hash;
	std::string data_hex;
	std::string transport_id;
};

std::vector<Case> cases;

// Packs one packet and records what it was built from. `data` is the payload
// handed to the packet, which for a plaintext destination is what the
// reference should read back verbatim.
// `destination_type` is taken from the Destination rather than from the packed
// Packet: microReticulum populates Packet::destination_type() only on inbound
// packets, so on an outbound one it reads SINGLE no matter what was addressed.
// The bytes on the wire are correct either way; the accessor isn't.
void emit(const char* name, RNS::Packet& packet,
          const char* header_type_name,
          const char* packet_type_name,
          const char* transport_type_name,
          int destination_type,
          const char* destination_type_name,
          const RNS::Bytes& data) {
	packet.pack();

	Case c;
	c.name                  = name;
	c.hex                   = packet.raw().toHex();
	c.header_type           = (int)packet.header_type();
	c.header_type_name      = header_type_name;
	c.packet_type           = (int)packet.packet_type();
	c.packet_type_name      = packet_type_name;
	c.context               = (int)packet.context();
	c.transport_type        = (int)packet.transport_type();
	c.transport_type_name   = transport_type_name;
	c.destination_type      = destination_type;
	c.destination_type_name = destination_type_name;
	c.destination_hash      = packet.destination_hash().toHex();
	c.data_hex              = data.toHex();
	c.transport_id          = packet.transport_id() ? packet.transport_id().toHex() : "";
	cases.push_back(c);
}

void print_json(FILE* f) {
	fprintf(f, "{\n  \"cases\": [\n");
	for (size_t i = 0; i < cases.size(); ++i) {
		const Case& c = cases[i];
		fprintf(f, "    {\n");
		fprintf(f, "      \"name\": \"%s\",\n", c.name.c_str());
		fprintf(f, "      \"hex\": \"%s\",\n", c.hex.c_str());
		fprintf(f, "      \"header_type\": %d, \"header_type_name\": \"%s\",\n",
		       c.header_type, c.header_type_name);
		fprintf(f, "      \"packet_type\": %d, \"packet_type_name\": \"%s\",\n",
		       c.packet_type, c.packet_type_name);
		fprintf(f, "      \"context\": %d,\n", c.context);
		fprintf(f, "      \"transport_type\": %d, \"transport_type_name\": \"%s\",\n",
		       c.transport_type, c.transport_type_name);
		fprintf(f, "      \"destination_type\": %d, \"destination_type_name\": \"%s\",\n",
		       c.destination_type, c.destination_type_name);
		fprintf(f, "      \"destination_hash\": \"%s\",\n", c.destination_hash.c_str());
		fprintf(f, "      \"data_hex\": \"%s\",\n", c.data_hex.c_str());
		fprintf(f, "      \"transport_id\": \"%s\"\n", c.transport_id.c_str());
		fprintf(f, "    }%s\n", (i + 1 < cases.size()) ? "," : "");
	}
	fprintf(f, "  ]\n}\n");
}

}   // namespace

int main(int argc, char** argv) {
	microStore::FileSystem filesystem{microStore::Adapters::UniversalFileSystem()};
	RNS::Utilities::OS::register_filesystem(filesystem);

	RNS::Reticulum reticulum;
	reticulum.transport_enabled(false);
	reticulum.start();

	RNS::Identity identity;

	// A SINGLE destination is the ordinary addressed case: every field in the
	// header is exercised except the transport-relay ones.
	RNS::Destination single(identity, RNS::Type::Destination::OUT,
	                        RNS::Type::Destination::SINGLE, APP_NAME, ASPECT);

	// A PLAIN destination is unencrypted, so the payload the reference reads
	// back is comparable byte for byte. On SINGLE it would be ciphertext and
	// the only checkable thing would be the header.
	RNS::Identity none_identity({RNS::Type::NONE});
	RNS::Destination plain(none_identity, RNS::Type::Destination::OUT,
	                       RNS::Type::Destination::PLAIN, APP_NAME, ASPECT);

	const RNS::Bytes payload((const uint8_t*)"thicket wire oracle", 19);
	const RNS::Bytes short_payload((const uint8_t*)"x", 1);

	{
		RNS::Packet p(plain, payload);
		p.packet_type(RNS::Type::Packet::DATA)
		 .context(RNS::Type::Packet::CONTEXT_NONE)
		 .header_type(RNS::Type::Packet::HEADER_1)
		 .transport_type(RNS::Type::Transport::BROADCAST);
		emit("plain_data_header1", p, "HEADER_1", "DATA", "BROADCAST",
		     (int)RNS::Type::Destination::PLAIN, "PLAIN", payload);
	}

	{
		RNS::Packet p(plain, payload);
		p.packet_type(RNS::Type::Packet::DATA)
		 .context(RNS::Type::Packet::REQUEST)
		 .header_type(RNS::Type::Packet::HEADER_1)
		 .transport_type(RNS::Type::Transport::BROADCAST);
		emit("plain_data_context_request", p, "HEADER_1", "DATA", "BROADCAST",
		     (int)RNS::Type::Destination::PLAIN, "PLAIN", payload);
	}

	{
		RNS::Packet p(plain, short_payload);
		p.packet_type(RNS::Type::Packet::DATA)
		 .context(RNS::Type::Packet::PATH_RESPONSE)
		 .header_type(RNS::Type::Packet::HEADER_1)
		 .transport_type(RNS::Type::Transport::BROADCAST);
		emit("plain_data_context_path_response", p, "HEADER_1", "DATA", "BROADCAST",
		     (int)RNS::Type::Destination::PLAIN, "PLAIN", short_payload);
	}

	{
		RNS::Packet p(single, payload);
		p.packet_type(RNS::Type::Packet::LINKREQUEST)
		 .context(RNS::Type::Packet::CONTEXT_NONE)
		 .header_type(RNS::Type::Packet::HEADER_1)
		 .transport_type(RNS::Type::Transport::BROADCAST);
		emit("single_linkrequest", p, "HEADER_1", "LINKREQUEST", "BROADCAST",
		     (int)RNS::Type::Destination::SINGLE, "SINGLE", payload);
	}

	{
		RNS::Packet p(single, payload);
		p.packet_type(RNS::Type::Packet::PROOF)
		 .context(RNS::Type::Packet::CONTEXT_NONE)
		 .header_type(RNS::Type::Packet::HEADER_1)
		 .transport_type(RNS::Type::Transport::BROADCAST);
		emit("single_proof", p, "HEADER_1", "PROOF", "BROADCAST",
		     (int)RNS::Type::Destination::SINGLE, "SINGLE", RNS::Bytes());
	}

	// HEADER_2 carries a transport id ahead of the destination hash, so every
	// field after it sits at a different offset than in HEADER_1. That makes it
	// the encoding most likely to drift unnoticed.
	//
	// ANNOUNCE is the only packet type whose payload survives a HEADER_2 pack.
	// This port and the reference both assign the body for ANNOUNCE alone and
	// leave it empty otherwise, so a HEADER_2 DATA packet built from a payload
	// packs to a header with nothing after it in either implementation. That
	// doesn't arise in transport, where a relayed packet is re-headered from
	// bytes it already holds rather than packed from data.
	{
		const RNS::Bytes transport_id = identity.hash();
		RNS::Packet p(plain, payload);
		p.packet_type(RNS::Type::Packet::ANNOUNCE)
		 .context(RNS::Type::Packet::CONTEXT_NONE)
		 .header_type(RNS::Type::Packet::HEADER_2)
		 .transport_type(RNS::Type::Transport::TRANSPORT)
		 .transport_id(transport_id);
		emit("plain_announce_header2_transport", p, "HEADER_2", "ANNOUNCE", "TRANSPORT",
		     (int)RNS::Type::Destination::PLAIN, "PLAIN", payload);
	}

	if (argc < 2) {
		fprintf(stderr, "usage: %s OUT.json\n", argv[0]);
		return 2;
	}
	FILE* out = fopen(argv[1], "w");
	if (!out) {
		fprintf(stderr, "cannot write %s\n", argv[1]);
		return 2;
	}
	print_json(out);
	fclose(out);
	return 0;
}
