cmake_minimum_required(VERSION 3.20)
project(FlashDB VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Compiler warnings
if(MSVC)
    add_compile_options(/W4)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
else()
    add_compile_options(-Wall -Wextra -Wpedantic -Werror=return-type)
endif()

# Source files for the library
set(FLASHDB_SOURCES
    src/config/Config.cpp
    src/parser/CommandParser.cpp
    src/storage/StorageEngine.cpp
    src/expiry/ExpiryManager.cpp
    src/persistence/AOFManager.cpp
    src/pubsub/PubSubManager.cpp
    src/transactions/TransactionManager.cpp
    src/replication/ReplicationManager.cpp
    src/monitoring/InfoManager.cpp
    src/server/Server.cpp
)

# Library target (used by both main executable and tests)
find_package(Threads REQUIRED)
add_library(flashdb_lib STATIC ${FLASHDB_SOURCES})
target_include_directories(flashdb_lib PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(flashdb_lib PUBLIC Threads::Threads)

if(WIN32)
    target_link_libraries(flashdb_lib PUBLIC ws2_32)
endif()

# Main executable
add_executable(flashdb src/main.cpp)
target_link_libraries(flashdb PRIVATE flashdb_lib)

# Tests
enable_testing()
add_subdirectory(tests)

# Benchmarks
add_subdirectory(benchmarks)
