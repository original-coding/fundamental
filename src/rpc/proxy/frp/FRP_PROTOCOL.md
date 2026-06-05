# FRP 协议文档

## 1. 架构

```
设备 (一个 UUID, 一条信号通道)          Server                         设备 (一个 UUID, 一条信号通道)
    |                                     |                                    |
    |== TCP 信号通道 (长连接) ============|== TCP 信号通道 (长连接) ============|
    |                                     |                                    |
    |  register_services(key, svcs...)    |  register_services(key, svcs...)   |
    |  subscribe_services(key)            |  subscribe_services(key)           |
    |                                     |                                    |
    |  数据通道 (按 flow 建立, KCP+TCP)   |  数据通道 (按 flow 建立, KCP+TCP) |
    |  可选 P2P 升级                      |  可选 P2P 升级                     |
```

- 一个设备 = 一个 UUID = 一条信号通道（TCP 长连接）
- 设备可同时注册服务（provider 端）和订阅服务（accessor 端）
- 数据通道按 flow 建立，`frp_proxy_data_channel` 封装 KCP + relay/P2P

### accessor / provider 角色

统一客户端中，accessor 和 provider 仅存在于数据通道的端点：

| 角色 | 职责 |
|------|------|
| accessor 端 | 监听本地端口，接收客户端连接，发起 `create_flow`，流量入口 |
| provider 端 | 注册后端服务，接收 `prepare_flow`，连接真实后端，流量出口 |

同一进程可同时承担两种角色，共用一条信号通道。

---

## 2. 启动流程

### 2.1 完整启动时序

```
Client                                          Server (UDP)
  |                                                 |
  |--- startup probe (p2p_probe, 加密) ----------->|  base port
  |<-- udp_echo (external_ip:port) ----------------|
  |--- startup probe (p2p_probe, 加密) ----------->|  base port + 1
  |<-- udp_echo (external_ip:port) ----------------|
  |                                                 |
  | 判断: 两次 ip:port 相同 → cone                  |
  |       不同 → symmetric                          |
  |       超时 → disabled                           |
  |                                                 |
  |=== time_sync (NTP-like, UDP 加密) =============>|  (如果 UDP 可用)
  |<== time_sync_response (seq, T1, T2, T3) =======|
  |  至少 3 个样本，去掉极值取平均                   |
  |  → server_clock_offset_us_                      |
  |                                                 |
  |--- TCP connect --------------------------------->|  (信号通道)
  |--- signal_open (1) ---------------------------->|
  |<-- server_hello (3, nonce) ---------------------|
  |--- auth_request (4, digest) ------------------->|  digest = HMAC-SHA256(secret, nonce)
  |<-- auth_response (5, ok) -----------------------|
```

- startup probe 使用 AES-256-CTR 加密，`flow_id=0` 的控制面 key
- 发送间隔 200ms，最多 10 次重试
- 探测完成后调用 `run_time_sync()` 进行时钟同步
- 时钟同步完成后才调用 `connect_signal_channel()` 建立信号通道
- 若 UDP 不可用（`public_server_udp_port == 0`），跳过 probe 和 time_sync，直接连接信号通道

### 2.2 时钟同步 (time_sync)

NTP-like 单向时钟同步，用于 P2P 打洞时双方同步启动时刻：

- Client 通过 UDP 发送 `time_sync_request` (39)，携带 `seq` 和 `client_send_ts`（本地 `steady_clock`，微秒）
- Server 收到后记录 `server_recv_ts`，立即回复 `time_sync_response` (40)，携带 `client_send_ts`（回显）、`server_recv_ts`、`server_send_ts`
- Client 收到后记录本地接收时刻 T4，计算：
  - `offset = ((server_recv_ts - client_send_ts) + (server_send_ts - T4)) / 2`
  - `delay = (T4 - client_send_ts) - (server_send_ts - server_recv_ts)`
- 每 100ms 发一次，至少收集 3 个样本；达到 3 个后再发 7 个，最多 30 个
- 取所有样本按 delay 排序，去掉最小和最大（若样本 >= 5），剩余取平均 offset
- 失败则重连后重试
- 使用 `flow_id=0` 的 KCP 加密 key

---

## 3. 信号通道协议

