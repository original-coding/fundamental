# FRP 重构决策记录

对比基准：`d001707` (旧 frp_runtime) → `e986674` (新 frp，当前 `frp_stash`)

---

## Track 1: 服务注册与发现 — 已确认（无变更）

服务注册与发现的交互序列和数据结构在新旧版本中完全一致：

```
Client                          Server
  │── signal_open ──────────────▶│
  │◀─ server_hello (nonce) ─────│
  │── auth_request (digest) ────▶│
  │◀─ auth_response (ok/msg) ───│
  │── register_services ────────▶│  frp_register_services_data
  │◀─ register_services_resp ───│  frp_register_services_resp_data
  │── subscribe_services ───────▶│  frp_subscribe_services_data
  │◀─ subscribe_services_resp ──│  frp_subscribe_services_resp_data
```

**字段一致**：命令结构、字段名、序列化方式与旧版相同。

**内部存储差异（D1）**：旧版用中心化字典 `services_by_register_key_` 存服务，新版把 `groups` 存在 session 自身的 `groups` 字段上，查询时遍历 sessions 匹配。`subscribe_services_resp` 返回给客户端的字段（`service_name`, `provider_uuid`, `provider_nat_type`, `service_type`, `enable_p2p`, `provider_startup_rtt_ms`）和排除自己的逻辑与旧版一致，业务功能无变化。

### 确认结果

- **D1 服务目录存储模型**：确认，不影响业务功能。

---

## Track 2a: relay 代理通道建立 — 已确认

### D2 — `channel_open` 两阶段握手替代 `create_flow + data_open + bind_data_session`

旧版 accessor 访问 provider 服务需要 4 步信令交互 + 2 次独立 TCP 连接 + 服务端 bind_data_session 配对。
新版简化为 2 次 TCP 连接 + server 路由配对，握手信息在 `channel_open` 的 payload 中由客户端协议层定义。

#### 服务端路由层

Server 只解析 `channel_open_request_data` 的外层字段：

| 字段 | 用途 |
|---|---|
| `from_uuid` | 发起方的 uuid |
| `dst_uuid` | 目标方的 uuid |
| `connection_uuid` | 客户端生成的通道标识（替代旧版 flow_id） |
| `status` | 0=阶段一, 1=阶段二 |
| `payload` | 客户端协议层数据，服务端原封转发 |

`register_data_channel` 行为：

| status | 行为 |
|---|---|
| 0 | 记录 `sessions[from_uuid][connection_uuid]`，查 `sessions[dst_uuid][dst_uuid]` 找对端信令 session，`send_raw(payload)` 转发 |
| 1 | 记录 `sessions[from_uuid][connection_uuid]`，校验 `sessions[dst_uuid][connection_uuid]` 已存在，`upgrade()` 双向绑定，`start_data_forward_read_loop()` |

#### 客户端协议层

payload 内部结构定义在 `frp_client_command.hpp`，服务端不解析：

```cpp
enum frp_client_command_type : std::uint8_t {
    // relay 通道建立
    frp_client_open   = 100, // accessor → provider: 请求连接服务
    frp_client_accept = 101, // provider → accessor: 后端就绪，接受连接
    frp_client_reject = 102, // provider → accessor: 后端连接失败，拒绝
    // P2P 打洞 (通过 frp_forward_command 透传，见 Track 2b)
    frp_client_accessor_punch_start     = 103,
    frp_client_provider_p2p_handshake   = 104,
    frp_client_accessor_handshake_ack   = 105,
    frp_client_accessor_punch_confirm   = 106,
    frp_client_provider_confirm_ack     = 107,
    frp_client_accessor_confirm_ok      = 108,
    frp_client_provider_probe_match     = 109,
};

struct frp_client_open_data {
    std::uint8_t command = frp_client_open;
    std::string accessor_uuid;      // 供 provider 回复时路由
    std::string connection_uuid;    // 通道标识
    std::string register_nonce;     // nonce for key verification
    std::string register_hash;      // SHA256(register_key + register_nonce)
    std::string service_name;       // 目标服务名
    std::uint8_t transport = 0;     // 0=tcp, 1=udp
};

struct frp_client_accept_data {
    std::uint8_t command = frp_client_accept;
    std::string connection_uuid;
};

struct frp_client_reject_data {
    std::uint8_t command = frp_client_reject;
    std::string connection_uuid;
    std::string reason;
};
```

