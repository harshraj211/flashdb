/**
 * @file benchmark.cpp
 * @brief Multi-threaded benchmark tool for FlashDB.
 *
 * Connects to a running FlashDB server and measures throughput (ops/sec)
 * and latency (avg, P99) for write, read, and mixed workloads.
 *
 * Usage:
 *   ./flashdb_benchmark [--host 127.0.0.1] [--port 6379] [--ops 100000] [--threads 10]
 *
 * The server must be running before launching the benchmark.
 */

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <numeric>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "platform/Platform.h"

// ============================================================================
// Configuration & Result Types
// ============================================================================

struct BenchmarkConfig {
    int numOperations = 100000;
    int numThreads = 10;
    int keySize = 16;
    int valueSize = 64;
    std::string host = "127.0.0.1";
    uint16_t port = 6379;
};

struct BenchmarkResult {
    double operationsPerSecond = 0;
    double avgLatencyMs = 0;
    double p99LatencyMs = 0;
    int totalOps = 0;
    int errors = 0;
};

// ============================================================================
// Utility Functions
// ============================================================================

std::string randomString(int length) {
    static const char chars[] =
        "abcdefghijklmnopqrstuvwxyz"
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "0123456789";

    thread_local std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<int> dist(0, sizeof(chars) - 2);

    std::string result;
    result.reserve(length);
    for (int i = 0; i < length; ++i) {
        result += chars[dist(rng)];
    }
    return result;
}

flashdb::platform::socket_t connectToServer(const std::string& host, uint16_t port) {
    flashdb::platform::socket_t fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (!flashdb::platform::isValidSocket(fd)) return flashdb::platform::INVALID_SOCK;

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);

    if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) <= 0) {
        flashdb::platform::closeSocket(fd);
        return flashdb::platform::INVALID_SOCK;
    }

    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        flashdb::platform::closeSocket(fd);
        return flashdb::platform::INVALID_SOCK;
    }

    return fd;
}

std::string sendCommand(flashdb::platform::socket_t fd, const std::string& cmd) {
    std::string msg = cmd + "\n";
    ssize_t sent = flashdb::platform::socketWrite(fd, msg.c_str(), msg.size());
    if (sent <= 0) return "";

    char buffer[4096] = {0};
    ssize_t n = flashdb::platform::socketRead(fd, buffer, sizeof(buffer) - 1);
    if (n <= 0) return "";

    return std::string(buffer, n);
}

// ============================================================================
// Benchmark Runners
// ============================================================================

BenchmarkResult runWriteBenchmark(const BenchmarkConfig& config) {
    std::vector<double> allLatencies;
    std::mutex latencyMutex;
    std::atomic<int> totalErrors{0};
    int opsPerThread = config.numOperations / config.numThreads;

    auto start = std::chrono::steady_clock::now();

    std::vector<std::thread> threads;
    for (int t = 0; t < config.numThreads; ++t) {
        threads.emplace_back([&, t]() {
            flashdb::platform::socket_t fd = connectToServer(config.host, config.port);
            if (!flashdb::platform::isValidSocket(fd)) {
                totalErrors += opsPerThread;
                return;
            }

            std::vector<double> localLatencies;
            localLatencies.reserve(opsPerThread);

            for (int i = 0; i < opsPerThread; ++i) {
                std::string key = randomString(config.keySize);
                std::string value = randomString(config.valueSize);
                std::string cmd = "SET " + key + " " + value;

                auto opStart = std::chrono::steady_clock::now();
                std::string response = sendCommand(fd, cmd);
                auto opEnd = std::chrono::steady_clock::now();

                double latencyMs =
                    std::chrono::duration<double, std::milli>(opEnd - opStart).count();
                localLatencies.push_back(latencyMs);

                if (response.empty() || response.find("OK") == std::string::npos) {
                    totalErrors++;
                }
            }

            flashdb::platform::closeSocket(fd);

            std::lock_guard<std::mutex> lock(latencyMutex);
            allLatencies.insert(allLatencies.end(), localLatencies.begin(),
                                localLatencies.end());
        });
    }

    for (auto& t : threads) t.join();

    auto end = std::chrono::steady_clock::now();
    double totalSeconds = std::chrono::duration<double>(end - start).count();

    // Compute statistics
    BenchmarkResult result;
    result.totalOps = static_cast<int>(allLatencies.size());
    result.errors = totalErrors.load();
    result.operationsPerSecond = result.totalOps / totalSeconds;

    if (!allLatencies.empty()) {
        std::sort(allLatencies.begin(), allLatencies.end());
        double sum = std::accumulate(allLatencies.begin(), allLatencies.end(), 0.0);
        result.avgLatencyMs = sum / allLatencies.size();
        size_t p99Index = static_cast<size_t>(allLatencies.size() * 0.99);
        result.p99LatencyMs = allLatencies[std::min(p99Index, allLatencies.size() - 1)];
    }

    return result;
}