### 3.1 握手

```
Client                              Server
  |                                    |
  |--- TCP connect ------------------>|
  |--- signal_open (1) -------------->|
  |<-- server_hello (3, nonce) -------|
  |--- auth_request (4, digest) ----->|  digest = HMAC-SHA256(secret, nonce)
  |<-- auth_response (5, ok) ---------|
```

### 3.2 服务注册 (register_services)

设备将所有 groups 批量发送一次。每个 group 携带 `register_key` 和 `services[]`。即使某 group 无 services（纯 accessor），也要发送以在 server 建立 register_key 关联。

```
Client                              Server
  |                                    |
  |--- register_services (31) ------->|
  |    uuid, nat_type,                |
  |    startup_rtt_ms,                |
  |    groups[{                        |
  |      register_key,                |
  |      services[{name,type,         |
  |        enable_p2p}]               |
  |    }]                             |
  |<-- register_services_resp (32) ---|
  |    ok, message                    |
```

### 3.3 服务订阅 (subscribe_services)

设备将所有 register_keys 批量发送一次。Server 返回所有 key 下的可见服务（已自过滤：排除 `provider_uuid == 订阅者 uuid` 的服务）。

```
Client                              Server
  |                                    |
  |--- subscribe_services (33) ------>|
  |    register_keys[]                |
  |<-- subscribe_services_resp (34) --|
  |    ok, message,                   |
  |    services[{name, provider_uuid, |
  |      provider_nat_type,           |
  |      provider_startup_rtt_ms,     |
  |      service_type, enable_p2p}]   |
```

### 3.4 自过滤

两层过滤防止设备连接自己的服务：

- **Server 侧**：`list_services_for_subscriber` 排除 `provider_uuid == subscriber_uuid`
- **Client 侧**：`reconcile_listeners` 直接比较 `uuid_ == service.provider_uuid` 兜底

### 3.5 轮询

有 listeners 的设备每 10s 重新发送 `subscribe_services`，获取最新服务目录并 reconcile。

---

## 4. 数据通道生命周期

### 4.1 完整时序

```
Accessor (uuid=AAA)              Server                   Provider (uuid=BBB)
  |                                 |                              |
  | 本地 client 连接 listener       |                              |
  |                                 |                              |
  |-- create_flow_request (6) ---->|                              |
  |   service_name, register_key,  |                              |
  |   transport (tcp_relay=1 /     |                              |
  |              udp_relay=2)      |                              |
  |                                 |                              |
  |                                 | 查找:                         |
  |                                 | services_by_register_key_    |
  |                                 |   [request.register_key]    |
  |                                 |   → 找到 BBB 的 service      |
  |                                 | 分配 flow_id                 |
  |                                 |                              |
  |<-- create_flow_response (7) ---|                              |
  |   result, flow_id,             |                              |
  |   provider_uuid                |                              |
  |                                 |-- prepare_flow (8) -------->|
  |                                 |   flow_id, service_name,    |
  |                                 |   accessor_uuid=AAA,        |
  |                                 |   transport                 |
  |                                 |                              |
  |== TCP 数据通道 ================|== TCP 数据通道 ==============|
  |   data_open(2, flow_id, uuid)  |   data_open(2, flow_id, uuid)|
  |   frp_proxy_data_channel       |   frp_proxy_data_channel     |
  |   KCP 激活                     |   KCP 激活                   |
  |                                 |                              |
  |                     bind_data_session:                        |
  |                     双方 data session 都连上 →                |
  |                     flow.transport_ready = true               |
  |                                 |                              |
  |<-- flow_transport_ready (12) --| (发送给 provider)            |
  |                                 |                              |
  |                                 |          BBB 连接 backend    |
  |                                 |          发送 flow_ready     |
  |                                 |                              |
  |<-- flow_ready (17) ------------|-- flow_ready (17) ----------|
  |                                 |                              |
  | start_local_read_loop()        |   start_backend_read_loop() |
  | 数据开始双向转发                 |   数据开始双向转发            |
  |                                 |                              |
  | [可选 P2P upgrade, 见 §5]      |   [可选 P2P upgrade]        |
```

### 4.2 数据通道 (frp_proxy_data_channel)

