#include "monitoring/InfoManager.h"
#include "storage/StorageEngine.h"
#include "replication/ReplicationManager.h"

#include <string>

namespace flashdb {

InfoManager::InfoManager(StorageEngine& storage, ReplicationManager& replication)
    : storage_(storage)
    , replication_(replication)
    , startTime_(std::chrono::steady_clock::now()) {
}

std::string InfoManager::getInfo() const {
    std::string info;
    info.reserve(512);

    // Server section
    info += "# Server\n";
    info += "uptime_seconds:" + std::to_string(getUptimeSeconds()) + "\n";

    // Clients section
    info += "# Clients\n";
    info += "connected_clients:" + std::to_string(connectedClients_.load()) + "\n";

    // Keyspace section
    info += "# Keyspace\n";
    info += "keys:" + std::to_string(storage_.size()) + "\n";

    // Stats section
    info += "# Stats\n";
    info += "total_commands_processed:" + std::to_string(totalCommandsProcessed_.load()) + "\n";

    // Replication section
    info += "# Replication\n";
    info += "role:";
    info += (replication_.isMaster() ? "master" : "slave");
    info += "\n";
    info += "connected_replicas:" + std::to_string(replication_.replicaCount()) + "\n";

    return info;
}

void InfoManager::clientConnected() {
    connectedClients_.fetch_add(1, std::memory_order_relaxed);
}

void InfoManager::clientDisconnected() {
    connectedClients_.fetch_sub(1, std::memory_order_relaxed);
}

void InfoManager::commandProcessed() {
    totalCommandsProcessed_.fetch_add(1, std::memory_order_relaxed);
}

long long InfoManager::getUptimeSeconds() const {
    auto now = std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::seconds>(now - startTime_).count();
}

} // namespace flashdb
