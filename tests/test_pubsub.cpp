/**
 * @file test_pubsub.cpp
 * @brief Unit tests for the PubSubManager component.
 *
 * Since we can't use real socket FDs in unit tests, we use fake FD
 * numbers (100, 101, 102) and capture publish() output via a lambda
 * callback instead of writing to actual sockets.
 */

#include <gtest/gtest.h>

#include <string>
#include <utility>
#include <vector>

#include "pubsub/PubSubManager.h"

using namespace flashdb;

class PubSubTest : public ::testing::Test {
protected:
    PubSubManager pubsub;

    // Capture messages sent by publish() for verification
    std::vector<std::pair<int, std::string>> capturedMessages;

    // Write function that captures messages instead of writing to sockets
    std::function<void(int, const std::string&)> captureWriteFn() {
        return [this](int fd, const std::string& msg) {
            capturedMessages.emplace_back(fd, msg);
        };
    }
};

// ============================================================================
// Subscribe / Unsubscribe
// ============================================================================

TEST_F(PubSubTest, SubscribeReturnsCount) {
    int count1 = pubsub.subscribe("news", 100);
    EXPECT_EQ(count1, 1);

    int count2 = pubsub.subscribe("sports", 100);
    EXPECT_EQ(count2, 2);

    int count3 = pubsub.subscribe("tech", 100);
    EXPECT_EQ(count3, 3);
}

TEST_F(PubSubTest, UnsubscribeReturnsRemainingCount) {
    pubsub.subscribe("news", 100);
    pubsub.subscribe("sports", 100);

    int remaining = pubsub.unsubscribe("news", 100);
    EXPECT_EQ(remaining, 1);

    remaining = pubsub.unsubscribe("sports", 100);
    EXPECT_EQ(remaining, 0);
}

// ============================================================================
// Publish
// ============================================================================

TEST_F(PubSubTest, PublishToSubscribers) {
    pubsub.subscribe("news", 100);
    pubsub.subscribe("news", 101);

    int receivers = pubsub.publish("news", "breaking update", captureWriteFn());
    EXPECT_EQ(receivers, 2);
    EXPECT_EQ(capturedMessages.size(), 2u);
}

TEST_F(PubSubTest, PublishToEmptyChannel) {
    int receivers = pubsub.publish("empty_channel", "hello", captureWriteFn());
    EXPECT_EQ(receivers, 0);
    EXPECT_EQ(capturedMessages.size(), 0u);
}

TEST_F(PubSubTest, PublishOnlyToCorrectChannel) {
    pubsub.subscribe("news", 100);
    pubsub.subscribe("sports", 101);

    int receivers = pubsub.publish("news", "update", captureWriteFn());
    EXPECT_EQ(receivers, 1);
    ASSERT_EQ(capturedMessages.size(), 1u);
    EXPECT_EQ(capturedMessages[0].first, 100);  // Only the news subscriber
}

// ============================================================================
// Client Lifecycle
// ============================================================================

TEST_F(PubSubTest, RemoveClientCleansUp) {
    pubsub.subscribe("news", 100);
    pubsub.subscribe("sports", 100);
    pubsub.subscribe("tech", 100);

    EXPECT_TRUE(pubsub.isSubscribed(100));

    pubsub.removeClient(100);
    EXPECT_FALSE(pubsub.isSubscribed(100));

    // Publishing to the channels should not reach the removed client
    int receivers = pubsub.publish("news", "test", captureWriteFn());
    EXPECT_EQ(receivers, 0);
}

TEST_F(PubSubTest, IsSubscribedTrue) {
    pubsub.subscribe("news", 100);
    EXPECT_TRUE(pubsub.isSubscribed(100));
}

TEST_F(PubSubTest, IsSubscribedFalse) {
    EXPECT_FALSE(pubsub.isSubscribed(999));
}

// ============================================================================
// Multiple Channels & Clients
// ============================================================================

TEST_F(PubSubTest, MultipleChannelsIndependent) {
    pubsub.subscribe("ch1", 100);
    pubsub.subscribe("ch2", 101);
    pubsub.subscribe("ch3", 102);

    // Publish to ch1 — only client 100
    int r1 = pubsub.publish("ch1", "msg1", captureWriteFn());
    EXPECT_EQ(r1, 1);

    capturedMessages.clear();

    // Publish to ch2 — only client 101
    int r2 = pubsub.publish("ch2", "msg2", captureWriteFn());
    EXPECT_EQ(r2, 1);

    capturedMessages.clear();

    // Publish to ch3 — only client 102
    int r3 = pubsub.publish("ch3", "msg3", captureWriteFn());
    EXPECT_EQ(r3, 1);
}
