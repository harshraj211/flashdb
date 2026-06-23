/**
 * @file test_transactions.cpp
 * @brief Unit tests for the TransactionManager component.
 *
 * Tests MULTI/EXEC/DISCARD transaction lifecycle, error handling,
 * and client session management.
 */

#include <gtest/gtest.h>

#include "parser/CommandParser.h"
#include "transactions/TransactionManager.h"

using namespace flashdb;

class TransactionTest : public ::testing::Test {
protected:
    TransactionManager txManager;
    static constexpr int CLIENT_FD = 100;

    // Helper to create a Command struct for queuing
    Command makeCommand(const std::string& name,
                        std::vector<std::string> args = {}) {
        Command cmd;
        cmd.name = name;
        cmd.args = std::move(args);
        cmd.valid = true;
        return cmd;
    }
};

// ============================================================================
// MULTI — Begin Transaction
// ============================================================================

TEST_F(TransactionTest, BeginTransaction) {
    std::string result = txManager.beginTransaction(CLIENT_FD);
    EXPECT_EQ(result, "OK");
    EXPECT_EQ(txManager.getState(CLIENT_FD), TransactionState::QUEUING);
}

TEST_F(TransactionTest, NestedMultiError) {
    txManager.beginTransaction(CLIENT_FD);
    std::string result = txManager.beginTransaction(CLIENT_FD);
    // Should return an error about nested MULTI
    EXPECT_NE(result, "OK");
    EXPECT_NE(result.find("nested"), std::string::npos);
}

// ============================================================================
// Queue Commands
// ============================================================================

TEST_F(TransactionTest, QueueCommand) {
    txManager.beginTransaction(CLIENT_FD);
    auto cmd = makeCommand("SET", {"key", "value"});
    std::string result = txManager.queueCommand(CLIENT_FD, cmd);
    EXPECT_EQ(result, "QUEUED");
}

// ============================================================================
// EXEC — Execute Transaction
// ============================================================================

TEST_F(TransactionTest, ExecuteReturnsQueuedCommands) {
    txManager.beginTransaction(CLIENT_FD);
    txManager.queueCommand(CLIENT_FD, makeCommand("SET", {"a", "10"}));
    txManager.queueCommand(CLIENT_FD, makeCommand("SET", {"b", "20"}));
    txManager.queueCommand(CLIENT_FD, makeCommand("GET", {"a"}));

    auto commands = txManager.executeTransaction(CLIENT_FD);
    EXPECT_EQ(commands.size(), 3u);
    EXPECT_EQ(commands[0].name, "SET");
    EXPECT_EQ(commands[1].name, "SET");
    EXPECT_EQ(commands[2].name, "GET");
}

TEST_F(TransactionTest, ExecuteResetsStateToNone) {
    txManager.beginTransaction(CLIENT_FD);
    txManager.queueCommand(CLIENT_FD, makeCommand("SET", {"k", "v"}));
    txManager.executeTransaction(CLIENT_FD);

    EXPECT_EQ(txManager.getState(CLIENT_FD), TransactionState::NONE);
    EXPECT_FALSE(txManager.inTransaction(CLIENT_FD));
}

TEST_F(TransactionTest, ExecuteEmptyQueue) {
    txManager.beginTransaction(CLIENT_FD);
    // Execute without queuing anything
    auto commands = txManager.executeTransaction(CLIENT_FD);
    EXPECT_TRUE(commands.empty());
    EXPECT_EQ(txManager.getState(CLIENT_FD), TransactionState::NONE);
}

// ============================================================================
// DISCARD — Abort Transaction
// ============================================================================

TEST_F(TransactionTest, DiscardClearsQueue) {
    txManager.beginTransaction(CLIENT_FD);
    txManager.queueCommand(CLIENT_FD, makeCommand("SET", {"x", "1"}));
    txManager.queueCommand(CLIENT_FD, makeCommand("SET", {"y", "2"}));

    std::string result = txManager.discardTransaction(CLIENT_FD);
    EXPECT_EQ(result, "OK");
    EXPECT_EQ(txManager.getState(CLIENT_FD), TransactionState::NONE);
}

TEST_F(TransactionTest, DiscardWithoutMultiError) {
    std::string result = txManager.discardTransaction(CLIENT_FD);
    EXPECT_NE(result, "OK");
    EXPECT_NE(result.find("without MULTI"), std::string::npos);
}

// ============================================================================
// State Queries
// ============================================================================

TEST_F(TransactionTest, InTransactionTrue) {
    txManager.beginTransaction(CLIENT_FD);
    EXPECT_TRUE(txManager.inTransaction(CLIENT_FD));
}

TEST_F(TransactionTest, InTransactionFalseInitially) {
    EXPECT_FALSE(txManager.inTransaction(999));
}

// ============================================================================
// Client Cleanup
// ============================================================================

TEST_F(TransactionTest, RemoveClientCleansUp) {
    txManager.beginTransaction(CLIENT_FD);
    txManager.queueCommand(CLIENT_FD, makeCommand("SET", {"k", "v"}));

    txManager.removeClient(CLIENT_FD);
    EXPECT_FALSE(txManager.inTransaction(CLIENT_FD));
    EXPECT_EQ(txManager.getState(CLIENT_FD), TransactionState::NONE);
}
