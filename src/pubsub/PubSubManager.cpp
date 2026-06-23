#include "pubsub/PubSubManager.h"

namespace flashdb {

int PubSubManager::subscribe(const std::string& channel, int clientFd) {
    std::lock_guard<std::mutex> lock(mutex_);
    channels_[channel].insert(clientFd);
    clientChannels_[clientFd].insert(channel);
    return static_cast<int>(clientChannels_[clientFd].size());
}

int PubSubManager::unsubscribe(const std::string& channel, int clientFd) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Remove fd from the channel's subscriber set
    auto channelIt = channels_.find(channel);
    if (channelIt != channels_.end()) {
        channelIt->second.erase(clientFd);
        if (channelIt->second.empty()) {
            channels_.erase(channelIt);
        }
    }

    // Remove channel from the client's subscription set
    auto clientIt = clientChannels_.find(clientFd);
    if (clientIt != clientChannels_.end()) {
        clientIt->second.erase(channel);
        int remaining = static_cast<int>(clientIt->second.size());
        if (clientIt->second.empty()) {
            clientChannels_.erase(clientIt);
        }
        return remaining;
    }

    return 0;
}

int PubSubManager::publish(const std::string& channel, const std::string& message,
                           std::function<void(int fd, const std::string& msg)> writeFn) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = channels_.find(channel);
    if (it == channels_.end()) {
        return 0;
    }

    // Format the pub/sub message: "message <channel> <message>\n"
    std::string formatted = "message " + channel + " " + message + "\n";

    int notified = 0;
    for (int fd : it->second) {
        writeFn(fd, formatted);
        ++notified;
    }

    return notified;
}

void PubSubManager::removeClient(int clientFd) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto clientIt = clientChannels_.find(clientFd);
    if (clientIt == clientChannels_.end()) {
        return;
    }

    // Remove this client from every channel it subscribed to
    for (const auto& channel : clientIt->second) {
        auto channelIt = channels_.find(channel);
        if (channelIt != channels_.end()) {
            channelIt->second.erase(clientFd);
            if (channelIt->second.empty()) {
                channels_.erase(channelIt);
            }
        }
    }

    // Remove the client's reverse index entry
    clientChannels_.erase(clientIt);
}

bool PubSubManager::isSubscribed(int clientFd) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = clientChannels_.find(clientFd);
    return it != clientChannels_.end() && !it->second.empty();
}

} // namespace flashdb
