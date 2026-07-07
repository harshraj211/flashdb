# FlashDB

FlashDB is a production-inspired, Redis-like, in-memory key-value database
built from scratch in C++20.

FlashDB ek Redis-inspired in-memory key-value database hai jo C++20 mein
scratch se banaya gaya hai. Iska goal Redis replace karna nahi hai; iska
goal database internals, networking, concurrency, persistence, TTL,
transactions, pub/sub, replication, aur benchmarking ko practically samajhna
hai.

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

English:

The client connects over TCP. The server reads commands from the socket,
parses them into structured commands, executes them against the storage
engine or supporting managers, and writes responses back to the client.

Roman Hindi:

Client TCP ke through connect karta hai. Server socket se command read karta
hai, parser us command ko structured form mein convert karta hai, phir command
storage engine ya kisi manager ke through execute hoti hai, aur response
client ko wapas bheja jaata hai.

## Core Components

### TCP Server

English:

The TCP server accepts connections and creates one thread for each client.
Each client thread reads commands, executes them, and sends responses.

Roman Hindi:

TCP server clients accept karta hai aur har client ke liye ek thread create
karta hai. Har thread apne client ki commands read, execute aur respond karta
hai.

Important concepts:

- Sockets
- Blocking I/O
- Thread-per-client model
- Client lifecycle
- Graceful shutdown

### Command Parser

English:

The parser converts raw text commands into a `Command` object.

Example:

```text
Input:  SET name Harsh
Output: Command{name="SET", args=["name", "Harsh"]}
```

Roman Hindi:

Parser raw text command ko `Command` object mein convert karta hai jisme
command name aur arguments hote hain.

### Storage Engine

English:

The storage engine stores data in memory using:

```cpp
std::unordered_map<std::string, std::string>
```

`unordered_map` is used because key-value databases need fast direct key
lookup. Average-case lookup, insert, and delete are O(1).

Roman Hindi:

Storage engine data ko memory mein `unordered_map` ke andar store karta hai.
Key-value database mein direct key lookup important hota hai, isliye
`unordered_map` use kiya gaya hai. Average case mein lookup, insert, delete
O(1) hote hain.

### Expiry Manager

English:

FlashDB supports TTL. A key can expire after a fixed time. Expiry is handled
using two strategies:

- Lazy expiry: check expiry when the key is accessed
- Active expiry: background thread periodically deletes expired keys

Roman Hindi:

FlashDB TTL support karta hai. Key ek fixed time ke baad expire ho sakti hai.
Expiry ke liye do strategies use hoti hain:

- Lazy expiry: key access ke time expiry check hoti hai
- Active expiry: background thread expired keys ko periodically delete karta hai

### AOF Persistence

English:

AOF means Append Only File. Every write command is appended to a file. On
restart, FlashDB replays the AOF to rebuild the database state.

Roman Hindi:

AOF ka full form Append Only File hai. Har write command file mein append hoti
hai. Restart ke baad FlashDB AOF file ko replay karke database state rebuild
karta hai.

Example AOF:

```text
SET name Harsh
SET city Delhi
DEL city
```

### Pub/Sub Manager

English:

Pub/Sub allows clients to subscribe to channels and receive messages published
to those channels.

Roman Hindi:

Pub/Sub clients ko channels subscribe karne aur published messages receive
karne ki facility deta hai.

Example:

```text
SUBSCRIBE news
PUBLISH news hello
```

### Transaction Manager

English:

Transactions allow clients to queue commands using `MULTI` and execute them
atomically using `EXEC`.

Roman Hindi:

Transactions clients ko `MULTI` ke baad commands queue karne aur `EXEC` ke
through atomically execute karne deti hain.

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

English:

Replication allows a replica server to copy data from a master server. It has
two phases:

1. Full sync: master sends current dataset to replica
2. Live propagation: future write commands are forwarded to replicas

Roman Hindi:

Replication replica server ko master ka data copy karne deti hai. Iske do
phases hote hain:

1. Full sync: master current dataset replica ko bhejta hai
2. Live propagation: future write commands replicas ko bheje jaate hain

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

## Important Design Decisions

### Why `unordered_map` instead of `map`?

English:

`unordered_map` gives average O(1) lookup. `map` gives O(log n) lookup and
keeps keys sorted, but FlashDB does not need sorted keys for normal operations.

Roman Hindi:

`unordered_map` average O(1) lookup deta hai. `map` O(log n) hota hai aur keys
sorted rakhta hai, lekin FlashDB ko normal operations ke liye sorted keys nahi
chahiye.

