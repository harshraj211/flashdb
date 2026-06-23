/**
 * @file test_storage.cpp
 * @brief Unit tests for the StorageEngine component.
 *
 * Tests CRUD operations, thread safety, and edge cases.
 * StorageEngine is tested without an ExpiryManager — the engine
 * handles nullptr expiryMgr_ gracefully by skipping expiry checks.
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <thread>
#include <vector>

#include "storage/StorageEngine.h"

using namespace flashdb;

class StorageTest : public ::testing::Test {
protected:
    StorageEngine storage;
};

// ============================================================================
// Basic CRUD
// ============================================================================

TEST_F(StorageTest, SetAndGet) {
    EXPECT_TRUE(storage.set("name", "Harsh"));
    auto val = storage.get("name");
    ASSERT_TRUE(val.has_value());
    EXPECT_EQ(*val, "Harsh");
}

TEST_F(StorageTest, GetNonExistent) {
    auto val = storage.get("nope");
    EXPECT_FALSE(val.has_value());
}

TEST_F(StorageTest, OverwriteValue) {
    storage.set("key", "value1");
    storage.set("key", "value2");
    auto val = storage.get("key");
    ASSERT_TRUE(val.has_value());
    EXPECT_EQ(*val, "value2");
}

// ============================================================================
// DEL
// ============================================================================

TEST_F(StorageTest, DeleteSingle) {
    storage.set("key", "value");
    int deleted = storage.del({"key"});
    EXPECT_EQ(deleted, 1);
    EXPECT_FALSE(storage.get("key").has_value());
}

TEST_F(StorageTest, DeleteMultiple) {
    storage.set("k1", "v1");
    storage.set("k2", "v2");
    storage.set("k3", "v3");
    int deleted = storage.del({"k1", "k2"});
    EXPECT_EQ(deleted, 2);
    // k3 should still exist
    EXPECT_TRUE(storage.get("k3").has_value());
    EXPECT_FALSE(storage.get("k1").has_value());
    EXPECT_FALSE(storage.get("k2").has_value());
}

TEST_F(StorageTest, DeleteNonExistent) {
    int deleted = storage.del({"nope"});
    EXPECT_EQ(deleted, 0);
}

// ============================================================================
// EXISTS
// ============================================================================

TEST_F(StorageTest, ExistsTrue) {
    storage.set("key", "value");
    EXPECT_TRUE(storage.exists("key"));
}

TEST_F(StorageTest, ExistsFalse) {
    EXPECT_FALSE(storage.exists("nope"));
}

// ============================================================================
// KEYS
// ============================================================================

TEST_F(StorageTest, KeysList) {
    storage.set("alpha", "1");
    storage.set("beta", "2");
    storage.set("gamma", "3");
    auto allKeys = storage.keys();
    EXPECT_EQ(allKeys.size(), 3u);

    // Sort for deterministic comparison (unordered_map gives arbitrary order)
    std::sort(allKeys.begin(), allKeys.end());
    EXPECT_EQ(allKeys[0], "alpha");
    EXPECT_EQ(allKeys[1], "beta");
    EXPECT_EQ(allKeys[2], "gamma");
}

// ============================================================================
// FLUSHALL & SIZE
// ============================================================================

TEST_F(StorageTest, FlushAll) {
    storage.set("k1", "v1");
    storage.set("k2", "v2");
    storage.set("k3", "v3");
    EXPECT_EQ(storage.size(), 3u);

    storage.flushAll();
    EXPECT_EQ(storage.size(), 0u);
    EXPECT_FALSE(storage.get("k1").has_value());
}

TEST_F(StorageTest, SizeTracking) {
    EXPECT_EQ(storage.size(), 0u);
    storage.set("a", "1");
    EXPECT_EQ(storage.size(), 1u);
    storage.set("b", "2");
    EXPECT_EQ(storage.size(), 2u);
    storage.set("c", "3");
    EXPECT_EQ(storage.size(), 3u);
    storage.del({"a"});
    EXPECT_EQ(storage.size(), 2u);
}

// ============================================================================
// getAllEntries (used by replication)
// ============================================================================

TEST_F(StorageTest, GetAllEntries) {
    storage.set("name", "Harsh");
    storage.set("city", "Delhi");
    auto entries = storage.getAllEntries();
    EXPECT_EQ(entries.size(), 2u);

    // Verify both entries exist (order is arbitrary)
    bool foundName = false, foundCity = false;
    for (const auto& [k, v] : entries) {
        if (k == "name" && v == "Harsh") foundName = true;
        if (k == "city" && v == "Delhi") foundCity = true;
    }
    EXPECT_TRUE(foundName);
    EXPECT_TRUE(foundCity);
}

// ============================================================================
// Concurrent Access (Thread Safety)
// ============================================================================

TEST_F(StorageTest, ConcurrentReadWrite) {
    constexpr int NUM_WRITERS = 10;
    constexpr int NUM_READERS = 10;
    constexpr int OPS_PER_THREAD = 1000;

    std::vector<std::thread> threads;

    // Writer threads: each sets keys with a unique prefix
    for (int w = 0; w < NUM_WRITERS; ++w) {
        threads.emplace_back([&, w]() {
            for (int i = 0; i < OPS_PER_THREAD; ++i) {
                std::string key = "w" + std::to_string(w) + "_k" + std::to_string(i);
                storage.set(key, "value_" + std::to_string(i));
            }
        });
    }

    // Reader threads: continuously read (may get nullopt or value — both are fine)
    for (int r = 0; r < NUM_READERS; ++r) {
        threads.emplace_back([&, r]() {
            for (int i = 0; i < OPS_PER_THREAD; ++i) {
                std::string key = "w" + std::to_string(r % NUM_WRITERS) + "_k" + std::to_string(i);
                auto val = storage.get(key);
                // We don't assert the value — we just verify no crash or data corruption
                if (val.has_value()) {
                    EXPECT_FALSE(val->empty());
                }
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    // After all writers complete, every key should exist
    for (int w = 0; w < NUM_WRITERS; ++w) {
        for (int i = 0; i < OPS_PER_THREAD; ++i) {
            std::string key = "w" + std::to_string(w) + "_k" + std::to_string(i);
            auto val = storage.get(key);
            ASSERT_TRUE(val.has_value()) << "Missing key: " << key;
        }
    }
}