```
业务数据 → KCP → [KCP output callback]
                      |
                      ├─ p2p_success_ → p2p_socket_ (UDP 直连)
                      └─ else        → relay_transport_ (TCP relay)
```

- **relay 阶段**：KCP 输出走 TCP relay 连接到 Server，Server 转发到对端
- **P2P 阶段**：KCP 输出走 UDP p2p_socket_ 直连
- KCP 实例不重建，切换时仅替换底层 output
- 数据进入 KCP 前统一经 AES-256-CTR 加密，密钥由 `HKDF-SHA256(traffic_secret, flow_id)` 派生

### 4.3 数据通道建立细节

1. accessor 发送 `create_flow_request(service_name, register_key, transport)`
2. server 在 `services_by_register_key_[request.register_key]` 查找 service_name → 得到 provider
3. server 分配 `flow_id` (next_flow_id_++)，存入 `flows_by_id_`
4. server 返回 `create_flow_response` 给 accessor，发送 `prepare_flow` 给 provider
5. 双方各自创建 `frp_proxy_data_channel`，建立独立 TCP 连接
6. server `bind_data_session` 配对双方 data session
7. 配对成功 → `flow_transport_ready` 发给 provider
8. provider 连接 backend → 发送 `flow_ready` → server 转发给 accessor
9. 数据开始双向转发

---

## 5. P2P 升级

### 5.1 前提条件

relay 建立后，双方独立检查以下条件决定是否发起 P2P 升级：

- `probed_nat_type_ != disabled`
- `config_.public_server_udp_port != 0`
- `data_channel` 存在
- `session/provider.enable_p2p` 均为 true
- 双方不同时为 symmetric（server 不拦截，由 punch engine 自行判断）

不满足条件则 relay 继续运行，P2P 静默跳过。

### 5.2 升级流程（基于时钟同步的同步打洞）

P2P 升级不再使用 `p2p_upgrade_request`，而是由 accessor 主动通过 `p2p_handshake` 发起，利用时钟同步实现双方同时打洞。

```
Accessor                              Server                        Provider
  |                                      |                              |
  | relay 运行中                          |    relay 运行中               |
  |                                      |                              |
  | create_accessor_punch_engine()       |                              |
  | punch_engine->start()                |                              |
  |  - bind UDP socket [25000,65535]    |                              |
  |  - start endpoint probe              |                              |
  |                                      |                              |
  |--- p2p_probe (9, UDP 加密) -------->|                              |
  |<-- udp_echo (10, ip:port) ----------|                              |
  |                                      |                              |
  | on_endpoint_ready(ip, port)          |                              |
  |                                      |                              |
  |-- p2p_handshake (42) -------------->|-- p2p_handshake ----------->|
  |   flow_id, uuid(provider),          |   (server 转发)              |
  |   external_ip, external_port,       |                              |
  |   nat_type, rtt_ms, punch_seq       |                              |
  |                                      |                              |
  |                                      |   provider 收到:              |
  |                                      |   create_provider_punch_engine()
  |                                      |   on_peer_info(accessor)     |
  |                                      |   punch_engine->start()      |
  |                                      |   - bind UDP socket          |
  |                                      |   - start endpoint probe     |
  |                                      |                              |
  |                                      |--- p2p_probe (UDP) -------->|
  |                                      |<-- udp_echo (ip:port) ------|
  |                                      |                              |
  |                                      |   on_endpoint_ready(ip,port) |
  |                                      |                              |
  |<-- p2p_handshake_ack (43) ----------|<-- p2p_handshake_ack -------|
  |   flow_id, uuid(accessor),          |   (server 转发)              |
  |   external_ip, external_port,       |                              |
  |   nat_type, rtt_ms, punch_seq       |                              |
  |                                      |                              |
  | accessor 计算 deadline:              |                              |
  |   now + offset + (my_rtt+peer_rtt)  |                              |
  |        × 3 × 1000 (微秒)             |                              |
  |                                      |                              |
  |-- punch_start (41) ---------------->|-- punch_start ------------->|
  |   flow_id, uuid(provider),          |   (server 转发)              |
  |   deadline_us (server 时钟)          |                              |
  |                                      |                              |
  | start_punch_at(local_deadline)       |   start_punch_at(local_deadline)
  |                                      |                              |
  |== 双方在同一时刻开始 UDP punch =====================================|
  |                                      |                              |
  | [收到 probe → 校验 flow_id、         |   [收到 probe → 校验 flow_id、  |
  |  端口和 target 匹配 → signal 握手]    |    端口和 target 匹配 → 握手]  |
  |                                      |                              |
  |-- punch_confirm (14) -------------->|-- punch_confirm ----------->|
  |<-- punch_confirm_ack (15) ----------|<-- punch_confirm_ack -------|
  |-- punch_confirm_ok (16) ----------->|-- punch_confirm_ok --------->|
  |                                      |                              |
  | accept_p2p(socket, peer_ep)          |   accept_p2p(socket, peer_ep)|
  | KCP output → UDP                     |   KCP output → UDP           |
  | release TCP relay                    |   release TCP relay          |
```