### Why AOF instead of snapshots?

English:

AOF records every write command, so it gives better durability than periodic
snapshots. Snapshots can lose writes made after the last snapshot.

Roman Hindi:

AOF har write command record karta hai, isliye periodic snapshots se better
durability milti hai. Snapshot ke baad jo writes hui hain wo crash mein lose
ho sakti hain.

### Why thread-per-client?

English:

Thread-per-client is simple to understand and implement. It is good for a
learning project, but it does not scale well to very high client counts.

Roman Hindi:

Thread-per-client model samajhne aur implement karne mein simple hai. Learning
project ke liye acha hai, lekin bahut zyada clients ke liye scalable nahi hai.

### Why `std::shared_mutex`?

English:

Read-heavy workloads benefit from allowing multiple readers at the same time.
`std::shared_mutex` allows many readers or one exclusive writer.

Roman Hindi:

Read-heavy workload mein multiple readers ko saath chalne dena useful hota hai.
`std::shared_mutex` many readers ya one exclusive writer allow karta hai.

## Interview Questions and Answers

### Q1. What is FlashDB?

English:

FlashDB is a Redis-inspired in-memory key-value database built in C++20.

Roman Hindi:

FlashDB ek Redis-inspired in-memory key-value database hai jo C++20 mein built
hai.

### Q2. Why did you build it?

English:

To understand database internals, networking, concurrency, persistence, TTL,
transactions, pub/sub, replication, and benchmarking.

Roman Hindi:

Database internals, networking, concurrency, persistence, TTL, transactions,
pub/sub, replication aur benchmarking ko deeply samajhne ke liye.

### Q3. What is the time complexity of `GET`?

English:

Average O(1), worst-case O(n) if many hash collisions happen.

Roman Hindi:

Average O(1), worst-case O(n) agar bahut hash collisions ho jaayein.

### Q4. How is thread safety handled?

English:

The storage engine uses `std::shared_mutex`. Reads take shared locks. Writes
take exclusive locks.

Roman Hindi:

Storage engine `std::shared_mutex` use karta hai. Reads shared lock leti hain.
Writes exclusive lock leti hain.

### Q5. What is TTL?

English:

TTL means Time To Live. It defines how long a key remains valid.

Roman Hindi:

TTL ka matlab Time To Live hai. Ye define karta hai ki key kitni der valid
rahegi.

### Q6. What is lazy expiry?

English:

Lazy expiry checks whether a key is expired only when the key is accessed.

Roman Hindi:

Lazy expiry key access ke time check karti hai ki key expire hui hai ya nahi.

### Q7. What is active expiry?

English:

Active expiry uses a background thread to periodically remove expired keys.

Roman Hindi:

Active expiry background thread use karti hai jo periodically expired keys ko
delete karta hai.

### Q8. Why use both lazy and active expiry?

English:

Lazy expiry avoids constant scanning. Active expiry prevents expired keys that
are never accessed again from staying in memory forever.

Roman Hindi:

Lazy expiry constant scanning avoid karti hai. Active expiry un expired keys
ko memory mein forever rehne se bachati hai jo dobara access nahi hoti.

### Q9. What is AOF?

English:

AOF means Append Only File. It stores every write command on disk for recovery.

Roman Hindi:

AOF ka matlab Append Only File hai. Ye recovery ke liye har write command ko
disk par store karta hai.

### Q10. How does crash recovery work?

English:

On startup, FlashDB reads the AOF file and replays write commands to rebuild
the in-memory state.

Roman Hindi:

Startup ke time FlashDB AOF file read karta hai aur write commands replay
karke in-memory state rebuild karta hai.

### Q11. What are transactions?

English:

Transactions are groups of commands executed atomically with `MULTI` and
`EXEC`.

Roman Hindi:

Transactions commands ka group hota hai jo `MULTI` aur `EXEC` ke through
atomically execute hota hai.

### Q12. How are transactions atomic?

English:

During `EXEC`, queued commands execute while holding the storage exclusive
lock, so other clients cannot interleave storage operations.

Roman Hindi:

`EXEC` ke time queued commands storage exclusive lock ke andar execute hoti
hain, isliye doosre clients beech mein storage operations interleave nahi kar
sakte.

### Q13. What is Pub/Sub?

English:

Pub/Sub is a messaging pattern where clients subscribe to channels and receive
messages published to those channels.

Roman Hindi:

Pub/Sub ek messaging pattern hai jisme clients channels subscribe karte hain
aur un channels par published messages receive karte hain.

