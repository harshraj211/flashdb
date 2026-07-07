#include "parser/CommandParser.h"
#include <algorithm>
#include <sstream>

namespace flashdb {

// Minimum required arguments for each supported command.
// Extra arguments beyond the minimum are silently accepted — this allows
// commands like SET to take optional flags (e.g., SET key value EX 10)
// without the parser rejecting them. Argument semantics are validated
// at the execution layer, not here.
const std::unordered_map<std::string, int> CommandParser::minArgs_ = {
    {"SET",       2},  // SET key value [EX seconds]
    {"GET",       1},  // GET key
    {"DEL",       1},  // DEL key [key ...]
    {"EXISTS",    1},  // EXISTS key
    {"KEYS",      0},  // KEYS [pattern] — pattern is optional
    {"FLUSHALL",  0},  // FLUSHALL
    {"TTL",       1},  // TTL key
    {"EXPIRE",    2},  // EXPIRE key seconds
    {"SUBSCRIBE", 1},  // SUBSCRIBE channel [channel ...]
    {"PUBLISH",   2},  // PUBLISH channel message
    {"MULTI",     0},  // MULTI
    {"EXEC",      0},  // EXEC
    {"DISCARD",   0},  // DISCARD
    {"REPLICAOF", 2},  // REPLICAOF host port
    {"SYNC",      0},  // Internal replication handshake
    {"INFO",      0},  // INFO [section]
    {"PING",      0},  // PING [message]
    {"AUTH",      1},  // AUTH password
};

// NOTE: Whitespace-delimited tokenization. Keys and values
// cannot contain spaces. For binary-safe parsing, implement
// RESP protocol. See docs/design_decisions.md for details.
std::vector<std::string> CommandParser::tokenize(const std::string& input) {
    std::vector<std::string> tokens;
    std::istringstream stream(input);
    std::string token;

    // operator>> skips all whitespace (spaces, tabs, multiple consecutive
    // spaces) and extracts non-whitespace tokens.
    while (stream >> token) {
        tokens.push_back(std::move(token));
    }

    return tokens;
}

Command CommandParser::parse(const std::string& rawInput) {
    Command cmd;

    // Strip trailing \r\n and \n (common in network-received data).
    std::string cleaned = rawInput;
    while (!cleaned.empty() && (cleaned.back() == '\n' || cleaned.back() == '\r')) {
        cleaned.pop_back();
    }

    // Handle empty or whitespace-only input.
    auto tokens = tokenize(cleaned);
    if (tokens.empty()) {
        cmd.valid = false;
        cmd.errorMessage = "ERR empty command";
        return cmd;
    }

    // Command name is always uppercase for consistent dispatch.
    cmd.name = tokens[0];
    std::transform(cmd.name.begin(), cmd.name.end(), cmd.name.begin(),
                   [](unsigned char c) {
                       return static_cast<char>(std::toupper(c));
                   });

    // Remaining tokens become command arguments.
    cmd.args.assign(tokens.begin() + 1, tokens.end());

    // Validate argument count against known commands.
    if (!validateArgs(cmd)) {
        cmd.valid = false;
        // errorMessage is set inside validateArgs
    }

    return cmd;
}

bool CommandParser::validateArgs(const Command& cmd) {
    auto it = minArgs_.find(cmd.name);
    if (it == minArgs_.end()) {
        // Unknown command — parsed as valid. Server will return "ERR unknown command".
        return true;
    }

    int minRequired = it->second;
    if (static_cast<int>(cmd.args.size()) < minRequired) {
        const_cast<Command&>(cmd).errorMessage =
            "ERR wrong number of arguments for '" + cmd.name + "' command";
        return false;
    }

    return true;
}

} // namespace flashdb