#### 完整 relay 通道建立流程

```
Accessor                                   Server                          Provider

  [信令通道已建立]                           [信令+数据]                     [信令通道已建立]
  本地listener accept用户连接
  生成 connection_uuid = X
  计算 register_nonce (随机)
  计算 register_hash = SHA256(key + nonce)

  ── 新TCP连接 ───────────────────────────▶
  ── channel_open(status=0) ──────────────▶
     from=A, dst=P, conn=X
     payload: { command=open,
       accessor_uuid=A, connection_uuid=X,
       register_nonce, register_hash,
       service_name="web", transport=tcp }
                                           │
                                           sessions[A][X] = A的数据session
                                           查 sessions[P][P] → P的信令session
                                           ── forward payload ────────────▶
                                                                          │  信令通道收到payload
                                                                          │  解析: command=open
                                                                          │  校验 register_hash ✓
                                                                          │  校验 service_name ✓
                                                                          │  记录 connection_uuid
                                                                          │  连接后端（TCP connect 或 UDP resolve+bind）
                                                                          │  成功 → frp_client_accept
                                                                          │  失败 → frp_client_reject
                                                                          │  新TCP连接
                                                                          │
                                           ◀── channel_open(status=1) ─────
                                              from=P, dst=A, conn=X
                                              payload: { command=accept, connection_uuid=X }
                                               或 { command=reject, connection_uuid=X, reason }
                                           │
                                           sessions[P][X] = P的数据session
                                           sessions[A][X] 存在 ✓
                                           upgrade() 双向绑定
                                           send_raw(payload) 确认给双方
                                           start_data_forward_read_loop
                                           │
  ◀── send_raw(payload) ──────────────────│  ── send_raw(payload) ───────▶
                                           │
  ═══ local_socket ◀══▶ TCP ═══ Server ═══ TCP ◀══▶ backend ══════════
```

**步骤：**

1. Accessor 生成 `connection_uuid`，计算 `register_nonce`（随机）和 `register_hash = SHA256(register_key + register_nonce)`，新建 TCP 连接，发送 `channel_open(status=0, from=A, dst=P, conn=X, payload={command=open, accessor_uuid, connection_uuid, register_nonce, register_hash, service_name, transport})`
2. Server `register_data_channel` status=0：记录 `sessions[A][X]`，查 `sessions[P][P]` 找到 provider 信令 session，`send_raw(payload)` 转发
3. Provider 信令通道收到 raw payload，解析出 `service_name`、`transport`、`register_nonce`、`register_hash`，**先校验 key**：`SHA256(register_key + register_nonce) == register_hash`，不匹配则 reject。校验通过后，**先连接后端**：
   - TCP：`async_connect` 到 `target_host:target_port`，成功后 → `frp_client_accept`，失败 → `frp_client_reject(reason)`
   - UDP：`resolve` + `bind` 本地 socket，成功后 → `frp_client_accept`，失败 → `frp_client_reject(reason)`
4. Provider 新建 TCP 连接，发送 `channel_open(status=1, from=P, dst=A, conn=X, payload={command=accept|reject})`
5. Server status=1：记录 `sessions[P][X]`，检测 `sessions[A][X]` 已存在 → `upgrade()` 双向绑定 → `send_raw` payload 给双方 → `start_data_forward_read_loop()`
6. Accessor 收到 accept 后开始从 local_socket 读数据发送；收到 reject 则关闭本地连接
7. 数据透明转发 — accessor 数据通道 ↔ provider 数据通道

### D3 — accept/reject 确认语义

provider 必须先连接后端，结果决定 payload.command：
- 后端连接成功 → `frp_client_accept` → accessor 开始读写
- 后端连接失败 → `frp_client_reject(reason)` → accessor 关闭本地连接，不开始读写

这替代了旧版的 `flow_ready` / `flow_failed` 两步信号，语义合一。
accessor 收到 `frp_client_accept` 后才知道后端已就绪，可以开始发送数据。收到 `frp_client_reject` 则直接失败。

### D4 — TCP/UDP 代理一致处理

旧版 TCP 和 UDP 代理走同一套 flow，差异仅在 provider "连接后端" 这一步：

