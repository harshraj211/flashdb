#include "expiry/ExpiryManager.h"
#include "storage/StorageEngine.h"
#include <vector>

namespace flashdb {

ExpiryManager::ExpiryManager() = default;

ExpiryManager::~ExpiryManager() = default;

void ExpiryManager::setExpiry(const std::string& key,
                              std::chrono::steady_clock::time_point expiresAt) {
    std::lock_guard lock(mutex_);
    expiryMap_[key] = expiresAt;
}

void ExpiryManager::setExpirySeconds(const std::string& key, int seconds) {
    auto expiresAt = std::chrono::steady_clock::now()
                   + std::chrono::seconds(seconds);
    setExpiry(key, expiresAt);
}

bool ExpiryManager::isExpired(const std::string& key) const {
    std::lock_guard lock(mutex_);
    auto it = expiryMap_.find(key);
    if (it == expiryMap_.end()) {
        // No expiry set for this key — it never expires.
        return false;
    }
    return std::chrono::steady_clock::now() >= it->second;
}

int ExpiryManager::getTTL(const std::string& key) const {
    std::lock_guard lock(mutex_);
    auto it = expiryMap_.find(key);
    if (it == expiryMap_.end()) {
        // Key has no expiry — return -1 (persistent).
        return -1;
    }

    auto now = std::chrono::steady_clock::now();
    auto remaining = std::chrono::duration_cast<std::chrono::seconds>(
        it->second - now
    ).count();

    if (remaining <= 0) {
        // Key has already expired — return -2.
        return -2;
    }

    return static_cast<int>(remaining);
}

bool ExpiryManager::hasExpiry(const std::string& key) const {
    std::lock_guard lock(mutex_);
    return expiryMap_.find(key) != expiryMap_.end();
}

void ExpiryManager::removeExpiry(const std::string& key) {
    std::lock_guard lock(mutex_);
    expiryMap_.erase(key);
}

void ExpiryManager::clear() {
    std::lock_guard lock(mutex_);
    expiryMap_.clear();
}

void ExpiryManager::startExpiryLoop(StorageEngine& storage) {
    if (expiryThread_.joinable()) {
        return; // Already running
    }

    expiryThread_ = std::jthread([this, &storage](std::stop_token stopToken) {
        while (!stopToken.stop_requested()) {
            std::vector<std::string> expiredKeys;

            // Phase 1: Collect expired keys under the ExpiryManager mutex.
            {
                std::lock_guard lock(mutex_);
                auto now = std::chrono::steady_clock::now();

                for (const auto& [key, expiresAt] : expiryMap_) {
                    if (now >= expiresAt) {
                        expiredKeys.push_back(key);
                    }
                }
            }

            // Phase 2: Delete expired keys WITHOUT holding ExpiryManager mutex.
            if (!expiredKeys.empty()) {
                storage.del(expiredKeys);
            }

            // Sleep between cleanup cycles.
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    });
}

} // namespace flashdb
