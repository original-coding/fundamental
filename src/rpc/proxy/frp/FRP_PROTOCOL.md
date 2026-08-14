# FRP 协议文档

本文档描述当前 FRP 实现的协议与功能链路。历史迁移/重构记录已清理，只保留与代码一致的现状说明。

## 1. 架构

```
设备 A（accessor）                          Public Server                         设备 B（provider）
  ┌───────────────────┐                    ┌──────────────┐                    ┌───────────────────┐
  │ frp_unified_client │                    │ frp_public_  │                    │ frp_unified_client │
  │  - signal channel  │◀── TCP 长连接 ────▶│   server     │◀── TCP 长连接 ────▶│  - signal channel  │
  │  - accessor        │                    │              │                    │  - provider        │
  │  - provider        │                    └──────┬───────┘                    │  - accessor        │
  └───────────────────┘                           │                             └───────────────────┘
           │ 本地 listener                       │ 信令 + relay 路由                     │ 后端 service
```

- 一个设备 = 一个 UUID = 一条 TCP 信号通道（长连接）。
- 同一进程可同时承担 provider 和 accessor 角色。
- 数据通道按连接建立，核心标识是 `connection_uuid`，不是旧版 `flow_id`。
- 每个数据通道包含 `relay_data_channel`、`kcp_channel` 和可选的 `frp_punch_engine`。

## 2. 命令模型

TCP 信令通道上的 JSON 命令使用 `frp_command_type`：

| 值 | 命令 | 方向 | 用途 |
|---:|------|------|------|
| 0 | `invalid` | — | 非法占位 |
| 1 | `signal_open` | C→S | 声明信令通道 |
| 2 | `server_hello` | S→C | 返回认证 nonce |
| 3 | `auth_request` | C→S | HMAC 认证请求 |
| 4 | `auth_response` | S→C | 认证结果 |
| 5 | `p2p_probe` | C→S/UDP | NAT 探测 / endpoint probe |
| 6 | `udp_echo` | S→C/UDP | 返回客户端公网 ip:port |
| 7 | `register_services` | C→S | 批量注册 provider 服务 |
| 8 | `register_services_resp` | S→C | 注册结果 |
| 9 | `subscribe_services` | C→S | 订阅服务目录 |
| 10 | `subscribe_services_resp` | S→C | 返回可见服务列表 |
| 11 | `time_sync_request` | C→S/UDP | NTP-like 时钟同步请求 |
| 12 | `time_sync_response` | S→C/UDP | 时钟同步响应 |
| 13 | `channel_open` | C→S | 建立/配对 relay 数据通道 |
| 14 | `forward_command` | C→S→C | 转发端到端 JSON payload |
| 15 | `signal_ping` | C→S | 信令保活探测 |
| 16 | `signal_pong` | S→C | 信令保活响应 |

`frp_forward_command_data` 只有 `dst_uuid` 和 `payload`，服务端只按 `dst_uuid` 路由，不解析 payload 业务语义。

客户端之间的 payload 命令使用 `frp_client_command_type`：

| 值 | 命令 | 方向 |
|---:|------|------|
| 100 | `frp_client_open` | accessor → provider，请求连接服务 |
| 101 | `frp_client_accept` | provider → accessor，后端已就绪 |
| 102 | `frp_client_reject` | provider → accessor，后端失败 |
| 103 | `frp_client_accessor_punch_start` | accessor → provider，开始 P2P |
| 104 | `frp_client_provider_p2p_handshake` | provider → accessor，交换公网端点 |
| 105 | `frp_client_accessor_handshake_ack` | accessor → provider，确认并开始打洞 |
| 106 | `frp_client_accessor_punch_confirm` | accessor → provider，打洞确认 |
| 107 | `frp_client_provider_confirm_ack` | provider → accessor，确认应答 |
| 108 | `frp_client_accessor_confirm_ok` | accessor → provider，握手完成 |
| 109 | `frp_client_provider_probe_match` | provider → accessor，探测命中提示 |

