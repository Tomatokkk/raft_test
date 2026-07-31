# StrongKV implementation and verification report

This report distinguishes implemented-and-tested behavior from design claims.
The test host was one CentOS Linux 8.5 VM running three independent StrongKV
processes on loopback. That topology tests process failure and Raft RPC
behavior, but not host/kernel failure.

## 1. Added files

The repository was initially empty. The implementation adds:

```text
CMakeLists.txt
README.md
conf/node{1,2,3}.yaml
include/strongkv/{client,config}.h
src/
  client/
  common/
  config/
  logging/
  protocol/
  raft/
  security/
  server/
  state_machine/
  storage/
  main.cpp
tools/strongkv-cli/main.cpp
tests/unit/*
tests/integration/concurrent_incr.cpp
scripts/{start,stop,status}_cluster.sh
scripts/{integration,failover}_test.sh
docs/{design,protocol,verification}.md
```

There were no pre-existing source files to modify.

## 2. Architecture

Client RESP2 traffic enters `TcpServer`, is parsed independently by the
protocol module, and is authorized by connection-scoped `ClientSession`.
`CommandService` maps commands to `RaftNode`. Writes and internal read
barriers are serialized and proposed through NuRaft. Only
`KvStateMachine::commit` changes KV data. Persistent NuRaft adapters and
logical snapshot callbacks are isolated under `storage` and `state_machine`.

See `docs/design.md` for the full request and recovery paths.

## 3. NuRaft APIs used

- `raft_launcher::init`, `raft_launcher::shutdown`
- `raft_server::append_entries_ext`
- `raft_server::{is_initialized,is_leader,get_leader,get_term}`
- `raft_server::{get_committed_log_idx,get_last_log_idx}`
- `raft_server::{get_last_snapshot_idx,get_srv_config_all}`
- `state_machine` commit/config/snapshot logical-object callbacks
- the complete `state_mgr` persistence interface
- the complete `log_store` interface, including `compact` and `flush`
- `raft_params` heartbeat, election, client timeout, snapshot distance,
  reserved items, blocking return, and disabled auto-forwarding
- `cb_func` role/config/snapshot notifications

Election, voting, replication, quorum computation, peer RPC, logical snapshot
transport, and compaction scheduling are not reimplemented.

## 4. SET call chain

```text
RESP SET
 -> session AUTH check
 -> Command(version=1, client_id, request_id, key, value)
 -> encode_command
 -> append_entries_ext(expected_term, blocking)
 -> NuRaft majority replication and commit
 -> KvStateMachine::commit(log_index)
 -> retry deduplication
 -> ordered KV mutation and durable kv-state.bin
 -> encoded state-machine result
 -> +OK
```

`get_accepted()` alone is never treated as success. StrongKV also requires
NuRaft result `OK` and a valid state-machine response.

## 5. Linearizable GET

NuRaft v3.0.0 has no public ReadIndex API. GET is leader-only and first
commits an internal `READ_BARRIER` in the current term using
`append_entries_ext`. Only after the blocking commit and apply succeeds does
the node read its local state machine. An isolated former leader has no
majority and cannot complete this barrier, so it cannot return a successful
stale GET.

This is implemented and exercised through normal reads and reads after two
leader changes. A strict firewall partition was not injected, so the
partition-specific claim remains reasoning-backed but not experimentally
verified in this run.

## 6. Leader failover

Two real failovers were tested:

1. Node1 was the leader and was sent SIGKILL. Node3 became leader, read the
   committed old value, committed an INCR, and returned the new value. Node1
   restarted as a follower and reached the same commit/applied index.
2. `scripts/failover_test.sh` detected Node3 as leader, sent SIGKILL, observed
   Node2 become leader, read the pre-crash value, and committed the next INCR.
   Node3 was then restarted and returned as a follower.

No configuration file was edited and no leader was manually appointed.

## 7. Persistence

Implemented binary, versioned, checksummed, atomic files:

```text
data/nodeN/raft/raft-log.bin
data/nodeN/raft/state.bin
data/nodeN/raft/config.bin
data/nodeN/kv-state.bin
data/nodeN/snapshot/latest.bin
```

Linux writes use a same-directory temporary file, fsync, rename, and directory
fsync. A full three-node graceful stop and restart retained ordinary values,
dedup results, snapshot-era values, and the 10000-INCR final value.

## 8. Snapshot and log compaction

With Node3 offline, 130 additional writes were committed. Node1 and Node2
created snapshot index 100. The leader log recorded:

```text
snapshot completed index=100
log_store_ compact upto 80
```

After Node3 restarted behind the compacted prefix, its log recorded:

```text
snapshot object received index=100
snapshot applied index=100
```

It then reached commit/last-log/last-applied index 149 and returned the value
written after the snapshot. This verifies create, logical transfer, apply,
suffix catch-up, and persistent restart metadata.

