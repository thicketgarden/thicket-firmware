// Protected-record eviction: soft priority.
//
// The record cap still holds exactly. Protection is a preference, not a
// guarantee: victims come from the unprotected records oldest-first, and only
// when none are left does eviction fall through to the oldest protected one.
//
// This is what keeps a contact's path alive under an announce flood from
// strangers. It's load bearing rather than a nicety, because microStore
// timestamps have one-second granularity: inside a single second every stamp
// ties and recency ordering degrades to hash-map order, so nothing else is
// keeping that path in the table.

#include <unity.h>

#include <microStore/FileStore.h>
#include <microStore/Adapters/StdioFileSystem.h>

#include <cstdint>
#include <memory>
#include <set>
#include <vector>

using Store = microStore::BasicFileStore<std::allocator<uint8_t>>;

namespace {

void make_key(uint32_t i, uint8_t out[16]) {
	const uint32_t h = i * 2654435761u;
	for (uint8_t b = 0; b < 16; b++) {
		out[b] = (uint8_t)(h >> ((b % 4) * 8)) ^ (uint8_t)(b * 31u + i);
	}
}

std::vector<uint8_t> key_of(uint32_t i) {
	uint8_t k[16];
	make_key(i, k);
	return std::vector<uint8_t>(k, k + 16);
}

void put_at(Store& store, uint32_t i, uint32_t ts) {
	uint8_t key[16];
	const uint8_t payload[8] = { 0 };
	make_key(i, key);
	store.put(key, sizeof(key), payload, sizeof(payload), 0, ts);
}

bool exists(Store& store, uint32_t i) {
	uint8_t key[16];
	make_key(i, key);
	return store.exists(key, sizeof(key));
}

// The protected set, and the callback the store asks. Deliberately a plain set
// lookup: the store calls this once per indexed record per prune.
std::set<std::vector<uint8_t>> g_protected;

bool protect_cb(const uint8_t* key, uint8_t key_len, void* /*ctx*/) {
	return g_protected.count(std::vector<uint8_t>(key, key + key_len)) > 0;
}

microStore::Adapters::StdioFileSystem g_fs("./");

bool open_store(Store& store, const char* dir) {
	g_protected.clear();
	return store.init(g_fs, dir, /*clearOnInit=*/true, 0, 0);
}

const uint32_t TS_BASE = 1000000;

} // namespace

void setUp(void) {}
void tearDown(void) {}

// The headline: the oldest record in the store survives a flood because it is
// protected, and the cap is still honoured exactly.
void test_protected_record_survives_flood(void) {
	Store store(65536, 4);
	TEST_ASSERT_TRUE(open_store(store, "./.pio/test_protect_survives/"));
	store.set_max_recs(10);
	store.set_protect_fn(protect_cb);

	// Record 0 is both the oldest and the one we care about.
	g_protected.insert(key_of(0));

	for (uint32_t i = 0; i < 40; i++) put_at(store, i, TS_BASE + i);

	TEST_ASSERT_EQUAL_UINT32(10, (uint32_t)store.size());
	TEST_ASSERT_TRUE(exists(store, 0));
}

// Same flood, same record, no protection: it goes. Without this the test above
// proves nothing.
void test_unprotected_control_is_evicted(void) {
	Store store(65536, 4);
	TEST_ASSERT_TRUE(open_store(store, "./.pio/test_protect_control/"));
	store.set_max_recs(10);
	store.set_protect_fn(protect_cb);
	// g_protected intentionally left empty.

	for (uint32_t i = 0; i < 40; i++) put_at(store, i, TS_BASE + i);

	TEST_ASSERT_EQUAL_UINT32(10, (uint32_t)store.size());
	TEST_ASSERT_FALSE(exists(store, 0));
}

// Soft priority: protecting more records than the cap must not grow the table.
// The cap wins; the oldest protected record is taken.
void test_cap_holds_when_everything_is_protected(void) {
	Store store(65536, 4);
	TEST_ASSERT_TRUE(open_store(store, "./.pio/test_protect_overflow/"));
	store.set_max_recs(10);
	store.set_protect_fn(protect_cb);

	for (uint32_t i = 0; i < 40; i++) g_protected.insert(key_of(i));
	for (uint32_t i = 0; i < 40; i++) put_at(store, i, TS_BASE + i);

	// Never exceeds the cap, and the victim was the oldest protected record.
	TEST_ASSERT_EQUAL_UINT32(10, (uint32_t)store.size());
	TEST_ASSERT_FALSE(exists(store, 0));
	TEST_ASSERT_TRUE(exists(store, 39));
}

// Protection is consulted per prune, not cached, so unpinning takes effect
// immediately on the next eviction.
void test_unprotecting_makes_a_record_evictable(void) {
	Store store(65536, 4);
	TEST_ASSERT_TRUE(open_store(store, "./.pio/test_protect_release/"));
	store.set_max_recs(10);
	store.set_protect_fn(protect_cb);

	g_protected.insert(key_of(0));
	for (uint32_t i = 0; i < 20; i++) put_at(store, i, TS_BASE + i);
	TEST_ASSERT_TRUE(exists(store, 0));

	g_protected.clear();
	for (uint32_t i = 20; i < 40; i++) put_at(store, i, TS_BASE + i);

	TEST_ASSERT_EQUAL_UINT32(10, (uint32_t)store.size());
	TEST_ASSERT_FALSE(exists(store, 0));
}

// No callback installed must behave exactly as before: plain oldest-first.
void test_no_policy_is_plain_oldest_first(void) {
	Store store(65536, 4);
	TEST_ASSERT_TRUE(open_store(store, "./.pio/test_protect_absent/"));
	store.set_max_recs(10);
	// set_protect_fn deliberately not called.

	for (uint32_t i = 0; i < 40; i++) put_at(store, i, TS_BASE + i);

	TEST_ASSERT_EQUAL_UINT32(10, (uint32_t)store.size());
	TEST_ASSERT_FALSE(exists(store, 0));
	TEST_ASSERT_TRUE(exists(store, 39));
}

int main(int, char**) {
	UNITY_BEGIN();
	RUN_TEST(test_protected_record_survives_flood);
	RUN_TEST(test_unprotected_control_is_evicted);
	RUN_TEST(test_cap_holds_when_everything_is_protected);
	RUN_TEST(test_unprotecting_makes_a_record_evictable);
	RUN_TEST(test_no_policy_is_plain_oldest_first);
	return UNITY_END();
}
