#include "config/Config.h"
#include "server/Server.h"
#include "platform/Platform.h"

#include <iostream>

int main(int argc, char* argv[]) {
    if (!flashdb::platform::initNetworking()) {
        std::cerr << "Failed to initialize networking.\n";
        return 1;
    }

    flashdb::Config config = flashdb::Config::fromArgs(argc, argv);

    std::cout << "=================================\n";
    std::cout << "  FlashDB v1.0.0\n";
    std::cout << "  In-Memory Key-Value Database\n";
    std::cout << "=================================\n";
    std::cout << "Host: " << config.host << "\n";
    std::cout << "Port: " << config.port << "\n";
    std::cout << "AOF:  " << (config.aofEnabled ? config.aofFilePath : "disabled") << "\n";
    std::cout << std::endl;

    {
        flashdb::Server server(config);
        server.start();
    }

    flashdb::platform::cleanupNetworking();
    return 0;
}
