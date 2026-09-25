/*
 * Unit tests for microStore::HeapStore — in-memory KV store.
 */

#include <unity.h>
#include <microStore/HeapStore.h>

#include <cstring>
#include <cstdio>
#include <iterator>
#include <map>
#include <string>
#include <vector>

using microStore::HeapStore;

/* ---- Tests ---- */

void test_heap_store_put_get() {
    HeapStore store;
    store.init();

    const char* val = "hello";
    TEST_ASSERT_TRUE(store.put("mykey", val, (uint16_t)strlen(val)));

    char buf[32];
    uint16_t size = sizeof(buf);
    TEST_ASSERT_TRUE(store.get("mykey", buf, &size));
    TEST_ASSERT_EQUAL(5u, size);
    TEST_ASSERT_EQUAL(0, memcmp(buf, "hello", 5));
}

void test_heap_store_overwrite() {
    HeapStore store;
    store.init();

    store.put("k", "first",  5u);
    store.put("k", "second", 6u);

    char buf[32];
    uint16_t size = sizeof(buf);
    TEST_ASSERT_TRUE(store.get("k", buf, &size));
    TEST_ASSERT_EQUAL(6u, size);
    TEST_ASSERT_EQUAL(0, memcmp(buf, "second", 6));
}

void test_heap_store_remove() {
    HeapStore store;
    store.init();

    store.put("gone", "val", 3u);
    TEST_ASSERT_TRUE(store.exists("gone"));

    store.remove("gone");
    TEST_ASSERT_FALSE(store.exists("gone"));

    char buf[32];
    uint16_t size = sizeof(buf);
    TEST_ASSERT_FALSE(store.get("gone", buf, &size));
}

void test_heap_store_size() {
    HeapStore store;
    store.init();

    TEST_ASSERT_EQUAL(0u, store.size());

    store.put("a", "1", 1u);
    TEST_ASSERT_EQUAL(1u, store.size());

    store.put("b", "2", 1u);
    TEST_ASSERT_EQUAL(2u, store.size());

    // Overwrite does not increase size
    store.put("a", "x", 1u);
    TEST_ASSERT_EQUAL(2u, store.size());

    store.remove("a");
    TEST_ASSERT_EQUAL(1u, store.size());
}

void test_heap_store_clear() {
    HeapStore store;
    store.init();

    store.put("x", "1", 1u);
    store.put("y", "2", 1u);
    TEST_ASSERT_EQUAL(2u, store.size());

    store.clear();
    TEST_ASSERT_EQUAL(0u, store.size());
    TEST_ASSERT_FALSE(store.exists("x"));
    TEST_ASSERT_FALSE(store.exists("y"));
}

void test_heap_store_iterator() {
    HeapStore store;
    store.init();

    store.put("alpha", "AAA", 3u, /*ttl=*/0, 1u);
    store.put("beta",  "BB",  2u, /*ttl=*/0, 2u);
    store.put("gamma", "C",   1u, /*ttl=*/0, 3u);

    std::map<std::string, std::string> seen;
    for (auto& e : store) {
        std::string k(e.key.begin(), e.key.end());
        std::string v(e.value.begin(), e.value.end());
        seen[k] = v;
    }

    TEST_ASSERT_EQUAL(3, (int)seen.size());
    TEST_ASSERT_EQUAL_STRING("AAA", seen["alpha"].c_str());
    TEST_ASSERT_EQUAL_STRING("BB",  seen["beta"].c_str());
    TEST_ASSERT_EQUAL_STRING("C",   seen["gamma"].c_str());
}

void test_heap_store_policy_max_recs() {
    HeapStore store;
    store.init();

    // Allow at most 2 records; oldest is evicted when limit is exceeded
    store.set_ttl_secs(0);
    store.set_max_recs(2);

    store.put("first",  "1", 1u, /*ttl=*/0, 1u);
    store.put("second", "2", 1u, /*ttl=*/0, 2u);
    TEST_ASSERT_EQUAL(2u, store.size());

    // Adding a third key evicts the smallest (first alphabetically — "first")
    store.put("third", "3", 1u, /*ttl=*/0, 3u);
    TEST_ASSERT_EQUAL(2u, store.size());

    // Overwriting an existing key does not evict
    store.put("second", "updated", 7u, /*ttl=*/0, 4u);
    TEST_ASSERT_EQUAL(2u, store.size());
    TEST_ASSERT_TRUE(store.exists("second"));
}

/* ---- TTL tests ---- */

