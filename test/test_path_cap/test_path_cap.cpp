// Cap-enforcement control for the path-table bounding work.
//
// The claim under test is that microStore's record cap is inert unless
// something calls set_max_recs(): policy_max_recs defaults to
// USTORE_DEFAULT_MAX_RECS, which is 0, and both the eviction in put() and the
// threshold-triggered compaction that reclaims expired records are gated on
// policy_max_recs > 0. microReticulum's Transport::start() applies the cap to
// the known-destinations and packet-hashlist stores but not to the path store,
// so a library consumer that does not set it itself runs uncapped.
//
// This runs on the host against a real BasicFileStore over StdioFileSystem, so
// it is evidence rather than a reading of the source. It needs no hardware.

#include <unity.h>

#include <microStore/FileStore.h>
#include <microStore/Adapters/StdioFileSystem.h>

#include <cstdint>
#include <memory>
#include <string>

using Store = microStore::BasicFileStore<std::allocator<uint8_t>>;

namespace {

// 16 bytes, matching an RNS truncated destination hash. Spread rather than
// sequential so bucket distribution resembles real hashes.
void make_key(uint32_t i, uint8_t out[16]) {
	const uint32_t h = i * 2654435761u;
	for (uint8_t b = 0; b < 16; b++) {
		out[b] = (uint8_t)(h >> ((b % 4) * 8)) ^ (uint8_t)(b * 31u + i);
	}
}

// ts_base of 0 means "let the store stamp it", which is what production does.
// A non-zero base stamps each record one second apart, which is the only way to
// get a deterministic recency order out of a store whose timestamps have
// one-second granularity - see test_capped_store_evicts_oldest_first.
void fill(Store& store, uint32_t from, uint32_t to, uint32_t ts_base = 0) {
	uint8_t key[16];
	const uint8_t payload[8] = { 0 };
	for (uint32_t i = from; i < to; i++) {
		make_key(i, key);
		if (ts_base == 0) {
			store.put(key, sizeof(key), payload, sizeof(payload));
		}
		else {
			store.put(key, sizeof(key), payload, sizeof(payload), 0, ts_base + i);
		}
	}
}

// Each test gets its own directory so a leftover index cannot leak between
// them; StdioFileSystem writes real files.
microStore::Adapters::StdioFileSystem g_fs("./");

bool open_store(Store& store, const char* dir) {
	return store.init(g_fs, dir, /*clearOnInit=*/true, 0, 0);
}

const uint32_t INSERTS = 300;
const uint32_t CAP     = 100;

} // namespace

void setUp(void) {}
void tearDown(void) {}

// The default. Nothing calls set_max_recs, so nothing bounds the index.
void test_uncapped_store_keeps_every_record(void) {
	Store store(65536, 4);
	TEST_ASSERT_TRUE(open_store(store, "./.pio/test_store_uncapped/"));
	TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)store.size());

	fill(store, 0, INSERTS);

	// Every distinct key is retained: the index grew with the insert count.
	TEST_ASSERT_EQUAL_UINT32(INSERTS, (uint32_t)store.size());
}

// The same store with the cap applied holds at the cap.
void test_capped_store_holds_at_cap(void) {
	Store store(65536, 4);
	TEST_ASSERT_TRUE(open_store(store, "./.pio/test_store_capped/"));
	store.set_max_recs(CAP);

	fill(store, 0, INSERTS);

	TEST_ASSERT_EQUAL_UINT32(CAP, (uint32_t)store.size());
}

// Eviction is oldest-first by timestamp, so the most recently stamped keys
// survive. This is what makes the timestamp semantics load bearing: whatever
// refreshes that field decides who gets evicted. Records are stamped a second
// apart here, because at the store's real granularity they would all tie - see
// the next test.
void test_capped_store_evicts_oldest_first(void) {
	Store store(65536, 4);
	TEST_ASSERT_TRUE(open_store(store, "./.pio/test_store_recency/"));
	store.set_max_recs(CAP);

	fill(store, 0, INSERTS, /*ts_base=*/1000000);

	uint8_t newest[16];
	uint8_t oldest[16];
	make_key(INSERTS - 1, newest);
	make_key(0, oldest);

	TEST_ASSERT_EQUAL_UINT32(CAP, (uint32_t)store.size());
	TEST_ASSERT_TRUE(store.exists(newest, sizeof(newest)));
	TEST_ASSERT_FALSE(store.exists(oldest, sizeof(oldest)));
}

// Timestamps have one-second granularity, so a burst of inserts inside the same
// second ties, and eviction order among ties is whatever the hash map iteration
// yields. The cap still holds - that part is guaranteed - but which records
// survive is not recency ordered at all.
//
// This is why a burst of announces cannot be relied on to evict sensibly, and
// why the first record inserted can outlive the last. Asserted here as observed
// behaviour so a future change to timestamp resolution shows up as a failure.
void test_same_second_inserts_do_not_evict_by_recency(void) {
	Store store(65536, 4);
	TEST_ASSERT_TRUE(open_store(store, "./.pio/test_store_ties/"));
	store.set_max_recs(CAP);

	fill(store, 0, INSERTS);

	uint8_t oldest[16];
	make_key(0, oldest);

	TEST_ASSERT_EQUAL_UINT32(CAP, (uint32_t)store.size());
	// The very first key survives 300 inserts at a cap of 100.
	TEST_ASSERT_TRUE(store.exists(oldest, sizeof(oldest)));
}

int main(int, char**) {
	UNITY_BEGIN();
	RUN_TEST(test_uncapped_store_keeps_every_record);
	RUN_TEST(test_capped_store_holds_at_cap);
	RUN_TEST(test_capped_store_evicts_oldest_first);
	RUN_TEST(test_same_second_inserts_do_not_evict_by_recency);
	return UNITY_END();
}