### Q14. What is replication?

English:

Replication copies data from a master server to replica servers and forwards
future writes.

Roman Hindi:

Replication master server ka data replica servers par copy karti hai aur
future writes forward karti hai.

### Q15. What is full sync?

English:

Full sync sends the complete current dataset from master to replica.

Roman Hindi:

Full sync master ka complete current dataset replica ko bhejta hai.

### Q16. What is live propagation?

English:

After full sync, every future write command is sent to connected replicas.

Roman Hindi:

Full sync ke baad har future write command connected replicas ko bheji jaati
hai.

### Q17. Why is thread-per-client limited?

English:

Each client needs a thread. Thousands of clients can create high memory usage
and context-switching overhead.

Roman Hindi:

Har client ke liye ek thread chahiye. Thousands of clients memory usage aur
context switching overhead badha dete hain.

### Q18. What is a scalable alternative?

English:

Use an event loop with non-blocking sockets, such as epoll, IOCP, or io_uring.

Roman Hindi:

Scalable alternative non-blocking sockets ke saath event loop hota hai, jaise
epoll, IOCP, ya io_uring.

### Q19. Is FlashDB binary-safe?

English:

No. It currently uses a whitespace-delimited text protocol, so keys and values
cannot safely contain spaces or newlines.

Roman Hindi:

Nahi. Ye whitespace-delimited text protocol use karta hai, isliye keys aur
values spaces ya newlines safely contain nahi kar sakti.

### Q20. How can binary safety be added?

English:

By implementing RESP, the Redis Serialization Protocol, which uses
length-prefixed values.

Roman Hindi:

RESP implement karke binary safety add ki ja sakti hai. RESP length-prefixed
values use karta hai.

### Q21. What is P99 latency?

English:

P99 latency means 99 percent of requests complete within that latency value.

Roman Hindi:

P99 latency ka matlab hai 99 percent requests us latency value ke andar
complete hoti hain.

### Q22. What is throughput?

English:

Throughput is the number of operations completed per second.

Roman Hindi:

Throughput ka matlab hai per second complete hone wale operations ki count.

### Q23. What was recently fixed?

English:

- Invalid `SET ... EX` no longer mutates storage
- Plain `SET` clears old TTL
- Pub/Sub no longer writes while holding the global pub/sub mutex
- Transaction write side effects are batched
- Expiry cleanup interval is configurable
- Replication handshake is wired through internal `SYNC`
- AOF replay matches server TTL behavior more closely

Roman Hindi:

- Invalid `SET ... EX` ab storage mutate nahi karta
- Plain `SET` old TTL clear karta hai
- Pub/Sub global mutex hold karke socket write nahi karta
- Transaction write side effects batch hote hain
- Expiry cleanup interval configurable hai
- Replication handshake internal `SYNC` se wired hai
- AOF replay server TTL behavior ke closer hai

## Deep Interview Topics

An interviewer can ask:

- Difference between `unordered_map` and `map`
- Average vs worst-case hash table complexity
- Hash collisions and rehashing
- `mutex` vs `shared_mutex`
- Race conditions and deadlocks
- Lazy expiry vs active expiry
- AOF vs snapshot persistence
- What happens if AOF is corrupted?
- What happens if server crashes before AOF write?
- Why fsync is expensive
- Why AOF rewrite is needed
- Thread-per-client vs event loop
- Why Redis uses an event loop
- Transaction atomicity
- Replication full sync and live propagation
- Pub/Sub client cleanup
- Binary-safe protocols
- RESP protocol
- P99 latency and throughput
- LRU/LFU eviction design
- Production hardening

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

## Resume Line

Built FlashDB, a Redis-inspired in-memory key-value database in C++20 with TTL
expiration, AOF persistence, pub/sub messaging, MULTI/EXEC transactions,
master-replica replication, concurrency control using shared mutexes, and
benchmarking support.

## Short Interview Pitch

English:

FlashDB is my C++20 systems project where I implemented a Redis-like
in-memory key-value database from scratch. It helped me understand hash-table
storage, TCP networking, command parsing, concurrency with reader-writer
locks, TTL expiration, AOF persistence, pub/sub, transactions, replication,
and benchmarking.

Roman Hindi:

FlashDB mera C++20 systems project hai jisme maine scratch se Redis-like
in-memory key-value database implement kiya. Is project se mujhe hash-table
storage, TCP networking, command parsing, reader-writer locks ke saath
concurrency, TTL expiration, AOF persistence, pub/sub, transactions,
replication aur benchmarking deeply samajhne ko mila.