void test_heap_store_ttl_expires_on_get() {
    HeapStore store;
    store.init();
    store.set_ttl_secs(60);

    // Store a record with a very old timestamp (epoch second 1).
    store.put("k", "hello", 5u, /*ttl=*/0, /*ts=*/1u);
    TEST_ASSERT_EQUAL(1u, store.size());

    // get() must detect expiry, evict the record, and return false.
    char buf[32];
    uint16_t sz = sizeof(buf);
    TEST_ASSERT_FALSE(store.get("k", buf, &sz));

    // Record should now be gone from the store.
    TEST_ASSERT_EQUAL(0u, store.size());
    TEST_ASSERT_FALSE(store.exists("k"));
}

void test_heap_store_ttl_keeps_fresh() {
    HeapStore store;
    store.init();
    store.set_ttl_secs(3600);  // 1-hour TTL

    uint32_t now = microStore::time();
    store.put("k", "world", 5u, /*ttl=*/0, now);

    char buf[32];
    uint16_t sz = sizeof(buf);
    TEST_ASSERT_TRUE(store.get("k", buf, &sz));
    TEST_ASSERT_EQUAL(5u, sz);
    TEST_ASSERT_EQUAL(0, memcmp(buf, "world", 5));
}

void test_heap_store_ttl_sweep_on_put() {
    HeapStore store;
    store.init();

    // Insert two records with very old timestamps while TTL is still disabled,
    // so no sweep occurs and both records are retained.
    store.put("old1", "a", 1u, /*ttl=*/0, /*ts=*/1u);
    store.put("old2", "b", 1u, /*ttl=*/0, /*ts=*/2u);
    TEST_ASSERT_EQUAL(2u, store.size());

    // Enable TTL and trigger a new put() — the sweep must remove old1/old2.
    store.set_ttl_secs(60);
    uint32_t now = microStore::time();
    store.put("fresh", "c", 1u, /*ttl=*/0, now);

    TEST_ASSERT_EQUAL(1u, store.size());
    TEST_ASSERT_FALSE(store.exists("old1"));
    TEST_ASSERT_FALSE(store.exists("old2"));
    TEST_ASSERT_TRUE(store.exists("fresh"));
}

void test_heap_store_ttl_zero_disables() {
    HeapStore store;
    store.init();
    // TTL=0 is the default; no expiration should occur regardless of timestamp.
    store.put("k", "v", 1u, /*ttl=*/0, /*ts=*/1u);
    store.put("other", "x", 1u, /*ttl=*/0, /*ts=*/1u);  // trigger any sweep logic

    TEST_ASSERT_EQUAL(2u, store.size());
    TEST_ASSERT_TRUE(store.exists("k"));
}

/* ---- Edge-case tests ---- */

void test_heap_store_exists() {
    HeapStore store;
    store.init();

    TEST_ASSERT_FALSE(store.exists("missing"));
    store.put("present", "v", 1u);
    TEST_ASSERT_TRUE(store.exists("present"));
    store.remove("present");
    TEST_ASSERT_FALSE(store.exists("present"));
}

void test_heap_store_get_missing() {
    HeapStore store;
    store.init();

    char buf[32];
    uint16_t sz = sizeof(buf);
    TEST_ASSERT_FALSE(store.get("nope", buf, &sz));
}

void test_heap_store_remove_nonexistent() {
    HeapStore store;
    store.init();

    // Should not crash and should return true (erase on absent key is a no-op).
    TEST_ASSERT_TRUE(store.remove("ghost"));
    TEST_ASSERT_EQUAL(0u, store.size());
}

void test_heap_store_key_too_long() {
    HeapStore store;
    store.init();

    // Build a key that is exactly one byte over the limit.
    std::vector<uint8_t> long_key(USTORE_MAX_KEY_LEN + 1, 'x');

    TEST_ASSERT_FALSE(store.put(long_key, "v", 1u));
    TEST_ASSERT_FALSE(store.exists(long_key));

    char buf[32]; uint16_t sz = sizeof(buf);
    TEST_ASSERT_FALSE(store.get(long_key, buf, &sz));
    TEST_ASSERT_FALSE(store.remove(long_key));  // remove rejects oversized key
}

void test_heap_store_value_too_long() {
    HeapStore store;
    store.init();

    std::vector<uint8_t> big_val(USTORE_MAX_VALUE_LEN + 1, 0xAB);
    TEST_ASSERT_FALSE(store.put("k", big_val.data(), (uint16_t)big_val.size()));
    TEST_ASSERT_EQUAL(0u, store.size());
}