| | TCP | UDP |
|---|---|---|
| 后端连接 | `async_connect` 握手 | `resolve` + `bind(0)` 本地 socket |
| 判断就绪 | connect 回调成功 | resolve + socket open 成功 |
| 就绪信号 | `flow_ready` | `flow_ready` |
| 失败信号 | `flow_failed` | `flow_failed` |

新版统一为 accept/reject：
- TCP：connect 成功 → accept；connect 失败 → reject("backend connect failed")
- UDP：resolve+bind 成功 → accept；失败 → reject("backend resolve failed")

UDP provider 侧在 resolve+bind 后记录 `backend_udp_target` 用于后续数据转发。accessor 侧 UDP listener 按 `remote_endpoint` 映射 session，`flow_ready`（新版 accept）之前收到的数据在 `pending_forwards` 缓冲区中暂存。

### D5 — `upgrade()` 回调链替代 `forward_flow_bytes()` 中转

旧版 server 的 `forward_flow_bytes()` 每次数据中转都查 `flows_by_id_[flow_id]` 获取 peer data session。
新版 `upgrade()` 在绑定阶段直接设置两个 session 之间的读写回调：

```
A.forward_cb = [w_P](data, len) { P->send_raw(data, len); }
P.forward_cb = [w_A](data, len) { A->send_raw(data, len); }
```

数据路径零查找开销，服务端不解析、不缓存数据。
A.release_cb 和 P.release_cb 互相注册，任一方 TCP 断开自动清理对端。

### D6 — `flow_failed` / `flow_closed` 移除

旧版通过信令通道显式传递 `flow_failed` / `flow_closed` 命令通知对端。
新版：TCP 会话存活即通道存活。任一端 TCP 断开 → `release_obj()` → release_cb 触发 → 对端 `remove_session()` → 对端 release。数据通道的关闭由 TCP 断开和 upgrade 时设置的 release_cb 自动处理，不需要额外协议命令。

provider 在连接后端阶段失败时，通过 channel_open(status=1) 的 payload.reject 通知 accessor，这是唯一的显式拒绝路径。

#### 与旧版的对照总结

| 旧版 | 新版 |
|---|---|
| `create_flow_request` 信令通道发起 | accessor 直接新 TCP + `channel_open(status=0)` |
| server 分配 `uint32 flow_id` | client 生成 `string connection_uuid` |
| server 查目录 + 组装 `prepare_flow` | server 只做路由转发 |
| provider 收到结构化 `prepare_flow` | provider 信令通道收到 raw payload，自行解析 |
| provider → `data_open(flow_id, uuid)` | provider → `channel_open(status=1, from, dst, conn)` |
| accessor → `data_open(flow_id, uuid)` | accessor 的 session 已在 status=0 时记录 |
| `bind_data_session` 查 flow 配对 | `register_data_channel` 查 connection_uuid 配对 |
| `flow_transport_ready` → 连接后端 | 先连后端再决定 accept/reject |
| `flow_ready` → accessor 开始读 | `frp_client_accept` 确认后端就绪 |
| `flow_failed` / `flow_closed` 通知对端 | TCP 断开自动清理 + release_cb |
| `forward_flow_bytes` 中转 | `upgrade()` 回调链直连 |
| P2P 命令走信令通道 | 移到客户端 UDP 层（见 Track 2b） |

---

---

## Track 2b: P2P 升级 — 已确认

P2P 打洞命令通过 `frp_forward_command` 在信令通道上透传，无需服务端改动。

### D7 — P2P 命令走 `frp_forward_command` 透传

旧版 P2P 打洞走信令通道的 6 个专用命令（`frp_runtime_punch_start/handshake/handshake_ack/confirm/confirm_ack/confirm_ok`），服务端通过 `relay_punch_message` 按 `uuid` 路由。

新版利用已有的 `frp_forward_command`，所有 P2P 命令封装在 payload 中透传：

```cpp
// frp_client_command.hpp
enum frp_client_command_type : std::uint8_t {
    // relay 通道
    frp_client_open   = 100,
    frp_client_accept = 101,
    frp_client_reject = 102,
    // P2P 打洞
    frp_client_accessor_punch_start     = 103,
    frp_client_provider_p2p_handshake   = 104,
    frp_client_accessor_handshake_ack   = 105,
    frp_client_accessor_punch_confirm   = 106,
    frp_client_provider_confirm_ack     = 107,
    frp_client_accessor_confirm_ok      = 108,
    frp_client_provider_probe_match     = 109,
};
```

