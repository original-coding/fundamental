# fundamental

C++ 工具库与网络库，为构建网络应用提供基础组件。基于 C++17，注重低延迟与高性能。

## 目录结构

```
.
├── CMakeLists.txt            # 顶层 CMake
├── applications/             # 独立可执行应用
├── assets/                   # 资源文件（证书等）
├── cmake/                    # CMake 模块
├── samples/                  # 测试与基准测试用例
├── scripts/                  # 辅助脚本
├── src/                      # 库源码
│   ├── fundamental/          # 核心工具库
│   ├── http/                 # HTTP 服务器
│   ├── network/              # 网络 IO（io_context_pool, RUDP）
│   ├── rpc/                  # RPC 框架与代理支持
│   └── database/             # 数据库封装
├── third-parties/            # 三方库源码导入
└── test-gen-linux*.sh        # 构建生成脚本
```

## 核心模块

### fundamental (`src/fundamental/`)

核心静态库，提供以下子系统：

| 子系统 | 功能说明 |
|-----------|-------------|
| **algorithm** | range set 操作、wyhash/BLAKE3 哈希工具 |
| **application** | 应用生命周期管理（单例事件循环） |
| **basic** | 内存分配器、命令行解析、base64、buffer、压缩（zlib + 并行 deflate）、大小端处理、错误码、文件读写、整数编码、日志（基于 spdlog）、MD5、文件锁、并行任务执行、随机数生成、字符串处理、URL 解析、UUID 生成 |
| **data_storage** | 基于 RTTR 反射的内存键值存储 |
| **delay_queue** | 定时器/延时任务 |
| **events** | 事件系统与信号槽模式（基于 eventpp） |
| **io** | CSV 文件读写 |
| **process** | 进程状态监控（CPU/内存使用） |
| **read_write_queue** | 无锁队列（readerwriterqueue、readerwritercircularbuffer）及 step task executor |
| **rttr_handler** | 基于 RTTR 反射的 JSON 序列化/反序列化、二进制打包 |
| **thread_pool** | 并行任务线程池 |
| **tracker** | 内存追踪与耗时分析 |

### network (`src/network/`)

- **io_context_pool** — 多线程 asio io_context 池，所有网络组件共享
- **RUDP** — 基于 KCP 的可靠 UDP 实现，包含连接状态管理（SYN/SYN_ACK/FIN/PING 控制协议）。可配置参数：MTU、收发窗口、重传间隔、no-delay 模式
- **SSL** — ASIO SSL stream 支持（TLS 客户端与服务端）

### rpc (`src/rpc/`)

可插拔 RPC 框架，支持多种传输与代理方式：

- **RPC 核心** — 请求/响应模式，包含序列化、客户端路由、连接管理
- **netlink** — 集群网络层，支持 flush 协议、心跳保活、自动重连
- **proxy** — 代理子系统：
  - **SOCKS5** — 完整 SOCKS5 代理会话（无认证、用户名密码认证、CONNECT 命令）
  - **WebSocket** — WebSocket 转发连接与升级会话
  - **Protocol pipe** — 基于命名管道的代理传输
  - **FRP（Fast Reverse Proxy）** — 见下方专节

### http (`src/http/`)

基于 asio 的轻量级 HTTP/1.1 服务器：请求解析、响应构建、路由匹配、连接管理。

### database (`src/database/`)

- **sqlite3** — SQLite3 的 C++ 封装，RAII 句柄管理、预编译语句支持、可选加载扩展支持
- **rocksdb** — 占位（默认禁用）

## 依赖

### 系统要求

| 要求 | 最低版本 |
|-------------|-----------------|
| Ubuntu | 22.04+ |
| Windows | MSVC 2022+ + vcpkg |
| CMake | 3.16+（建议 3.22+） |
| GCC | 9+ |
| C++ 标准 | 17 |

### 三方库

所有三方依赖通过 `third-parties/` 以源码方式导入，或通过 CMake 拉取：

