// touch() semantics, which are what make eviction mean "least recently used"
// rather than "least recently announced".
//
// Reticulum's reference implementation refreshes a path's timestamp whenever it
// forwards to that destination, so DESTINATION_TIMEOUT means "unused for a
// week". On a flash-backed store the obvious way to reproduce that - re-put()
// the record - costs a segment write per forwarded packet. touch() updates the
// in-memory index timestamp instead, which is what the record cap orders by and
// what TTL is measured against.

#include <unity.h>

#include <microStore/FileStore.h>
#include <microStore/Adapters/StdioFileSystem.h>

#include <cstdint>
#include <memory>

using Store = microStore::BasicFileStore<std::allocator<uint8_t>>;

namespace {

void make_key(uint32_t i, uint8_t out[16]) {
	const uint32_t h = i * 2654435761u;
	for (uint8_t b = 0; b < 16; b++) {
		out[b] = (uint8_t)(h >> ((b % 4) * 8)) ^ (uint8_t)(b * 31u + i);
	}
}

void put_at(Store& store, uint32_t i, uint32_t ts) {
	uint8_t key[16];
	const uint8_t payload[8] = { 0 };
	make_key(i, key);
	store.put(key, sizeof(key), payload, sizeof(payload), 0, ts);
}

microStore::Adapters::StdioFileSystem g_fs("./");

bool open_store(Store& store, const char* dir) {
	return store.init(g_fs, dir, /*clearOnInit=*/true, 0, 0);
}

const uint32_t TS_BASE = 1000000;

} // namespace

void setUp(void) {}
void tearDown(void) {}

// The point of the whole exercise: a record that is touched survives an
// eviction that would otherwise have taken it, without any flash write.
void test_touched_record_survives_eviction(void) {
	Store store(65536, 4);
	TEST_ASSERT_TRUE(open_store(store, "./.pio/test_touch_survives/"));
	store.set_max_recs(10);

	// Ten records, oldest first. Record 0 is the eviction candidate.
	for (uint32_t i = 0; i < 10; i++) put_at(store, i, TS_BASE + i);

	uint8_t oldest[16];
	make_key(0, oldest);

	// Mark it as still in use, newer than anything written so far.
	TEST_ASSERT_TRUE(store.touch(oldest, sizeof(oldest), TS_BASE + 100));

	// Now force evictions. Without the touch, record 0 goes first.
	for (uint32_t i = 10; i < 20; i++) put_at(store, i, TS_BASE + i);

	TEST_ASSERT_EQUAL_UINT32(10, (uint32_t)store.size());
	TEST_ASSERT_TRUE(store.exists(oldest, sizeof(oldest)));
}

// The control for the above: same sequence, no touch, record 0 is gone.
void test_untouched_record_is_evicted(void) {
	Store store(65536, 4);
	TEST_ASSERT_TRUE(open_store(store, "./.pio/test_touch_control/"));
	store.set_max_recs(10);

	for (uint32_t i = 0; i < 20; i++) put_at(store, i, TS_BASE + i);

	uint8_t oldest[16];
	make_key(0, oldest);

	TEST_ASSERT_EQUAL_UINT32(10, (uint32_t)store.size());
	TEST_ASSERT_FALSE(store.exists(oldest, sizeof(oldest)));
}

// touch() must not resurrect or invent records.
void test_touch_missing_key_returns_false(void) {
	Store store(65536, 4);
	TEST_ASSERT_TRUE(open_store(store, "./.pio/test_touch_missing/"));

	uint8_t absent[16];
	make_key(9999, absent);

	TEST_ASSERT_FALSE(store.touch(absent, sizeof(absent)));
	TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)store.size());
}

// Touching restarts the TTL, which is the other half of "unused for a week".
// Written with a TTL of 10s at an old timestamp, the record is expired; a touch
// with a current timestamp brings it back into life.
void test_touch_restarts_ttl(void) {
	Store store(65536, 4);
	TEST_ASSERT_TRUE(open_store(store, "./.pio/test_touch_ttl/"));

	uint8_t key[16];
	const uint8_t payload[8] = { 0 };
	make_key(1, key);

	const uint32_t now = microStore::time();
	// Stamped 100 s ago with a 10 s TTL: already expired.
	store.put(key, sizeof(key), payload, sizeof(payload), /*ttl=*/10, now - 100);

	// An expired record cannot be touched, and touching it reaps it.
	TEST_ASSERT_FALSE(store.touch(key, sizeof(key), now));
	TEST_ASSERT_FALSE(store.exists(key, sizeof(key)));

	// A live record can be, and the refreshed stamp is what TTL measures from.
	store.put(key, sizeof(key), payload, sizeof(payload), /*ttl=*/10, now);
	TEST_ASSERT_TRUE(store.touch(key, sizeof(key), now));
	TEST_ASSERT_TRUE(store.exists(key, sizeof(key)));
}

int main(int, char**) {
	UNITY_BEGIN();
	RUN_TEST(test_touched_record_survives_eviction);
	RUN_TEST(test_untouched_record_is_evicted);
	RUN_TEST(test_touch_missing_key_returns_false);
	RUN_TEST(test_touch_restarts_ttl);
	return UNITY_END();
}
