# UDP 打洞核心流程

## 1. 前提

双方在开始打洞前，已通过信令通道完成：

1. `create_flow(transport=tcp_relay)` → relay 建立，`proxy_data_channel` 进入会话期
2. 双方各自收到 `flow_ready` 后，向 server 发送 `p2p_upgrade_request(flow_id)`
3. 各自创建建联 UDP socket
4. flow 级 `udp echo probe`（向 public_server 发探测包，获取自身外部 IP:Port）
5. `flow_endpoint_ready`（server 确认双方端点已就绪）
6. `flow_p2p_peer`（双方获知对端外部 IP:Port 及对端 NAT 类型）

打洞期间 relay 保持运行，数据持续通过 TCP relay 传输。

## 2. NAT 类型

节点在 startup 阶段完成探测后，确定自身 NAT 类型，并在 `join` 时上报给 server，再通过 `flow_p2p_peer` 告知对端。

三种类型：

| 类型 | 含义 |
|------|------|
| `disabled` | 禁用 P2P，不参与打洞流程 |
| `symmetric` | 对称型 NAT，对不同目标分配不同外部端口 |
| `full` | 完全支持 P2P（Full Cone / Restricted Cone） |

只要任意一方为 `disabled`，直接走 TCP relay，不进入打洞流程。

## 3. 打洞原理

NAT 的核心规则：**出站包会在 NAT 上建立映射，后续来自同一目标的入站包才被放行。**

打洞利用这一规则：双方同时向对端外部地址发包，各自的出站包在本地 NAT 上建立映射，对端的入站包因此得以通过。

**双方必须同时发包**，单方向发包无法完成打洞。

### Symmetric NAT 的特殊性

Symmetric NAT 对不同目标分配不同的外部端口，因此对端通过 `udp echo probe` 观察到的端口，与实际打洞时使用的端口不同。

解决方式：**端口预测 + 多 socket 覆盖**

- `symmetric` 一侧：基于自身 `udp echo probe` 得到的本地端口 `xxx`，在 `[xxx-128, xxx+128]` 区间内随机选取 31 个端口，加上 `xxx` 本身，共 32 个本地端口绑定探测 socket；所有端口值必须在 `[1024, 65535]` 范围内，不合法则重新随机
- `full` 一侧：已知对端 `udp echo probe` 的外网端口 `yyy`，第一轮优先探测 `[yyy, yyy+63]` 共 64 个端口（随机顺序），之后每轮从 `[23, 65535]` 中随机选取 128 个端口，跳过已探测过的端口，最多 32 轮
- 普通打洞（双方均为 `full`）：每侧绑 1 个 socket，`full` 一侧按上述规则扫描对端端口

## 4. 探测包格式

固定 6 字节明文包：

```text
+----------------------+---------------------+
| 4 bytes flow_id LE   | 2 bytes local_port LE|
+----------------------+---------------------+
```

- `flow_id`：用于校验包的合法性，匹配当前 flow 即认为合法
- `local_port`：发送方本地 UDP socket 绑定的端口，供对端命中后定位对应 socket

## 5. 打洞流程

```text
accessor                  public_server                  provider
    |                           |                           |
    |--- p2p_upgrade_request -->|<-- p2p_upgrade_request ---|
    |   (relay 已运行)           |        (relay 已运行)     |
    |                           |                           |
    | 创建建联 UDP socket        |        创建建联 UDP socket |
    | 执行 flow_endpoint_probe  |        执行 flow_endpoint_probe
    |--- flow_endpoint_ready -->|<-- flow_endpoint_ready ---|
    |                           |                           |
    |<------ flow_p2p_peer -----|----- flow_p2p_peer ------>|
    |   (含对端 nat_type)        |        (含对端 nat_type)  |
    |                           |                           |
    | 按角色绑定本地 socket      |        按角色绑定本地 socket
    |                           |                           |
    |-- punch (同时) -------------------------------------------------->|
    |<------------------------------------------------- punch (同时) --|
    |                           |                           |
    | [任意一方收到对端探测包]   |                           |
    | 从包中读出对端 local_port  |                           |
    |-- p2p_connected --------->|                           |
    |   (含 matched_peer_local_port)                        |
    |                           |-- peer_p2p_connected ---->|
    |                           |   (含 matched_peer_local_port)
    |                           |                           |
    | 保留命中的 socket          |          保留对应 socket  |
    | 关闭其余 socket            |          关闭其余 socket  |
    |                           |                           |
    | KCP output 切换为 UDP      |     KCP output 切换为 UDP |
    | release TCP relay          |     release TCP relay     |
    |                           |                           |
    |          [proxy_data_channel 底层切换完成]             |
```

双方收到 `flow_p2p_peer` 后立即开始向对端发探测包，无需等待对方先发。

## 6. 轮次机制

### symmetric 一侧（发包方）

- 基于 `udp echo probe` 得到的本地端口 `xxx`，从 `[xxx-128, xxx+128]` 中随机选取 31 个合法端口（范围 `[1024, 65535]`，不合法重新随机），加上 `xxx` 本身，共 32 个本地端口
- 每轮绑定这 32 个端口的 UDP socket，每个 socket 向对端已知外网 `ip:port` 发探测包
- 每个探测包在本轮内最多重发 5 次，重发间隔 100ms
- 轮间隔 1s，关闭本轮 socket，重新生成一组新的本地端口，最多 32 轮

### full 一侧（扫描方）

- 绑定 1 个本地 UDP socket
- **第 0 轮**：对对端外网端口 `yyy` 后续的 64 个端口 `[yyy, yyy+63]`，按随机顺序逐一发探测包
- **第 1～32 轮**：每轮从 `[23, 65535]` 中随机选取 128 个端口，跳过已探测过的端口，逐一发探测包
- 每个探测包在本轮内最多重发 5 次，重发间隔 100ms
- 轮间隔 1s，计时器驱动，独立于对端

### 双方均为 full

- 双方各绑 1 个本地 socket，按 `full` 一侧的扫描规则互相探测

## 7. 成功与失败

**成功条件：** 任意一方收到对端发来的 6 字节探测包，且 `flow_id` 匹配。

**失败条件：** 32 轮内双方均未收到对端探测包，失败原因记为 `punch_timeout`，回退到 TCP relay。

## 8. 成功后本地动作

1. 从探测包中读出对端 `local_port`，保留对应 socket，关闭其余 socket
2. 向 public_server 发送 `p2p_connected(flow_id, matched_peer_local_port)`
3. 收到 `peer_p2p_connected(flow_id, matched_peer_local_port)` 后，另一侧同样执行步骤 1
4. accessor 启动 1 字节 UDP keepalive
5. `proxy_data_channel` 内部将 KCP output 从 TCP relay 切换为 UDP socket
6. KCP 对未 ACK 的 segment 通过 UDP 路径重传，保证数据连续性
7. release TCP relay 连接，停止 relay 接收
8. 触发 `on_p2p_upgraded` 回调

## 9. 与 UDP 小包协议的关系

本地 UDP 收包入口严格按以下顺序分流：

1. 长度 `== 1`：keepalive，静默丢弃
2. 长度 `== 6`：打洞探测包；探测阶段处理，其他阶段静默丢弃
3. 长度 `< KCP 头最小长度` 且不属于前两类：静默丢弃
4. 其余包：进入 KCP 数据面
