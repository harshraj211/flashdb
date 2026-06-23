# Design Decisions

This document explains the key architectural decisions made in FlashDB, the
tradeoffs involved, and how a production system would address the limitations.

---

## 1. Why `unordered_map` over `map`?

`std::map` uses a red-black tree internally. Every operation — insert, lookup,
delete — is O(log n). For a database with 1 million keys, that's approximately
20 comparisons per lookup, each involving string comparison and pointer chasing
through a tree structure that is inherently cache-unfriendly.

`std::unordered_map` uses a hash table. In the average case, operations are O(1):
a single hash computation followed by an array index and (usually) a short
collision chain walk. For a database doing millions of operations per second,
this difference is enormous — potentially 20x fewer comparisons per operation.

**Tradeoffs we accept:**
- **Hash collisions** can degrade to O(n) in the worst case. This is mitigated by
  the default hash function's quality and the load factor (default 1.0).
- **Memory overhead**: Hash tables use more memory than trees due to bucket arrays
  and load factor headroom. For FlashDB, this is acceptable — memory is our
  primary resource anyway.
- **No ordering**: `KEYS` returns keys in arbitrary order. If sorted output were
  needed, we'd sort the result vector (O(n log n) per call, not per lookup).
- **Rehashing pauses**: When the load factor exceeds the threshold, the entire table
  is rehashed — an O(n) operation that causes a latency spike. In production Redis,
  this is mitigated by incremental rehashing across multiple operations.

**Interview insight**: If asked "when would you use `map`?", the answer is: when
you need ordered iteration (range queries), when your keys don't have a good hash
function, or when worst-case O(log n) is preferable to amortized O(1) with
occasional O(n) rehashing spikes.

---

## 2. Why AOF over RDB Snapshots?

The Append-Only File (AOF) approach writes every mutation command to disk as it
happens. In the worst case — a crash between two writes — you lose exactly one
command. This gives a Recovery Point Objective (RPO) of approximately one operation.

RDB snapshots take a point-in-time copy of the entire dataset to a binary file.
They're fast to load on restart but inherently lose data: everything written between
the last snapshot and the crash is gone. With snapshots every 5 minutes, you could
lose up to 5 minutes of writes.

**Why AOF for FlashDB:**
- **Simplicity**: The AOF format is human-readable (`SET key value\n`). You can
  open it in a text editor, debug it, manually edit it. RDB is a binary format
  requiring a dedicated encoder/decoder.
- **Durability**: One command RPO vs. minutes of data loss.
- **Code reuse**: `loadAOF()` replays commands through the same parser and storage
  engine code path. No separate serialization/deserialization logic needed.

**Tradeoffs we accept:**
- **File size**: AOF grows without bound. A key written 1000 times appears 1000 times.
  Redis solves this with AOF rewriting (background process that compacts the file to
  only the current state).
- **Startup time**: Replaying millions of commands is slower than loading a binary
  snapshot. Redis uses a hybrid: load the RDB snapshot first, then replay only the
  AOF entries written after the snapshot.

---

## 3. Why Thread-per-Client over Event Loop?

The thread-per-client model spawns a new `std::thread` for every client connection.
Each thread has its own stack, runs its own read/process/write loop, and can block
on I/O without affecting other clients.

An event loop (using `epoll` on Linux) multiplexes all connections in a single
thread. It never blocks on any single client — instead, the OS tells the server
which sockets have data ready, and the server processes them in a tight loop.

**Why thread-per-client for FlashDB:**
- **Simplicity**: Each client's lifecycle is a straightforward loop: read → parse →
  execute → respond. The code reads sequentially, is easy to debug with GDB, and
  has obvious error handling (try/catch per thread).
- **Sufficient for learning**: For hundreds of concurrent clients, thread-per-client
  works fine. The bottleneck is memory (each thread stack is ~8MB by default on
  Linux) and context switching overhead, but these only matter at scale.
- **Natural parallelism**: Multiple client threads genuinely run in parallel on
  multi-core CPUs. An event loop processes one event at a time (per thread).

**Tradeoffs we accept:**
- **C10K problem**: With 10,000 clients, you'd have 10,000 threads consuming ~80GB
  of stack space and causing heavy context switching. This is why production servers
  like Redis, Nginx, and Node.js use event loops.
- **Thread overhead**: Creating and destroying threads has non-trivial cost. Thread
  pools mitigate this but add complexity.

**How to fix at scale**: Replace with `epoll` (Linux) or `io_uring` (Linux 5.1+).
Use a small thread pool (1 thread per CPU core) with non-blocking I/O. This is
listed as a stretch goal.

---

## 4. Why `shared_mutex` for Reader-Writer Locking?

Cache workloads are overwhelmingly read-heavy. In a typical production cache, 80-95%
of operations are reads (GET, EXISTS, KEYS) and only 5-20% are writes (SET, DEL).
With a simple `std::mutex`, every operation — read or write — would serialize,
meaning only one client could access the storage at a time.

`std::shared_mutex` allows multiple readers to proceed simultaneously. Only writes
acquire an exclusive lock. For our workload profile, this means 80%+ of operations
can run concurrently, dramatically improving throughput.

