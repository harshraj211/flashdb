#pragma once
#include <string>
#include <fstream>
#include <mutex>

namespace flashdb {

class StorageEngine;
class ExpiryManager;

class AOFManager {
public:
    explicit AOFManager(const std::string& filePath);
    ~AOFManager();
    
    // Append a write command to AOF file (thread-safe)
    void appendCommand(const std::string& rawCommand);
    
    // Load and replay commands from AOF on startup
    // Returns number of commands successfully replayed
    int loadAOF(StorageEngine& storage, ExpiryManager& expiry);
    
    // Force flush OS buffer to disk
    void sync();
    
    // Close file handle
    void close();
    
    // Check if AOF is enabled/open
    bool isOpen() const;

private:
    std::string filePath_;
    std::ofstream aofFile_;
    std::mutex writeMutex_;
    
    // Ensure the data directory exists
    void ensureDirectory();
};

} // namespace flashdb
