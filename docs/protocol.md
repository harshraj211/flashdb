# FlashDB Protocol Specification

This document describes the wire protocol used by FlashDB for client-server
communication.

## Wire Format

FlashDB uses a simple, human-readable, newline-delimited text protocol:

- **Client → Server**: `COMMAND arg1 arg2 ...\n`
- **Server → Client**: `response\n`

All communication is over TCP. Commands are terminated by `\n` (LF) or `\r\n`
(CRLF). The server strips both before parsing. Multiple spaces between tokens
are treated as a single delimiter.

### Comparison with Redis RESP Protocol

Redis uses the **REdis Serialization Protocol (RESP)**, a binary-safe protocol:

```
*3\r\n$3\r\nSET\r\n$4\r\nname\r\n$5\r\nHarsh\r\n
```

FlashDB's text protocol is simpler but has limitations:
- No binary-safe values (values cannot contain spaces or newlines)
- No structured error types
- No bulk responses

The RESP protocol is listed as a stretch goal. Implementing it would make FlashDB
compatible with `redis-cli` and any Redis client library.

---

## Command Reference

### Data Commands

#### SET

Store a key-value pair. Overwrites existing values.

```
SET key value [EX seconds]
```

| Parameter | Required | Description |
|-----------|----------|-------------|
| key       | Yes      | The key to store |
| value     | Yes      | The value to associate |
| EX seconds | No     | Set expiry in seconds |

**Response**: `OK`

**Examples**:
```
SET name Harsh         → OK
SET token abc EX 60    → OK
SET name NewValue      → OK  (overwrites)
```

---

#### GET

Retrieve the value associated with a key.

```
GET key
```

**Response**: The value, or `(nil)` if the key doesn't exist or has expired.

**Examples**:
```
GET name               → Harsh
GET nonexistent        → (nil)
GET expired_key        → (nil)
```

---

#### DEL

Delete one or more keys.

```
DEL key [key ...]
```

**Response**: `(integer) N` where N is the number of keys actually deleted.

**Examples**:
```
DEL name               → (integer) 1
DEL a b c              → (integer) 3
DEL nonexistent        → (integer) 0
```

---

#### EXISTS

Check if a key exists (and is not expired).

```
EXISTS key
```

**Response**: `(integer) 1` if the key exists, `(integer) 0` otherwise.

---

#### KEYS

Return all non-expired keys in the database.

```
KEYS
```

**Response**: Numbered list of keys, or `(empty list)`.

**Example**:
```
KEYS
1) name
2) city
3) token
```

---

#### FLUSHALL

Delete all keys and their associated expiry records.

```
FLUSHALL
```

**Response**: `OK`

---

### Expiry Commands

#### TTL

Get the remaining time-to-live for a key, in seconds.

```
TTL key
```

**Response**:
- `(integer) N` — remaining seconds until expiry
- `(integer) -1` — key exists but has no expiry set
- `(integer) -2` — key does not exist or has already expired

---

#### EXPIRE

Set a timeout on an existing key.

```
EXPIRE key seconds
```

**Response**: `(integer) 1` if the timeout was set, `(integer) 0` if the key
does not exist.

---

### Pub/Sub Commands

#### SUBSCRIBE

Subscribe to one or more channels. The client enters subscription mode and can
only send `SUBSCRIBE`, `UNSUBSCRIBE`, and `PING` commands until unsubscribed.

```
SUBSCRIBE channel
```

**Response**: `Subscribed to channel (N total)` where N is the total number of
subscriptions for this client.

---

#### PUBLISH

Publish a message to a channel. All subscribers receive the message.

```
PUBLISH channel message
```

**Response**: `(integer) N` where N is the number of clients that received the
message.

**Subscriber receives**: `message channel <the message content>`

---

### Transaction Commands

#### MULTI

Begin a transaction. All subsequent commands are queued, not executed.

```
MULTI
```

**Response**: `OK`
**Error**: `ERR MULTI calls can not be nested` (if already in MULTI)

---

#### EXEC

Execute all queued commands atomically.

```
EXEC
```

**Response**: Results of each queued command, numbered:
```
1) OK
2) OK
3) Harsh
```

**Error**: `ERR EXEC without MULTI` (if not in a transaction)

---

#### DISCARD

Abort the current transaction, clearing the command queue.

```
DISCARD
```

**Response**: `OK`
**Error**: `ERR DISCARD without MULTI` (if not in a transaction)

---

### Transaction Example

```
MULTI                  → OK
SET counter 100        → QUEUED
SET name Harsh         → QUEUED
GET counter            → QUEUED
EXEC
1) OK
2) OK
3) 100
```

---

### Replication Commands

#### REPLICAOF

Configure this server as a replica of the specified master.

```
REPLICAOF host port
```

**Response**: `OK`

This initiates:
1. TCP connection to master
2. Full dataset synchronization (FULLSYNC protocol)
3. Ongoing command propagation

---

### Replication Protocol (Internal)

The replication stream between master and replica uses the same text protocol:

```
Master → Replica: FULLSYNC\n
Master → Replica: SET key1 value1\n
Master → Replica: SET key2 value2\n
Master → Replica: FULLSYNC_DONE\n
Master → Replica: SET newkey newvalue\n    (ongoing propagation)
```

---

### Monitoring Commands

#### INFO

Return server statistics and configuration.

```
INFO
```

**Response**: Multi-line key-value pairs grouped by section:
```
# Server
uptime_seconds:3600
# Clients
connected_clients:5
# Keyspace
keys:1523
# Stats
total_commands_processed:48291
# Replication
role:master
connected_replicas:1
```

---

#### PING

Health check command.

```
PING
```

**Response**: `PONG`

---

## Error Responses

Errors are returned as plain text strings prefixed with `ERR`:

```
ERR unknown command 'FOOBAR'
ERR SET requires at least 2 arguments
ERR EXEC without MULTI
ERR MULTI calls can not be nested
ERR DISCARD without MULTI
```

---

## Connection Lifecycle

1. Client connects via TCP to `host:port`
2. Client sends commands, one per line
3. Server responds to each command
4. Connection persists across multiple commands (keep-alive)
5. Client disconnects by closing the TCP connection
6. Server cleans up: removes pub/sub subscriptions, discards pending transactions

---

## Subscription Mode

When a client sends `SUBSCRIBE`, it enters subscription mode:

- Only `SUBSCRIBE`, `UNSUBSCRIBE`, and `PING` are accepted
- All other commands return an error
- Messages from published channels are pushed asynchronously
- Client exits subscription mode when all channels are unsubscribed