**How it works in FlashDB:**
- `GET`, `EXISTS`, `KEYS`, `TTL`, `SIZE` acquire `std::shared_lock` (shared/read).
- `SET`, `DEL`, `FLUSHALL`, `EXPIRE` acquire `std::unique_lock` (exclusive/write).
- When a writer holds the lock, all readers and other writers block.
- When readers hold the lock, other readers proceed but writers block.

**Tradeoffs:**
- `shared_mutex` has higher per-operation overhead than `mutex` (approximately 2x
  on uncontended paths). For write-heavy workloads, a simple `mutex` may actually
  be faster.
- Writer starvation: If readers continuously hold the shared lock, writers may wait
  indefinitely. The C++ standard does not specify fairness guarantees for
  `shared_mutex`. In practice, most implementations use a fair (FIFO) policy.

---

## 5. Known Limitations and How to Fix Them at Scale

### Thread-per-Client → `epoll` / `io_uring`
As described above, the thread model doesn't scale past ~1000 clients. Replace with
an event loop using `epoll` for O(1) readiness notification, or `io_uring` for
zero-copy asynchronous I/O.

### Single-Node → Clustering / Sharding
FlashDB runs on a single machine. For datasets larger than memory or throughput
beyond one machine, implement consistent hash ring sharding (like Redis Cluster)
where different key ranges are owned by different nodes.

### AOF Grows Unbounded → Background Rewrite
The AOF file grows forever. Implement AOF rewriting: fork a child process that
writes the current dataset state to a new AOF file, then atomically replaces the
old file. This is how Redis keeps the AOF file size manageable.

### Strings Only → Data Structures
FlashDB only supports string values. Redis supports lists, sets, sorted sets,
hashes, streams, and more. Each data structure would require its own storage
and command set.

### No Authentication → AUTH Command
Any client can connect and execute any command. Implement `AUTH password` with
SHA-256 hashed passwords stored in configuration. Reject all commands until
`AUTH` succeeds.

### Memory Unbounded → LRU Eviction
FlashDB has no memory limit. Implement an LRU (Least Recently Used) eviction
policy using a doubly-linked list + hash map for O(1) eviction. When memory
exceeds a configured threshold, evict the least recently accessed key.

### No TLS → Encrypted Connections
All traffic is plaintext. For production use, wrap sockets with OpenSSL/TLS
to encrypt data in transit.

---

## 6. Extra Arguments Handling

**Decision**: Extra arguments beyond the minimum required are silently accepted.

For example, `GET key extra stuff` is valid — the parser accepts it because `GET`
requires at least 1 argument and receives 3. The extra tokens are available in
`cmd.args` but are ignored by the command handler.

**Rationale**: This matches Redis behavior, where many commands accept optional
flags and arguments that older implementations silently ignore. It also makes the
protocol more forward-compatible — new flags can be added without breaking existing
parsers.

**Alternative**: Strict mode that rejects any command with more arguments than
expected. This would catch typos but break forward compatibility.

---

## 7. Expiry: Relative vs. Absolute Timestamps in AOF

**The Problem**: When `SET token abc EX 60` is written to AOF and replayed on
restart, it sets the expiry to 60 seconds from *now* — the restart time — not
from the original set time. A key that should have expired hours ago comes back
alive for 60 more seconds.

**Example scenario:**
```
T=0s:    Client runs SET session xyz EX 3600 (1 hour TTL)
T=3601s: Key expires in memory
T=7200s: Server crashes and restarts
T=7200s: AOF replays SET session xyz EX 3600
T=7200s: Key is alive again with 1 hour TTL!
```

**How Redis solves this:**
1. Redis stores expiry as absolute UNIX timestamps in the AOF file:
   `SET session xyz PXAT 1705334400000` (millisecond Unix timestamp)
2. On replay, if the timestamp is in the past, the key is immediately expired.
3. Redis periodically rewrites the AOF to remove entries for already-expired keys.

**FlashDB's position**: We document this as a known limitation. Implementing
absolute timestamps would require:
- Converting `EX seconds` to `PXAT timestamp` before writing to AOF
- Using `system_clock` for AOF timestamps (wall-clock, not monotonic)
- Handling clock skew between save and restore

This is a deliberate simplification for a learning project, but understanding
*why* it's wrong and *how* Redis fixes it is the learning objective.

---

## 8. Separation of Storage and Expiry (Option B)

**Decision**: ExpiryManager is a separate class from StorageEngine.

**Option A** (rejected): StorageEngine owns the expiry map and checks expiry on
every `get()` call internally. Simpler, but couples two concerns: data storage
and time-based lifecycle management.

**Option B** (chosen): ExpiryManager maintains its own expiry map with its own
mutex. StorageEngine holds a pointer to ExpiryManager and queries it during
read operations. This has several advantages:

1. **Single Responsibility**: StorageEngine handles data, ExpiryManager handles time.
2. **Independent testing**: Each class can be unit-tested in isolation.
3. **Independent locking**: The expiry map has its own mutex, reducing contention
   with the storage map's shared_mutex.
4. **Extensibility**: The expiry strategy can be swapped (e.g., from lazy to
   timer-wheel) without modifying StorageEngine.

The main complexity this introduces is the circular dependency: StorageEngine
calls ExpiryManager to check expiry, and ExpiryManager's background thread calls
StorageEngine to delete expired keys. This is resolved with a carefully designed
locking hierarchy documented in `docs/architecture.md`.
