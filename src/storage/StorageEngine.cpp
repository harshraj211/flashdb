#include "storage/StorageEngine.h"
#include "expiry/ExpiryManager.h"

namespace flashdb {

StorageEngine::StorageEngine() = default;

void StorageEngine::setExpiryManager(ExpiryManager* expiryMgr) {
    expiryMgr_ = expiryMgr;
}

bool StorageEngine::isKeyExpired(const std::string& key) const {
    // ExpiryManager has its own mutex, so it's safe to call while
    // holding the storage shared_lock — no lock ordering violation.
    if (expiryMgr_ && expiryMgr_->isExpired(key)) {
        return true;
    }
    return false;
}

std::optional<std::string> StorageEngine::getInternal(const std::string& key) {
    auto it = store_.find(key);
    if (it == store_.end()) {
        return std::nullopt;
    }
    return it->second;
}

bool StorageEngine::set(const std::string& key, const std::string& value) {
    std::unique_lock lock(rwMutex_);
    store_[key] = value;
    return true;
}

std::optional<std::string> StorageEngine::get(const std::string& key) {
    // Phase 1: shared_lock to check existence and expiry.
    // C++ does not support upgrading a shared_lock to a unique_lock,
    // so we use a two-phase approach: check under shared lock, release,
    // then re-acquire as unique lock if deletion is needed.
    {
        std::shared_lock lock(rwMutex_);
        auto it = store_.find(key);
        if (it == store_.end()) {
            return std::nullopt;
        }

        if (!isKeyExpired(key)) {
            // Key exists and is not expired — return the value.
            return it->second;
        }
    }

    // Phase 2: key is expired — acquire unique_lock to delete it.
    {
        std::unique_lock lock(rwMutex_);

        // Re-check: another thread may have already deleted this key
        // between releasing the shared_lock and acquiring the unique_lock.
        auto it = store_.find(key);
        if (it != store_.end() && isKeyExpired(key)) {
            store_.erase(it);
            if (expiryMgr_) {
                expiryMgr_->removeExpiry(key);
            }
        }
    }

    return std::nullopt;
}

int StorageEngine::del(const std::vector<std::string>& keys) {
    std::unique_lock lock(rwMutex_);
    int deletedCount = 0;

    for (const auto& key : keys) {
        auto it = store_.find(key);
        if (it != store_.end()) {
            store_.erase(it);
            if (expiryMgr_) {
                expiryMgr_->removeExpiry(key);
            }
            ++deletedCount;
        }
    }

    return deletedCount;
}

bool StorageEngine::exists(const std::string& key) {
    std::shared_lock lock(rwMutex_);
    auto it = store_.find(key);
    if (it == store_.end()) {
        return false;
    }

    // Lazy expiry check: if the key is expired, treat it as non-existent.
    // The actual deletion will happen on the next get() or by the expiry loop.
    if (isKeyExpired(key)) {
        return false;
    }

    return true;
}

std::vector<std::string> StorageEngine::keys() {
    std::shared_lock lock(rwMutex_);
    std::vector<std::string> result;
    result.reserve(store_.size());

    for (const auto& [key, value] : store_) {
        // Skip keys that have expired but haven't been cleaned up yet.
        if (!isKeyExpired(key)) {
            result.push_back(key);
        }
    }

    return result;
}

void StorageEngine::flushAll() {
    std::unique_lock lock(rwMutex_);
    store_.clear();
    if (expiryMgr_) {
        expiryMgr_->clear();
    }
}

size_t StorageEngine::size() const {
    std::shared_lock lock(rwMutex_);
    return store_.size();
}

std::vector<std::pair<std::string, std::string>> StorageEngine::getAllEntries() const {
    std::shared_lock lock(rwMutex_);
    std::vector<std::pair<std::string, std::string>> entries;
    entries.reserve(store_.size());

    for (const auto& [key, value] : store_) {
        // Only include non-expired entries for replication.
        if (!isKeyExpired(key)) {
            entries.emplace_back(key, value);
        }
    }

    return entries;
}

} // namespace flashdb