### 帧格式

每个 TCP 命令帧为：

```text
uint32 little-endian payload_len + JSON payload
```

`payload_len` 必须大于 0 且不超过 `frp_command_base::kMaxCommandPayloadLen`（64 KiB）。

## 3. 启动与认证

1. 客户端连接服务器 TCP 端口，可选 TLS。
2. 客户端发送 `signal_open`。
3. 服务器返回 `server_hello` 和 `server_nonce`。
4. 客户端发送 `auth_request`，`digest = HMAC-SHA256(traffic_secret, server_nonce)`。
5. 服务器校验后返回 `auth_response`。
6. 认证通过后，如果 `public_server_udp_port != 0`，客户端通过 UDP 向服务器两个连续端口发送加密 `p2p_probe`。
7. 两次 `udp_echo` 返回的 `ip:port` 相同判为 cone，不同判为 symmetric；任一端口超时判为 disabled。
8. UDP 可用时继续执行 `time_sync`，计算 `server_clock_offset_us`。

认证成功后，统一客户端依次执行：

- `register_services`：注册所有 provider services。
- `subscribe_services`：订阅 accessor listeners 所需的所有 `register_keys`。
- 信令保活：每 30s 发送 `signal_ping`；连续 3 次没有服务器命令响应则判定假死并重连。

UDP 探测、time sync 和 KCP 数据面都使用 AES-256-CTR。密钥由 `HKDF-SHA256(traffic_secret, info=salt)` 派生，HKDF 固定 salt 为 `frp-kcp-v1`；startup probe / time sync 使用 info `frp-default`，数据通道使用 `connection_uuid` 作为 info。

## 4. 服务注册与发现

### 4.1 注册

`frp_register_services_data` 包含：

- `uuid`
- `nat_type`
- `startup_rtt_ms`
- `groups[]`，每个 group 含 `register_key` 和 `services[]`

服务端在 `register_client_services` 中：

- 校验 `register_key` 必须在 `allowed_register_keys` 中。
- 校验同 key 下 `service_name` 不重复。
- 在 `sessions_by_uuid_[uuid]` 中保存信号 session 弱引用和 groups。

### 4.2 订阅

`frp_subscribe_services_data` 只包含 `register_keys[]`。

服务端遍历所有注册 session，排除 `provider_uuid == 订阅者 uuid`，返回 `frp_visible_service_data`：

- `service_name`
- `provider_uuid`
- `provider_nat_type`
- `provider_startup_rtt_ms`
- `service_type`
- `enable_p2p`

客户端 accessor 按 `service_name:service_type` 匹配本地 listeners，创建或更新 TCP acceptor / UDP socket。订阅刷新采用快速路径 1s×10 次，满足后回落到 30s 周期刷新。

## 5. relay 数据通道

### 5.1 建连时序

```text
Accessor                              Server                              Provider
  │ 本地 listener accept                 │                                    │
  │ 生成 connection_uuid                 │                                    │
  │ 新 TCP 连接                          │                                    │
  │ channel_open(status=0) ─────────────▶│                                    │
  │  payload=frp_client_open             │  转发 payload 到 provider 信令 session│
  │                                       │───────────────────────────────────▶│
  │                                       │                    校验 register_hash
  │                                       │                    连接后端 backend
  │                                       │◀────────────────────────────────────│
  │                                       │   新 TCP 连接 + channel_open(status=1)
  │                                       │   payload=frp_client_accept/reject
  │◀──────────────────────────────────────│
  │ 收到 accept 后 init_kcp              │  双方 data session 升级为 raw relay
```

### 5.2 `frp_client_open`

accessor 生成：

- `connection_uuid`
- `register_nonce`（随机）
- `register_hash = SHA256(register_key + register_nonce)`

`frp_client_open_data` 字段：

