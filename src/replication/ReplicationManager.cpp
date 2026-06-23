#include "replication/ReplicationManager.h"
#include "storage/StorageEngine.h"
#include "expiry/ExpiryManager.h"
#include "parser/CommandParser.h"

#include <iostream>
#include <sstream>
#include <algorithm>
#include <cerrno>
#include <cstring>

namespace flashdb {

ReplicationManager::ReplicationManager() = default;

ReplicationManager::~ReplicationManager() {
    stopReplication();

    // Close master connection if we were a replica
    if (platform::isValidSocket(masterFd_)) {
        platform::closeSocket(masterFd_);
        masterFd_ = platform::INVALID_SOCK;
    }

    // Close all replica connections if we were a master
    {
        std::lock_guard<std::mutex> lock(replicaMutex_);
        for (platform::socket_t fd : replicaFds_) {
            platform::closeSocket(fd);
        }
        replicaFds_.clear();
    }
}

void ReplicationManager::addReplica(platform::socket_t replicaFd, StorageEngine& storage) {
    {
        std::lock_guard<std::mutex> lock(replicaMutex_);
        replicaFds_.push_back(replicaFd);
    }
    sendFullSync(replicaFd, storage);
}

void ReplicationManager::sendFullSync(platform::socket_t replicaFd, StorageEngine& storage) {
    // Send sync start marker
    writeToFd(replicaFd, "FULLSYNC\n");

    // getAllEntries() acquires storage's shared_lock, ensuring we get a
    // consistent snapshot. Writes that arrive during sync will be propagated
    // separately after FULLSYNC_DONE.
    auto entries = storage.getAllEntries();
    for (const auto& [key, value] : entries) {
        std::string cmd = "SET " + key + " " + value + "\n";
        if (!writeToFd(replicaFd, cmd)) {
            std::cerr << "[REPL] Failed to send SET during full sync to fd "
                      << replicaFd << "\n";
            return;
        }
    }

    // Send sync completion marker
    writeToFd(replicaFd, "FULLSYNC_DONE\n");
    std::cout << "[REPL] Full sync sent to replica fd " << replicaFd
              << " (" << entries.size() << " keys)\n";
}

void ReplicationManager::propagate(const std::string& command) {
    std::lock_guard<std::mutex> lock(replicaMutex_);

    if (replicaFds_.empty()) {
        return;
    }

    std::string data = command + "\n";
    std::vector<platform::socket_t> failedFds;

    for (platform::socket_t fd : replicaFds_) {
        if (!writeToFd(fd, data)) {
            std::cerr << "[REPL] Failed to propagate to replica fd " << fd
                      << ", marking for removal.\n";
            failedFds.push_back(fd);
        }
    }

    // Remove failed replicas
    for (platform::socket_t failedFd : failedFds) {
        replicaFds_.erase(
            std::remove(replicaFds_.begin(), replicaFds_.end(), failedFd),
            replicaFds_.end()
        );
        platform::closeSocket(failedFd);
    }
}

void ReplicationManager::removeReplica(platform::socket_t replicaFd) {
    std::lock_guard<std::mutex> lock(replicaMutex_);
    auto it = std::find(replicaFds_.begin(), replicaFds_.end(), replicaFd);
    if (it != replicaFds_.end()) {
        replicaFds_.erase(it);
        platform::closeSocket(replicaFd);
        std::cout << "[REPL] Removed replica fd " << replicaFd << "\n";
    }
}

void ReplicationManager::connectToMaster(const std::string& host, uint16_t port,
                                          StorageEngine& storage, ExpiryManager& expiry) {
    // Create TCP socket
    platform::socket_t sockFd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (!platform::isValidSocket(sockFd)) {
        std::cerr << "[REPL] Failed to create socket: " << platform::getLastSocketError() << "\n";
        return;
    }

    // Resolve address and connect
    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) <= 0) {
        std::cerr << "[REPL] Invalid master address: " << host << "\n";
        platform::closeSocket(sockFd);
        return;
    }

    if (::connect(sockFd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::cerr << "[REPL] Failed to connect to master " << host << ":"
                  << port << ": " << platform::getLastSocketError() << "\n";
        platform::closeSocket(sockFd);
        return;
    }

    std::cout << "[REPL] Connected to master " << host << ":" << port << "\n";

    // Transition to replica mode
    isMaster_ = false;
    masterFd_ = sockFd;
    replicating_ = true;

    // Send handshake
    writeToFd(sockFd, "REPLICAOF\n");

    // Launch background thread for replication stream processing
    replicationThread_ = std::thread([this, &storage, &expiry]() {
        processReplicationStream(masterFd_, storage, expiry);
    });
}