| 库 | 用途 |
|---------|---------|
| **asio** | 异步 IO（standalone 模式） |
| **eventpp** | 异构事件分发（事件/信号） |
| **nlohmann/json** | JSON 解析与序列化 |
| **rttr** | 运行时类型反射 |
| **spdlog** | 日志框架 |
| **quickjs/quickjspp** | JavaScript 嵌入式脚本（可选） |
| **OpenSSL** | TLS/SSL、加密原语 |
| **zlib** | 压缩 |
| **SQLite3** | 嵌入式数据库 |
| **Google Test** | 单元测试 |
| **Google Benchmark** | 性能基准测试 |

## 构建

### 快速构建（Linux）

```bash
# Release 构建（RelWithDebInfo）
./test-gen-linux.sh
cd ./build-linux && make -j$(nproc)

# Debug 构建（含 Address Sanitizer）
./test-gen-linux-debug.sh
cd ./build-linux-debug && make -j$(nproc)

# Debug 构建 + clang-tidy
./test-gen-linux-debug-with-clang.sh
```

### CMake 选项

构建时通过 `-DOPTION=VALUE` 传入：

| 选项 | 默认值 | 说明 |
|--------|---------|-------------|
| `FUNDAMENTAL_BUILD_NETWORK` | ON | 构建网络库与 RPC 模块 |
| `FUNDAMENTAL_ENABLE_DATABASE_SUPPORT` | ON | 构建数据库模块 |
| `FUNDAMENTAL_ENABLE_SQLITE3_SUPPORT` | ON | SQLite3 支持 |
| `FUNDAMENTAL_ENABLE_SQLITE3_LOADABLE_EXT_SUPPORT` | OFF | SQLite3 可加载扩展 |
| `FUNDAMENTAL_ENABLE_ROCKSDB_SUPPORT` | OFF | RocksDB 支持 |
| `FUNDAMENTAL_BUILD_RTTR` | ON | RTTR 序列化/反射 |
| `FUNDAMENTAL_BUILD_EVENTS` | ON | 事件系统 |
| `FUNDAMENTAL_BUILD_APPLICATIONS` | ON | 构建应用可执行文件 |
| `FUNDAMENTAL_ENABLE_SCRIPT_SUPPORT` | ON | JavaScript 脚本支持（QuickJS） |
| `IMPORT_GTEST` | ON | 构建 Google Test 目标 |
| `IMPORT_BENCHMARK` | ON | 构建 benchmark 目标 |
| `DISABLE_DEBUG_SANITIZE_ADDRESS_CHECK` | OFF | 禁用 Debug 模式的 ASAN |
| `ENABLE_JEMALLOC_MEMORY_PROFILING` | OFF | 启用 jemalloc 堆分析 |

### 运行测试

```bash
# 运行全部测试
cd build-linux && ctest --output-on-failure

# 运行单个测试
./build-linux/samples/TestBasic/TestBasic

# 运行基准测试
./build-linux/samples/RpcBenchmark/RpcBenchmark
```

## 应用

当 `FUNDAMENTAL_BUILD_APPLICATIONS=ON` 时，构建以下独立可执行文件：

| 应用 | 说明 |
|-------------|-------------|
| `frp_proxy_server` | FRP 公共服务端（信令协调、relay 中转） |
| `frp_proxy_client` | FRP 统一客户端（同一设备一条信号通道，可同时注册服务 provider 和订阅服务 accessor） |
| `frp_echo_test` | TCP 回显服务端/客户端，用于 FRP 集成测试 |
| `rudp_delay_test_server` | RUDP 延迟测试服务 |
| `tcp_custom_proxy_server` | SOCKS5 代理服务 |
| `traceroute` | 路径追踪工具 |

---

## FRP（Fast Reverse Proxy）

FRP 是一个 NAT 穿透反向代理系统，用于访问位于 NAT/防火墙后的服务。支持两种传输模式：

- **TCP Relay** — 流量经公共服务端通过 TCP 中转。始终可用，延迟较高。
- **P2P Upgrade** — relay 建立后通过 NAT 打洞建立端到端直连 UDP 通道。延迟更低，需要双方 NAT 类型兼容。

