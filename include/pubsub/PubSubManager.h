#pragma once
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <functional>

namespace flashdb {

class PubSubManager {
public:
    // Subscribe a client fd to a channel
    // Returns number of channels this client is now subscribed to
    int subscribe(const std::string& channel, int clientFd);
    
    // Unsubscribe from a channel
    // Returns number of channels this client is still subscribed to
    int unsubscribe(const std::string& channel, int clientFd);
    
    // Publish message to all subscribers of a channel
    // Uses writeFn callback to write to client sockets
    // Returns number of clients that received the message
    int publish(const std::string& channel, const std::string& message,
                std::function<void(int fd, const std::string& msg)> writeFn);
    
    // Remove all subscriptions for a disconnecting client
    void removeClient(int clientFd);
    
    // Check if a client is in subscription mode
    bool isSubscribed(int clientFd) const;

private:
    // channel → set of subscriber fds
    std::unordered_map<std::string, std::unordered_set<int>> channels_;
    // fd → set of channels (reverse index for fast cleanup)
    std::unordered_map<int, std::unordered_set<std::string>> clientChannels_;
    mutable std::mutex mutex_;
};

} // namespace flashdb
