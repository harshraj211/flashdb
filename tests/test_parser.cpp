/**
 * @file test_parser.cpp
 * @brief Unit tests for the CommandParser component.
 *
 * Tests tokenization, case normalization, argument validation,
 * and edge-case handling across all 16 supported commands.
 */

#include <gtest/gtest.h>

#include "parser/CommandParser.h"

using namespace flashdb;

// ============================================================================
// Basic Command Parsing
// ============================================================================

TEST(ParserTest, BasicSet) {
    auto cmd = CommandParser::parse("SET name Harsh");
    EXPECT_TRUE(cmd.valid);
    EXPECT_EQ(cmd.name, "SET");
    ASSERT_GE(cmd.args.size(), 2u);
    EXPECT_EQ(cmd.args[0], "name");
    EXPECT_EQ(cmd.args[1], "Harsh");
}

TEST(ParserTest, BasicGet) {
    auto cmd = CommandParser::parse("GET name");
    EXPECT_TRUE(cmd.valid);
    EXPECT_EQ(cmd.name, "GET");
    ASSERT_EQ(cmd.args.size(), 1u);
    EXPECT_EQ(cmd.args[0], "name");
}

TEST(ParserTest, BasicDel) {
    auto cmd = CommandParser::parse("DEL key1 key2 key3");
    EXPECT_TRUE(cmd.valid);
    EXPECT_EQ(cmd.name, "DEL");
    EXPECT_EQ(cmd.args.size(), 3u);
}

TEST(ParserTest, BasicExists) {
    auto cmd = CommandParser::parse("EXISTS mykey");
    EXPECT_TRUE(cmd.valid);
    EXPECT_EQ(cmd.name, "EXISTS");
    ASSERT_EQ(cmd.args.size(), 1u);
    EXPECT_EQ(cmd.args[0], "mykey");
}

TEST(ParserTest, KeysNoArgs) {
    auto cmd = CommandParser::parse("KEYS");
    EXPECT_TRUE(cmd.valid);
    EXPECT_EQ(cmd.name, "KEYS");
    EXPECT_EQ(cmd.args.size(), 0u);
}

TEST(ParserTest, FlushAll) {
    auto cmd = CommandParser::parse("FLUSHALL");
    EXPECT_TRUE(cmd.valid);
    EXPECT_EQ(cmd.name, "FLUSHALL");
    EXPECT_EQ(cmd.args.size(), 0u);
}

TEST(ParserTest, Ping) {
    auto cmd = CommandParser::parse("PING");
    EXPECT_TRUE(cmd.valid);
    EXPECT_EQ(cmd.name, "PING");
    EXPECT_EQ(cmd.args.size(), 0u);
}

// ============================================================================
// Case Insensitivity
// ============================================================================

TEST(ParserTest, CaseInsensitiveLowercase) {
    auto cmd = CommandParser::parse("set name harsh");
    EXPECT_TRUE(cmd.valid);
    EXPECT_EQ(cmd.name, "SET");
    // Arguments should preserve their original case
    EXPECT_EQ(cmd.args[0], "name");
    EXPECT_EQ(cmd.args[1], "harsh");
}

TEST(ParserTest, CaseInsensitiveMixedCase) {
    auto cmd = CommandParser::parse("SeT NaMe VaLuE");
    EXPECT_TRUE(cmd.valid);
    EXPECT_EQ(cmd.name, "SET");
    // Args preserve original case — only command name is uppercased
    EXPECT_EQ(cmd.args[0], "NaMe");
    EXPECT_EQ(cmd.args[1], "VaLuE");
}

// ============================================================================
// Argument Validation
// ============================================================================

TEST(ParserTest, SetMissingValue) {
    auto cmd = CommandParser::parse("SET key");
    EXPECT_FALSE(cmd.valid);
    EXPECT_FALSE(cmd.errorMessage.empty());
}

TEST(ParserTest, GetMissingKey) {
    auto cmd = CommandParser::parse("GET");
    EXPECT_FALSE(cmd.valid);
}

TEST(ParserTest, DelMissingKey) {
    auto cmd = CommandParser::parse("DEL");
    EXPECT_FALSE(cmd.valid);
}

TEST(ParserTest, ExpireNeedsTwo) {
    auto cmd = CommandParser::parse("EXPIRE key");
    EXPECT_FALSE(cmd.valid);
}

TEST(ParserTest, ExpireValid) {
    auto cmd = CommandParser::parse("EXPIRE key 30");
    EXPECT_TRUE(cmd.valid);
    EXPECT_EQ(cmd.name, "EXPIRE");
    EXPECT_EQ(cmd.args[0], "key");
    EXPECT_EQ(cmd.args[1], "30");
}

TEST(ParserTest, PublishNeedsTwo) {
    auto cmd = CommandParser::parse("PUBLISH channel");
    EXPECT_FALSE(cmd.valid);
}

