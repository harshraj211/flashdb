#pragma once
#include <string>
#include <vector>
#include <unordered_map>

namespace flashdb {

struct Command {
    std::string name;               // Always uppercase: "SET", "GET", "DEL"
    std::vector<std::string> args;  // Remaining tokens
    bool valid = true;
    std::string errorMessage;
};

class CommandParser {
public:
    static Command parse(const std::string& rawInput);
    static bool validateArgs(const Command& cmd);

private:
    static std::vector<std::string> tokenize(const std::string& input);
    static const std::unordered_map<std::string, int> minArgs_;
};

} // namespace flashdb
