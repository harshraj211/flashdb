#pragma once
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "config/Config.h"
#include "expiry/ExpiryManager.h"
#include "monitoring/InfoManager.h"
#include "parser/CommandParser.h"
#include "persistence/AOFManager.h"
#include "platform/Platform.h"
#include "pubsub/PubSubManager.h"
#include "replication/ReplicationManager.h"
#include "storage/StorageEngine.h"
#include "transactions/TransactionManager.h"

namespace flashdb {

class Server {
public:
    Server(const Config& config);
    ~Server();

    // Start accepting connections — blocking call
    void start();

    // Gracefully stop the server
    void stop();

private:
    // Create and configure the listening socket
    platform::socket_t createListeningSocket();

    // Handle a single client connection (runs in its own thread)
    void handleClient(platform::socket_t clientFd, std::string clientAddress);

    // Process a single command from a client and return the response string
    std::string processCommand(const Command& cmd, platform::socket_t clientFd);

    // Execute a command (shared between direct execution and transaction EXEC)
    std::string executeCommand(const Command& cmd, platform::socket_t clientFd);

    // Execute a command without acquiring StorageEngine lock (called during atomic EXEC)
    std::string executeCommandUnlocked(const Command& cmd, platform::socket_t clientFd,
                                       bool recordSideEffects = true);

    // Write response to client fd (thread-safe per-client)
    void writeToClient(platform::socket_t clientFd, const std::string& response);

    // Authentication helpers. If no password is configured, every client is allowed.
    bool isAuthenticated(platform::socket_t clientFd);
    void markAuthenticated(platform::socket_t clientFd);

    // Config
    Config config_;

    // Server socket
    platform::socket_t serverFd_ = platform::INVALID_SOCK;
    std::atomic<bool> running_{false};

    // Components
    StorageEngine storage_;
    ExpiryManager expiry_;
    std::unique_ptr<AOFManager> aof_;
    PubSubManager pubsub_;
    TransactionManager txManager_;
    ReplicationManager replication_;
    InfoManager info_;

    // Client management
    std::vector<std::thread> clientThreads_;
    std::mutex clientsMutex_;
    std::unordered_map<platform::socket_t, std::shared_ptr<std::mutex>> clientWriteMutexes_;
    std::mutex writeMutexMapMutex_;
    std::unordered_map<platform::socket_t, bool> authenticatedClients_;
    std::mutex authMutex_;

    // Signal handling
    static Server* instance_;  // For signal handler access
    static void signalHandler(int signum);
};

}  // namespace flashdb
