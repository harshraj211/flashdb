#pragma once
#include <string>
#include <chrono>
#include <atomic>

namespace flashdb {

class StorageEngine;
class ReplicationManager;

class InfoManager {
public:
    InfoManager(StorageEngine& storage, ReplicationManager& replication);
    
    // Get formatted INFO string
    std::string getInfo() const;
    
    // Track connected clients
    void clientConnected();
    void clientDisconnected();
    
    // Track commands processed
    void commandProcessed();

private:
    StorageEngine& storage_;
    ReplicationManager& replication_;
    std::chrono::steady_clock::time_point startTime_;
    std::atomic<int> connectedClients_{0};
    std::atomic<long long> totalCommandsProcessed_{0};
    
    long long getUptimeSeconds() const;
};

} // namespace flashdb