### 架构

```
┌──────────────────────────────┐          ┌──────────────────────────────┐
│       Client (设备A)          │          │       Client (设备B)          │
│  ┌────────┐  ┌────────────┐  │          │  ┌────────┐  ┌────────────┐  │
│  │Provider │  │  Accessor  │  │          │  │Provider │  │  Accessor  │  │
│  │(注册服务)│  │(监听端口)  │  │          │  │(注册服务)│  │(监听端口)  │  │
│  └────┬───┘  └─────┬──────┘  │          │  └────┬───┘  └─────┬──────┘  │
│       └──────┬─────┘         │          │       └──────┬─────┘         │
│              │               │          │              │               │
│       一条信号通道(TCP)       │          │       一条信号通道(TCP)       │
└──────────────┬───────────────┘          └──────────────┬───────────────┘
               │                                         │
               │    ┌──────────────────────┐             │
               └────│    Public Server     │─────────────┘
                    │     (公共服务端)      │
                    └──────────────────────┘
                     信号协调 + relay 中转
```

**核心设计：** 一个设备 = 一个 UUID = 一条 TCP 信号通道（长连接）。同一进程可同时承担 provider 和 accessor 两种角色，共用一条信号通道。

**角色说明：**

- **Public Server**（公共服务端）— 中心协调器。负责认证、服务注册与发现、flow 创建、relay 配对以及 P2P upgrade 协调。不承载会话数据。
- **Provider 端** — 注册本地后端服务（如 `grpc-backend → 127.0.0.1:50051`），等待 flow 分配。流量出口。
- **Accessor 端** — 在本地端口监听客户端连接。客户端连接后创建 flow 以访问 provider 的后端服务。流量入口。

> Provider 和 Accessor 是同一 `frp_proxy_client` 进程内的端点角色，不是独立二进制。

**核心概念：**

- **Flow** — 以 `flow_id` 标识的单次代理会话。生命周期：创建 → relay 建立 → （可选 P2P upgrade）→ 数据传输 → 关闭。
- **Transport** — 数据路径类型：`tcp_relay`（经服务器中转）或 `p2p`（直连 UDP，upgrade 成功后）。
- **NAT Type** — `disabled`(0)、`symmetric`(1)、`cone`(2)。决定 P2P upgrade 是否可行。双方均为 symmetric 或任一方 disabled 则无法 P2P。
- **Startup Probe** — 信号通道建立前，设备通过向服务器两个 UDP 端口发送加密探测包判断自身 NAT 类型。两次回显的 `ip:port` 相同 → cone，不同 → symmetric。
- **Time Sync** — startup probe 后，设备通过 NTP-like 协议与服务器进行时钟同步（`time_sync_request/response`），计算时钟偏移量。P2P 打洞时双方基于同步时钟同时启动，最大化打洞成功率。
- **P2P Upgrade** — relay 建立后，accessor 主动发起 `p2p_handshake` 交换双方公网端点信息，然后通过时钟同步的 `punch_start` 实现双方同时打洞。成功后 KCP 传输层从 TCP relay 切换为 UDP，不重建 KCP 实例。

### 本地开发快速启动

```bash
# 1. 构建项目
./test-gen-linux.sh && cd build-linux && make -j$(nproc)

# 2. 启动 FRP 全栈（server + 两个 client 分别充当 provider 和 accessor）
bash src/rpc/proxy/frp/dev-local.sh

# 3. 将客户端连接到 accessor 端口（默认 127.0.0.1:15051）
#    流量路径：客户端 → accessor → provider → backend(127.0.0.1:50051)

# 4. 停止
bash src/rpc/proxy/frp/dev-local.sh stop
```

### 配置说明

打印示例配置：

```bash
./build-linux/applications/frp_proxy_server/frp_proxy_server --print-example-config
./build-linux/applications/frp_proxy_client/frp_proxy_client --print-example-config
```

#### Public Server 配置 (`server.json`)