P2P 升级由 `frp_punch_engine` 独立组件管理，与 `frp_proxy_data_channel` 解耦：
- punch engine 负责 endpoint probe → UDP punch → signal handshake 全流程
- 成功时返回 `{socket, peer_endpoint}` 给 data_channel
- 失败时自毁，由 caller 按重试策略重建

### 5.3 UDP 打洞原理

NAT 规则：出站包在 NAT 上建立映射，后续来自同一目标的入站包才被放行。双方同时向对端外部地址发包，各自出站包建立映射，对端入站包得以通过。

通过时钟同步（time_sync），双方在相同的绝对时刻（以 server `steady_clock` 为基准）同时开始打洞，最大化打洞成功率。

**Symmetric 侧**：
- 65 个本地 socket（基端口 = p2p_socket_ 端口 + 64 个 spread-random 端口）
- 5 次重传，每次 shuffle 顺序，发送到 Cone 侧固定 ip:port

**Cone 侧（对 Symmetric）**：
- 1 个 socket (p2p_socket_)
- 探测端口：shuffle [512, 65535] 全部端口，取前 3000 个
- 5 次重传

**双方 Cone**：各 1 socket，1 轮直接互发 peer 已知端点。

### 5.4 探测包格式 (8 字节明文)

```
+--------------------------+--------------------------+---------------------------+
| 4 bytes flow_id LE       | 2 bytes src_port LE      | 2 bytes target_port LE    |
+--------------------------+--------------------------+---------------------------+
```

- `flow_id`：完整的 32 位 flow_id
- `src_port`：发送方本地 UDP 端口
- `target_port`：对端外部端口（被打洞的目标端口）

### 5.5 打洞成功判定

收到探测包后 punch engine 按以下条件判定匹配：

1. 包长度 == 8 字节
2. `flow_id` 与本 flow 匹配
3. 接收端口 (`local_port`) 属于本方的 punch socket 集合（含 `p2p_socket_` 和 `punch_sockets_`）
4. cone 侧（打 sym 时）额外校验 `target_port` 在 `current_cone_targets_` 集合中

条件满足后 engine 触发 `on_probe_match_` 回调，外部 caller 调用 `is_valid_probe_pair` 二次校验后执行 `send_punch_confirm` 进入 signal 握手。收到任何探测包总是立即回复。

### 5.6 打洞参数

| 参数 | Sym 侧 | Cone 侧 |
|------|--------|---------|
| socket 数 | 65 (1 基 + 64 spread) | 1 |
| 每轮目标 | 1 (Cone 固定端口) | 3000 (shuffle 后取前 3000) |
| 轮数 | 1 | 1 |
| 重传/轮 | 5 | 5 |
| 重传间隔 | (my_rtt+peer_rtt)×3, clamp[1000,10000]ms |
| 端口范围 | [512,65535] | [512,65535] |

### 5.7 打洞失败与重试

打洞轮次耗尽 → `frp_punch_engine` 自毁 → caller 按以下策略重试：

| 重试次数 | 间隔 |
|---------|------|
| 1 | 10s |
| 2 | 20s |
| 3 | 30s |
| ... | +10s/次 |
| ≥6 | 60s (封顶) |
| 上限 | 10000 次 |

---

## 6. P2P Signal 握手

