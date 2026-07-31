# StrongKV client protocol

StrongKV uses a RESP2 subset on `client_port`. Raft RPC uses the separate
`raft_port`; clients must never send RESP traffic to a Raft port.

## Framing

A request is a non-empty RESP array. Every command name and argument must be a
bulk string or simple string. Command names are ASCII case-insensitive.

Supported response types:

- simple string: `+OK\r\n`;
- error: `-NOAUTH Authentication required\r\n`;
- integer: `:11\r\n`;
- bulk string and null bulk string;
- array (used by `ROLE`).

The parser accepts arbitrary TCP packet boundaries and multiple requests in
one packet. `server.max_request_size` limits buffered and declared frame
sizes. Invalid framing returns a protocol error and closes the connection.

## Authentication

`PING` and `AUTH` are allowed before authentication. Every other command
requires successful connection-scoped authentication:

```text
AUTH <password>
```

Success is `+OK`. Failure is
`-WRONGPASS invalid username-password pair`. Passwords are not replicated and
are never written to logs.

## Commands

```text
PING [message]
SET key value
GET key
DEL key
INCR key
DECR key
INFO
ROLE
```

`GET` returns a bulk string or `$-1`. `DEL`, `INCR`, and `DECR` return RESP
integers. `INCR`/`DECR` use strict signed 64-bit decimal conversion and reject
non-integers, overflow, and underflow.

`INFO` returns a bulk string containing CRLF-delimited `name:value` fields.
`ROLE` returns `["leader"]`, `["follower", "host:port"]`, or a follower entry
with an empty endpoint while no leader is known.

## Leader redirect

Followers never execute `SET`, `GET`, `DEL`, `INCR`, or `DECR`. They return:

```text
-NOT_LEADER <leader_host> <leader_client_port>\r\n
```

If an election is in progress and NuRaft does not know a live leader, the
response is `-NOT_LEADER\r\n`. The C++ Client follows the supplied endpoint or
cycles its seed list.

## Retry identity

For a retry-safe write, append this extension:

```text
SET key value CLIENT <client_id> REQUEST <request_id>
DEL key       CLIENT <client_id> REQUEST <request_id>
INCR key      CLIENT <client_id> REQUEST <request_id>
DECR key      CLIENT <client_id> REQUEST <request_id>
```

Both IDs are non-zero unsigned 64-bit decimals. The exact
`client_id/request_id` pair returns the cached state-machine result without
applying twice. A lower request ID for that client returns `STALE_REQUEST`.
The SDK adds these fields and preserves them across redirects/retries.

Plain Redis-style writes remain supported but are not safe to retry blindly
after an ambiguous timeout.