```json
{
  "threads": 2,
  "listen_tcp_port": 32000,
  "listen_udp_port": 32001,
  "traffic_secret": "traffic-secret-demo",
  "allowed_register_keys": ["demo-register-key"],
  "data_channel_idle_timeout_seconds": 120,
  "ssl": { "disable_ssl": true }
}
```

| 字段 | 说明 |
|-------|-------------|
| `listen_tcp_port` | 信令与 relay 连接的 TCP 端口 |
| `listen_udp_port` | UDP 探测与 P2P 协调的基础端口，设为 `0` 则禁用 P2P（仅 relay 模式） |
| `traffic_secret` | 用于 HMAC 认证和 AES-256-CTR 加密的共享密钥 |
| `allowed_register_keys` | 允许客户端注册的密钥列表 |
| `data_channel_idle_timeout_seconds` | 数据通道空闲超时（秒），`0` 则禁用 |

#### Client 统一配置 (`client.json`)

`frp_proxy_client` 使用 `groups` 结构，每个 group 携带一个 `register_key`，可同时包含 `services`（provider 端）和 `listeners`（accessor 端）：

```json
{
  "threads": 4,
  "public_server_host": "127.0.0.1",
  "public_server_tcp_port": 32000,
  "public_server_udp_port": 32001,
  "traffic_secret": "traffic-secret-demo",
  "nat_type": 2,
  "local_ip": "",
  "data_channel_idle_timeout_seconds": 120,
  "ssl": { "disable_ssl": true },
  "groups": [
    {
      "register_key": "key1",
      "services": [
        {
          "service_name": "echo",
          "target_host": "127.0.0.1",
          "target_port": 12345,
          "service_type": 0,
          "enable_p2p": true
        }
      ],
      "listeners": [
        {
          "service_name": "rdp",
          "listen_host": "0.0.0.0",
          "listen_port": 19001,
          "service_type": 0,
          "enable_p2p": true
        }
      ]
    }
  ]
}
```

| 字段 | 说明 |
|-------|-------------|
| `groups` | 至少一项，每组一个 `register_key`（跨组不能重复），`services + listeners` 不能都为空 |
| `groups[].register_key` | 该组的注册密钥，用于服务目录隔离 |
| `groups[].services` | provider 端：注册到服务目录的后端服务列表 |
| `groups[].listeners` | accessor 端：本地监听端口列表，从服务目录匹配后创建 |
| `services[].service_type` | `0` = TCP，`1` = UDP |
| `services[].enable_p2p` | 是否允许该服务的 P2P upgrade |
| `nat_type` | NAT 类型提示。`0`=disabled，`1`=symmetric，`2`=cone。startup probe 可能会覆盖此值 |
| `local_ip` | 局域网 IP，用于同网段 P2P 候选。为空则跳过 |
| `public_server_udp_port` | 设为 `0` 则仅 relay 模式 |

### 单独启动各组件

`frp_proxy_client` 是统一客户端，同一进程可同时担任 provider 和 accessor：

```bash
# 启动公共服务端
./build-linux/applications/frp_proxy_server/frp_proxy_server --config server.json

# 启动纯 provider（仅暴露后端服务）
./build-linux/applications/frp_proxy_client/frp_proxy_client --config provider.json

# 启动纯 accessor（仅监听客户端连接）
./build-linux/applications/frp_proxy_client/frp_proxy_client --config accessor.json

# 启动混合端（同时 provider + accessor，共用一条信号通道）
./build-linux/applications/frp_proxy_client/frp_proxy_client --config both.json
```

### 运行验证脚本

```bash
# TCP relay 测试
bash src/rpc/proxy/frp/verify-relay-local.sh

# TCP P2P upgrade 测试
bash src/rpc/proxy/frp/verify-p2p-local.sh

# UDP relay 测试
bash src/rpc/proxy/frp/verify-udp-proxy.sh

# UDP P2P upgrade 测试
bash src/rpc/proxy/frp/verify-udp-p2p.sh
```

每个测试脚本执行以下流程：
1. 随机分配空闲端口
2. 依次启动 echo backend、public server、provider client、accessor client
3. 通过 FRP 链路运行 echo 客户端
4. 验证数据正确回传，并检查对应传输模式的日志证据
5. 退出时清理所有进程