打洞匹配后通过 server 中转完成三次握手（server 转发原始 JSON payload，避免 RTTR 类型切片）。
握手由 `frp_punch_engine` 内部管理，ack/ok 自动回复无需 caller 介入：

```
匹配方 (engine)                Server                        对端 (engine)
  |                            |                             |
  |-- punch_confirm (14) ----->|---- punch_confirm --------->|
  |                            |                             |
  |                            |        on_punch_confirm     |
  |                            |        send ack (engine)    |
  |                            |        → on_success()       |
  |                            |                             |
  |<--- punch_confirm_ack (15)-|<--- punch_confirm_ack ------|
  |                            |                             |
  | on_punch_confirm_ack       |                             |
  | send ok (engine)           |                             |
  | → on_success()             |                             |
  |                            |                             |
  |--- punch_confirm_ok (16) ->|---- punch_confirm_ok ------>|
  |                            |                             |
  |                            |   on_punch_confirm_ok       |
  |                            |   (already succeeded,       |
  |                            |    result_delivered_=true)  |
```

### 6.1 端口 swap 规则

- `punch_confirm` / `punch_confirm_ack`：port 字段 swap 以匹配接收方视角
- 由 `frp_punch_engine` 内部处理，caller 仅负责将 signal channel 收到的消息路由给 engine 的 `on_punch_confirm/ack/ok` 方法

### 6.2 relay 断开兜底

`frp_proxy_data_channel` 提供 `expect_p2p_disconnect()` 方法。punch engine 进入 confirmation 阶段时 caller 调用此方法。relay 断开时若 `p2p_expected_` 为 true 则静默忽略（不触发 disconnect 通知），等待 engine 成功后 `accept_p2p()` 接管。

---

## 7. 超时设计

### 7.1 协议层超时

| 超时 | 值 | 说明 |
|------|-----|------|
| UDP echo probe 重发间隔 | 200ms | startup probe 和 flow endpoint probe 通用 |
| UDP echo probe 最大次数 | 10 | 超时视为 NAT disabled |
| UDP punch 最大轮数 | 1 | 超时静默放弃，trigger retry |
| UDP punch 重传/轮 | 5 | |
| UDP punch 重传间隔 | (my_rtt+peer_rtt)×3, clamp[1000,10000]ms | |
| 时钟同步间隔 | 100ms | time_sync 请求间隔 |
| 时钟同步最小样本 | 3 | 少于 3 则视为失败 |
| 时钟同步最大发送 | 30 | |
| 信号通道重连间隔 | 2s → 4s → ... → 32s | 每次翻倍，最多 32s |
| 服务目录轮询 | 10s | 仅 accessor 端 |
| punch 重试间隔 | 10s → 20s → ... → 60s | 每次+10s，60s 封顶，最多 10000 次 |
| handshake ack 超时 | 30s | accessor 等待 provider 回复 ack |

### 7.2 数据通道超时 (`data_channel_idle_timeout_seconds`)

| 阶段 | 行为 |
|------|------|
| relay 阶段 | 无数据收包超时 → 断开数据通道 → `on_disconnected` → `fail_session` / `flow_failed` |
| P2P 阶段 | 10s idle → 每 2s 发 1 字节 keepalive → 最多 15 次 → 超时断开 |
| keepalive | 1 字节: 发送值 0..127 (探测), 回复值 128..255 (不回回复以防死循环) |

### 7.3 keepalive 分流规则

P2P 收包分为两个阶段，不同组件处理：

**打洞阶段**（`frp_punch_engine::start_punch_read_loop`）：
1. 8 字节 → punch 探测包，解析匹配，立即回复
2. 其余 → 静默丢弃

**数据传输阶段**（`frp_proxy_data_channel::start_p2p_read_loop`，`accept_p2p` 之后接管）：
1. 长度 == 1 → keepalive，值 0..127 回复值+128，重置 idle timer
2. 长度 < 24 → 静默丢弃
3. 其余 → KCP 数据包

---

## 8. 命令全集