TEST(ParserTest, PublishValid) {
    auto cmd = CommandParser::parse("PUBLISH channel hello world");
    EXPECT_TRUE(cmd.valid);
    EXPECT_EQ(cmd.name, "PUBLISH");
    // Extra args beyond minimum 2 are silently accepted
    EXPECT_GE(cmd.args.size(), 2u);
}

// ============================================================================
// SET with EX (Expiry)
// ============================================================================

TEST(ParserTest, SetWithExpiry) {
    auto cmd = CommandParser::parse("SET token abc EX 60");
    EXPECT_TRUE(cmd.valid);
    EXPECT_EQ(cmd.name, "SET");
    ASSERT_GE(cmd.args.size(), 4u);
    EXPECT_EQ(cmd.args[0], "token");
    EXPECT_EQ(cmd.args[1], "abc");
    EXPECT_EQ(cmd.args[2], "EX");
    EXPECT_EQ(cmd.args[3], "60");
}

// ============================================================================
// Edge Cases — Empty / Whitespace
// ============================================================================

TEST(ParserTest, EmptyInput) {
    auto cmd = CommandParser::parse("");
    EXPECT_FALSE(cmd.valid);
}

TEST(ParserTest, WhitespaceOnly) {
    auto cmd = CommandParser::parse("   ");
    EXPECT_FALSE(cmd.valid);
}

TEST(ParserTest, MultipleSpacesBetweenTokens) {
    auto cmd = CommandParser::parse("SET   name    Harsh");
    EXPECT_TRUE(cmd.valid);
    EXPECT_EQ(cmd.name, "SET");
    EXPECT_EQ(cmd.args[0], "name");
    EXPECT_EQ(cmd.args[1], "Harsh");
}

// ============================================================================
// Edge Cases — Line Endings
// ============================================================================

TEST(ParserTest, TrailingCRLF) {
    auto cmd = CommandParser::parse("GET name\r\n");
    EXPECT_TRUE(cmd.valid);
    EXPECT_EQ(cmd.name, "GET");
    ASSERT_EQ(cmd.args.size(), 1u);
    EXPECT_EQ(cmd.args[0], "name");
}

TEST(ParserTest, TrailingLF) {
    auto cmd = CommandParser::parse("GET name\n");
    EXPECT_TRUE(cmd.valid);
    EXPECT_EQ(cmd.name, "GET");
    ASSERT_EQ(cmd.args.size(), 1u);
    EXPECT_EQ(cmd.args[0], "name");
}

// ============================================================================
// Extra Arguments — Silently Accepted (Design Decision)
// ============================================================================

TEST(ParserTest, GetExtraArgsAccepted) {
    // Extra args beyond minimum are silently accepted per design decision.
    // The server handles unknown extra tokens; the parser only validates minimums.
    auto cmd = CommandParser::parse("GET key extra stuff");
    EXPECT_TRUE(cmd.valid);
    EXPECT_EQ(cmd.name, "GET");
    EXPECT_GE(cmd.args.size(), 1u);
}

// ============================================================================
// Transaction & Replication Commands
// ============================================================================

TEST(ParserTest, MultiCommand) {
    auto cmd = CommandParser::parse("MULTI");
    EXPECT_TRUE(cmd.valid);
    EXPECT_EQ(cmd.name, "MULTI");
    EXPECT_EQ(cmd.args.size(), 0u);
}

TEST(ParserTest, ExecCommand) {
    auto cmd = CommandParser::parse("EXEC");
    EXPECT_TRUE(cmd.valid);
    EXPECT_EQ(cmd.name, "EXEC");
}

TEST(ParserTest, DiscardCommand) {
    auto cmd = CommandParser::parse("DISCARD");
    EXPECT_TRUE(cmd.valid);
    EXPECT_EQ(cmd.name, "DISCARD");
}

TEST(ParserTest, ReplicaOf) {
    auto cmd = CommandParser::parse("REPLICAOF 127.0.0.1 6379");
    EXPECT_TRUE(cmd.valid);
    EXPECT_EQ(cmd.name, "REPLICAOF");
    ASSERT_EQ(cmd.args.size(), 2u);
    EXPECT_EQ(cmd.args[0], "127.0.0.1");
    EXPECT_EQ(cmd.args[1], "6379");
}

TEST(ParserTest, InfoCommand) {
    auto cmd = CommandParser::parse("INFO");
    EXPECT_TRUE(cmd.valid);
    EXPECT_EQ(cmd.name, "INFO");
}

// ============================================================================
// Unknown Commands — Parser accepts, Server handles
// ============================================================================

TEST(ParserTest, UnknownCommandAccepted) {
    // Commands not in minArgs_ map should still be parsed as valid.
    // The server is responsible for returning "ERR unknown command".
    auto cmd = CommandParser::parse("FOOBAR arg1 arg2");
    EXPECT_TRUE(cmd.valid);
    EXPECT_EQ(cmd.name, "FOOBAR");
}
