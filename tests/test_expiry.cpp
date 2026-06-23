/**
 * @file test_expiry.cpp
 * @brief Unit tests for the ExpiryManager component.
 *
 * Tests TTL setting, expiry checking, and time-based behavior.
 * Uses std::this_thread::sleep_for for timing-dependent tests.
 */

#include <gtest/gtest.h>

#include <chrono>
#include <thread>

#include "expiry/ExpiryManager.h"

using namespace flashdb;

class ExpiryTest : public ::testing::Test {
protected:
    ExpiryManager expiry;
};

// ============================================================================
// Basic TTL Operations
// ============================================================================

TEST_F(ExpiryTest, SetAndCheckTTL) {
    expiry.setExpirySeconds("key", 60);
    int ttl = expiry.getTTL("key");
    // TTL should be approximately 60 (allow 2 second tolerance for test overhead)
    EXPECT_GE(ttl, 58);
    EXPECT_LE(ttl, 60);
}

TEST_F(ExpiryTest, NotExpiredYet) {
    expiry.setExpirySeconds("key", 60);
    EXPECT_FALSE(expiry.isExpired("key"));
}

TEST_F(ExpiryTest, HasExpiryTrue) {
    expiry.setExpirySeconds("key", 60);
    EXPECT_TRUE(expiry.hasExpiry("key"));
}

TEST_F(ExpiryTest, HasExpiryFalse) {
    EXPECT_FALSE(expiry.hasExpiry("nonexistent"));
}

// ============================================================================
// TTL Return Values
// ============================================================================

TEST_F(ExpiryTest, NoExpiryReturnsMinusOne) {
    // Key with no expiry set → getTTL returns -1
    int ttl = expiry.getTTL("no_expiry_key");
    EXPECT_EQ(ttl, -1);
}

TEST_F(ExpiryTest, ExpiredKeyReturnsMinusTwo) {
    expiry.setExpirySeconds("key", 1);
    // Wait for expiry
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    int ttl = expiry.getTTL("key");
    EXPECT_EQ(ttl, -2);
}

// ============================================================================
// Expiry Behavior
// ============================================================================

TEST_F(ExpiryTest, KeyExpiresAfterTTL) {
    expiry.setExpirySeconds("key", 1);
    EXPECT_FALSE(expiry.isExpired("key"));

    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    EXPECT_TRUE(expiry.isExpired("key"));
}

TEST_F(ExpiryTest, OverwriteExpiry) {
    expiry.setExpirySeconds("key", 60);
    expiry.setExpirySeconds("key", 10);
    int ttl = expiry.getTTL("key");
    // After overwrite, TTL should be ~10, not ~60
    EXPECT_GE(ttl, 8);
    EXPECT_LE(ttl, 10);
}

TEST_F(ExpiryTest, ZeroSecondExpiresImmediately) {
    expiry.setExpirySeconds("key", 0);
    // With 0 seconds, the key should be expired immediately or within milliseconds
    // Allow a tiny sleep for the time_point to pass
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    EXPECT_TRUE(expiry.isExpired("key"));
}

// ============================================================================
// Remove & Clear
// ============================================================================

TEST_F(ExpiryTest, RemoveExpiry) {
    expiry.setExpirySeconds("key", 60);
    EXPECT_TRUE(expiry.hasExpiry("key"));

    expiry.removeExpiry("key");
    EXPECT_FALSE(expiry.hasExpiry("key"));
    // After removing expiry, getTTL should return -1 (no expiry)
    EXPECT_EQ(expiry.getTTL("key"), -1);
}

TEST_F(ExpiryTest, ClearAllExpiries) {
    expiry.setExpirySeconds("k1", 60);
    expiry.setExpirySeconds("k2", 120);
    expiry.setExpirySeconds("k3", 180);

    expiry.clear();

    EXPECT_FALSE(expiry.hasExpiry("k1"));
    EXPECT_FALSE(expiry.hasExpiry("k2"));
    EXPECT_FALSE(expiry.hasExpiry("k3"));
}
