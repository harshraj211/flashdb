# FlashDB Architecture

This document describes the internal architecture of FlashDB, a production-inspired
in-memory key-value database built in C++20.

## System Architecture Diagram

```
┌─────────────────────────────────────────────────────────┐
│                       CLIENT                            │
│              (telnet / redis-cli / app)                  │
└────────────────────────┬────────────────────────────────┘
                         │ TCP (port 6379)
┌────────────────────────▼────────────────────────────────┐
│                    TCP SERVER                            │
│           (accept loop + thread per client)              │
│                                                         │
│  ┌─────────────┐  ┌─────────────┐  ┌─────────────┐     │
│  │  Client A    │  │  Client B    │  │  Client C    │    │
│  │  Thread      │  │  Thread      │  │  Thread      │    │
│  └──────┬──────┘  └──────┬──────┘  └──────┬──────┘     │
└─────────┼────────────────┼────────────────┼─────────────┘
          │                │                │
          └────────────────┼────────────────┘
                           │
┌──────────────────────────▼──────────────────────────────┐
│                   COMMAND PARSER                         │
│           (tokenize → validate → Command{})              │
└──────┬──────────────┬──────────────┬───────────┬────────┘
       │              │              │           │
┌──────▼──────┐ ┌─────▼──────┐ ┌────▼────┐ ┌────▼─────────┐
│  STORAGE    │ │   EXPIRY   │ │ PUB/SUB │ │ TRANSACTION  │
│  ENGINE     │ │  MANAGER   │ │ MANAGER │ │   MANAGER    │
│             │ │            │ │         │ │              │
│ unordered   │ │ lazy +     │ │ channel │ │ MULTI/EXEC   │
│ _map<K,V>   │ │ active     │ │ → fds   │ │ queue        │
│ shared      │ │ expiry     │ │         │ │              │
│ _mutex      │ │ bg thread  │ │         │ │              │
└──────┬──────┘ └────────────┘ └─────────┘ └──────────────┘
       │
       ├──────────────────────────┐
       │                          │
┌──────▼──────┐          ┌───────▼────────┐
│ PERSISTENCE │          │  REPLICATION   │
│  (AOF File) │          │   MANAGER      │
│             │          │                │
│ append-only │          │ full sync +    │
│ write-ahead │          │ cmd propagate  │
└─────────────┘          └────────────────┘
```

## Component Descriptions

### TCP Server (`server/`)

The TCP server is the entry point for all client connections. It creates a cross-platform
TCP socket (abstracted using POSIX sockets on Linux/Unix and Winsock2 on Windows),
binds to a configurable host and port (default `127.0.0.1:6379`), and enters a blocking
`accept()` loop. Each incoming connection is handed off to a dedicated `std::thread`,
enabling simultaneous multi-client access.

