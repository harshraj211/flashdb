#include "persistence/AOFManager.h"
#include "parser/CommandParser.h"
#include "storage/StorageEngine.h"
#include "expiry/ExpiryManager.h"

#include <filesystem>
#include <iostream>
#include <algorithm>
#include <cctype>

namespace flashdb {

AOFManager::AOFManager(const std::string& filePath)
    : filePath_(filePath) {
    ensureDirectory();
    aofFile_.open(filePath_, std::ios::app | std::ios::out);
    if (!aofFile_.is_open()) {
        std::cerr << "[AOF] WARNING: Failed to open AOF file: " << filePath_ << "\n";
    }
}

AOFManager::~AOFManager() {
    close();
}

void AOFManager::ensureDirectory() {
    std::filesystem::path p(filePath_);
    std::filesystem::path dir = p.parent_path();
    if (!dir.empty() && !std::filesystem::exists(dir)) {
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        if (ec) {
            std::cerr << "[AOF] WARNING: Failed to create directory '"
                      << dir.string() << "': " << ec.message() << "\n";
        }
    }
}

void AOFManager::appendCommand(const std::string& rawCommand) {
    std::lock_guard<std::mutex> lock(writeMutex_);
    if (aofFile_.is_open()) {
        aofFile_ << rawCommand << '\n';
        aofFile_.flush();
    }
}

int AOFManager::loadAOF(StorageEngine& storage, ExpiryManager& expiry) {
    std::ifstream inFile(filePath_);
    if (!inFile.is_open()) {
        std::cout << "[AOF] No AOF file found at '" << filePath_
                  << "', starting with empty dataset.\n";
        return 0;
    }

    int replayed = 0;
    int skipped = 0;
    std::string line;

    while (std::getline(inFile, line)) {
        // Trim trailing \r if present (cross-platform line ending handling)
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        // Skip blank lines
        if (line.empty()) {
            continue;
        }

        // Parse the command
        Command cmd = CommandParser::parse(line);
        if (!cmd.valid) {
            std::cerr << "[AOF] Skipping corrupted line: " << cmd.errorMessage
                      << " (line: '" << line << "')\n";
            ++skipped;
            continue;
        }

        // Execute the command against storage/expiry
        // Convert command name to uppercase for case-insensitive matching
        std::string cmdName = cmd.name;

        if (cmdName == "SET") {
            if (cmd.args.size() < 2) {
                std::cerr << "[AOF] SET requires at least 2 args, skipping.\n";
                ++skipped;
                continue;
            }
            storage.set(cmd.args[0], cmd.args[1]);

            // Check for EX option: SET key value EX seconds
            if (cmd.args.size() >= 4) {
                std::string option = cmd.args[2];
                // Normalize to uppercase
                std::transform(option.begin(), option.end(), option.begin(),
                    [](unsigned char c) { return std::toupper(c); });
                if (option == "EX") {
                    try {
                        int seconds = std::stoi(cmd.args[3]);
                        expiry.setExpirySeconds(cmd.args[0], seconds);
                    } catch (const std::exception& e) {
                        std::cerr << "[AOF] Invalid EX value '" << cmd.args[3]
                                  << "', ignoring expiry.\n";
                    }
                }
            }
        } else if (cmdName == "DEL") {
            if (cmd.args.empty()) {
                std::cerr << "[AOF] DEL requires at least 1 arg, skipping.\n";
                ++skipped;
                continue;
            }
            storage.del(cmd.args);
        } else if (cmdName == "EXPIRE") {
            if (cmd.args.size() < 2) {
                std::cerr << "[AOF] EXPIRE requires 2 args, skipping.\n";
                ++skipped;
                continue;
            }
            try {
                int seconds = std::stoi(cmd.args[1]);
                expiry.setExpirySeconds(cmd.args[0], seconds);
            } catch (const std::exception& e) {
                std::cerr << "[AOF] Invalid EXPIRE seconds '" << cmd.args[1]
                          << "', skipping.\n";
                ++skipped;
                continue;
            }
        } else if (cmdName == "FLUSHALL") {
            storage.flushAll();
            expiry.clear();
        } else {
            // Non-write commands (GET, PING, etc.) are not persisted normally,
            // but if they appear in AOF, just skip silently
            ++skipped;
            continue;
        }

        ++replayed;
    }

    std::cout << "[AOF] Loaded " << replayed << " commands, Skipped "
              << skipped << " corrupted lines\n";
    return replayed;
}

void AOFManager::sync() {
    if (aofFile_.is_open()) {
        aofFile_.flush();
        // NOTE (Linux-specific): For true durability, call fdatasync() on the
        // underlying file descriptor after flush(). std::ofstream does not
        // expose the native fd portably. On Linux, you can obtain it via:
        //
        //   #include <unistd.h>
        //   // Implementation-specific: extract fd from ofstream's streambuf
        //   // fdatasync(fd);
        //
        // aofFile_.flush() ensures the C++ library buffer is written to the OS
        // page cache. fdatasync() would ensure the OS writes it to stable
        // storage. For most use cases, flush() provides sufficient durability
        // since the OS will write dirty pages to disk within a few seconds.
    }
}

void AOFManager::close() {
    std::lock_guard<std::mutex> lock(writeMutex_);
    if (aofFile_.is_open()) {
        aofFile_.flush();
        aofFile_.close();
    }
}

bool AOFManager::isOpen() const {
    return aofFile_.is_open();
}

} // namespace flashdb
