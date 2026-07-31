# StrongKV design

## 1. Scope and source baseline

StrongKV is a standalone, strongly consistent `string -> string` service. A
deployment consists of three voting NuRaft members. Client traffic and Raft
traffic use different TCP ports.

The repository was empty when implementation started. The implementation is
therefore based on the source and examples of
[eBay/NuRaft v3.0.0](https://github.com/eBay/NuRaft/tree/v3.0.0), commit
`0563f31059cae5b45d2cb57576916984d9513630`. NuRaft is fetched by CMake at that
tag. StrongKV does not implement Raft elections, voting, replication,
membership, heartbeat handling, snapshot transport, or quorum calculation.

NuRaft v3.0.0 provides:

- leader election, pre-vote, leadership expiration and voting membership;
- append/replication/commit through `raft_server::append_entries` and
  `append_entries_ext`;
- dynamic membership through `add_srv`, `remove_srv`, and `cluster_config`;
- log compaction coordinated with snapshot creation;
- logical snapshot transport to lagging followers;
- an application-defined durable `log_store` interface;
- persistent term/vote/config hooks through `state_mgr`;
- TLS for Raft transport when built with OpenSSL. StrongKV enables ordinary
  Raft TCP in this prototype; client TLS is out of scope.

The concrete NuRaft APIs used by StrongKV are:

- `nuraft::raft_launcher::init` and `shutdown`;
- `nuraft::raft_server::append_entries_ext`, `is_leader`, `get_leader`,
  `get_term`, `get_last_log_idx`, `get_committed_log_idx`,
  `get_log_idx_at_becoming_leader`, `get_srv_config_all`,
  `get_last_snapshot_idx`, and `is_initialized`;
- `nuraft::state_machine::{commit,commit_config,last_commit_index,
  create_snapshot,last_snapshot,read_logical_snp_obj,
  save_logical_snp_obj,apply_snapshot,free_user_snp_ctx}`;
- `nuraft::state_mgr::{load_config,save_config,save_state,read_state,
  load_log_store,server_id,system_exit}`;
- all required `nuraft::log_store` methods, including `compact` and `flush`;
- `nuraft::raft_params` fields for heartbeat/election timeouts,
  `snapshot_distance_`, `reserved_log_items_`, blocking return mode, client
  timeout, and auto-forwarding;
- `nuraft::cb_func` role and snapshot callbacks.

## 2. Architecture

```text
BCS / strongkv-cli / C++ Client
             |
             | RESP2 subset, client_port
             v
        TcpServer + Connection
             |
             v
        CommandService
        /      |       \
     AUTH     reads     replicated writes
   (session)  ROLE/INFO  SET/DEL/INCR/DECR
                |              |
                |         serialize Command v1
                |              |
                +-----> RaftNode::append()
                              |
                         NuRaft quorum
                              |
                    KvStateMachine::commit()
                              |
                    durable KV state image
```

Modules are deliberately separated:

- `config`: strict parser for the shipped YAML subset and validation;
- `protocol`: incremental RESP2 parser/encoder and connection state;
- `server`: TCP acceptor, session authentication, command dispatch;
- `raft`: NuRaft launcher, leader discovery, role callbacks;
- `storage`: crash-safe file utilities, durable Raft log store/state manager;
- `state_machine`: command codec, ordered KV apply, deduplication, snapshot;
- `client`: seed discovery, authentication, redirects, retry identity;
- `tools/strongkv-cli`: interactive client.

## 3. Write path

`SET`, `DEL`, `INCR`, and `DECR` never mutate the KV map in the network or
command layer.

```text
client request
  -> authenticate session
  -> reject/redirect if this node is not leader
  -> validate and serialize Command(version=1, client_id, request_id, ...)
  -> raft_server::append_entries_ext({buffer}, expected_term) in blocking mode
  -> NuRaft appends locally and replicates to voting peers
  -> a majority acknowledges
  -> NuRaft advances commit index
  -> KvStateMachine::commit(log_index, command)
  -> dedup check, ordered mutation, durable state image
  -> state-machine result returned by append_entries_ext
  -> RESP reply returned to client
```

`get_accepted()` only means NuRaft accepted the proposal. StrongKV additionally
requires a final `cmd_result_code::OK` and a non-null state-machine result.
Timeout, term mismatch, leadership loss, or quorum loss is returned as an
error; success is never guessed.

INCR/DECR parse the previous value with a strict decimal `int64_t` parser.
Non-decimal data, overflow, and underflow return deterministic replicated
errors. The read-modify-write operation executes once in the ordered state
machine, so concurrent increments cannot lose updates.

## 4. Linearizable GET

NuRaft v3.0.0 does not expose a public Raft `ReadIndex` API. Merely checking
`is_leader()` is unsafe: an isolated old leader may not yet know that a
majority elected a new leader.

StrongKV therefore implements a conservative replicated read barrier:

1. reject a node that is not currently leader;
2. append an internal `READ_BARRIER` command using NuRaft blocking mode;
3. wait until that entry is committed and applied by `KvStateMachine`;
4. read the local KV map only after the barrier succeeds.

An isolated old leader cannot commit the barrier without a majority and
therefore cannot return a successful GET. A successful barrier is ordered
after all preceding writes in the Raft log. The GET is linearized at the
barrier commit; losing leadership after that point does not invalidate the
read because later writes may legally linearize after it.

This provides strict linearizable GET semantics, including across leader
changes and partitions, at the cost of one replicated log entry and quorum
round trip per GET. It is intentionally slower than lease or ReadIndex reads.
Replacing it with a verified public ReadIndex mechanism is a future
optimization, not a correctness fix.

`ROLE` can be served by any node because it is diagnostic. `INFO` describes
local status. Neither is a KV data read.

## 5. Command and result format

All integers use fixed-width big-endian encoding. Strings are
`u32 length + bytes`; no C++ object representation is copied.

Command v1:

```text
magic "SKVC"       4 bytes
version            u16 (=1)
type               u8 (SET, DEL, INCR, DECR, READ_BARRIER)
reserved           u8
client_id          u64
request_id         u64
key                u32 + bytes
value              u32 + bytes
```

The maximum serialized command is bounded by `server.max_request_size`.
Unknown versions/types and truncated/trailing data produce an encoded
`BAD_COMMAND` result rather than undefined behavior.

The state-machine result has its own versioned binary envelope and includes a
status plus an optional string/integer value. Followers compute the identical
result even though only the proposing leader returns it to its client.

## 6. Retry deduplication

The replicated state contains, for each non-zero `client_id`, the greatest
applied `request_id` and its exact result.

- same client and same request: return the stored result without applying;
- greater request: apply and replace the stored record;
- smaller request: return `STALE_REQUEST` without applying.

The C++ Client creates one random non-zero client ID and monotonically
increases request IDs. On a timeout or `NOT_LEADER`, it reconnects but retries
with the same pair. The RESP extension is:

```text
SET key value CLIENT <client_id> REQUEST <request_id>
DEL key       CLIENT <client_id> REQUEST <request_id>
INCR key      CLIENT <client_id> REQUEST <request_id>
DECR key      CLIENT <client_id> REQUEST <request_id>
```

Ordinary Redis-style commands remain accepted with `client_id=0`, but they do
not have retry deduplication. If such a client's reply is lost, it must not
blindly retry a non-idempotent command. BCS should use the C++ Client or the
metadata extension.

Dedup records are included in the durable KV image and snapshots. The
prototype retains one record per client without expiration; bounded retention
is a documented future operational improvement.

## 7. Threads and synchronization

- NuRaft owns its ASIO Raft transport threads plus commit/append workers.
- StrongKV uses one owned blocking session thread per accepted client
  connection. Reads and writes on a connection are therefore serialized.
- A session thread may block in NuRaft blocking append mode. This is
  acceptable for the correctness-first prototype, but a bounded async worker
  design is the production follow-up.
- `KvStateMachine` protects KV, dedup records, commit index, and snapshot
  metadata with one mutex. Commit order is the NuRaft invocation order.
- `FileLogStore` protects its in-memory index and disk rewrite with one mutex.
- `FileStateManager` serializes config/state writes independently.
- logging is serialized by its own mutex and never logs passwords or complete
  values.

Shutdown stops accepting clients, closes sessions, drains/stops the client
`io_context`, invokes `raft_launcher::shutdown`, flushes the log store, then
closes logging. Signal handlers only set an atomic flag; cleanup runs in normal
control flow.

## 8. Persistence model

The prototype intentionally avoids a RocksDB dependency. It uses small,
versioned binary files with checksums and atomic replacement:

```text
data/nodeN/
  kv-state.bin       complete KV + dedup + last applied index
  raft/
    raft-log.bin     Raft entries and compacted start index
    state.bin        current term, vote and election flags
    config.bin       committed cluster configuration
  snapshot/
    latest.bin       NuRaft snapshot metadata + complete state payload
```

Durable updates write a temporary file in the same directory, flush and
`fsync` it, rename over the destination, then `fsync` the directory on Linux.
Every Raft log append/overwrite is durable before the method returns. The
implementation rewrites `raft-log.bin`; this is slow but simple and crash-safe for
a prototype. A production BCS integration should replace the implementation
behind `nuraft::log_store` with RocksDB or a segmented WAL without changing the
Raft/state-machine interfaces.

`kv-state.bin` is atomically rewritten after every committed mutation or
barrier. This means `last_commit_index()` is only advanced together with the
state it describes. On restart:

1. `FileStateManager` loads term/vote, committed config, and Raft log;
2. `KvStateMachine` loads `kv-state.bin` and the latest snapshot metadata;
3. NuRaft starts from those durable indexes and replays any remaining
   committed suffix;
4. normal election resumes with no configured leader.

Checksums detect torn/corrupt files. Storage errors are fatal to the node:
continuing with an acknowledged but non-durable Raft state would violate the
service contract.

## 9. Snapshot and compaction

NuRaft triggers `create_snapshot` when `snapshot_distance_` is reached.
StrongKV creates snapshots synchronously on the state-machine path:

1. clone NuRaft `snapshot` metadata;
2. under the state mutex serialize KV, dedup, and the exact applied index;
3. atomically persist `snapshot/latest.bin`;
4. update `last_snapshot()` and invoke the completion callback.

Logical snapshot transport uses the official NuRaft protocol. The sender's
`read_logical_snp_obj` returns the complete versioned state payload as one
logical object. The receiver's `save_logical_snp_obj` durably stores it with
the received NuRaft metadata. `apply_snapshot` atomically replaces its KV,
dedup map, and applied index.

After the completion callback reports success, NuRaft calls
`FileLogStore::compact`. NuRaft calculates the cut point using
`reserved_log_items_`, so a recent suffix remains available to moderately
lagging followers. “Compression” means snapshot plus old-log truncation, not
gzip.

## 10. Cluster configuration and leader changes

All three configuration files contain the same three voting
`nuraft::srv_config` entries and different local node IDs/ports/directories.
The initial persisted `cluster_config` contains all three members. No node has
priority zero, no learner flag is set, and no leader ID is configured.

On leader failure:

1. followers stop receiving heartbeats;
2. NuRaft election timers and voting elect one remaining voting member;
3. requests sent to followers receive `NOT_LEADER` with the last known leader
   client endpoint, or without an endpoint while election is in progress;
4. the Client follows a redirect or cycles its seed list;
5. the restarted node loads its state/log/snapshot and NuRaft catches it up.

An old leader without a majority cannot successfully append writes or read
barriers. When it receives a higher-term message after connectivity returns,
NuRaft steps it down and reconciles its uncommitted suffix.

## 11. Protocol and authentication

The client port supports RESP2 arrays of bulk strings and encodes simple
strings, errors, integers, bulk strings (including nil), and arrays. Parsing is
incremental and handles partial input, coalesced requests, multiple requests,
invalid/negative lengths, nesting limits, oversized frames, and disconnects.
The configured `max_request_size` bounds buffered and declared data.

`PING` and `AUTH` are allowed before authentication. All other commands return
`NOAUTH Authentication required`. Authentication is per connection and is
never replicated. Password comparison is isolated and constant-time with
respect to equal-length inputs. Logs contain only authentication success or
failure, never the supplied or configured password.

## 12. Verification topology and limits

The primary integration topology is one CentOS VM with three StrongKV
processes:

```text
node1: client 127.0.0.1:7401, raft 127.0.0.1:7501
node2: client 127.0.0.1:7402, raft 127.0.0.1:7502
node3: client 127.0.0.1:7403, raft 127.0.0.1:7503
```

This topology genuinely verifies election, quorum replication, leader process
failure, new election, restart catch-up, full-cluster restart, snapshots,
compaction, authentication, redirects, and concurrent increments. It does not
prove behavior under a host/kernel failure and does not create a strict
network partition unless firewall/network-namespace rules are explicitly
enabled. Partition behavior follows NuRaft's quorum protocol and the read
barrier design, but is reported as untested until such a test is actually run.

## 13. Known prototype trade-offs

- one full Raft log file rewrite per change and one full KV image per commit;
- one quorum round trip and log record per GET;
- one dedup record retained per client indefinitely;
- one operating-system thread per connected client;
- synchronous SDK sockets currently have no per-operation deadline;
- no online membership command even though the persistent configuration uses
  NuRaft membership objects;
- no client TLS and no Raft TLS configuration in the first prototype;
- no Redis TTL, transactions, scripting, Pub/Sub, sharding, or complex types.

These choices reduce implementation surface without weakening acknowledged
write durability or KV consistency.
