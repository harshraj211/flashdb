# FlashDB

FlashDB is a production-inspired, Redis-like, in-memory key-value database
built from scratch in C++20. Its goal is not to replace Redis, but to serve
as a practical deep-dive into database internals, networking, concurrency,
persistence, TTL, transactions, pub/sub, replication, and benchmarking.

## Highlights

- In-memory key-value storage using `std::unordered_map`
- Average O(1) `GET`, `SET`, and `DEL` operations
- TTL support with lazy expiry and active background cleanup
- Append Only File (AOF) persistence for crash recovery
- Pub/Sub messaging with channels and subscribers
- `MULTI` / `EXEC` / `DISCARD` transactions
- Master-replica replication with full sync and live propagation
- Thread-safe reads and writes using `std::shared_mutex`
- TCP server with a thread-per-client model
- Server stats through the `INFO` command
- Benchmark tool for throughput and latency testing
- GoogleTest test suite
- Docker and GitHub Actions CI support

## Architecture

```text
CLIENT
  -> TCP SERVER
  -> COMMAND PARSER
  -> STORAGE ENGINE
       -> EXPIRY MANAGER
       -> AOF PERSISTENCE
       -> PUB/SUB MANAGER
       -> TRANSACTION MANAGER
       -> REPLICATION MANAGER
       -> INFO MANAGER
```

The client connects over TCP. The server reads commands from the socket,
parses them into structured commands, executes them against the storage
engine or supporting managers, and writes responses back to the client.

## Core Components

### TCP Server

The TCP server accepts connections and creates one thread for each client.
Each client thread reads commands, executes them, and sends responses.

Important concepts:

- Sockets
- Blocking I/O
- Thread-per-client model
- Client lifecycle
- Graceful shutdown

### Command Parser

The parser converts raw text commands into a `Command` object.

Example:

```text
Input:  SET name Harsh
Output: Command{name="SET", args=["name", "Harsh"]}
```

### Storage Engine

The storage engine stores data in memory using:

```cpp
std::unordered_map<std::string, std::string>
```

`unordered_map` is used because key-value databases need fast direct key
lookup. Average-case lookup, insert, and delete are O(1).

### Expiry Manager

FlashDB supports TTL. A key can expire after a fixed time. Expiry is handled
using two strategies:

- **Lazy expiry**: check expiry when the key is accessed
- **Active expiry**: background thread periodically deletes expired keys

### AOF Persistence

AOF means Append Only File. Every write command is appended to a file. On
restart, FlashDB replays the AOF to rebuild the database state.

Example AOF:

```text
SET name Harsh
SET city Delhi
DEL city
```

### Pub/Sub Manager

Pub/Sub allows clients to subscribe to channels and receive messages published
to those channels.

Example:

```text
SUBSCRIBE news
PUBLISH news hello
```

### Transaction Manager

Transactions allow clients to queue commands using `MULTI` and execute them
atomically using `EXEC`.

Example:

```text
MULTI
SET a 1
SET b 2
GET a
EXEC
```

During `EXEC`, FlashDB holds an exclusive storage lock so other clients cannot
interleave storage operations.

### Replication Manager

Replication allows a replica server to copy data from a master server. It has
two phases:

1. **Full sync**: master sends the current dataset to the replica
2. **Live propagation**: future write commands are forwarded to replicas

## Supported Commands

```text
SET key value [EX seconds]
GET key
DEL key [key ...]
EXISTS key
KEYS
FLUSHALL
TTL key
EXPIRE key seconds
SUBSCRIBE channel
UNSUBSCRIBE channel
PUBLISH channel message
MULTI
EXEC
DISCARD
REPLICAOF host port
INFO
PING
AUTH password
```

## Build Instructions

### Windows

```powershell
cmake -B build -S . -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
```

### Linux

```bash
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

### Run Server

```bash
./flashdb
```

Windows:

```powershell
.\build\Release\flashdb.exe
```

## Server Options

```text
--host HOST
--port PORT
--aof-path PATH
--no-aof
--requirepass PASS
--expiry-interval MS
--help
```

## Benchmarking

Run the server first, then run:

```bash
./flashdb_benchmark --ops 100000 --threads 10
```

The benchmark measures:

- Write throughput
- Read throughput
- Mixed read/write throughput
- Average latency
- P99 latency

## Design Decisions

### Why `unordered_map` instead of `map`?

`unordered_map` gives average O(1) lookup. `map` gives O(log n) lookup and
keeps keys sorted, but FlashDB does not need sorted keys for normal
operations.

### Why AOF instead of snapshots?

AOF records every write command, so it gives better durability than periodic
snapshots. Snapshots can lose writes made after the last snapshot.

### Why thread-per-client?

Thread-per-client is simple to understand and implement. It works well for a
learning project, but it does not scale well to very high client counts.

### Why `std::shared_mutex`?

Read-heavy workloads benefit from allowing multiple readers at the same time.
`std::shared_mutex` allows many concurrent readers or one exclusive writer.

## Recently Fixed

- Invalid `SET ... EX` no longer mutates storage
- Plain `SET` clears old TTL
- Pub/Sub no longer writes while holding the global pub/sub mutex
- Transaction write side effects are batched
- Expiry cleanup interval is configurable
- Replication handshake is wired through internal `SYNC`
- AOF replay matches server TTL behavior more closely

## Known Limitations

- Not a Redis replacement
- Text protocol is not binary-safe
- RESP support is not implemented yet
- No TLS
- Basic password authentication only
- No clustering or sharding
- No LRU/LFU eviction yet
- AOF rewrite is not implemented yet
- Thread-per-client does not scale to very high client counts

## Future Improvements

- RESP protocol for Redis CLI compatibility
- AOF rewrite and compaction
- Configurable fsync policy
- Snapshot persistence
- LRU/LFU eviction
- TLS support
- Stronger authentication and password hashing
- Better replication acknowledgements
- Integration tests with real TCP clients
- Event-loop networking
- Metrics and observability
- Cluster/sharding support

## Summary

Built FlashDB, a Redis-inspired in-memory key-value database in C++20 with
TTL expiration, AOF persistence, pub/sub messaging, MULTI/EXEC transactions,
master-replica replication, concurrency control using shared mutexes, and
benchmarking support.
