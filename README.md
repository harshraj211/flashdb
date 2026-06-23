# FlashDB

A production-inspired in-memory key-value database built from scratch in C++20.

FlashDB implements the core subsystems of a real database — networking, parsing,
storage, concurrency, persistence, pub/sub messaging, transactions, and replication —
to provide a deep understanding of systems programming from first principles.

## Features

- **In-memory storage** with O(1) average-case lookups via hash table
- **Key expiration** with dual strategy: lazy expiry on access + active background cleanup
- **AOF persistence** for crash recovery — every write command logged to disk
- **Pub/Sub messaging** — publish/subscribe between decoupled clients
- **MULTI/EXEC transactions** — atomic command batching with no interleaving
- **Master-replica replication** — full sync + live command propagation
- **Thread-safe concurrent access** — reader-writer locks (`shared_mutex`)
- **Server monitoring** — uptime, client count, keyspace stats via `INFO`
- **Benchmarking tool** — measure ops/sec and P99 latency under load

## Quick Start

```bash
# Clone the repository
git clone https://github.com/yourusername/flashdb.git
cd flashdb

# Build
mkdir build && cd build
cmake ..
make -j$(nproc)

# Run the server
./flashdb

# In another terminal, connect with telnet
telnet 127.0.0.1 6379
```

```
SET name Harsh
OK
GET name
Harsh
SET token abc EX 60
OK
TTL token
(integer) 59
```

## Build Instructions

### Prerequisites

- **C++20 compiler**: GCC 11+ / Clang 14+ (Linux), MSVC 19.30+ (Visual Studio 2022) / MinGW (Windows)
- **CMake 3.20+**
- **OS**: Windows or Linux (cross-platform socket layer fully integrated)
- **GoogleTest**: Fetched automatically via CMake FetchContent

### Build Commands

Modern CMake supports cross-platform builds out-of-the-box using the `-B` and `--build` flags:

```bash
# 1. Configure the build directory
cmake -B build -S .

# 2. Compile all targets (Server, Benchmark, and Tests)
cmake --build build --config Debug
```

The compiled binaries will be generated under the `build/` directory (e.g. `build/Debug/` or `build/Release/` on Windows, and directly under `build/` on Linux).

### Server Options

```bash
# On Linux:
./build/flashdb [OPTIONS]

# On Windows:
.\build\Debug\flashdb.exe [OPTIONS]

Options:
  --host HOST          Bind address (default: 127.0.0.1)
  --port PORT          Listen port (default: 6379)
  --aof-path PATH      AOF file path (default: data/appendonly.aof)
  --no-aof             Disable AOF persistence
  --help               Show help
```

## Supported Commands

| Command | Syntax | Description |
|---------|--------|-------------|
| **SET** | `SET key value [EX seconds]` | Store a key-value pair, optionally with TTL |
| **GET** | `GET key` | Retrieve value by key |
| **DEL** | `DEL key [key ...]` | Delete one or more keys |
| **EXISTS** | `EXISTS key` | Check if key exists |
| **KEYS** | `KEYS` | List all non-expired keys |
| **FLUSHALL** | `FLUSHALL` | Delete all keys |
| **TTL** | `TTL key` | Get remaining TTL in seconds |
| **EXPIRE** | `EXPIRE key seconds` | Set expiry on existing key |
| **SUBSCRIBE** | `SUBSCRIBE channel` | Subscribe to pub/sub channel |
| **PUBLISH** | `PUBLISH channel message` | Publish to channel |
| **MULTI** | `MULTI` | Begin transaction |
| **EXEC** | `EXEC` | Execute transaction |
| **DISCARD** | `DISCARD` | Abort transaction |
| **REPLICAOF** | `REPLICAOF host port` | Set replication master |
| **INFO** | `INFO` | Server statistics |
| **PING** | `PING` | Health check (returns PONG) |

See [docs/protocol.md](docs/protocol.md) for the complete protocol specification.

## Architecture Overview

```
CLIENT → TCP SERVER → COMMAND PARSER → STORAGE ENGINE → AOF PERSISTENCE
                                     → EXPIRY MANAGER
                                     → PUB/SUB MANAGER
                                     → TRANSACTION MANAGER
                                     → REPLICATION MANAGER
```

FlashDB uses a thread-per-client model with `std::shared_mutex` for reader-writer
locking. Read operations proceed concurrently; write operations acquire exclusive
access. See [docs/architecture.md](docs/architecture.md) for detailed component
descriptions, data flow diagrams, and the locking hierarchy.