The server sets `SO_REUSEADDR` on the listening socket to allow immediate restart
after a crash (avoiding "address already in use" errors from TCP's TIME_WAIT state).
On startup, platform networking is initialized (via WSAStartup on Windows). Signal
handling is registered for `SIGINT` (graceful shutdown) and `SIGPIPE` is ignored on Linux
to prevent crashes when writing to disconnected clients.

Each client thread runs a buffered read loop, accumulating bytes until a complete
newline-delimited command is received. The thread then parses, processes, and
responds to the command. On disconnect, the thread cleans up all client state
(pub/sub subscriptions, transaction queues) before exiting.

### Command Parser (`parser/`)

The command parser transforms raw bytes from the network into structured `Command`
objects. It strips trailing `\r\n` and `\n`, splits on whitespace (handling multiple
consecutive spaces), and converts the command name to uppercase for case-insensitive
matching. Arguments preserve their original casing.

Each known command has a minimum argument count defined in a static lookup table.
The parser validates that the received arguments meet this minimum. Extra arguments
beyond the minimum are silently accepted — this design decision matches Redis
behavior and is documented in `docs/design_decisions.md`. Unknown commands are
parsed as valid; the server is responsible for returning an error response.

### Storage Engine (`storage/`)

The storage engine is the heart of FlashDB. It maps string keys to string values
using `std::unordered_map`, providing O(1) average-case lookups. The choice of
hash table over balanced tree (std::map) is driven by the database's access pattern:
millions of point lookups where constant-time access dominates.

Thread safety is implemented via `std::shared_mutex`, enabling a reader-writer lock
pattern: read operations (`get`, `exists`, `keys`) acquire a shared lock allowing
concurrent readers, while write operations (`set`, `del`, `flushAll`) acquire an
exclusive lock blocking all other access. This is critical for cache workloads that
are typically 80%+ reads.

Lazy expiry is integrated into every read path: before returning a value, the
storage engine checks with the ExpiryManager whether the key has expired. If so,
it transparently deletes the key and returns `nullopt`. Since C++ does not support
upgrading a shared_lock to a unique_lock, a two-phase approach is used: check under
shared lock, release, re-acquire exclusive, re-verify, then delete.

### Expiry Manager (`expiry/`)

The ExpiryManager maintains a separate `std::unordered_map` mapping keys to their
expiry timestamps (`std::chrono::steady_clock::time_point`). Using `steady_clock`
(not `system_clock`) ensures TTL calculations are monotonic and unaffected by NTP
adjustments or manual clock changes.

Two expiry strategies work in tandem:
1. **Lazy expiry**: Every `get()`, `exists()`, and `keys()` call checks the key's
   expiry status. Expired keys are deleted on access. This is the primary mechanism.
2. **Active expiry**: A background thread runs every 100ms, sampling keys from the
   expiry map and deleting those that have passed their deadline. This prevents
   memory waste from keys that are never accessed after expiring.

The background thread uses a careful locking protocol to avoid deadlocks: it collects
expired keys under its own mutex, releases the mutex, then calls `storage.del()`
(which acquires storage's lock and calls back to remove expiry records).

### AOF Persistence (`persistence/`)

The Append-Only File (AOF) provides crash recovery by logging every write command
to disk as it executes. The format is human-readable: one command per line, exactly
as received from the client (e.g., `SET user John`). On startup, the AOF is replayed
line-by-line through the parser and storage engine, reconstructing the database state.

A `std::mutex` protects the file handle to ensure thread-safe writes from multiple
client threads. Each write is followed by a `flush()` to push data to the OS buffer.
For true durability, `fsync()` can be called (the `sync()` method) though this
trades latency for durability guarantees.

Corruption handling is resilient: blank lines are skipped, unparseable lines are
logged and skipped (not fatal), and the total number of skipped lines is reported
at the end of loading. This ensures that partial writes from mid-crash scenarios
don't prevent recovery.

### Pub/Sub Manager (`pubsub/`)

The Pub/Sub system implements a publish/subscribe messaging pattern. Clients
subscribe to named channels with `SUBSCRIBE channel` and enter a special subscription
mode where only `SUBSCRIBE`, `UNSUBSCRIBE`, and `PING` commands are accepted.

Internally, two maps provide O(1) access patterns: a forward map (`channel → set<fd>`)
for efficient message delivery, and a reverse map (`fd → set<channel>`) for O(1)
client disconnect cleanup. When a client disconnects, all their subscriptions are
removed in a single pass through the reverse map.

The `publish()` method takes a write callback function instead of writing directly
to sockets, allowing the server to inject per-client write mutex protection. This
ensures that messages from different publishers don't interleave on a subscriber's
socket.

### Transaction Manager (`transactions/`)

Transactions provide atomic execution of command batches via `MULTI`/`EXEC`/`DISCARD`.
Each client connection has a `ClientSession` tracking its transaction state: `NONE`
(normal), `QUEUING` (between MULTI and EXEC), or `EXECUTING` (briefly during EXEC).

During `QUEUING`, all commands except `EXEC`, `DISCARD`, and `MULTI` are buffered
in a queue rather than executed. On `EXEC`, the server acquires the storage engine's
exclusive lock, executes all queued commands sequentially, and releases the lock.
This guarantees atomicity — no other client can interleave commands.

`DISCARD` clears the command queue and returns the client to normal mode. Nested
`MULTI` is rejected with an error. `EXEC` without prior `MULTI` is similarly rejected.

### Replication Manager (`replication/`)

Replication creates copies of the master's dataset on replica servers for high
availability and read scaling. When a replica connects (via `REPLICAOF host port`),
a two-phase sync begins:

1. **Full sync**: The master acquires a consistent snapshot of all key-value pairs
   via `storage.getAllEntries()` (which holds a shared lock internally), then sends
   each pair as a `SET` command framed between `FULLSYNC` and `FULLSYNC_DONE`
   markers. This atomic snapshot ensures the replica gets a consistent view even
   if writes arrive during sync.

2. **Command propagation**: After full sync, every write command executed on the
   master is asynchronously forwarded to all connected replicas. Failed writes
   trigger replica removal.

On the replica side, a background thread processes the incoming replication stream,
parsing each line as a command and executing it against the local storage engine —
reusing the same code path as AOF replay.

### Info Manager (`monitoring/`)

The InfoManager tracks runtime statistics using lock-free atomic counters for
connected clients and total commands processed. The `INFO` command returns a
multi-section response including uptime, client count, key count, total commands,
and replication role (master/slave with replica count).

## Data Flow: SET Command

```
Client sends: "SET name Harsh\n"
         │
         ▼
    TCP read() → buffer accumulation → extract line
         │
         ▼
    CommandParser::parse("SET name Harsh")
    → Command{name="SET", args=["name","Harsh"], valid=true}
         │
         ▼
    Server::processCommand()
    → Check: pub/sub mode? No
    → Check: transaction mode? No
    → Call executeCommand()
         │
         ▼
    StorageEngine::set("name", "Harsh")
    → unique_lock(rwMutex_)
    → store_["name"] = "Harsh"
    → unlock
         │
         ├──→ AOFManager::appendCommand("SET name Harsh")
         │    → lock(writeMutex_), write to file, flush
         │
         ├──→ ReplicationManager::propagate("SET name Harsh")
         │    → lock(replicaMutex_), write to all replica fds
         │
         ▼
    Server writes "OK\n" to client socket
```

## Threading Model

```
Main Thread
    │
    ├── Signal Handler (SIGINT → stop())
    │
    ├── accept() loop ──────────────────────────────────────
    │                                                       │
    ├── Client Thread A ──→ read → lock → process → unlock → write
    ├── Client Thread B ──→ read → lock → process → unlock → write
    ├── Client Thread C ──→ read → lock → process → unlock → write
    │
    └── Background Threads
        ├── ExpiryManager cleanup (every 100ms)
        └── ReplicationManager stream reader (on replica)
```

## Locking Hierarchy

To prevent deadlocks, locks are always acquired in this order:

1. `StorageEngine::rwMutex_` (outermost — shared or exclusive)
2. `ExpiryManager::mutex_`
3. `AOFManager::writeMutex_`
4. `PubSubManager::mutex_`
5. `TransactionManager::mutex_`
6. `ReplicationManager::replicaMutex_`
7. Per-client write mutexes (innermost)

**Critical rule**: The ExpiryManager's background thread must NOT hold its own mutex
when calling `storage.del()`, because `del()` acquires `rwMutex_` (level 1) and
then calls `removeExpiry()` which acquires `ExpiryManager::mutex_` (level 2). If the
background thread held level 2 and tried to acquire level 1, it would deadlock with
a client thread holding level 1 and waiting for level 2.
