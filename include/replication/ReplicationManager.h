#pragma once
#include <string>
#include <vector>
#include <mutex>
#include <thread>
#include <atomic>
#include <functional>

#include "platform/Platform.h"

namespace flashdb {

class StorageEngine;
class ExpiryManager;

class ReplicationManager {
public:
    ReplicationManager();
    ~ReplicationManager();
    
    // === Master-side methods ===
    
    // Register a new replica connection and send full sync
    // Takes a reference to storage for full sync data
    void addReplica(platform::socket_t replicaFd, StorageEngine& storage);
    
    // Propagate a write command to all connected replicas
    void propagate(const std::string& command);
    
    // Remove a replica that disconnected
    void removeReplica(platform::socket_t replicaFd);
    
    // === Replica-side methods ===
    
    // Connect to master and begin replication
    void connectToMaster(const std::string& host, uint16_t port,
                         StorageEngine& storage, ExpiryManager& expiry);
    
    // Process incoming replication stream (runs in background thread)
    void processReplicationStream(platform::socket_t masterFd,
                                  StorageEngine& storage,
                                  ExpiryManager& expiry,
                                  std::stop_token stopToken);
    
    // === Status ===
    bool isMaster() const { return isMaster_; }
    size_t replicaCount() const;

private:
    std::vector<platform::socket_t> replicaFds_;       // On master: connected replica fds
    mutable std::mutex replicaMutex_;
    std::atomic<bool> isMaster_{true};
    platform::socket_t masterFd_ = platform::INVALID_SOCK; // On replica: connection to master
    std::jthread replicationThread_;
    
    // Send full dataset to a newly connected replica
    // IMPORTANT: Must acquire storage read lock during full sync to ensure
    // atomic snapshot. If keys are written during sync, replica could get
    // partial state.
    bool sendFullSync(platform::socket_t replicaFd, StorageEngine& storage);
    
    // Write a string to a socket fd
    bool writeToFd(platform::socket_t fd, const std::string& data);
};

} // namespace flashdb