P2P 命令数据中使用 `connection_uuid`（替代旧版 `flow_id`）关联 relay 通道。

交互流程：

```
Accessor                                Server                                Provider
  │                                        │                                      │
  │ [relay TCP 通道已建立，数据通过 server 中转]                                     │
  │                                        │                                      │
  │── frp_forward_command ────────────────▶│                                      │
  │   dst_uuid=P                           │── forward_data ────────────────────▶│
  │   payload: { command=accessor_punch_start,                                   │
  │     connection_uuid=X,                 │                                      │
  │     deadline_us=... }                  │                                      │
  │                                        │                                      │
  │                                        │◀── frp_forward_command ─────────────│
  │◀── forward_data ──────────────────────│   dst_uuid=A                          │
  │   payload: { command=provider_p2p_handshake,                                 │
  │     connection_uuid=X,                │   payload: { command=provider_p2p_handshake,
  │     internal_ip, external_ip,         │     connection_uuid=X,               │
  │     nat_type, rtt_ms, punch_seq }     │     internal_ip, external_ip,         │
  │                                        │     nat_type, rtt_ms, punch_seq }     │
  │── frp_forward_command(accessor_handshake_ack) ─▶│── forward_data ───────────▶│
  │                                        │                                      │
  │                                        │◀── frp_forward_command ─────────────│
  │◀── forward_data ──────────────────────│   dst_uuid=A                          │
  │   payload: { command=provider_probe_match,                                    │
  │     connection_uuid=X,                │   payload: { command=provider_probe_match,
  │     local_port, peer_port,            │     connection_uuid=X,               │
  │     external_local_port,              │     local_port, peer_port,            │
  │     external_peer_port }              │     external_local_port,              │
  │                                        │     external_peer_port }              │
  │── frp_forward_command(accessor_punch_confirm) ─▶│── forward_data ───────────▶│
  │                                        │                                      │
  │                                        │◀── frp_forward_command ─────────────│
  │◀── forward_data ──────────────────────│   dst_uuid=A                          │
  │   payload: { command=provider_confirm_ack,                                    │
  │     connection_uuid=X,                │   payload: { command=provider_confirm_ack,
  │     local_port, peer_port,            │     connection_uuid=X,               │
  │     external_local_port,              │     local_port, peer_port,            │
  │     external_peer_port }              │     external_local_port,              │
  │                                        │     external_peer_port }              │
  │── frp_forward_command(accessor_confirm_ok) ──▶│── forward_data ─────────────▶│
  │                                        │                                      │
  │═══════ KCP over UDP 直连 ════════════════════════════════════════════════════│
  │  (relay TCP 数据连接可以关闭)           │                                      │
```

**与旧版的对照：**

| 旧版 | 新版 |
|---|---|
| 专用命令 `frp_runtime_punch_*` | 封装在 `frp_forward_command.payload` |
| `relay_punch_message` 按 `uuid` 路由 | `forward_data` 按 `dst_uuid` 路由 |
| `flow_id` 关联 relay 通道 | `connection_uuid` 关联 relay 通道 |
| `deadline_us` 基于 server 时钟 | 同，通过 UDP `time_sync` 同步 |
| 6 步握手 (start/handshake/handshake_ack/confirm/confirm_ack/confirm_ok) | 7 步，新增 `provider_probe_match` (对称 NAT 探测匹配) |
| 命令不带角色前缀 | 命令带 accessor/provider 角色前缀 (如 `frp_client_accessor_punch_start`) |

服务端零改动 — `handle_authenticated_phase` 已有的 `frp_forward_command` 分支即可处理所有 P2P 透传。

---

---

## Track 2 附录: 原有 relay 交互流程（已记录，供参考）

### 前置状态

- Accessor 和 Provider 各自的信令通道已建立，认证通过，已注册/订阅服务。
- Accessor 本地已根据 `subscribe_services_resp` 的返回结果，为每个需要的服务启动了本地监听端口（TCP acceptor 或 UDP socket）。
- 当 Accessor 的本地监听端口收到来自用户的连接时，触发以下流程。