BenchmarkResult runReadBenchmark(const BenchmarkConfig& config) {
    // Phase 1: Pre-populate keys
    int opsPerThread = config.numOperations / config.numThreads;
    std::vector<std::string> allKeys;
    {
        flashdb::platform::socket_t fd = connectToServer(config.host, config.port);
        if (!flashdb::platform::isValidSocket(fd)) {
            std::cerr << "Failed to connect for pre-population\n";
            return {};
        }
        for (int i = 0; i < config.numOperations; ++i) {
            std::string key = "rkey_" + std::to_string(i);
            allKeys.push_back(key);
            sendCommand(fd, "SET " + key + " " + randomString(config.valueSize));
        }
        flashdb::platform::closeSocket(fd);
    }

    // Phase 2: Read benchmark
    std::vector<double> allLatencies;
    std::mutex latencyMutex;
    std::atomic<int> totalErrors{0};

    auto start = std::chrono::steady_clock::now();

    std::vector<std::thread> threads;
    for (int t = 0; t < config.numThreads; ++t) {
        threads.emplace_back([&, t]() {
            flashdb::platform::socket_t fd = connectToServer(config.host, config.port);
            if (!flashdb::platform::isValidSocket(fd)) {
                totalErrors += opsPerThread;
                return;
            }

            std::vector<double> localLatencies;
            localLatencies.reserve(opsPerThread);
            thread_local std::mt19937 rng(std::random_device{}());
            std::uniform_int_distribution<int> dist(0, allKeys.size() - 1);

            for (int i = 0; i < opsPerThread; ++i) {
                const std::string& key = allKeys[dist(rng)];
                auto opStart = std::chrono::steady_clock::now();
                std::string response = sendCommand(fd, "GET " + key);
                auto opEnd = std::chrono::steady_clock::now();

                double latencyMs =
                    std::chrono::duration<double, std::milli>(opEnd - opStart).count();
                localLatencies.push_back(latencyMs);

                if (response.empty()) {
                    totalErrors++;
                }
            }

            flashdb::platform::closeSocket(fd);

            std::lock_guard<std::mutex> lock(latencyMutex);
            allLatencies.insert(allLatencies.end(), localLatencies.begin(),
                                localLatencies.end());
        });
    }

    for (auto& t : threads) t.join();

    auto end = std::chrono::steady_clock::now();
    double totalSeconds = std::chrono::duration<double>(end - start).count();

    BenchmarkResult result;
    result.totalOps = static_cast<int>(allLatencies.size());
    result.errors = totalErrors.load();
    result.operationsPerSecond = result.totalOps / totalSeconds;

    if (!allLatencies.empty()) {
        std::sort(allLatencies.begin(), allLatencies.end());
        double sum = std::accumulate(allLatencies.begin(), allLatencies.end(), 0.0);
        result.avgLatencyMs = sum / allLatencies.size();
        size_t p99Index = static_cast<size_t>(allLatencies.size() * 0.99);
        result.p99LatencyMs = allLatencies[std::min(p99Index, allLatencies.size() - 1)];
    }

    // Cleanup
    {
        flashdb::platform::socket_t fd = connectToServer(config.host, config.port);
        if (flashdb::platform::isValidSocket(fd)) {
            sendCommand(fd, "FLUSHALL");
            flashdb::platform::closeSocket(fd);
        }
    }

    return result;
}