## 9. Client protocol and AUTH

Implemented RESP2 arrays, bulk/simple strings, errors, integers, null bulk
strings, and arrays. Unit tests cover partial packets, coalesced requests,
multiple requests, invalid lengths, oversize input, invalid prefixes, and
response round trips.

Manual live tests verified:

- unauthenticated PING succeeds;
- unauthenticated GET returns `NOAUTH`;
- incorrect AUTH returns `WRONGPASS`;
- correct AUTH succeeds;
- follower GET and SET return an exact leader endpoint;
- int64 overflow, underflow, and non-integer errors;
- DEL followed by GET returns nil.

Authentication is connection-local and never enters a Raft command. Logs only
contain AUTH success/failure.

## 10. SDK, CLI, and configuration

The synchronous C++ SDK accepts seed endpoints, authenticates, follows
`NOT_LEADER`, cycles seeds when the leader is unknown, re-authenticates after
reconnect, and adds stable retry identity to writes. From follower-only seed
lists it successfully redirected SET and GET to the leader.

The CLI supports one-shot and interactive commands. Three YAML files contain
the same voting membership and distinct local client/Raft ports and storage
paths.

## 11. Build commands executed

Debug:

```bash
cmake -S /root/strongkv-src \
  -B /root/strongkv-src/build-gcc8 \
  -DSTRONGKV_NURAFT_SOURCE=/root/nuraft-v3.0.0 \
  -DCMAKE_CXX_COMPILER=/usr/bin/g++
cmake --build /root/strongkv-src/build-gcc8 -j1
ctest --test-dir /root/strongkv-src/build-gcc8 --output-on-failure
```

Release:

```bash
cmake -S /root/strongkv-src \
  -B /root/strongkv-src/build-release \
  -DSTRONGKV_NURAFT_SOURCE=/root/nuraft-v3.0.0 \
  -DCMAKE_CXX_COMPILER=/usr/bin/g++ \
  -DCMAKE_BUILD_TYPE=Release
cmake --build /root/strongkv-src/build-release -j2
ctest --test-dir /root/strongkv-src/build-release --output-on-failure
```

Both configurations built `strongkv-server`, `strongkv-cli`, unit tests, and
the concurrent integration tool. Both CTest runs passed.

A third clean Release build omitted `STRONGKV_NURAFT_SOURCE`. CMake fetched
the pinned NuRaft commit and ASIO submodule from GitHub through
`FetchContent`; the full build and CTest run also passed. This verifies the
README's default clone-and-build path rather than only the pre-cloned
dependency path.

## 12. Tests executed and results

Implemented and passed:

- 19 unit test cases inside `strongkv_unit_tests`
- one leader and two followers
- SET/GET/DEL/nil
- INCR/DECR and int64 boundary errors
- follower read/write redirect
- two leader failure/election sequences
- failed leader restart and index catch-up
- full-cluster stop/restart recovery
- same client/request INCR replay applied once
- snapshot creation and compaction
- lagging follower snapshot recovery
- 10 threads × 1000 INCR = exactly 10000
- Debug and Release builds
- startup/status/shutdown/integration/failover scripts
- SIGTERM graceful shutdown of all three nodes with
  `NuRaft stopped, graceful=true`

The final VM state after verification has no StrongKV server processes
running. Persistent test data remains under `/root/strongkv-src/data`.

## 13. Not tested or not implemented

Not tested:

- strict firewall/network-namespace partition between the minority old leader
  and the two-node majority (explicitly omitted for this one-VM run);
- multi-host deployment, host crash, kernel crash, or storage device failure;
- Redis CLI compatibility beyond the documented RESP2 subset;
- sustained 10000 simultaneous connections.

Intentionally not implemented:

- TTL, transactions, Lua, Pub/Sub, complex Redis types, sharding, Multi-Raft;
- online membership administration;
- client TLS and Raft TLS configuration;
- ACL/RBAC or a management UI.

## 14. Known risks and BCS follow-up

- The correctness-first file store rewrites complete files and fsyncs every
  commit. Replace it with RocksDB or a segmented WAL before high-QPS use.
- Every GET adds a committed log entry. Replace only after a supported,
  verified ReadIndex/lease-read design is available.
- The server uses one OS thread per connection. Replace it with bounded async
  I/O before large connection counts.
- SDK synchronous sockets have no operation deadline. Add connect/read/write
  deadlines, cancellation, metrics, and backoff policy for production.
- Dedup retains one record per client indefinitely. BCS needs a bounded,
  snapshot-safe lifecycle policy.
- BCS should reuse long-lived `strongkv::Client` instances, supply all three
  seed endpoints, keep a non-zero stable client ID through SDK lifetime, and
  treat timeout/unavailable as ambiguous rather than success. The SDK already
  preserves request identity across its own retries.