- `accessor_uuid`
- `connection_uuid`
- `register_nonce`
- `register_hash`
- `service_name`
- `transport`：`0` = TCP，`1` = UDP

provider 通过遍历本地 services 的 key，匹配 `SHA256(key + register_nonce)` 来确认授权，并找到目标 service。

### 5.3 服务端路由

`channel_open` 的外层字段为：

- `from_uuid`
- `dst_uuid`
- `connection_uuid`
- `status`：`0` 第一阶段，`1` 第二阶段
- `payload`

服务端只解析这些外层字段。第一阶段记录 `sessions_by_uuid_[from_uuid][connection_uuid]` 并把 payload 转发给目标信号 session；第二阶段验证双方数据 session 均存在，然后双向 `upgrade`，开始 raw relay 透传。

### 5.4 KCP 数据面

relay 阶段：

```text
业务明文 → kcp_channel AES-256-CTR 加密 → KCP output → relay TCP → Server raw relay → 对端 KCP input
```

P2P 阶段：

```text
业务明文 → kcp_channel AES-256-CTR 加密 → KCP output → P2P UDP → 对端 KCP input
```

`kcp_channel` 不依赖底层是 TCP 还是 UDP，P2P 升级只替换 `on_output` 的底层 socket，不重建 KCP 实例。

## 6. P2P upgrade

### 6.1 触发条件

当前 `maybe_start_p2p` 只检查：

- `public_server_udp_port != 0`
- `probed_nat_type != disabled`
- 通道未关闭且尚未 P2P 成功

注意：`enable_p2p` 字段会写入配置和服务目录，但当前 `maybe_start_p2p` 没有用它做最终开关；实际是否打洞由 UDP 配置和 NAT 探测结果决定。

### 6.2 时序

```text
Accessor                                  Server                            Provider
  │ relay 运行中                             │                                  │ relay 运行中
  │ 创建 punch_engine / endpoint probe       │                                  │
  │ accessor_punch_start ───────────────────▶──────────────────────────────────▶│
  │                                          │                                  │ 创建 punch_engine / endpoint probe
  │◀───────────────────────────────────────────────────────────────────────────│ provider_p2p_handshake
  │ accessor_handshake_ack ─────────────────▶──────────────────────────────────▶│
  │ 双方 start_punch_at 后同时打洞           │                                  │ 双方 start_punch_at 后同时打洞
  │ [UDP punch]                              │                                  │ [UDP punch]
  │ 命中后 signal 握手：                      │                                  │
  │ provider_probe_match → accessor_punch_confirm → provider_confirm_ack → accessor_confirm_ok
  │ accept_p2p，释放 TCP relay               │                                  │ accept_p2p，释放 TCP relay
```

### 6.3 打洞参数

当前 `frp_punch_engine` 的常量：

| 参数 | 值 |
|---|---:|
| endpoint probe 间隔 / 最大次数 | 100ms / 50 |
| punch 轮数 | 1 |
| sym 侧 socket 数 | 550 |
| cone 侧目标端口数 | 550 |
| 重传遍数 | 4 |
| 每批发送数 | 55 |
| 批间隔 | 500ms |
| 端口范围 | [512, 65535] |

sym 侧源端口围绕 base port 做连续端口块扫描，cone 侧目标端口从 [512, 65535] shuffle 后取前 550 个。

### 6.4 探测包与匹配

UDP punch 探测包为 8 字节明文：

```text
uint32 punch_tag LE + uint16 src_port LE + uint16 target_port LE
```

- `punch_tag` 由 `hash(connection_uuid)` 生成，`0` 时置为 `1`。
- 收到后校验 `punch_tag` 和端口集合；命中后记录实际收到包的对端 endpoint。

### 6.5 失败重试

P2P 失败或 30s 握手超时后，accessor 以 `10s, 20s, ..., 60s`（封顶 60s）间隔重试，最多 10000 次。

## 7. 超时与保活