| 编号 | 命令 | 方向 | 用途 |
|------|------|------|------|
| 1 | `signal_open` | C→S | 声明信令通道 |
| 2 | `data_open` | C→S | 声明数据通道 (含 flow_id, uuid) |
| 3 | `server_hello` | S→C | 下发 nonce |
| 4 | `auth_request` | C→S | HMAC 认证 |
| 5 | `auth_response` | S→C | 认证结果 |
| 6 | `create_flow_request` | C→S | 创建数据流 (service_name, register_key, transport) |
| 7 | `create_flow_response` | S→C | 创建结果 (result, flow_id, provider_uuid) |
| 8 | `prepare_flow` | S→C | 通知 provider 准备流 |
| 9 | `p2p_probe` | C→S (UDP) | P2P 端点探测 (startup probe / flow endpoint probe 共用) |
| 10 | `udp_echo` | S→C (UDP) | UDP 回显 external_ip:port |
| 12 | `flow_transport_ready` | S→C | relay 数据通道配对完成 |
| 14 | `punch_confirm` | C→S→C | 打洞三次握手 |
| 15 | `punch_confirm_ack` | C→S→C | |
| 16 | `punch_confirm_ok` | C→S→C | |
| 17 | `flow_ready` | C→S→C | 流就绪（端到端链路通） |
| 18 | `flow_failed` | C→S→C | 流失败 (flow_id, reason, message) |
| 19 | `flow_data` | C→S→C | 信号通道数据转发 (保留，当前忽略) |
| 20 | `flow_closed` | C→S→C | 流关闭 (flow_id) |
| 21 | `ping_request` | C→S | 心跳 |
| 22 | `ping_response` | S→C | 心跳回应 |
| 31 | `register_services` | C→S | 批量注册服务 (uuid, nat_type, rtt, groups) |
| 32 | `register_services_resp` | S→C | 注册结果 |
| 33 | `subscribe_services` | C→S | 批量订阅服务目录 (register_keys[]) |
| 34 | `subscribe_services_resp` | S→C | 服务列表 |
| 39 | `time_sync_request` | C→S (UDP) | NTP-like 时钟同步请求 (seq, client_send_ts) |
| 40 | `time_sync_response` | S→C (UDP) | 时钟同步响应 (seq, client_send_ts, server_recv_ts, server_send_ts) |
| 41 | `punch_start` | C→S→C | 时钟同步打洞启动 (deadline_us, 以 server 时钟为基准) |
| 42 | `p2p_handshake` | C→S→C | P2P 握手：accessor → provider (external_ip, external_port, nat_type, rtt_ms, punch_seq) |
| 43 | `p2p_handshake_ack` | C→S→C | P2P 握手应答：provider → accessor |

### flow_failed 原因码

| 原因 | 说明 |
|------|------|
| `relay_channel_open_failed` | relay 数据通道建立失败 |
| `backend_connect_failed` | provider 连接后端失败 |

---

## 9. 资源管理

### 9.1 Server 端

**clients_by_uuid_**：每个 client 的运行时状态（uuid, register_key, nat_type, services set, session weak_ptr）

- 创建：`register_client_services` 时建立
- 更新：重新注册时覆盖 nat_type、rtt、services
- 清理：`clear_client_state_locked` — 移除 services_by_register_key_ 中的对应条目，清理 clients_by_uuid_

**flows_by_id_**：每个 flow 的运行状态（flow_id, provider/accessor uuid, service_name, transport, data sessions）

- 创建：`create_flow` 分配 flow_id 时
- 清理：
  - `flow_failed` → 立即擦除
  - `flow_closed` (非 P2P) → 等待 data session 断开后擦除
  - `flow_closed` (P2P 已完成) → 立即擦除 (data session 已提前 release)
  - 信号通道断开 → `release_session_state` 擦除关联 flows

**sessions_by_uuid_**：uuid → signal session weak_ptr 映射

**services_by_register_key_**：`register_key → (service_name → directory_entry)` 二级映射

- 创建：`register_client_services` 写入
- 清理：client 断开时清除其所有 service 条目；register_key 下无服务时清除 key

### 9.2 Client 端

**信号通道 (channel_)**：
- 单例，TCP 长连接
- 断线 → `on_disconnected` → 清理 listeners_、accessor_sessions_、pending_sessions_、provider_flows_、last_known_services_ → `schedule_reconnect` → `run_time_sync` → `connect_signal_channel`
- `release_obj` → `channel_->release_obj()`