## Design Decisions

Key architectural decisions are documented with tradeoff analysis:

1. **`unordered_map` over `map`** — O(1) vs O(log n) for cache workloads
2. **AOF over RDB snapshots** — durability vs. startup speed
3. **Thread-per-client over event loop** — simplicity vs. C10K scalability
4. **`shared_mutex` for read-heavy workloads** — concurrent readers, exclusive writers
5. **Separate ExpiryManager** — single responsibility, independent locking

See [docs/design_decisions.md](docs/design_decisions.md) for the full analysis.

## Running Tests

```bash
cd build

# Run all tests
ctest --output-on-failure

# Or run the test binary directly for verbose output
./tests/flashdb_tests

# Run specific test suite
./tests/flashdb_tests --gtest_filter="ParserTest.*"
./tests/flashdb_tests --gtest_filter="StorageTest.*"
./tests/flashdb_tests --gtest_filter="ExpiryTest.*"
./tests/flashdb_tests --gtest_filter="PubSubTest.*"
./tests/flashdb_tests --gtest_filter="TransactionTest.*"
```

### Test Suites

| Suite | Tests | Coverage |
|-------|-------|----------|
| ParserTest | 20 | Tokenization, case handling, arg validation, edge cases |
| StorageTest | 13 | CRUD, concurrent access, replication support |
| ExpiryTest | 11 | TTL, expiry behavior, timing-based tests |
| PubSubTest | 8 | Subscribe/publish, client cleanup |
| TransactionTest | 11 | MULTI/EXEC/DISCARD lifecycle, error handling |

## Benchmarks

```bash
# Start the server first
./flashdb &

# Run benchmarks
./benchmarks/flashdb_benchmark --ops 100000 --threads 10

# Options
./benchmarks/flashdb_benchmark --help
```

Benchmarks measure:
- **Write throughput**: SET operations with random keys
- **Read throughput**: GET operations on pre-populated data
- **Mixed throughput**: 80% reads, 20% writes (typical cache workload)
- **Latency**: Average and P99 per-operation latency

Results are printed to stdout and written to `benchmarks/benchmark_report.md`.

## Project Structure

```
flashdb/
├── CMakeLists.txt                  # Root build file
├── README.md                       # This file
├── .gitignore
├── .clang-format                   # Google C++ style, 4-space indent
│
├── include/                        # Public headers
│   ├── config/Config.h
│   ├── server/Server.h
│   ├── parser/CommandParser.h
│   ├── storage/StorageEngine.h
│   ├── persistence/AOFManager.h
│   ├── expiry/ExpiryManager.h
│   ├── pubsub/PubSubManager.h
│   ├── replication/ReplicationManager.h
│   ├── transactions/TransactionManager.h
│   ├── platform/Platform.h
│   └── monitoring/InfoManager.h
│
├── src/                            # Implementation files
│   ├── main.cpp
│   ├── config/Config.cpp
│   ├── server/Server.cpp
│   ├── parser/CommandParser.cpp
│   ├── storage/StorageEngine.cpp
│   ├── persistence/AOFManager.cpp
│   ├── expiry/ExpiryManager.cpp
│   ├── pubsub/PubSubManager.cpp
│   ├── replication/ReplicationManager.cpp
│   ├── transactions/TransactionManager.cpp
│   └── monitoring/InfoManager.cpp
│
├── tests/                          # GoogleTest suites
│   ├── CMakeLists.txt
│   ├── test_parser.cpp
│   ├── test_storage.cpp
│   ├── test_expiry.cpp
│   ├── test_pubsub.cpp
│   └── test_transactions.cpp
│
├── benchmarks/                     # Benchmark tool
│   ├── CMakeLists.txt
│   └── benchmark.cpp
│
├── docs/                           # Documentation
│   ├── architecture.md
│   ├── design_decisions.md
│   └── protocol.md
│
└── data/                           # Runtime (gitignored)
    └── appendonly.aof
```

## Contributing

1. Fork the repository
2. Create a feature branch: `git checkout -b feature/my-feature`
3. Commit per feature with descriptive messages
4. Write tests for new functionality
5. Ensure all tests pass: `ctest --output-on-failure`
6. Submit a pull request

## License

This project is built for educational purposes. See individual files for details.
