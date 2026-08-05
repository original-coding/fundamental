# FRP 迁移记录

对比基准：`d001707` (旧 frp_runtime) → `e986674` (新 frp，当前 `frp_stash`)

## 状态概览

| 组件 | 状态 |
|---|---|
| 服务端 (`frp_server`) | 已完成 |
| 客户端协议定义 (`frp_client_command.hpp`) | 已完成 |
| 统一客户端 (`frp_client`) | 已完成 |
| P2P 打洞引擎 (`frp_punch_engine`) | 已完成 |
| app 层适配 | 已完成 |
| KCP 密钥派生适配 (`frp_kcp_crypto`) | 已完成 |
| 旧代码清理 | 已完成 |

所有组件已实现并编译通过。

---

## 一、新增文件清单

### 1.1 `src/rpc/proxy/frp/frp_client_command.hpp` — 客户端协议层命令

Payload 的内部命令定义，服务端不解析，通过 `channel_open` 或 `frp_forward_command` 的 payload 字段透传。

```cpp
enum frp_client_command_type : std::uint8_t {
    // relay 通道建立
    frp_client_open   = 100, // accessor → provider: 请求连接服务
    frp_client_accept = 101, // provider → accessor: 后端就绪，接受
    frp_client_reject = 102, // provider → accessor: 后端失败，拒绝
    // P2P 打洞 (通过 frp_forward_command 透传)
    frp_client_accessor_punch_start     = 103,
    frp_client_provider_p2p_handshake   = 104,
    frp_client_accessor_handshake_ack   = 105,
    frp_client_accessor_punch_confirm   = 106,
    frp_client_provider_confirm_ack     = 107,
    frp_client_accessor_confirm_ok      = 108,
    frp_client_provider_probe_match     = 109,
};
```

对应 struct：`frp_client_open_data`、`frp_client_accept_data`、`frp_client_reject_data`、P2P 各命令的 data struct。

`frp_client_open_data` 包含 `accessor_uuid`、`connection_uuid`、`register_nonce`、`register_hash`、`service_name`、`transport`。
`frp_client_accept_data` 和 `frp_client_reject_data` 都包含 `connection_uuid` 用于路由匹配。

### 1.2 `src/rpc/proxy/frp/frp_client.cpp/hpp` — 统一客户端 (已完成)

实现 `frp_unified_client`，替代旧的 `frp_runtime_unified_client_agent`。1670 行实现。

核心能力：
- 信令通道管理（连接、重连、认证、注册、订阅）— `frp_tcp_channel` 封装 TCP 帧读写 + SSL
- relay 通道建立（channel_open 两阶段握手，service_key 验证）
- 数据转发（local_socket ↔ data channel ↔ 对端）
- P2P 升级（frp_forward_command 透传打洞命令，含 provider_probe_match）
- UDP NAT 探测 & 时间同步
- TCP/UDP listener 管理 (reconcile_listeners)

### 1.3 `applications/frp_proxy_client/src/frp_proxy_client.cpp` — App 层适配 (已完成)

include 已从旧 `frp_runtime_*` 切换到新 `frp_client.hpp` + `frp_config.hpp`。

### 1.4 `applications/CMakeLists.txt` — 客户端编译 (已恢复)

```cmake
add_subdirectory(frp_proxy_server)
add_subdirectory(frp_proxy_client)
add_subdirectory(frp_echo_test)
```

---

## 二、客户端实现详细

### 2.1 信令通道

复刻旧版 `frp_runtime_signal_client_channel`，适配新命令：

| 阶段 | 发送 | 接收 |
|---|---|---|
| 连接 | `signal_open` | `server_hello` |
| 认证 | `auth_request(digest)` | `auth_response` |
| 注册 | `register_services(groups)` | `register_services_resp` |
| 订阅 | `subscribe_services(keys)` | `subscribe_services_resp` |

差异：旧版用 `frp_runtime_*` 命令，新版用 `frp_*` 命令。命令名去掉 `runtime_` 前缀，字段结构一致。

### 2.2 服务注册与发现

- 注册：构造 `frp_register_services_data`，填入 `uuid`、`nat_type`、`startup_rtt_ms`、`groups`
- 订阅：构造 `frp_subscribe_services_data`，填入 `register_keys`
- 收到 `subscribe_services_resp` → `reconcile_listeners`：为每个 visible service 创建本地 listener（TCP acceptor 或 UDP socket）

### 2.3 Relay 通道建立

当 accessor 本地 listener accept 到用户连接时：

```
1. 生成 connection_uuid
2. 计算 register_nonce + SHA256(register_key + nonce) → register_hash
3. 新 TCP 连接 → channel_open(status=0, from=A, dst=P, conn=X,
   payload={command=open, accessor_uuid=A, connection_uuid=X,
            register_nonce, register_hash,
            service_name, transport})
4. 等待对端 status=1
5. 收到 accept → 开始 local_socket ↔ data channel 读写
   收到 reject → 关闭本地 socket
```

