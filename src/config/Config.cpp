#include "config/Config.h"
#include <iostream>
#include <stdexcept>
#include <cstdlib>

namespace flashdb {

Config Config::fromArgs(int argc, char* argv[]) {
    Config config;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];

        if (arg == "--host") {
            if (i + 1 >= argc) {
                std::cerr << "[Config] --host requires a value\n";
                std::exit(1);
            }
            config.host = argv[++i];
        } else if (arg == "--port") {
            if (i + 1 >= argc) {
                std::cerr << "[Config] --port requires a value\n";
                std::exit(1);
            }
            try {
                int port = std::stoi(argv[++i]);
                if (port < 1 || port > 65535) {
                    std::cerr << "[Config] --port must be between 1 and 65535\n";
                    std::exit(1);
                }
                config.port = static_cast<uint16_t>(port);
            } catch (const std::exception&) {
                std::cerr << "[Config] --port requires a valid integer\n";
                std::exit(1);
            }
        } else if (arg == "--aof-path") {
            if (i + 1 >= argc) {
                std::cerr << "[Config] --aof-path requires a value\n";
                std::exit(1);
            }
            config.aofFilePath = argv[++i];
        } else if (arg == "--no-aof") {
            config.aofEnabled = false;
        } else if (arg == "--requirepass") {
            if (i + 1 >= argc) {
                std::cerr << "[Config] --requirepass requires a value\n";
                std::exit(1);
            }
            config.requirePassword = argv[++i];
        } else if (arg == "--expiry-interval") {
            if (i + 1 >= argc) {
                std::cerr << "[Config] --expiry-interval requires a value\n";
                std::exit(1);
            }
            try {
                config.expiryCleanupIntervalMs = std::stoi(argv[++i]);
                if (config.expiryCleanupIntervalMs < 10) {
                    std::cerr << "[Config] --expiry-interval must be at least 10ms\n";
                    std::exit(1);
                }
            } catch (const std::exception&) {
                std::cerr << "[Config] --expiry-interval requires a valid integer\n";
                std::exit(1);
            }
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "FlashDB - In-memory key-value database\n"
                      << "\n"
                      << "Usage:\n"
                      << "  flashdb [options]\n"
                      << "\n"
                      << "Options:\n"
                      << "  --host <addr>            Bind address (default: 127.0.0.1)\n"
                      << "  --port <port>            Listen port (default: 6379)\n"
                      << "  --aof-path <path>        AOF file path (default: data/appendonly.aof)\n"
                      << "  --no-aof                 Disable AOF persistence\n"
                      << "  --requirepass <password>  Require client authentication\n"
                      << "  --expiry-interval <ms>   Expiry cleanup interval in ms (default: 100)\n"
                      << "  --help, -h               Show this help message\n";
            std::exit(0);
        } else {
            std::cerr << "[Config] Unknown argument: " << arg << "\n";
            std::cerr << "         Run with --help for usage information.\n";
            std::exit(1);
        }
    }

    return config;
}

} // namespace flashdb