### 7.1 服务端

- 信令握手阶段：`data_channel_idle_timeout_seconds / 4`，最小 15s。
- 已认证信令 session：按命令读取重置超时。
- relay data session：启用完整 `data_channel_idle_timeout_seconds`。

### 7.2 客户端 relay_data_channel

- 每收到一帧业务数据调用 `reset_idle_timer()`，超时为配置的 `data_channel_idle_timeout_seconds`，默认 600s，`0` 禁用。
- `kcp_channel` 使用 `ikcp_enable_keepalive(kcp, 2000, 5)`：每 2s 探测，连续 5 次无响应判定数据面死链并关闭通道。

没有单独的“10s idle → 每 2s 发 1 字节探测”逻辑；P2P 死链检测由 KCP keepalive 承担，业务空闲超时由 `reset_idle_timer()` 承担。

### 7.3 信令保活

- 客户端每 30s 发送 `signal_ping`。
- 服务端回 `signal_pong`。
- 客户端连续 3 次没有服务器响应则判定假死，释放信令通道并触发重连。

## 8. 资源管理

### 8.1 Server

- `sessions_by_uuid_`：`uuid → map<connection_uuid, weak_ptr<frp_signal_session>>`。
  - 信令 session 的 key 为自身 `uuid`。
  - relay data session 的 key 为 `connection_uuid`。
- `allowed_register_keys_cache`：`register_key → set<service_name>`，用于注册时校验 key 与服务名唯一性。
- `release_obj` 关闭 acceptor 和 UDP socket，并释放所有 session。

### 8.2 Client

- `frp_signal_client` 持有单条 signal channel、probe socket 和 reconnect/poll/probe 定时器。
- `frp_accessor` 持有 listeners 和 `channels_`（`connection_uuid → relay_data_channel`）。
- `frp_provider` 持有 `services_by_key_` 和 `channels_`。
- `relay_data_channel` 持有 KCP、relay TCP、P2P UDP socket、后端/本地 socket 以及 idle/handshake/punch retry 定时器。

## 9. 配置模型

当前默认值来自 `frp_config_types.hpp`：

```json
{
  "threads": 8,
  "listen_tcp_port": 32000,
  "listen_udp_port": 32001,
  "traffic_secret": "traffic-secret-demo",
  "allowed_register_keys": ["demo-register-key"],
  "data_channel_idle_timeout_seconds": 600,
  "log_output_path": "logs",
  "log_program_name": "frp_proxy_server",
  "log_level": 1,
  "enable_console_output": true,
  "ssl": { "disable_ssl": true }
}
```

```json
{
  "threads": 8,
  "public_server_host": "127.0.0.1",
  "public_server_tcp_port": 32000,
  "public_server_udp_port": 32001,
  "traffic_secret": "traffic-secret-demo",
  "nat_type": 2,
  "local_ip": "192.168.1.100",
  "data_channel_idle_timeout_seconds": 600,
  "log_output_path": "logs",
  "log_program_name": "frp_proxy_client",
  "log_level": 1,
  "enable_console_output": true,
  "ssl": { "disable_ssl": true },
  "groups": [
    {
      "register_key": "demo-key-1",
      "services": [
        { "service_name": "echo-tcp", "target_host": "127.0.0.1", "target_port": 18080,
          "service_type": 0, "enable_p2p": true }
      ],
      "listeners": [
        { "service_name": "rdp", "listen_host": "0.0.0.0", "listen_port": 19001,
          "service_type": 0, "enable_p2p": true }
      ]
    }
  ]
}
```

- `listen_udp_port` / `public_server_udp_port` 为 `0` 时禁用 UDP 探测、time sync 和 P2P。
- `traffic_secret` 用于 HMAC 认证与 KCP 数据面加密。
- `service_type`：`0` = TCP，`1` = UDP。
- `local_ip` 目前仅作为配置项存在，数据路径未使用。