当 provider 信令通道收到 `frp_client_open` payload 时：

```
1. 解析 service_name、transport、connection_uuid、register_nonce、register_hash
2. 校验 register_hash == SHA256(register_key + register_nonce) → 防止未授权访问
3. 连接后端 target
   - TCP: async_connect
   - UDP: resolve + bind
4. 新 TCP 连接 → channel_open(status=1, from=P, dst=A, conn=X,
   payload={command=accept|reject, connection_uuid})
5. accept 后服务端 upgrade，数据通道打通
```

### 2.4 数据转发

Accessor 侧：
- `local_socket.async_read_some` → `send_raw` 到数据通道
- 数据通道 `on_data` → `local_socket.async_write`

Provider 侧：
- `backend_socket.async_read_some` → `send_raw` 到数据通道
- 数据通道 `on_data` → `backend_socket.async_write`

UDP 差异：
- Accessor 侧按 `remote_endpoint` 映射 session
- Provider 侧按 `backend_udp_target` 做收发

### 2.5 P2P 升级

P2P 打洞不再需要服务端参与，直接通过信令通道 `frp_forward_command` 透传：

```
1. Accessor 发 accessor_punch_start (带 deadline_us，基于 server 时钟同步后的时间)
2. Provider 回 provider_p2p_handshake (IP, port, nat_type, rtt_ms, punch_seq)
3. Accessor 发 accessor_handshake_ack
4. Provider 发 provider_probe_match (检测到对称 NAT 端口对匹配) [新增]
5. Accessor 发 accessor_punch_confirm (确认端口对)
6. Provider 发 provider_confirm_ack
7. Accessor 发 accessor_confirm_ok
8. 打洞成功 → relay 数据通道升级为 KCP over UDP 直连
9. relay TCP 数据连接关闭
```

时间同步：UDP 通道发 `time_sync_request` 到 server，server 返回 `time_sync_response`(T1/T2/T3/T4)，client 计算 `server_clock_offset_us`。

NAT 探测：UDP 通道发 `p2p_probe` 到 server，server 返回 `udp_echo(external_ip, external_port)`。

### 2.6 断开处理

- TCP 数据通道断开 → release_obj → release_cb 自动清理对端
- 信令通道断开 → 重连（reconnect_timer），重连后重新注册+订阅
- P2P 通道断开 → 降级回 relay TCP（如果仍存在）

---

## 三、变更文件清单

### 3.1 新增文件

| 文件 | 说明 |
|---|---|
| `frp_client.cpp/hpp` | 统一客户端（1670 行），替代 `frp_runtime_client` |
| `frp_client_command.hpp` | 客户端协议层命令定义（服务端不解析） |
| `frp_server.cpp/hpp` | 新服务端（861 行），替代 `frp_runtime_server` |
| `frp_command.hpp` | 服务端路由层命令定义，整合旧 `frp_runtime_command` |

### 3.2 重命名/重构文件

| 旧文件 | 新文件 | 说明 |
|---|---|---|
| `frp_runtime_common.cpp/hpp` | `frp_common.cpp/hpp` | 去掉 `runtime_` 前缀 |
| `frp_runtime_command.hpp` | `frp_command.hpp` | 命令重命名，去掉 `runtime_` 前缀 |

### 3.3 修改文件

| 文件 | 变更 |
|---|---|
| `frp_config.hpp` | include 切换到 `frp_command.hpp`，类型引用去掉 `runtime_` 前缀 |
| `frp_kcp_crypto.cpp/hpp` | `frp_derive_kcp_flow_key(flow_id)` → `frp_derive_kcp_key(salt)`; salt 从 `"flow:N"` 改为 `connection_uuid` 字符串 |
| `frp_punch_engine.cpp/hpp` | `flow_id` → `connection_uuid`; 新增 `send_provider_probe_match` 支持对称 NAT; 回调接口更新为角色化方法 |
| `applications/frp_proxy_client/` | include 切换到新 API |
| `applications/frp_proxy_server/` | 适配新 server API |

### 3.4 删除文件

| 文件 | 说明 |
|---|---|
| `frp_runtime_client.cpp/hpp` (1611+172 行) | 被 `frp_client` 替代 |
| `frp_runtime_server.cpp/hpp` (1037+250 行) | 被 `frp_server` 替代 |
| `frp_runtime_command.hpp` (501 行) | 被 `frp_command.hpp` 替代 |
| `frp_proxy_data_channel.cpp/hpp` (464+183 行) | 不再需要独立封装，功能整合到 `frp_client` |
| `frp_client_upstream.hpp` (231 行) | 被 `frp_tcp_channel` 替代 |
