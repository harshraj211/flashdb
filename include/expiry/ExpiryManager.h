#pragma once
#include <string>
#include <unordered_map>
#include <chrono>
#include <mutex>
#include <atomic>
#include <thread>

namespace flashdb {

class StorageEngine;  // Forward declaration

class ExpiryManager {
public:
    ExpiryManager();
    ~ExpiryManager();

    void setExpiry(const std::string& key,
                   std::chrono::steady_clock::time_point expiresAt);
    void setExpirySeconds(const std::string& key, int seconds);
    bool isExpired(const std::string& key) const;
    int getTTL(const std::string& key) const;
    bool hasExpiry(const std::string& key) const;
    void removeExpiry(const std::string& key);
    void clear();

    void startExpiryLoop(StorageEngine& storage,
                         std::chrono::milliseconds cleanupInterval =
                             std::chrono::milliseconds(100));

private:
    std::unordered_map<std::string,
        std::chrono::steady_clock::time_point> expiryMap_;
    mutable std::mutex mutex_;
    std::jthread expiryThread_;
};

} // namespace flashdb