BenchmarkResult runMixedBenchmark(const BenchmarkConfig& config) {
    // 80% reads, 20% writes
    int opsPerThread = config.numOperations / config.numThreads;

    // Pre-populate some keys
    {
        flashdb::platform::socket_t fd = connectToServer(config.host, config.port);
        if (!flashdb::platform::isValidSocket(fd)) return {};
        for (int i = 0; i < 10000; ++i) {
            sendCommand(fd, "SET mkey_" + std::to_string(i) + " " +
                                randomString(config.valueSize));
        }
        flashdb::platform::closeSocket(fd);
    }

    std::vector<double> allLatencies;
    std::mutex latencyMutex;
    std::atomic<int> totalErrors{0};

    auto start = std::chrono::steady_clock::now();

    std::vector<std::thread> threads;
    for (int t = 0; t < config.numThreads; ++t) {
        threads.emplace_back([&, t]() {
            flashdb::platform::socket_t fd = connectToServer(config.host, config.port);
            if (!flashdb::platform::isValidSocket(fd)) {
                totalErrors += opsPerThread;
                return;
            }

            std::vector<double> localLatencies;
            localLatencies.reserve(opsPerThread);
            thread_local std::mt19937 rng(std::random_device{}());
            std::uniform_int_distribution<int> keyDist(0, 9999);
            std::uniform_int_distribution<int> opDist(0, 99);

            for (int i = 0; i < opsPerThread; ++i) {
                std::string cmd;
                int keyIdx = keyDist(rng);

                if (opDist(rng) < 80) {
                    // 80% reads
                    cmd = "GET mkey_" + std::to_string(keyIdx);
                } else {
                    // 20% writes
                    cmd = "SET mkey_" + std::to_string(keyIdx) + " " +
                          randomString(config.valueSize);
                }

                auto opStart = std::chrono::steady_clock::now();
                std::string response = sendCommand(fd, cmd);
                auto opEnd = std::chrono::steady_clock::now();

                double latencyMs =
                    std::chrono::duration<double, std::milli>(opEnd - opStart).count();
                localLatencies.push_back(latencyMs);

                if (response.empty()) {
                    totalErrors++;
                }
            }

            flashdb::platform::closeSocket(fd);

            std::lock_guard<std::mutex> lock(latencyMutex);
            allLatencies.insert(allLatencies.end(), localLatencies.begin(),
                                localLatencies.end());
        });
    }

    for (auto& t : threads) t.join();

    auto end = std::chrono::steady_clock::now();
    double totalSeconds = std::chrono::duration<double>(end - start).count();

    BenchmarkResult result;
    result.totalOps = static_cast<int>(allLatencies.size());
    result.errors = totalErrors.load();
    result.operationsPerSecond = result.totalOps / totalSeconds;

    if (!allLatencies.empty()) {
        std::sort(allLatencies.begin(), allLatencies.end());
        double sum = std::accumulate(allLatencies.begin(), allLatencies.end(), 0.0);
        result.avgLatencyMs = sum / allLatencies.size();
        size_t p99Index = static_cast<size_t>(allLatencies.size() * 0.99);
        result.p99LatencyMs = allLatencies[std::min(p99Index, allLatencies.size() - 1)];
    }

    // Cleanup
    {
        flashdb::platform::socket_t fd = connectToServer(config.host, config.port);
        if (flashdb::platform::isValidSocket(fd)) {
            sendCommand(fd, "FLUSHALL");
            flashdb::platform::closeSocket(fd);
        }
    }

    return result;
}

// ============================================================================
// Report Generation
// ============================================================================

void printResult(const std::string& name, const BenchmarkResult& result) {
    std::cout << "\n  " << name << "\n";
    std::cout << "  ─────────────────────────────────────\n";
    std::cout << std::fixed << std::setprecision(3);
    std::cout << "  Operations:     " << result.totalOps << "\n";
    std::cout << "  Throughput:     " << std::setprecision(0) << result.operationsPerSecond
              << " ops/sec\n";
    std::cout << std::setprecision(3);
    std::cout << "  Avg Latency:    " << result.avgLatencyMs << " ms\n";
    std::cout << "  P99 Latency:    " << result.p99LatencyMs << " ms\n";
    std::cout << "  Errors:         " << result.errors << "\n";
}