### 流程概览

```
Accessor                              Server                            Provider
 (信令)                                (信令+数据)                        (信令)
   │                                     │                                 │
   │──── 1. create_flow_request ────────▶│                                 │
   │                                     │── 2. 查服务目录，分配flow_id ───│
   │                                     │── 3. prepare_flow ────────────▶│
   │                                     │                                 │── 4. 校验service_name
   │                                     │                                 │── 5. 创建data_channel对象
   │                                     │                                 │── 6. 新TCP连接
   │                                     │◀── 7. data_open(flow_id,uuid) ─│
   │                                     │── 8. bind_data_session ────────│
   │◀──── 9. create_flow_response ──────│                                 │
   │── 10. 创建data_channel对象          │                                 │
   │── 11. 新TCP连接                     │                                 │
   │── 12. data_open(flow_id,uuid) ────▶│                                 │
   │                                     │── 13. bind_data_session ───────│
   │                                     │   transport_ready!              │
   │                                     │── 14. flow_transport_ready ───▶│
   │                                     │                                 │── 15. 连接后端target
   │                                     │◀── 16. flow_ready ─────────────│
   │◀─── 17. flow_ready ────────────────│                                 │
   │                                     │                                 │
   │═════ 18. 数据中转 ═══════════════ SERVER RELAY ════ 数据中转 ════════│
```

### 逐步说明

#### 步骤 1: Accessor 发起 flow 请求 (信令通道)

Accessor 本地的 listener accept 到用户连接后，创建 `accessor_session_context`，通过信令通道发送：

```json
{
  "command": "create_flow_request",
  "service_name": "web",
  "register_key": "demo-key",
  "transport": "tcp_relay"
}
```

`transport`：`tcp_relay=1`，`udp_relay=2`

#### 步骤 2: Server 处理 create_flow (服务端内部)

Server 的 `create_flow()`：
- 查 `clients_by_uuid_` 校验 accessor 存在
- 查 `services_by_register_key_[register_key][service_name]` 获取 provider_uuid
- 通过 `clients_by_uuid_` 获取 provider session
- `next_flow_id_++` 分配新 flow_id
- 创建 `flow_runtime_state` 存入 `flows_by_id_[flow_id]`
- 校验 transport 必须是 `tcp_relay` 或 `udp_relay`

#### 步骤 3: Server → Provider: prepare_flow (信令通道)

```json
{
  "command": "prepare_flow",
  "flow_id": 1,
  "accessor_uuid": "acc-xxxx",
  "service_name": "web",
  "transport": "tcp_relay"
}
```

#### 步骤 4: Provider 校验

`handle_prepare_flow()` 在本地的 `services_by_name_` 中查找 `service_name`，确认是本设备注册的服务。若找不到则回复 `flow_failed`。

#### 步骤 5: Provider 创建 data_channel 对象

Provider 创建 `frp_proxy_data_channel` 对象：
- 封装了到 server 的数据连接（内部会创建新 TCP 连接）
- 封装了 KCP + AES-256-CTR 加密
- 设置 `on_data` 回调（收到数据 → write 到 backend）
- 设置 `on_disconnected` 回调（发 `flow_failed`，清理资源）
- 设置 `on_p2p_upgraded` 回调（P2P 升级完成通知）
- 调用 `start()` 建立到 server 的 TCP 连接

#### 步骤 6-7: Provider 打开数据连接

`frp_proxy_data_channel::start()` 内部新建 TCP 连接，连接成功后在**新连接**上发送：

```json
{
  "command": "data_open",
  "flow_id": 1,
  "uuid": "provider-uuid"
}
```

#### 步骤 8: Server bind_data_session (provider 侧)

Server `bind_data_session()`：
- 查 `flows_by_id_[flow_id]`
- 校验 `data.uuid == flow.provider_uuid`
- 将 session 记录为 `flow.provider_data_session`
- 检查 `accessor_data_session` 是否也已到齐 → 若到齐设 `transport_ready = true`

#### 步骤 9: Server → Accessor: create_flow_response (信令通道)

```json
{
  "command": "create_flow_response",
  "result": "accepted",
  "flow_id": 1,
  "provider_uuid": "provider-uuid",
  "message": ""
}
```

`result`：`accepted=1`，`rejected=2`

