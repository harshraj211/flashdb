#include "server/Server.h"

#include <chrono>
#include <csignal>
#include <cstring>
#include <algorithm>
#include <cctype>
#include <iostream>
#include <limits>
#include <sstream>

namespace flashdb {

namespace {

std::string toUpperCopy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) {
                       return static_cast<char>(std::toupper(c));
                   });
    return value;
}

bool parseNonNegativeSeconds(const std::string& token, int& seconds) {
    try {
        size_t consumed = 0;
        long long value = std::stoll(token, &consumed);
        if (consumed != token.size() || value < 0 ||
            value > std::numeric_limits<int>::max()) {
            return false;
        }
        seconds = static_cast<int>(value);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

struct SetExpiryOptions {
    bool valid = true;
    bool hasExpiry = false;
    int seconds = 0;
    std::string error;
};

SetExpiryOptions parseSetExpiryOptions(const Command& cmd) {
    SetExpiryOptions options;
    if (cmd.args.size() == 2) {
        return options;
    }

    if (cmd.args.size() != 4 || toUpperCopy(cmd.args[2]) != "EX") {
        options.valid = false;
        options.error = "ERR syntax error\n";
        return options;
    }

    if (!parseNonNegativeSeconds(cmd.args[3], options.seconds)) {
        options.valid = false;
        options.error = "ERR value is not an integer or out of range\n";
        return options;
    }

    options.hasExpiry = true;
    return options;
}

bool isWriteCommand(const Command& cmd) {
    return cmd.name == "SET" || cmd.name == "DEL" ||
           cmd.name == "FLUSHALL" || cmd.name == "EXPIRE";
}

std::string serializeCommand(const Command& cmd) {
    std::string raw = cmd.name;
    for (const auto& arg : cmd.args) {
        raw += " " + arg;
    }
    return raw;
}

bool isErrorResponse(const std::string& response) {
    return response.rfind("ERR", 0) == 0;
}

} // namespace

// Static member initialization
Server* Server::instance_ = nullptr;

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------
Server::Server(const Config& config)
    : config_(config),
      storage_(),
      expiry_(),
      aof_(nullptr),
      pubsub_(),
      txManager_(),
      replication_(),
      info_(storage_, replication_) {
    // Wire up the expiry manager into the storage engine so lazy-expiry
    // checks work transparently on every get()/exists() call.
    storage_.setExpiryManager(&expiry_);

    // Restore persisted state from AOF before accepting any connections.
    if (config_.aofEnabled) {
        aof_ = std::make_unique<AOFManager>(config_.aofFilePath);
        if (aof_->isOpen()) {
            int replayed = aof_->loadAOF(storage_, expiry_);
            if (replayed > 0) {
                std::cout << "[FlashDB] AOF loaded: " << replayed
                          << " commands replayed\n";
            }
        }
    }

    // Start the background thread that actively expires keys whose TTLs
    // have elapsed, preventing unbounded memory growth from lazy-only expiry.
    expiry_.startExpiryLoop(
        storage_, std::chrono::milliseconds(config_.expiryCleanupIntervalMs));

    // Store instance pointer for the static signal handler.
    instance_ = this;
}

// ---------------------------------------------------------------------------
// Destructor
// ---------------------------------------------------------------------------
Server::~Server() {
    if (running_.load()) {
        stop();
    }
    instance_ = nullptr;
}

// ---------------------------------------------------------------------------
// start() — blocking accept loop
// ---------------------------------------------------------------------------
void Server::start() {
    running_ = true;

    serverFd_ = createListeningSocket();
    if (!platform::isValidSocket(serverFd_)) {
        std::cerr << "[FlashDB] Failed to create listening socket. Aborting.\n";
        return;
    }

    // Graceful shutdown on Ctrl-C and ignore SIGPIPE on Unix.
    platform::installSignalHandler(signalHandler);

    std::cout << "[FlashDB] Server listening on " << config_.host << ":"
              << config_.port << "\n";

    // ---- Accept loop ----
    while (running_.load()) {
        struct sockaddr_in clientAddr {};
#ifdef FLASHDB_WINDOWS
        int addrLen = sizeof(clientAddr);
#else
        socklen_t addrLen = sizeof(clientAddr);
#endif

        platform::socket_t clientFd =
            accept(serverFd_, reinterpret_cast<struct sockaddr*>(&clientAddr),
                   &addrLen);

        if (!platform::isValidSocket(clientFd)) {
            // accept() can legitimately fail when stop() closes serverFd_.
            if (running_.load()) {
                std::cerr << "[FlashDB] accept failed: " << platform::getLastSocketError() << "\n";
            }
            continue;
        }

        // Convert the peer address to a human-readable string.
        char addrBuf[INET_ADDRSTRLEN];
        ::inet_ntop(AF_INET, &clientAddr.sin_addr, addrBuf, sizeof(addrBuf));
        std::string clientAddress =
            std::string(addrBuf) + ":" + std::to_string(ntohs(clientAddr.sin_port));

        std::cout << "[FlashDB] New connection from " << clientAddress << "\n";

        // Spawn a dedicated thread per client.  The thread is detached from
        // the accept-loop logic but tracked in clientThreads_ so we can
        // join on shutdown.
        {
            std::lock_guard<std::mutex> lock(clientsMutex_);
            clientThreads_.emplace_back(&Server::handleClient, this, clientFd,
                                         std::move(clientAddress));
        }
    }

    // ---- Cleanup: wait for every client thread to finish ----
    {
        std::lock_guard<std::mutex> lock(clientsMutex_);
        for (auto& t : clientThreads_) {
            if (t.joinable()) {
                t.join();
            }
        }
        clientThreads_.clear();
    }
}

// ---------------------------------------------------------------------------
// stop() — initiate graceful shutdown
// ---------------------------------------------------------------------------
void Server::stop() {
    bool expected = true;
    if (!running_.compare_exchange_strong(expected, false)) {
        return;  // Already stopped or stopping.
    }

    // Close the listening socket to unblock accept().
    if (platform::isValidSocket(serverFd_)) {
        platform::closeSocket(serverFd_);
        serverFd_ = platform::INVALID_SOCK;
    }

    // ExpiryManager background thread stops automatically when server is destroyed.

    // Persist any buffered AOF writes and close the file.
    if (aof_) {
        aof_->sync();
        aof_->close();
    }

    std::cout << "[FlashDB] Server stopped\n";
}

// ---------------------------------------------------------------------------
// createListeningSocket()
// ---------------------------------------------------------------------------
platform::socket_t Server::createListeningSocket() {
    platform::socket_t fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (!platform::isValidSocket(fd)) {
        std::cerr << "[FlashDB] socket creation failed: " << platform::getLastSocketError() << "\n";
        return platform::INVALID_SOCK;
    }

    // Allow rapid restart without TIME_WAIT issues.
    if (!platform::setReuseAddr(fd)) {
        std::cerr << "[FlashDB] setsockopt SO_REUSEADDR failed: " << platform::getLastSocketError() << "\n";
        platform::closeSocket(fd);
        return platform::INVALID_SOCK;
    }

    struct sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(config_.port);

    if (::inet_pton(AF_INET, config_.host.c_str(), &addr.sin_addr) <= 0) {
        std::cerr << "[FlashDB] Invalid bind address: " << config_.host << "\n";
        platform::closeSocket(fd);
        return platform::INVALID_SOCK;
    }

    if (::bind(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::cerr << "[FlashDB] bind failed: " << platform::getLastSocketError() << "\n";
        platform::closeSocket(fd);
        return platform::INVALID_SOCK;
    }

    if (::listen(fd, SOMAXCONN) < 0) {
        std::cerr << "[FlashDB] listen failed: " << platform::getLastSocketError() << "\n";
        platform::closeSocket(fd);
        return platform::INVALID_SOCK;
    }

    return fd;
}

// ---------------------------------------------------------------------------
// handleClient() — per-client read loop (runs in its own thread)
// ---------------------------------------------------------------------------
void Server::handleClient(platform::socket_t clientFd, std::string clientAddress) {
    std::cout << "[Client] Connected: " << clientAddress << "\n";
    info_.clientConnected();

    // Initialize the per-client write mutex.
    {
        std::lock_guard<std::mutex> lock(writeMutexMapMutex_);
        clientWriteMutexes_[clientFd] = std::make_shared<std::mutex>();
    }

    static constexpr size_t kMaxAccumulatedBytes = 65536; // 64KB
    constexpr size_t kBufSize = 4096;
    char buf[kBufSize];
    std::string accumulated;  // Holds partial reads across recv() calls.

    while (running_.load()) {
        ssize_t bytesRead = platform::socketRead(clientFd, buf, kBufSize);

        if (bytesRead <= 0) {
            if (bytesRead == 0) {
                std::cout << "[Client] Disconnected: " << clientAddress << "\n";
            } else {
                // read error — could be ECONNRESET or similar.
                if (!platform::wasInterrupted()) {
                    std::cerr << "[Client] Read error from " << clientAddress
                              << ": " << platform::getLastSocketError() << "\n";
                }
            }
            break;
        }

        accumulated.append(buf, static_cast<size_t>(bytesRead));

        if (accumulated.size() > kMaxAccumulatedBytes) {
            writeToClient(clientFd, "ERR max input buffer exceeded, disconnecting\n");
            std::cerr << "[WARN] Client " << clientFd 
                      << " exceeded max buffer size, disconnecting\n";
            break;
        }

        // Process every complete newline-delimited command.
        size_t pos;
        while ((pos = accumulated.find('\n')) != std::string::npos) {
            std::string line = accumulated.substr(0, pos + 1);
            accumulated.erase(0, pos + 1);

            Command cmd = CommandParser::parse(line);
            if (!cmd.valid) {
                writeToClient(clientFd, cmd.errorMessage + "\n");
            } else {
                std::string response = processCommand(cmd, clientFd);
                writeToClient(clientFd, response);
            }
            info_.commandProcessed();
        }
    }

    // ---- Client teardown ----
    pubsub_.removeClient(static_cast<int>(clientFd));
    txManager_.removeClient(static_cast<int>(clientFd));

    if (!replication_.removeReplica(clientFd)) {
        platform::closeSocket(clientFd);
    }

    // Remove authentication state for this socket.
    {
        std::lock_guard<std::mutex> lock(authMutex_);
        authenticatedClients_.erase(clientFd);
    }

    // Remove the per-client write mutex.
    {
        std::lock_guard<std::mutex> lock(writeMutexMapMutex_);
        clientWriteMutexes_.erase(clientFd);
    }

    info_.clientDisconnected();
}

// ---------------------------------------------------------------------------
// processCommand() — state-machine dispatch before executeCommand()
// ---------------------------------------------------------------------------
std::string Server::processCommand(const Command& cmd, platform::socket_t clientFd) {
    int clientKey = static_cast<int>(clientFd);

    // ---- Authentication guard ----
    // AUTH is handled before pub/sub and transaction state, so locked-out
    // clients cannot queue protected work or use special modes to bypass auth.
    if (!config_.requirePassword.empty()) {
        if (cmd.name == "AUTH") {
            if (cmd.args[0] == config_.requirePassword) {
                markAuthenticated(clientFd);
                return "OK\n";
            }
            return "ERR invalid password\n";
        }

        if (!isAuthenticated(clientFd) && cmd.name != "PING") {
            return "NOAUTH Authentication required\n";
        }
    } else if (cmd.name == "AUTH") {
        return "ERR AUTH called without any password configured\n";
    }
    
    // ---- Pub/Sub mode guard ----
    // Once a client enters subscription mode, only a narrow set of commands
    // is permitted; everything else is rejected.
    if (pubsub_.isSubscribed(clientKey)) {
        if (cmd.name != "SUBSCRIBE" && cmd.name != "UNSUBSCRIBE" &&
            cmd.name != "PING") {
            return "ERR only SUBSCRIBE, UNSUBSCRIBE, and PING are allowed "
                   "in pub/sub mode\n";
        }
    }

    // ---- Transaction QUEUING state ----
    TransactionState txState = txManager_.getState(clientKey);

    if (txState == TransactionState::QUEUING) {
        if (cmd.name == "EXEC") {
            // Atomically execute all queued commands.
            std::vector<Command> queued = txManager_.executeTransaction(clientKey);

            if (queued.empty()) {
                return "(empty transaction)\n";
            }

            std::ostringstream oss;
            std::vector<std::string> sideEffectCommands;
            // Acquire exclusive lock on StorageEngine ONCE for the duration of the transaction.
            {
                std::unique_lock<std::shared_mutex> txLock(storage_.getMutex());
                for (size_t i = 0; i < queued.size(); ++i) {
                    std::string result = executeCommandUnlocked(queued[i], clientFd, false);
                    oss << (i + 1) << ") " << result;
                    if (isWriteCommand(queued[i]) && !isErrorResponse(result)) {
                        sideEffectCommands.push_back(serializeCommand(queued[i]));
                    }
                }
            }

            if (!sideEffectCommands.empty()) {
                if (aof_) {
                    aof_->appendCommands(sideEffectCommands);
                }
                replication_.propagateBatch(sideEffectCommands);
            }

            return oss.str();
        }

        if (cmd.name == "DISCARD") {
            return txManager_.discardTransaction(clientKey) + "\n";
        }

        if (cmd.name == "MULTI") {
            return "ERR MULTI calls can not be nested\n";
        }

        // Queue any other command.
        txManager_.queueCommand(clientKey, cmd);
        return "QUEUED\n";
    }

    // ---- Normal (non-transaction) dispatch ----
    return executeCommand(cmd, clientFd);
}

// ---------------------------------------------------------------------------
// executeCommand() — the main command router
// ---------------------------------------------------------------------------
std::string Server::executeCommand(const Command& cmd, platform::socket_t clientFd) {
    int clientKey = static_cast<int>(clientFd);
    
    // Helper: reconstruct the raw command string for AOF / replication.
    auto rawCommand = [&]() -> std::string {
        return serializeCommand(cmd);
    };

    // ---- PING ----
    if (cmd.name == "PING") {
        if (!cmd.args.empty()) {
            return cmd.args[0] + "\n";
        }
        return "PONG\n";
    }

    // ---- SET key value [EX seconds] ----
    if (cmd.name == "SET") {
        const std::string& key = cmd.args[0];
        const std::string& value = cmd.args[1];

        SetExpiryOptions expiryOptions = parseSetExpiryOptions(cmd);
        if (!expiryOptions.valid) {
            return expiryOptions.error;
        }

        storage_.set(key, value);
        if (expiryOptions.hasExpiry) {
            expiry_.setExpirySeconds(key, expiryOptions.seconds);
        } else {
            expiry_.removeExpiry(key);
        }

        if (aof_) {
            aof_->appendCommand(rawCommand());
        }
        replication_.propagate(rawCommand());

        return "OK\n";
    }

    // ---- GET key ----
    if (cmd.name == "GET") {
        auto val = storage_.get(cmd.args[0]);
        return val ? *val + "\n" : "(nil)\n";
    }

    // ---- DEL key [key ...] ----
    if (cmd.name == "DEL") {
        int count = storage_.del(cmd.args);

        if (aof_) {
            aof_->appendCommand(rawCommand());
        }
        replication_.propagate(rawCommand());

        return "(integer) " + std::to_string(count) + "\n";
    }

    // ---- EXISTS key ----
    if (cmd.name == "EXISTS") {
        return std::string("(integer) ") +
               (storage_.exists(cmd.args[0]) ? "1" : "0") + "\n";
    }

    // ---- KEYS ----
    if (cmd.name == "KEYS") {
        auto allKeys = storage_.keys();
        if (allKeys.empty()) {
            return "(empty list)\n";
        }
        std::ostringstream oss;
        for (size_t i = 0; i < allKeys.size(); ++i) {
            oss << (i + 1) << ") " << allKeys[i] << "\n";
        }
        return oss.str();
    }

    // ---- FLUSHALL ----
    if (cmd.name == "FLUSHALL") {
        storage_.flushAll();

        if (aof_) {
            aof_->appendCommand(rawCommand());
        }
        replication_.propagate(rawCommand());

        return "OK\n";
    }

    // ---- TTL key ----
    if (cmd.name == "TTL") {
        const std::string& key = cmd.args[0];
        if (!storage_.exists(key)) {
            return "(integer) -2\n";
        }
        int ttl = expiry_.getTTL(key);
        return "(integer) " + std::to_string(ttl) + "\n";
    }

    // ---- EXPIRE key seconds ----
    if (cmd.name == "EXPIRE") {
        const std::string& key = cmd.args[0];
        int seconds = 0;
        if (!parseNonNegativeSeconds(cmd.args[1], seconds)) {
            return "ERR value is not an integer or out of range\n";
        }

        if (!storage_.exists(key)) {
            return "(integer) 0\n";
        }
        expiry_.setExpirySeconds(key, seconds);

        if (aof_) {
            aof_->appendCommand(rawCommand());
        }
        replication_.propagate(rawCommand());

        return "(integer) 1\n";
    }

    // ---- SUBSCRIBE channel ----
    if (cmd.name == "SUBSCRIBE") {
        const std::string& channel = cmd.args[0];
        int count = pubsub_.subscribe(channel, clientKey);
        return "Subscribed to " + channel + " (" + std::to_string(count) +
               " total)\n";
    }

    // ---- UNSUBSCRIBE channel ----
    if (cmd.name == "UNSUBSCRIBE") {
        const std::string& channel = cmd.args[0];
        int count = pubsub_.unsubscribe(channel, clientKey);
        return "Unsubscribed from " + channel + " (" + std::to_string(count) +
               " remaining)\n";
    }

    // ---- PUBLISH channel message ----
    if (cmd.name == "PUBLISH") {
        const std::string& channel = cmd.args[0];
        const std::string& message = cmd.args[1];

        auto writeFn = [this](int fd, const std::string& msg) {
            writeToClient(static_cast<platform::socket_t>(fd), msg);
        };

        int receivers = pubsub_.publish(channel, message, writeFn);
        return "(integer) " + std::to_string(receivers) + "\n";
    }

    // ---- MULTI ----
    if (cmd.name == "MULTI") {
        return txManager_.beginTransaction(clientKey) + "\n";
    }

    // ---- EXEC (outside of transaction context) ----
    if (cmd.name == "EXEC") {
        return "ERR EXEC without MULTI\n";
    }

    // ---- DISCARD (outside of transaction context) ----
    if (cmd.name == "DISCARD") {
        return txManager_.discardTransaction(clientKey) + "\n";
    }

    // ---- SYNC (internal replication handshake) ----
    if (cmd.name == "SYNC") {
        replication_.addReplica(clientFd, storage_);
        return "";
    }

    // ---- REPLICAOF host port ----
    if (cmd.name == "REPLICAOF") {
        try {
            uint16_t port = static_cast<uint16_t>(std::stoi(cmd.args[1]));
            replication_.connectToMaster(cmd.args[0], port, storage_, expiry_);
        } catch (const std::exception&) {
            return "ERR invalid port number\n";
        }
        return "OK\n";
    }

    // ---- INFO ----
    if (cmd.name == "INFO") {
        return info_.getInfo();
    }

    if (cmd.name == "AUTH") {
        return "ERR AUTH called without any password configured\n";
    }

    // ---- Unknown command ----
    return "ERR unknown command '" + cmd.name + "'\n";
}

// ---------------------------------------------------------------------------
// executeCommandUnlocked() — same as executeCommand but uses unlocked storage calls
// ---------------------------------------------------------------------------
std::string Server::executeCommandUnlocked(const Command& cmd, platform::socket_t clientFd,
                                           bool recordSideEffects) {
    int clientKey = static_cast<int>(clientFd);
    
    // Helper: reconstruct the raw command string for AOF / replication.
    auto rawCommand = [&]() -> std::string {
        return serializeCommand(cmd);
    };

    // ---- PING ----
    if (cmd.name == "PING") {
        if (!cmd.args.empty()) {
            return cmd.args[0] + "\n";
        }
        return "PONG\n";
    }

    // ---- SET key value [EX seconds] ----
    if (cmd.name == "SET") {
        const std::string& key = cmd.args[0];
        const std::string& value = cmd.args[1];

        SetExpiryOptions expiryOptions = parseSetExpiryOptions(cmd);
        if (!expiryOptions.valid) {
            return expiryOptions.error;
        }

        storage_.setUnlocked(key, value);
        if (expiryOptions.hasExpiry) {
            expiry_.setExpirySeconds(key, expiryOptions.seconds);
        } else {
            expiry_.removeExpiry(key);
        }

        if (recordSideEffects && aof_) {
            aof_->appendCommand(rawCommand());
        }
        if (recordSideEffects) {
            replication_.propagate(rawCommand());
        }

        return "OK\n";
    }

    // ---- GET key ----
    if (cmd.name == "GET") {
        auto val = storage_.getUnlocked(cmd.args[0]);
        return val ? *val + "\n" : "(nil)\n";
    }

    // ---- DEL key [key ...] ----
    if (cmd.name == "DEL") {
        int count = storage_.delUnlocked(cmd.args);

        if (recordSideEffects && aof_) {
            aof_->appendCommand(rawCommand());
        }
        if (recordSideEffects) {
            replication_.propagate(rawCommand());
        }

        return "(integer) " + std::to_string(count) + "\n";
    }

    // ---- EXISTS key ----
    if (cmd.name == "EXISTS") {
        return std::string("(integer) ") +
               (storage_.existsUnlocked(cmd.args[0]) ? "1" : "0") + "\n";
    }

    // ---- KEYS ----
    if (cmd.name == "KEYS") {
        auto allKeys = storage_.keysUnlocked();
        if (allKeys.empty()) {
            return "(empty list)\n";
        }
        std::ostringstream oss;
        for (size_t i = 0; i < allKeys.size(); ++i) {
            oss << (i + 1) << ") " << allKeys[i] << "\n";
        }
        return oss.str();
    }

    // ---- FLUSHALL ----
    if (cmd.name == "FLUSHALL") {
        storage_.flushAllUnlocked();

        if (recordSideEffects && aof_) {
            aof_->appendCommand(rawCommand());
        }
        if (recordSideEffects) {
            replication_.propagate(rawCommand());
        }

        return "OK\n";
    }

    // ---- TTL key ----
    if (cmd.name == "TTL") {
        const std::string& key = cmd.args[0];
        if (!storage_.existsUnlocked(key)) {
            return "(integer) -2\n";
        }
        int ttl = expiry_.getTTL(key);
        return "(integer) " + std::to_string(ttl) + "\n";
    }

    // ---- EXPIRE key seconds ----
    if (cmd.name == "EXPIRE") {
        const std::string& key = cmd.args[0];
        int seconds = 0;
        if (!parseNonNegativeSeconds(cmd.args[1], seconds)) {
            return "ERR value is not an integer or out of range\n";
        }

        if (!storage_.existsUnlocked(key)) {
            return "(integer) 0\n";
        }
        expiry_.setExpirySeconds(key, seconds);

        if (recordSideEffects && aof_) {
            aof_->appendCommand(rawCommand());
        }
        if (recordSideEffects) {
            replication_.propagate(rawCommand());
        }

        return "(integer) 1\n";
    }

    // ---- SUBSCRIBE channel ----
    if (cmd.name == "SUBSCRIBE") {
        const std::string& channel = cmd.args[0];
        int count = pubsub_.subscribe(channel, clientKey);
        return "Subscribed to " + channel + " (" + std::to_string(count) +
               " total)\n";
    }

    // ---- UNSUBSCRIBE channel ----
    if (cmd.name == "UNSUBSCRIBE") {
        const std::string& channel = cmd.args[0];
        int count = pubsub_.unsubscribe(channel, clientKey);
        return "Unsubscribed from " + channel + " (" + std::to_string(count) +
               " remaining)\n";
    }

    // ---- PUBLISH channel message ----
    if (cmd.name == "PUBLISH") {
        const std::string& channel = cmd.args[0];
        const std::string& message = cmd.args[1];

        auto writeFn = [this](int fd, const std::string& msg) {
            writeToClient(static_cast<platform::socket_t>(fd), msg);
        };

        int receivers = pubsub_.publish(channel, message, writeFn);
        return "(integer) " + std::to_string(receivers) + "\n";
    }

    // ---- MULTI ----
    if (cmd.name == "MULTI") {
        return txManager_.beginTransaction(clientKey) + "\n";
    }

    // ---- EXEC (outside of transaction context) ----
    if (cmd.name == "EXEC") {
        return "ERR EXEC without MULTI\n";
    }

    // ---- DISCARD (outside of transaction context) ----
    if (cmd.name == "DISCARD") {
        return txManager_.discardTransaction(clientKey) + "\n";
    }

    // ---- SYNC cannot run inside a transaction ----
    if (cmd.name == "SYNC") {
        return "ERR SYNC is only valid as a replication handshake\n";
    }

    if (cmd.name == "REPLICAOF") {
        return "ERR REPLICAOF is not allowed in transactions\n";
    }

    // ---- INFO ----
    if (cmd.name == "INFO") {
        return info_.getInfo();
    }

    if (cmd.name == "AUTH") {
        return "ERR AUTH called without any password configured\n";
    }

    // ---- Unknown command ----
    return "ERR unknown command '" + cmd.name + "'\n";
}

// ---------------------------------------------------------------------------
// Authentication helpers
// ---------------------------------------------------------------------------
bool Server::isAuthenticated(platform::socket_t clientFd) {
    if (config_.requirePassword.empty()) {
        return true;
    }

    std::lock_guard<std::mutex> lock(authMutex_);
    auto it = authenticatedClients_.find(clientFd);
    return it != authenticatedClients_.end() && it->second;
}

void Server::markAuthenticated(platform::socket_t clientFd) {
    std::lock_guard<std::mutex> lock(authMutex_);
    authenticatedClients_[clientFd] = true;
}

// ---------------------------------------------------------------------------
// writeToClient() — thread-safe per-client socket write
// ---------------------------------------------------------------------------
void Server::writeToClient(platform::socket_t clientFd, const std::string& response) {
    std::shared_ptr<std::mutex> writeMutex;
    {
        std::lock_guard<std::mutex> lock(writeMutexMapMutex_);
        auto it = clientWriteMutexes_.find(clientFd);
        if (it == clientWriteMutexes_.end()) {
            return; // already gone
        }
        writeMutex = it->second;
    }

    // Serialize writes to this particular client socket.
    std::lock_guard<std::mutex> clientLock(*writeMutex);

    const char* data = response.c_str();
    size_t remaining = response.size();

    while (remaining > 0) {
        ssize_t written = platform::socketWrite(clientFd, data, remaining);
        if (written < 0) {
            if (platform::wasInterrupted()) {
                continue;  // Interrupted by signal — retry.
            }
            // EPIPE / ECONNRESET → client gone; nothing useful to do.
            break;
        }
        if (written == 0) {
            break;
        }
        data += written;
        remaining -= static_cast<size_t>(written);
    }
}

// ---------------------------------------------------------------------------
// signalHandler() — static handler wired to SIGINT
// ---------------------------------------------------------------------------
void Server::signalHandler(int /*signum*/) {
    std::cout << "\n[FlashDB] Caught interrupt signal. Shutting down...\n";
    if (instance_ != nullptr) {
        instance_->stop();
    }
}

}  // namespace flashdb
