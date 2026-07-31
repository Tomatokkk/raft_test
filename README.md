# StrongKV

StrongKV 是一个基于 eBay NuRaft 的三节点强一致 `string -> string` KV
服务原型。Raft 选举、复制、quorum、成员配置、snapshot 传输和日志裁剪均使用
NuRaft 官方实现；StrongKV 负责 RESP2 服务层、认证、持久化适配器、状态机、
SDK 和测试工具。

当前基线固定为 NuRaft v3.0.0，commit
`0563f31059cae5b45d2cb57576916984d9513630`。

## 功能

- `AUTH / SET / GET / DEL / INCR / DECR / PING / INFO / ROLE`
- 三个 voting members，自动选主和 Leader 故障切换
- 写入只在 NuRaft quorum commit 后进入 `KvStateMachine::commit`
- GET 先提交 `READ_BARRIER`，防止隔离的旧 Leader 返回陈旧数据
- 持久化 Raft log、term/vote、cluster config、KV 和去重状态
- NuRaft logical snapshot、落后节点 snapshot 恢复和旧日志裁剪
- RESP2 半包/粘包/多请求/大小限制
- 连接级密码认证，日志不记录密码或完整 value
- C++ SDK 自动发现 Leader、重新认证和安全重试

详细正确性设计见 [docs/design.md](docs/design.md)，协议见
[docs/protocol.md](docs/protocol.md)。

## 依赖与编译

需要 Linux、支持 C++17 的 GCC/Clang、Git、pthread 和 CMake 3.26+。
CMake 默认从 GitHub 获取固定 commit 的 NuRaft 及其 submodule：

```bash
git clone https://github.com/Tomatokkk/raft_test.git
cd raft_test
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

如果 NuRaft 已在本地：

```bash
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DSTRONGKV_NURAFT_SOURCE=/path/to/NuRaft
cmake --build build -j
```

本项目在 CentOS 8.5、系统 GCC 8.5.0 和 CMake 3.31.12 上实际编译验证。

## 启动三节点

配置使用不同的客户端端口和 Raft 端口：

| 节点 | client | Raft |
|---|---|---|
| node1 | `127.0.0.1:7401` | `127.0.0.1:7501` |
| node2 | `127.0.0.1:7402` | `127.0.0.1:7502` |
| node3 | `127.0.0.1:7403` | `127.0.0.1:7503` |

在仓库根目录启动：

```bash
export STRONGKV_BUILD_DIR="$PWD/build"
./scripts/start_cluster.sh
./scripts/status_cluster.sh
```

停止会先发送 SIGTERM 并等待优雅退出：

```bash
./scripts/stop_cluster.sh
```

配置中的 `host` 是节点之间可达的 Raft 广播地址。多机器部署时必须把
`127.0.0.1` 改为各机器真实地址，三个配置的 `cluster.nodes` 必须一致；
`network.bind` 可以保持 `0.0.0.0`。不要把 Leader 写死在配置中。

示例密码 `strong123` 只用于本地测试，部署前必须更换，并限制配置文件权限。

## CLI

一次性命令：

```bash
./build/strongkv-cli \
  --seed 127.0.0.1:7401 \
  --seed 127.0.0.1:7402 \
  --seed 127.0.0.1:7403 \
  -a strong123 SET a 1

./build/strongkv-cli --seed 127.0.0.1:7402 -a strong123 GET a
```

交互模式：

```bash
./build/strongkv-cli -h 127.0.0.1 -p 7401
strongkv> AUTH strong123
strongkv> SET a 1
strongkv> GET a
strongkv> INCR a
strongkv> ROLE
strongkv> INFO
```

CLI/SDK 连接 Follower 时会使用 `NOT_LEADER` 信息自动重连 Leader。

## C++ SDK 接入

链接 `strongkv_core`，包含 `include/strongkv/client.h`：

```cpp
#include <strongkv/client.h>

strongkv::Client client({
    "127.0.0.1:7401",
    "127.0.0.1:7402",
    "127.0.0.1:7403",
});
client.connect();
client.auth("strong123");
client.set("counter", "10");
auto value = client.incr("counter");  // 11
```

每个 SDK 实例生成稳定的非零 `client_id`，每次逻辑写入分配递增
`request_id`。重连重试复用同一对 ID，状态机不会把一次 INCR 应用两次。
BCS 进程应长期复用 Client 实例，不要为每次请求创建新实例。

## 持久化目录

默认配置在仓库根目录下生成：

```text
data/nodeN/
  kv-state.bin
  raft/
    raft-log.bin
    state.bin
    config.bin
  snapshot/
    latest.bin
logs/nodeN.log
```

文件带版本、长度和 checksum。更新采用同目录临时文件、fsync、原子 rename
以及目录 fsync。原型为了简单，每次更新重写完整日志/状态文件，正确性优先，
不适合高 QPS 生产负载；生产版建议保持接口不变，替换为 RocksDB 或分段 WAL。

## Snapshot 与日志裁剪

`raft.snapshot_distance` 控制触发距离，
`raft.reserved_log_items` 控制 snapshot 后保留的日志尾部。snapshot 包含完整
KV、last applied index 和去重表。NuRaft 负责把 logical snapshot 发送给日志
已被裁剪的落后 Follower；成功后调用持久化 LogStore 的 `compact`。

这里的 compaction 是 snapshot + 旧 Raft log 截断，不是 gzip。

## 测试

单元测试：

```bash
ctest --test-dir build --output-on-failure
```

已启动三节点后执行功能和 10000 次并发 INCR：

```bash
STRONGKV_BUILD_DIR="$PWD/build" ./scripts/integration_test.sh
```

故障切换脚本会强制终止当前 Leader：

```bash
STRONGKV_BUILD_DIR="$PWD/build" ./scripts/failover_test.sh
```

脚本运行后需要用 `start_cluster.sh` 恢复被终止节点。

## 常见错误

- `NOAUTH`：先执行 AUTH，或 CLI 使用 `-a`。
- `WRONGPASS`：密码与当前节点配置不一致。
- `NOT_LEADER`：直接协议客户端应按返回 endpoint 重连；SDK 会自动处理。
- `TIMEOUT`/`UNAVAILABLE`：当前没有多数派或正在选举，不能把写入当成功。
- `value is not an integer`：INCR/DECR 的旧值不是合法 int64。
- 启动时报端口占用：确认 7401–7403 和 7501–7503 没有旧进程。
- 持久化 checksum 错误：节点会拒绝继续服务，先保留故障文件并从可靠副本恢复，
  不要删除文件后假装数据安全。

## 原型边界

当前没有 TTL、事务、Lua、Pub/Sub、分片、Multi-Raft、客户端 TLS 或在线成员
管理。客户端服务目前每连接一个线程，SDK 同步 socket 也没有单操作 deadline；
它们是生产化前需要替换的容量与运维能力，不影响已确认请求的 Raft 安全语义。
