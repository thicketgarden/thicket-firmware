// Thicket interop scenario: POOL SOAK.
//
// Copyright (C) 2026 Thicket contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The question this answers
// -------------------------
// The RNS container pool is known to fragment with traffic rather than with
// uptime: it measured 2% fragmented at boot and 16% after a single inbound
// message and reply. Nobody has run it further, so the shape of that curve is
// unknown, and the two possible shapes call for opposite decisions:
//
//   plateau  -- fragmentation settles at some steady level. The pool is sized
//               once, against the plateau, and the matter is closed.
//   climb    -- fragmentation rises without bound. The pool exhausts after
//               some number of messages, and no sizing fixes it; the
//               allocation pattern has to change.
//
// This binary drives message traffic through the real LXMRouter in a loop and
// samples the pool every cycle, so the curve can be looked at instead of
// guessed at.
//
// A HOST RUN DOES NOT GIVE THE BOARD'S NUMBERS. A host probe once read 36%
// where the board read 68%, because the host scenario had no LXMF router. This
// one does, which is the whole point -- but pointer width, allocator alignment
// and the absence of a radio still differ. What transfers is the SHAPE of the
// curve, not its absolute height. Confirm the level on hardware.
//
// Usage:
//   ./program [CYCLES] [CSV_OUT]
//
// Exit 0 if the run completed; the verdict is in the CSV and the summary.
// This binary doesn't decide whether the curve is acceptable -- it measures.

#include <microStore/FileSystem.h>
#include <microStore/Adapters/UniversalFileSystem.h>

#include <microReticulum/Identity.h>
#include <microReticulum/Destination.h>
#include <microReticulum/Reticulum.h>
#include <microReticulum/Transport.h>
#include <microReticulum/Utilities/Memory.h>

#include <LXMF/LXMRouter.h>
#include <LXMF/LXMessage.h>

#include <microReticulum/Utilities/tlsf/tlsf.h>

#include <stdio.h>
#include <string.h>
#include <string>

namespace {

struct PoolSample {
	uint32_t used_count = 0;
	uint32_t used_size = 0;
	uint32_t free_count = 0;
	uint32_t free_size = 0;
	uint32_t free_max = 0;      // largest single free block
};

// Own walker rather than the library's, so this harness depends only on the
// public pool handle.
void walk(void* /*ptr*/, size_t size, int used, void* user) {
	PoolSample* s = (PoolSample*)user;
	if (!s) return;
	if (used) {
		s->used_count++;
		s->used_size += (uint32_t)size;
	} else {
		s->free_count++;
		s->free_size += (uint32_t)size;
		if ((uint32_t)size > s->free_max) s->free_max = (uint32_t)size;
	}
}

PoolSample sample_pool() {
	PoolSample s;
	auto& info = RNS::Utilities::Memory::heap_pool_info;
	if (!info.tlsf) return s;
	tlsf_walk_pool(tlsf_get_pool(info.tlsf), walk, &s);
	return s;
}

// Fragmentation as the share of free space unavailable to a single allocation
// of the largest free block's size. 0% means all free space is one run; 90%
// means the pool is free on paper and can't satisfy a large request.
double fragmentation(const PoolSample& s) {
	if (s.free_size == 0) return 0.0;
	return 100.0 * (1.0 - (double)s.free_max / (double)s.free_size);
}

size_t delivered = 0;

void on_delivery(LXMF::LXMessage& /*message*/) {
	++delivered;
}

}   // namespace