void test_heap_store_iterator_empty() {
    HeapStore store;
    store.init();

    int count = 0;
    for (auto& e : store) { (void)e; count++; }
    TEST_ASSERT_EQUAL(0, count);
}

void test_heap_store_iterator_timestamps() {
    HeapStore store;
    store.init();

    store.put("a", "A", 1u, /*ttl=*/0, /*ts=*/42u);
    store.put("b", "B", 1u, /*ttl=*/0, /*ts=*/99u);

    std::map<std::string, uint32_t> ts_map;
    for (auto& e : store) {
        std::string k(e.key.begin(), e.key.end());
        ts_map[k] = e.timestamp;
    }

    TEST_ASSERT_EQUAL(42u, ts_map["a"]);
    TEST_ASSERT_EQUAL(99u, ts_map["b"]);
}

void test_heap_store_max_recs_zero_unlimited() {
    HeapStore store;
    store.init();
    store.set_max_recs(0);  // disabled — no eviction

    for (int i = 0; i < 10; i++) {
        char key[8];
        snprintf(key, sizeof(key), "k%d", i);
        store.put(key, "v", 1u);
    }
    TEST_ASSERT_EQUAL(10u, store.size());
}

void test_heap_store_ttl_and_max_recs() {
    HeapStore store;
    store.init();
    store.set_ttl_secs(60);
    store.set_max_recs(3);

    uint32_t now = microStore::time();

    // Two fresh records, one expired.
    store.put("fresh1", "a", 1u, /*ttl=*/0, now);
    store.put("fresh2", "b", 1u, /*ttl=*/0, now);
    store.put("old",    "c", 1u, /*ttl=*/0, /*ts=*/1u);

    // A fourth put triggers TTL sweep (removes "old"), then max_recs check.
    store.put("fresh3", "d", 1u, /*ttl=*/0, now);

    // "old" swept by TTL; fresh1/fresh2/fresh3 all within max_recs=3.
    TEST_ASSERT_EQUAL(3u, store.size());
    TEST_ASSERT_FALSE(store.exists("old"));
    TEST_ASSERT_TRUE(store.exists("fresh1"));
    TEST_ASSERT_TRUE(store.exists("fresh2"));
    TEST_ASSERT_TRUE(store.exists("fresh3"));
}

/* ---- Iterator trait check ---- */

void test_heap_store_iterator_traits() {
    using It = HeapStore::iterator;
    static_assert(
        std::is_same<
            std::iterator_traits<It>::iterator_category,
            std::forward_iterator_tag
        >::value,
        "iterator_category must be forward_iterator_tag"
    );
    static_assert(
        std::is_same<
            std::iterator_traits<It>::value_type,
            HeapStore::Entry
        >::value,
        "value_type must be Entry"
    );
    TEST_PASS();
}

/* ---- Main ---- */

int runUnityTests(void) {
    UNITY_BEGIN();
    RUN_TEST(test_heap_store_put_get);
    RUN_TEST(test_heap_store_overwrite);
    RUN_TEST(test_heap_store_remove);
    RUN_TEST(test_heap_store_size);
    RUN_TEST(test_heap_store_clear);
    RUN_TEST(test_heap_store_iterator);
    RUN_TEST(test_heap_store_policy_max_recs);
    RUN_TEST(test_heap_store_iterator_traits);
    // TTL tests
    RUN_TEST(test_heap_store_ttl_expires_on_get);
    RUN_TEST(test_heap_store_ttl_keeps_fresh);
    RUN_TEST(test_heap_store_ttl_sweep_on_put);
    RUN_TEST(test_heap_store_ttl_zero_disables);
    // Edge-case tests
    RUN_TEST(test_heap_store_exists);
    RUN_TEST(test_heap_store_get_missing);
    RUN_TEST(test_heap_store_remove_nonexistent);
    RUN_TEST(test_heap_store_key_too_long);
    RUN_TEST(test_heap_store_value_too_long);
    RUN_TEST(test_heap_store_iterator_empty);
    RUN_TEST(test_heap_store_iterator_timestamps);
    RUN_TEST(test_heap_store_max_recs_zero_unlimited);
    RUN_TEST(test_heap_store_ttl_and_max_recs);
    return UNITY_END();
}

// For native dev-platform or for some embedded frameworks
int main(void) {
	return runUnityTests();
}

#ifdef ARDUINO
// For Arduino framework
void setup() {
	// Wait ~2 seconds before the Unity test runner
	// establishes connection with a board Serial interface
	delay(2000);

	runUnityTests();
}
void loop() {}
#endif

// For ESP-IDF framework
void app_main() {
	runUnityTests();
}
