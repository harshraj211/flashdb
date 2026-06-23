#pragma once
#include <string>
#include <vector>
#include <optional>
#include <unordered_map>
#include <shared_mutex>

namespace flashdb {

class ExpiryManager;  // Forward declaration

class StorageEngine {
    friend class Server;
    friend class ReplicationManager;

public:
    StorageEngine();

    // Set ExpiryManager reference (called during initialization)
    void setExpiryManager(ExpiryManager* expiryMgr);

    bool set(const std::string& key, const std::string& value);
    std::optional<std::string> get(const std::string& key);
    int del(const std::vector<std::string>& keys);
    bool exists(const std::string& key);
    std::vector<std::string> keys();
    void flushAll();
    size_t size() const;

    // Expose the raw shared_mutex for transaction locking
    std::shared_mutex& getMutex() const { return rwMutex_; }

    // Get all key-value pairs (used by replication full sync)
    std::vector<std::pair<std::string, std::string>> getAllEntries() const;

private:
    std::unordered_map<std::string, std::string> store_;
    mutable std::shared_mutex rwMutex_;
    ExpiryManager* expiryMgr_ = nullptr;

    // Unlocked versions of storage operations for use during locked transactions
    bool setUnlocked(const std::string& key, const std::string& value);
    std::optional<std::string> getUnlocked(const std::string& key);
    int delUnlocked(const std::vector<std::string>& keys);
    bool existsUnlocked(const std::string& key);
    std::vector<std::string> keysUnlocked();
    void flushAllUnlocked();
    std::vector<std::pair<std::string, std::string>> getAllEntriesUnlocked() const;

    // Internal get without locking (caller must hold lock)
    std::optional<std::string> getInternal(const std::string& key);
    // Internal lazy expiry check (caller must hold at least shared lock)
    bool isKeyExpired(const std::string& key) const;
};

} // namespace flashdb