int main(int argc, char** argv) {
	const long cycles = (argc > 1) ? strtol(argv[1], nullptr, 10) : 1000;
	const char* csv_path = (argc > 2) ? argv[2] : nullptr;

	microStore::FileSystem filesystem{microStore::Adapters::UniversalFileSystem()};
	RNS::Utilities::OS::register_filesystem(filesystem);

	RNS::Reticulum reticulum;
	reticulum.transport_enabled(false);
	reticulum.start();

	RNS::Identity identity;
	LXMF::LXMRouter router(identity, "", false);
	router.register_delivery_callback(on_delivery);

	RNS::Identity peer_identity;
	RNS::Destination peer(peer_identity, RNS::Type::Destination::OUT,
	                      RNS::Type::Destination::SINGLE, "lxmf", "delivery");

	FILE* csv = csv_path ? fopen(csv_path, "w") : nullptr;
	if (csv) fprintf(csv, "cycle,used_size,used_count,free_size,free_count,free_max,frag_pct\n");

	const PoolSample at_boot = sample_pool();

	// A pool that's not backed reports zero used and zero free, which reads
	// as "no fragmentation" and is the most dangerous result this harness
	// could print. Refuse to run rather than produce a reassuring nothing.
	if (at_boot.used_size == 0 && at_boot.free_size == 0) {
		fprintf(stderr,
		        "[soak] the container pool is not backed in this build: every\n"
		        "[soak] sample would read zero. Build with the firmware's\n"
		        "[soak] allocator flags (RNS_CONTAINER_ALLOCATOR and\n"
		        "[soak] RNS_HEAP_POOL_BUFFER_SIZE) -- see platformio.ini.\n");
		return 2;
	}
	printf("[soak] pool at boot: used %u B in %u blocks, free %u B in %u blocks, "
	       "largest free %u B, fragmentation %.1f%%\n",
	       at_boot.used_size, at_boot.used_count, at_boot.free_size,
	       at_boot.free_count, at_boot.free_max, fragmentation(at_boot));
	printf("[soak] driving %ld message cycles through LXMRouter\n", cycles);

	uint32_t high_water = at_boot.used_size;
	double   worst_frag = fragmentation(at_boot);
	long     high_water_cycle = 0, worst_frag_cycle = 0;

	char body[96];
	for (long i = 0; i < cycles; ++i) {
		// A message built, packed and routed is the allocation pattern real
		// traffic produces: containers for the message, its fields and the
		// router's bookkeeping, allocated and released per message.
		snprintf(body, sizeof(body), "soak message %ld", i);
		{
			const RNS::Bytes content((const uint8_t*)body, strlen(body));
			const RNS::Bytes title((const uint8_t*)"soak", 4);
			LXMF::LXMessage message(peer, router.delivery_destination(),
			                        content, title, LXMF::Type::Message::DIRECT);
			message.pack();
			router.handle_outbound(message);
		}
		router.process_outbound();
		router.process_inbound();

		const PoolSample s = sample_pool();
		const double frag = fragmentation(s);
		if (s.used_size > high_water) { high_water = s.used_size; high_water_cycle = i; }
		if (frag > worst_frag)        { worst_frag = frag;        worst_frag_cycle = i; }

		if (csv)
			fprintf(csv, "%ld,%u,%u,%u,%u,%u,%.2f\n", i, s.used_size, s.used_count,
			        s.free_size, s.free_count, s.free_max, frag);

		// Progress on a log-ish cadence: enough to watch a long run, not so
		// much that the output is the experiment.
		if (i < 10 || (i < 1000 && i % 100 == 0) || i % 1000 == 0)
			printf("[soak] cycle %-7ld used %7u B  free %7u B  largest %7u B  frag %5.1f%%\n",
			       i, s.used_size, s.free_size, s.free_max, frag);
	}

	const PoolSample at_end = sample_pool();
	if (csv) fclose(csv);

	printf("\n[soak] === after %ld cycles ===\n", cycles);
	printf("[soak] delivered callbacks:   %zu\n", delivered);
	printf("[soak] used at boot:          %u B\n", at_boot.used_size);
	printf("[soak] used at end:           %u B  (%+d B)\n", at_end.used_size,
	       (int)at_end.used_size - (int)at_boot.used_size);
	printf("[soak] high-water used:       %u B at cycle %ld\n", high_water, high_water_cycle);
	printf("[soak] fragmentation at boot: %.1f%%\n", fragmentation(at_boot));
	printf("[soak] fragmentation at end:  %.1f%%\n", fragmentation(at_end));
	printf("[soak] worst fragmentation:   %.1f%% at cycle %ld\n", worst_frag, worst_frag_cycle);
	if (csv_path) printf("[soak] per-cycle CSV:         %s\n", csv_path);

	// Growth in USED bytes across a run whose messages are all released is the
	// signal that matters: a pool that keeps more after each cycle exhausts
	// eventually, whatever its fragmentation reads.
	const int drift = (int)at_end.used_size - (int)at_boot.used_size;
	printf("\n[soak] retained across the run: %+d B. Whether that is a leak, a\n"
	       "[soak] cache filling to its bound, or noise is not decided here --\n"
	       "[soak] read the curve in the CSV.\n", drift);

	// Coverage, stated by the run itself. Without a peer nothing is delivered,
	// so the inbound decrypt-and-deliver path never allocates -- and that is
	// the path the board's worst fragmentation figure came from. A clean
	// result here doesn't speak for it.
	if (delivered == 0) {
		printf("\n[soak] COVERAGE: no message was delivered in this run, so only the\n"
		       "[soak] outbound construct-and-queue path was exercised. The inbound\n"
		       "[soak] path is NOT covered by this result. Confirm it on hardware,\n"
		       "[soak] or against a second router.\n");
	}
	return 0;
}
