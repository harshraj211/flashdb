#include "transactions/TransactionManager.h"
#include "parser/CommandParser.h"

#include <utility>

namespace flashdb {

std::string TransactionManager::beginTransaction(int clientFd) {
    std::lock_guard<std::mutex> lock(mutex_);
    ClientSession& session = getSession(clientFd);
    if (session.txState == TransactionState::QUEUING) {
        return "ERR MULTI calls can not be nested";
    }
    session.txState = TransactionState::QUEUING;
    session.txQueue.clear();
    return "OK";
}

std::string TransactionManager::queueCommand(int clientFd, const Command& cmd) {
    std::lock_guard<std::mutex> lock(mutex_);
    ClientSession& session = getSession(clientFd);
    session.txQueue.push_back(cmd);
    return "QUEUED";
}

std::vector<Command> TransactionManager::executeTransaction(int clientFd) {
    std::lock_guard<std::mutex> lock(mutex_);
    ClientSession& session = getSession(clientFd);

    // Move the queued commands out for the caller to execute
    session.txState = TransactionState::EXECUTING;
    std::vector<Command> commands = std::move(session.txQueue);

    // Reset session state — the caller (Server) will execute these
    // commands atomically while holding an exclusive lock
    session.txState = TransactionState::NONE;
    session.txQueue.clear();

    return commands;
}

std::string TransactionManager::discardTransaction(int clientFd) {
    std::lock_guard<std::mutex> lock(mutex_);
    ClientSession& session = getSession(clientFd);
    if (session.txState != TransactionState::QUEUING) {
        return "ERR DISCARD without MULTI";
    }
    session.txQueue.clear();
    session.txState = TransactionState::NONE;
    return "OK";
}

TransactionState TransactionManager::getState(int clientFd) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = sessions_.find(clientFd);
    if (it == sessions_.end()) {
        return TransactionState::NONE;
    }
    return it->second.txState;
}

void TransactionManager::removeClient(int clientFd) {
    std::lock_guard<std::mutex> lock(mutex_);
    sessions_.erase(clientFd);
}

bool TransactionManager::inTransaction(int clientFd) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = sessions_.find(clientFd);
    if (it == sessions_.end()) {
        return false;
    }
    return it->second.txState != TransactionState::NONE;
}

ClientSession& TransactionManager::getSession(int clientFd) {
    auto it = sessions_.find(clientFd);
    if (it == sessions_.end()) {
        // Create a new session with default NONE state
        sessions_[clientFd] = ClientSession{clientFd, TransactionState::NONE, {}};
        return sessions_[clientFd];
    }
    return it->second;
}

} // namespace flashdb