### FRP 协议概要

FRP 协议通过 TCP 信令通道传输 JSON 编码的命令消息，UDP 探测包使用 AES-256-CTR 对称加密。详见 [FRP_PROTOCOL.md](src/rpc/proxy/frp/FRP_PROTOCOL.md)。

**建联流程：**
1. （UDP 可用时）客户端通过 UDP 向服务器两个端口发送加密 `p2p_probe` 探测包，判断 NAT 类型（cone/symmetric/disabled）
2. （UDP 可用时）客户端通过 NTP-like 协议与服务器进行时钟同步（`time_sync_request/response`），计算 `server_clock_offset`
3. 客户端通过 TCP 连接公共服务端
4. TLS 握手（可选，可配置）
5. 客户端发送 `signal_open` 声明信令通道
6. 服务端发送 `server_hello` 附带 nonce
7. 客户端发送 `auth_request`，包含 `HMAC-SHA256(secret, nonce)` 摘要
8. 认证通过后，客户端发送 `register_services` 批量注册所有 groups（即使某 group 无 services 也要发送，用于建立 register_key 关联）
9. 客户端发送 `subscribe_services` 订阅所有 register_keys 的服务目录

**数据流转（TCP Relay）：**
1. 外部客户端连接 Accessor 监听端口
2. Accessor 向服务端发送 `create_flow_request(service_name, register_key, transport)`
3. 服务端在 `services_by_register_key_` 中查找，配对 provider，分配 `flow_id`
4. 服务端向 provider 发送 `prepare_flow`，双方各自建立 TCP relay 连接（`data_open` + KCP 激活）
5. 服务端 `bind_data_session` 配对双方 data session，发送 `flow_transport_ready`
6. Provider 连接后端 → 发送 `flow_ready` → 服务端转发给 accessor
7. 数据流：外部客户端 ↔ accessor ↔ KCP/TCP-relay ↔ server ↔ provider ↔ 后端

**P2P Upgrade 流程：**
1. relay 建立后，accessor 检查条件满足，创建 `frp_punch_engine` 并启动 endpoint probe（UDP `p2p_probe`，200ms 间隔 × 10 次）
2. accessor 探得自身公网 `ip:port` 后，通过 signal channel 发送 `p2p_handshake`（含 external_ip、external_port、nat_type、rtt_ms）给 provider
3. provider 收到后创建 `punch_engine`，启动自身 endpoint probe，完成后回复 `p2p_handshake_ack`
4. accessor 根据时钟偏移计算同步 deadline，通过 `punch_start` 下发给 provider（以 server 时钟为基准）
5. 双方在 deadline 时刻同时启动 UDP punch：sym 侧 65 socket × 5 重传，cone 侧 3000 target × 5 重传
6. 收到匹配探测包 → punch engine 通过 signal channel 完成 `punch_confirm → punch_confirm_ack → punch_confirm_ok` 三次握手
7. 打洞成功：KCP output 从 TCP relay 切换为 UDP（KCP 实例不重建，仅替换底层 output）
8. TCP relay 被释放，数据继续通过 P2P UDP 传输，启用 keepalive（10s idle → 每 2s 发 1 字节探测）

---

## 内存泄漏排查

```bash
# 使用 jemalloc 堆分析构建
cmake -B build-leak-check \
    -DDISABLE_DEBUG_SANITIZE_ADDRESS_CHECK=ON \
    -DENABLE_JEMALLOC_MEMORY_PROFILING=ON
cd build-leak-check && make -j$(nproc)

# 运行时启用堆分析
export MALLOC_CONF="prof:true,prof_active:true,lg_prof_sample:0,prof_leak:true,prof_accum:true"
./samples/TestBasic/TestBasic

# 对比两次堆快照
jeprof --text --show_bytes --lines --base=1.out ./samples/TestBasic/TestBasic 2.out
```

## 许可证

见 [LICENSE.txt](LICENSE.txt)。
