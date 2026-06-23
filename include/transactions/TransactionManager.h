#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>

namespace flashdb {

struct Command;  // Forward declaration from CommandParser.h

enum class TransactionState {
    NONE,
    QUEUING,
    EXECUTING
};

struct ClientSession {
    int fd;
    TransactionState txState = TransactionState::NONE;
    std::vector<Command> txQueue;
};

class TransactionManager {
public:
    // Begin a transaction for a client
    // Returns error message if already in MULTI, else "OK"
    std::string beginTransaction(int clientFd);
    
    // Queue a command during a transaction
    // Returns "QUEUED"
    std::string queueCommand(int clientFd, const Command& cmd);
    
    // Execute all queued commands atomically
    // Returns vector of results, one per queued command
    // The caller (Server) is responsible for actually executing commands
    // with an exclusive lock held
    std::vector<Command> executeTransaction(int clientFd);
    
    // Discard (abort) a transaction
    // Returns error if not in transaction, else "OK"
    std::string discardTransaction(int clientFd);
    
    // Get transaction state for a client
    TransactionState getState(int clientFd) const;
    
    // Remove client session (called on disconnect)
    void removeClient(int clientFd);
    
    // Check if client is in a transaction
    bool inTransaction(int clientFd) const;

private:
    std::unordered_map<int, ClientSession> sessions_;
    mutable std::mutex mutex_;
    
    // Get or create session
    ClientSession& getSession(int clientFd);
};

} // namespace flashdb
