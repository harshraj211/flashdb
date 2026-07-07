#pragma once
#include <string>
#include <cstdint>

namespace flashdb {

struct Config {
    std::string host = "127.0.0.1";
    uint16_t port = 6379;
    std::string aofFilePath = "data/appendonly.aof";
    bool aofEnabled = true;
    int expiryCleanupIntervalMs = 100;
    std::string requirePassword = "";  // empty = no auth

    // Parse from command line args.
    static Config fromArgs(int argc, char* argv[]);
};

} // namespace flashdb