void writeReport(const BenchmarkConfig& config, const BenchmarkResult& writeRes,
                 const BenchmarkResult& readRes, const BenchmarkResult& mixedRes) {
    std::ofstream report("benchmarks/benchmark_report.md");
    if (!report.is_open()) {
        std::cerr << "Warning: Could not write benchmark_report.md\n";
        return;
    }

    // Get current time
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);

    report << "# FlashDB Benchmark Report\n\n";
    report << "Date: " << std::ctime(&time);
    report << "Target: " << config.host << ":" << config.port << "\n";
    report << "Threads: " << config.numThreads << "\n\n";

    report << std::fixed;

    report << "## Write Benchmark\n";
    report << "| Metric | Value |\n|--------|-------|\n";
    report << "| Operations | " << writeRes.totalOps << " |\n";
    report << std::setprecision(0);
    report << "| Throughput | " << writeRes.operationsPerSecond << " ops/sec |\n";
    report << std::setprecision(3);
    report << "| Avg Latency | " << writeRes.avgLatencyMs << " ms |\n";
    report << "| P99 Latency | " << writeRes.p99LatencyMs << " ms |\n";
    report << "| Errors | " << writeRes.errors << " |\n\n";

    report << "## Read Benchmark\n";
    report << "| Metric | Value |\n|--------|-------|\n";
    report << "| Operations | " << readRes.totalOps << " |\n";
    report << std::setprecision(0);
    report << "| Throughput | " << readRes.operationsPerSecond << " ops/sec |\n";
    report << std::setprecision(3);
    report << "| Avg Latency | " << readRes.avgLatencyMs << " ms |\n";
    report << "| P99 Latency | " << readRes.p99LatencyMs << " ms |\n";
    report << "| Errors | " << readRes.errors << " |\n\n";

    report << "## Mixed Benchmark (80% Read / 20% Write)\n";
    report << "| Metric | Value |\n|--------|-------|\n";
    report << "| Operations | " << mixedRes.totalOps << " |\n";
    report << std::setprecision(0);
    report << "| Throughput | " << mixedRes.operationsPerSecond << " ops/sec |\n";
    report << std::setprecision(3);
    report << "| Avg Latency | " << mixedRes.avgLatencyMs << " ms |\n";
    report << "| P99 Latency | " << mixedRes.p99LatencyMs << " ms |\n";
    report << "| Errors | " << mixedRes.errors << " |\n";

    report.close();
    std::cout << "\n  Report written to benchmarks/benchmark_report.md\n";
}

// ============================================================================
// Main
// ============================================================================

int main(int argc, char* argv[]) {
    if (!flashdb::platform::initNetworking()) {
        std::cerr << "Failed to initialize platform networking.\n";
        return 1;
    }

    BenchmarkConfig config;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--host" && i + 1 < argc) {
            config.host = argv[++i];
        } else if (arg == "--port" && i + 1 < argc) {
            config.port = static_cast<uint16_t>(std::stoi(argv[++i]));
        } else if (arg == "--ops" && i + 1 < argc) {
            config.numOperations = std::stoi(argv[++i]);
        } else if (arg == "--threads" && i + 1 < argc) {
            config.numThreads = std::stoi(argv[++i]);
        } else if (arg == "--help") {
            std::cout << "Usage: flashdb_benchmark [OPTIONS]\n"
                      << "  --host HOST     Server host (default: 127.0.0.1)\n"
                      << "  --port PORT     Server port (default: 6379)\n"
                      << "  --ops N         Total operations (default: 100000)\n"
                      << "  --threads N     Number of threads (default: 10)\n";
            flashdb::platform::cleanupNetworking();
            return 0;
        }
    }

    std::cout << "╔══════════════════════════════════════╗\n";
    std::cout << "║     FlashDB Benchmark Tool           ║\n";
    std::cout << "╚══════════════════════════════════════╝\n";
    std::cout << "  Target:     " << config.host << ":" << config.port << "\n";
    std::cout << "  Operations: " << config.numOperations << "\n";
    std::cout << "  Threads:    " << config.numThreads << "\n";

    // Verify connection
    flashdb::platform::socket_t testFd = connectToServer(config.host, config.port);
    if (!flashdb::platform::isValidSocket(testFd)) {
        std::cerr << "\n  ERROR: Cannot connect to FlashDB at " << config.host << ":"
                  << config.port << "\n";
        std::cerr << "  Make sure the server is running first.\n";
        flashdb::platform::cleanupNetworking();
        return 1;
    }
    flashdb::platform::closeSocket(testFd);

    std::cout << "\n  Running write benchmark...\n";
    auto writeResult = runWriteBenchmark(config);
    printResult("Write Benchmark", writeResult);

    std::cout << "\n  Running read benchmark...\n";
    auto readResult = runReadBenchmark(config);
    printResult("Read Benchmark", readResult);

    std::cout << "\n  Running mixed benchmark (80/20)...\n";
    auto mixedResult = runMixedBenchmark(config);
    printResult("Mixed Benchmark (80% Read / 20% Write)", mixedResult);

    writeReport(config, writeResult, readResult, mixedResult);

    flashdb::platform::cleanupNetworking();
    return 0;
}