void ReplicationManager::processReplicationStream(platform::socket_t masterFd,
                                                   StorageEngine& storage,
                                                   ExpiryManager& expiry) {
    std::string buffer;
    char readBuf[4096];

    while (replicating_.load()) {
        ssize_t bytesRead = platform::socketRead(masterFd, readBuf, sizeof(readBuf));
        if (bytesRead <= 0) {
            if (bytesRead == 0) {
                std::cout << "[REPL] Master disconnected.\n";
            } else if (!platform::wasInterrupted()) {
                std::cerr << "[REPL] Read error from master: "
                          << platform::getLastSocketError() << "\n";
            }
            break;
        }

        buffer.append(readBuf, static_cast<size_t>(bytesRead));

        // Process complete lines
        size_t pos;
        while ((pos = buffer.find('\n')) != std::string::npos) {
            std::string line = buffer.substr(0, pos);
            buffer.erase(0, pos + 1);

            // Trim trailing \r
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }

            if (line.empty()) {
                continue;
            }

            // Handle replication control messages
            if (line == "FULLSYNC") {
                std::cout << "[REPL] Full sync started from master.\n";
                continue;
            }
            if (line == "FULLSYNC_DONE") {
                std::cout << "[REPL] Full sync complete from master.\n";
                continue;
            }

            // Parse and execute the replicated command
            // (same logic as AOF replay)
            Command cmd = CommandParser::parse(line);
            if (!cmd.valid) {
                std::cerr << "[REPL] Skipping invalid replicated command: "
                          << cmd.errorMessage << "\n";
                continue;
            }

            if (cmd.name == "SET") {
                if (cmd.args.size() >= 2) {
                    storage.set(cmd.args[0], cmd.args[1]);
                    // Handle SET key value EX seconds
                    if (cmd.args.size() >= 4) {
                        std::string option = cmd.args[2];
                        std::transform(option.begin(), option.end(), option.begin(),
                            [](unsigned char c) { return std::toupper(c); });
                        if (option == "EX") {
                            try {
                                int seconds = std::stoi(cmd.args[3]);
                                expiry.setExpirySeconds(cmd.args[0], seconds);
                            } catch (...) {}
                        }
                    }
                }
            } else if (cmd.name == "DEL") {
                if (!cmd.args.empty()) {
                    storage.del(cmd.args);
                }
            } else if (cmd.name == "EXPIRE") {
                if (cmd.args.size() >= 2) {
                    try {
                        int seconds = std::stoi(cmd.args[1]);
                        expiry.setExpirySeconds(cmd.args[0], seconds);
                    } catch (...) {}
                }
            } else if (cmd.name == "FLUSHALL") {
                storage.flushAll();
                expiry.clear();
            }
        }
    }

    std::cout << "[REPL] Replication stream processing stopped.\n";
}

void ReplicationManager::stopReplication() {
    replicating_ = false;

    // Close master connection to unblock read() in the replication thread
    if (platform::isValidSocket(masterFd_)) {
        platform::closeSocket(masterFd_);
        masterFd_ = platform::INVALID_SOCK;
    }

    // Join the replication thread if it's running
    if (replicationThread_.joinable()) {
        replicationThread_.join();
    }
}

size_t ReplicationManager::replicaCount() const {
    std::lock_guard<std::mutex> lock(replicaMutex_);
    return replicaFds_.size();
}

bool ReplicationManager::writeToFd(platform::socket_t fd, const std::string& data) {
    const char* ptr = data.c_str();
    size_t remaining = data.size();

    while (remaining > 0) {
        ssize_t written = platform::socketWrite(fd, ptr, remaining);
        if (written < 0) {
            if (platform::wasInterrupted()) {
                continue;  // Retry on interrupt
            }
            std::cerr << "[REPL] write() failed on fd " << fd << ": "
                      << platform::getLastSocketError() << "\n";
            return false;
        }
        ptr += written;
        remaining -= static_cast<size_t>(written);
    }

    return true;
}

} // namespace flashdb