#### 步骤 10: Accessor 创建 data_channel 对象

Accessor 收到 `create_flow_response(result=accepted)`：
- 从 `pending_sessions_` 中 FIFO 取出匹配的 session context
- 记录 `flow_id` 和 `provider_uuid` 到 session
- 创建 `frp_proxy_data_channel`：
  - 设置 `on_data` 回调（收到数据 → write 到 local_socket）
  - 设置 `on_disconnected` 回调（调用 `fail_session` 清理本地资源）
  - 设置 `on_p2p_upgraded` 回调
- 若双方都 enable_p2p 且 NAT 已知，创建 punch_engine
- 调用 `start()` 建立到 server 的 TCP 连接

#### 步骤 11-12: Accessor 打开数据连接

新建 TCP 连接，发送 `data_open`：

```json
{
  "command": "data_open",
  "flow_id": 1,
  "uuid": "accessor-uuid"
}
```

#### 步骤 13: Server bind_data_session (accessor 侧)

Server 再次调用 `bind_data_session()`：
- 校验 `data.uuid == flow.accessor_uuid`
- 将 session 记录为 `flow.accessor_data_session`
- provider_data_session 已存在 → `transport_ready = true`

#### 步骤 14: Server → Provider: flow_transport_ready (信令通道)

```json
{
  "command": "flow_transport_ready",
  "flow_id": 1
}
```

#### 步骤 15: Provider 连接后端

Provider 收到 `flow_transport_ready` → `start_provider_backend_connect()`：
- **TCP**: resolve + async_connect 到 `target_host:target_port`
- **UDP**: resolve + bind UDP socket
- 成功后开启双向读写 loop（backend ↔ data_channel）

#### 步骤 16: Provider → Server: flow_ready (信令通道)

```json
{
  "command": "flow_ready",
  "flow_id": 1
}
```

#### 步骤 17: Server → Accessor: flow_ready (信令通道)

Server `provider_mark_flow_ready()` 转发。Accessor 收到后：
- 标记 `session->ready = true`
- 启动 `start_local_read_loop`（local_socket → data_channel）
- 若 P2P 可行，启动 punch_engine

#### 步骤 18: 数据中转

```
Accessor                          Server                         Provider
local_socket                       relay                         backend
    │ read_some                                                      │
    │ → data_channel.send_bytes()                                    │
    │                    forward_flow_bytes():                       │
    │                    查 flows_by_id_[flow_id]                    │
    │                    找 peer session                             │
    │                    peer_session->send_raw(data) ──────────────▶│
    │                                                         on_data回调
    │                                                         backend.write
    │                                                      backend.read_some
    │                    peer_session->send_raw(data) ◀──────────────│
    │ on_data回调                                                     │
    │ local_socket.write                                              │
```

Server 每次中转都查 `flows_by_id_[flow_id]` 获取 peer data session。

#### 断开/异常处理 (信令通道)

| 命令 | 方向 | 触发条件 | 服务端行为 | 对端行为 |
|---|---|---|---|---|
| `flow_failed` | 任一方 → Server → 另一方 | 后端连接失败、data_channel 断开 | 查 flow → 转发 → 删除 flow | 清理 session |
| `flow_closed` | 任一方 → Server → 另一方 | 用户侧 TCP 断开 | 转发，flow 保留等 data session 断开 | 关闭 socket |

#### P2P 打洞 (信令通道)

P2P 命令走信令通道，Server 通过 `relay_punch_message()` 按 `uuid` 字段查 `clients_by_uuid_` 找对端转发：

| 命令 | 携带的关键数据 |
|---|---|
| `punch_start` | `flow_id`, `uuid`, `deadline_us` (server 时钟) |
| `p2p_handshake` | `flow_id`, `uuid`, `internal_ip`, `internal_port`, `external_ip`, `external_port`, `rtt_ms`, `nat_type`, `punch_seq` |
| `p2p_handshake_ack` | 同上 |
| `punch_confirm` | `flow_id`, `uuid`, `local_port`, `peer_port`, `external_local_port`, `external_peer_port` |
| `punch_confirm_ack` | 同上 |
| `punch_confirm_ok` | 同上 |

打洞成功 → `frp_proxy_data_channel` 切换到 KCP over UDP 模式，绕过 server relay。

---