**接入端资源 (accessor)**：

| 资源 | 生命周期 |
|------|---------|
| `listeners_` (TCP acceptor / UDP socket) | `reconcile_listeners` 创建/更新，`release_obj` 关闭所有 acceptor |
| `accessor_sessions_` | `create_flow_response` 创建，`fail_session` / `release_obj` 移除 |
| `pending_sessions_` | `request_flow` 创建，`create_flow_response` 取出 (FIFO) |
| `poll_timer_` | 10s 周期性 `subscribe_services`，`release_obj` cancel |

**提供端资源 (provider)**：

| 资源 | 生命周期 |
|------|---------|
| `provider_flows_` | `handle_prepare_flow` 创建，`flow_failed` / `release_obj` 移除 |
| `flows[id].data_channel` | prepare_flow 时创建，`flow_failed` / `release_obj` 时 release |
| `flows[id].punch_engine` | 收到 `p2p_handshake` 时创建，success/failure 时 reset |
| `flows[id].backend_socket` | `start_provider_backend_connect` 异步连接，flow 关闭时析构 |
| `flows[id].punch_retry_timer` | 打洞失败后重试 timer |

**打洞引擎 (frp_punch_engine)**：
- 独立组件，管理 endpoint probe → UDP punch → signal handshake 全生命周期
- provider 侧：收到 `p2p_handshake` 后创建，`on_peer_info` 设置对端信息
- accessor 侧：relay 建立后主动创建，endpoint probe 完成后发送 `p2p_handshake`
- 成功后通过 `on_success` 回调返回 `{socket, peer_endpoint}`，caller 调用 `data_channel->accept_p2p()`
- 失败后自毁，caller 按重试策略重建新实例
- 内部状态：p2p_socket_, punch_sockets_, endpoint_probe_timer_, punch_timer_, deadline_timer_

**数据通道 (frp_proxy_data_channel)**：
- KCP 实例：贯穿 relay 和 P2P 两阶段
- relay TCP 连接：P2P 成功后 release
- p2p_socket_：通过 `accept_p2p()` 从 punch engine 接管；成功后作为主要发送 socket
- p2p_timer_：P2P keepalive timer

### 9.3 清理顺序

**Server 侧 `release_session_state`**：
1. 清理 `sessions_by_uuid_` 中的过期弱引用
2. 遍历 `flows_by_id_`：匹配 data session → 通知 peer 断开 → 擦除 flow
3. 遍历 `flows_by_id_`：P2P-signaled 且信号 session 断开 → 通知 peer → 擦除 flow

**Client 侧 `release_obj`**：
1. cancel timer (reconnect, poll)
2. 释放 signal channel
3. 释放所有 accessor sessions 的 data_channel
4. 释放所有 provider flows 的 data_channel
5. 关闭所有 listener acceptors / UDP sockets
6. 清空所有 map

---

## 10. 配置模型

```json
{
  "threads": 4,
  "public_server_host": "127.0.0.1",
  "public_server_tcp_port": 15000,
  "public_server_udp_port": 15000,
  "traffic_secret": "shared-secret",
  "nat_type": 2,
  "local_ip": "",
  "data_channel_idle_timeout_seconds": 120,
  "ssl": { "disable_ssl": true },
  "groups": [
    {
      "register_key": "key1",
      "services": [
        { "service_name": "echo", "service_type": 0,
          "target_host": "127.0.0.1", "target_port": 12345,
          "enable_p2p": true }
      ],
      "listeners": [
        { "service_name": "rdp", "service_type": 0,
          "listen_host": "0.0.0.0", "listen_port": 19001,
          "enable_p2p": true }
      ]
    }
  ]
}
```

- `groups` 至少一项；每组一个 `register_key`，跨组不能重复
- 每组 `services + listeners` 不能都为空
- 同 key 下 service_name 唯一
- services → provider 端（注册到服务目录）
- listeners → accessor 端（从服务目录匹配后创建本地监听）
- `service_type`：0 = TCP，1 = UDP
- `nat_type`：0 = disabled，1 = symmetric，2 = cone
- `nat_type` 为配置提示值，实际值由 startup probe 探测决定；若探测失败则使用 `disabled`
