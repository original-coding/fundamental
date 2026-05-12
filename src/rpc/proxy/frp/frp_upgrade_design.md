# FRP 原理原型协议设计

## 1. 目标与范围

当前目标是收敛 `src/rpc/proxy/frp/` 的原理原型协议基线，并完成两条本地主线验证：

- `tcp_relay` 原型链路可建立并完成转发
- `p2p` 原型链路可完成 `udp echo probe -> udp_punch -> KCP -> flow_ready -> 转发`

当前阶段不是产品化方案，不保留旧协议兼容层，不再使用 Docker 验收，只做本地模拟验证。

## 2. 统一术语表

- `nat_type`
  节点级运行配置，表示该角色的 NAT 类型，取值为 `disabled` / `symmetric` / `full`。
- `transport`
  会话建立时选定的正式传输类型，正式取值仅有 `tcp_relay`（p2p upgrade 通过独立流程完成，不作为 transport 枚举值）。
- `udp echo probe`
  使用加密 JSON UDP 回显协议向 `public_server` 获取某个 UDP socket 外网 `ip:port` 的流程。
- `startup_probe`
  启动期全局 `udp echo probe`，用于判断当前运行时的 NAT 类型。两次探测结果相同为 `full`，不同为 `symmetric`，失败为 `disabled`。
- `flow_endpoint_probe`
  单个 flow p2p upgrade 期的 `udp echo probe`，用于获取建联 UDP socket 的外网 `ip:port`。
- `udp_punch`
  双方建联 UDP socket 间的直连打洞探测流程，详见 `udp_p2p_flow.md`。
- `flow`
  建立期控制单元，由 `flow_id` 标识；生命周期止于 `flow terminal result`。
- `flow terminal result`
  建立期终结结果，只包含 `flow_ready` 与 `flow_failed`。
- `session`
  `flow_ready` 之后进入会话期的自治数据面对象，不再受 `flow_id` 控制面驱动。
- `proxy_data_channel`（`frp_proxy_data_channel`）
  会话期顶层数据面对象，统一封装 relay 和 p2p 两条底层链路，对上层暴露统一的 KCP 传输接口。relay 建立后可发起 p2p upgrade，upgrade 成功后底层切换为 UDP，KCP 负责重传保证数据连续性。
- `kcp session`
  `proxy_data_channel` 内部的 KCP 实例，贯穿 relay 和 p2p 两个阶段，不是顶层会话概念。
- `p2p upgrade`
  relay 建立后，在同一 flow 上发起的 UDP 打洞升级流程。成功后 `proxy_data_channel` 底层从 TCP relay 切换为 UDP，失败则静默保持 relay。

## 3. 废弃词

以下词汇不再作为当前原型协议的正式用语：

- `enable_p2p`
- `supports_p2p`
- `prefer_p2p`
- `relay`（作为正式 transport 枚举值）
- `xtcp`
- `stcp`
- `low_ttl_probe`
- `relay_channel`（改用 `proxy_data_channel`）
- `p2p session`（改用 `proxy_data_channel`）

## 4. 角色职责

### 4.1 public_server

`public_server` 是建立期协调器，不是会话期控制器。

职责：

- 维护 `provider` / `accessor` 的 signal 长连接
- 处理 `create_flow`
- 为 `tcp_relay` 配对 `relay_channel`
- 为 `p2p` 中转 `udp echo probe` 结果与 `udp_punch` 成功通知
- 同步发送 `flow terminal result`

约束：

- 已完成 `flow terminal result` 发送后，`flow_id` 立即从控制面失效
- 不再控制已完成建立的 `session`
- 对过期 `flow_id` 的迟到 signal/UDP 包输出 `WARN`

### 4.2 provider

`provider` 是会话期所有者之一。

职责：

- 读取配置并确定自身 `nat_type`
- 在 startup 阶段完成 `startup_probe`
- 在 `join.nat_type` 中向 `public_server` 投影最终运行时 NAT 类型
- 在 `prepare_flow` 后按 `transport` 进入 `tcp_relay` 或 `p2p` 建立期
- 数据通道建立后连接 backend
- 发送 `flow_ready` 或 `flow_failed`
- 在会话期负责 backend 侧收尾与本端数据面清理

### 4.3 accessor

`accessor` 是会话期所有者之一。

职责：

- 读取配置并确定自身 `nat_type`
- 在 startup 阶段完成 `startup_probe`
- 基于本地运行时状态与服务目录选择 `transport`
- 为每条本地 accepted TCP 连接创建建立期 `flow`
- 在 `p2p` 路径中驱动 `udp_punch`
- 收到 `flow_ready` 后开始本地业务转发
- 在会话期负责 accepted TCP 连接侧收尾与本端数据面清理

## 5. 配置与状态模型

### 5.1 配置输入

- `public_server`
  - TCP 监听地址/端口
  - 两个 UDP 监听地址/端口
  - `traffic_secret`
  - 注册密钥许可列表
- `provider`
  - `public_server` 地址信息
  - `traffic_secret`
  - `register_key`
  - `nat_type`
  - 服务列表：`service_name -> target_host:target_port`
- `accessor`
  - `public_server` 地址信息
  - `traffic_secret`
  - `register_key`
  - `nat_type`
  - 监听列表：`service_name -> listen_host:listen_port`

### 5.2 协议常量

- `udp_echo_probe` 重发间隔：`200ms`
- `udp_echo_probe` 最大发送次数：`10`
- `udp_punch` symmetric 侧本地 socket 数：`32`
- `udp_punch` full 侧第 0 轮扫描端口数：`64`
- `udp_punch` full 侧后续每轮扫描端口数：`128`
- `udp_punch` 每包最大重发次数：`5`
- `udp_punch` 重发间隔：`100ms`
- `udp_punch` 轮间隔：`1s`
- `udp_punch` 最大轮次：`32`
- `p2p` 会话 keepalive 周期：`10s`

### 5.3 运行时状态

- startup probe 结果：`full` / `symmetric` / `disabled`
- provider 对外 `join.nat_type`（由 startup probe 结果决定，非配置直传）
- 单个 `flow` 的建立期状态
- `flow_ready` 后的会话期自治对象（`proxy_data_channel`）

运行时状态仅存在于内存对象与日志中，不写回配置，不做状态持久化或查询接口。进程退出后全部丢失。

## 6. 协议常量与部署参数边界

- 协议常量可以先硬编码
- 地址、端口、`service_name`、密钥、`nat_type` 都属于部署参数，必须通过配置提供

## 7. 启动期 udp echo probe

### 7.1 目标

在 signal 建立前，用同一个 UDP socket 对 `public_server` 的两个 UDP 端口做回显探测，判断当前运行时是否支持 P2P。

### 7.2 协议

- 使用加密 JSON UDP 报文
- 使用基于 `traffic_secret` 派生的 AES-256-CTR 对称加密，`flow_id=0` 的 key 用于控制面 UDP 包
- 使用临时 `probe_uuid`
- `probe_uuid` 与 runtime `uuid`、`flow_id` 完全无关

### 7.3 流程

1. 若 `nat_type=disabled`，跳过探测，本次运行视为 relay-only，`join.nat_type=disabled`。
2. 创建一个 UDP socket。
3. 生成 `probe_uuid`。
4. 向 server UDP 端口 1 发送 `udp echo probe`，每 `200ms` 重发，最多 `10` 次，直到收到回包。
5. 使用同一个 socket 对 server UDP 端口 2 重复同样流程。
6. 比较两次回包中的外网 `ip:port`。
7. 一致则 `nat_type=full`；不一致则 `nat_type=symmetric`；任一超时则 `nat_type=disabled`。
8. 探测结束后，才进入后续 signal 流程。

### 7.4 日志

启动探测结束后，`provider` 与 `accessor` 都要输出统一总结日志，至少包含：

- `probe_result_1=<ip:port>`
- `probe_result_2=<ip:port>`
- `detected_nat_type=full|symmetric|disabled`

## 8. transport 选择规则

`accessor` 始终使用 `transport=tcp_relay` 发起 `create_flow`。

p2p upgrade 是 relay 建立后的独立升级流程，由以下条件共同决定是否发起：

- accessor 配置 `nat_type != disabled`
- accessor startup probe 结果 `!= disabled`
- 服务目录里的 `provider_nat_type != disabled`
- 双方不同时为 `symmetric`（服务器在 `create_flow` 时已拦截此情况）

不满足上述条件时，relay 建立后不发起 p2p upgrade，直接保持 relay 运行。

`create_flow` 使用显式 `transport=tcp_relay`，不再保留任何”偏好”字段。

## 9. flow 生命周期规则

- `flow_id` 仅在 `create_flow_result=accepted` 时分配
- `flow_id` 从 `create_flow` 成功创建开始生效
- `flow_id` 对应的建立期三元组固定为：
  - `provider_uuid`
  - `accessor_uuid`
  - `service_name`
- `flow_id` 在对应 `flow terminal result` 同步发送完成后立即失效

`flow_ready` 是唯一成功放流信号。  
`flow_failed` 是建立期唯一失败终结信号。

## 10. 建立期流程

所有 flow 统一走 `tcp_relay` 建立，relay 就绪后按条件发起 p2p upgrade。

### 10.1 relay 建立期流程

1. accessor 发送 `create_flow(service_name, tcp_relay)`
2. server 返回 `accepted(flow_id)` 并向 provider 下发 `prepare_flow(flow_id, service_name, tcp_relay)`
3. provider 与 accessor 各自新建一条 relay TCP 数据连接，同时创建 KCP 实例（KCP output → TCP relay）
4. 双方用 `flow_id + runtime uuid` 做握手
5. server 将两条 TCP 连接配对成 relay 通道
6. provider 在 relay 通道成立后连接 backend
7. provider 发送 `flow_ready(flow_id)` 或 `flow_failed(flow_id, reason, message)`
8. server 将 terminal result 同步发送给 accessor 后销毁该 `flow_id`
9. `proxy_data_channel` 进入会话期自治运行，底层为 TCP relay

### 10.2 p2p upgrade 流程

relay 建立后，accessor 与 provider 各自收到 `flow_ready` 后立即发起 p2p upgrade（若满足条件）：

1. accessor 与 provider 各自向 server 发送 `p2p_upgrade_request(flow_id)`
2. server 在双方都请求后，协调双方执行 `flow_endpoint_probe`，此时 server 将 flow 标记为 `p2p_signaled`（后续 relay 断开不再触发 flow 清理）
3. 双方各自创建建联 UDP socket，执行 `flow_endpoint_probe`（向 server UDP 端口发探测包）
4. server 在双方都 `flow_endpoint_ready` 后，分别下发 `flow_p2p_peer(flow_id, peer_ip, peer_port, peer_nat_type)`
5. 双方执行 `udp_punch`（详见 `udp_p2p_flow.md`）—— 打洞确认不经过公共服务器，双方通过 UDP 直连完成
6. 任一方收到对方探测包后**立即回复**一个正确填充 `peer_port` 的探测包；连续收到 2 次 `reflected_port == my_port` 的匹配包即判定打洞成功
7. 判定成功后，本地直接执行 `switch_to_p2p()`：将 KCP output 从 TCP relay 切换为 UDP socket，release relay 连接
8. KCP 对未 ACK 的 segment 通过 UDP 路径重传，保证数据连续性
9. 若本方已发送过确认探测包但尚未收到 2 次匹配，此时 relay 被对方关闭（对方已判定成功），本方也视为 p2p 成功，执行 `switch_to_p2p()`
10. `proxy_data_channel` 底层切换完成，触发 `on_p2p_upgraded` 回调，同时启动 10s idle 超时定时器（无数据收包则每 2s 发 1 字节保活探测，最多 15 次，收到对方保活包立即回复）

**upgrade 失败处理：**

- probe 超时或 punch 超时：静默放弃，relay 继续运行，触发 `on_p2p_upgrade_failed` 回调
- 不影响已建立的 relay 会话

说明：

- relay 建立成功日志统一打印：`transport=tcp_relay / flow_id / service_name / local / remote`
- p2p upgrade 成功日志打印：`transport=p2p / flow_id / service_name / local / remote`
- `proxy_data_channel` 可保留只读元信息：`flow_id`、`provider_uuid`、`accessor_uuid`、`service_name`、当前底层 transport

## 12. udp echo probe

### 12.1 通用协议

`udp echo probe` 是统一的加密 JSON UDP 回显协议，使用 AES-256-CTR 对称加密。

关联键规则：

- startup probe：`probe_uuid` 有值且 `flow_id=0`
- flow endpoint probe：`flow_id!=0` 且 `probe_uuid` 为空

其他组合视为非法。

### 12.2 flow_endpoint_probe

作用：获取单个 `p2p flow` 建联 UDP socket 的外网 `ip:port`。

规则：

- 使用新的建联 UDP socket
- 只对 `public_server` 的第一个 UDP 端口探测
- 使用与 startup probe 相同的 `200ms * 10` 重发常量
- 外网 `ip:port` 通过 signal 中的 `flow_endpoint_ready` 返回

## 13. udp_punch

详见 [udp_p2p_flow.md](udp_p2p_flow.md)。

协议常量摘要：

- `symmetric` 一侧本地 socket 数：`32`（`xxx` + 31 个随机端口，范围 `[xxx-128, xxx+128]`，合法范围 `[1024, 65535]`）。每轮发探测包前对所有 socket 顺序做 `std::shuffle`，避免源端口顺序访问被误判为端口扫描攻击
- `full` 一侧第 0 轮扫描端口数：`64`（`[yyy, yyy+63]`，`std::shuffle` 后随机顺序）
- `full` 一侧后续每轮扫描端口数：`128`，从 `[23, 65535]` 随机选取且不重复，`std::shuffle` 后随机顺序
- 轮间隔：`1s`，计时器驱动，双方独立
- 最大轮次：`32`
- `p2p` 会话 idle 超时：`10s`（无数据收包则进入探测态）
- `p2p` keepalive 探测间隔：`2s`（1 字节 UDP 保活包，收到对方保活包立即回复）
- `p2p` keepalive 最大探测次数：`15`（超出判定连接断开，触发 disconnect）
- 任何收包（KCP 数据或保活）均重置 idle 超时

## 14. UDP 小包协议

只定义两种非 KCP 小包：

- `1` 字节 keepalive：发送值 `0..127`（探测），回复值 `128..255`（不回复回复，防止死循环）
- `6` 字节 `udp_punch` 探测包

### 14.1 探测包格式

```text
+----------------------+---------------------+---------------------+
| 2 bytes flow_id LE   | 2 bytes local_port LE| 2 bytes peer_port LE|
+----------------------+---------------------+---------------------+
```

- `flow_id`：取低 16 位，用于校验包的合法性
- `local_port`：发送方本地 UDP socket 绑定的端口
- `peer_port`：对方的内网端口，未知时填 0

### 14.2 即时回复规则

收到对方探测包后，**立即回复**一个正确填充 `peer_port` 的探测包（`peer_port` = 收包中的 `local_port`）。此回复不等下一轮 `do_punch_round`。

### 14.3 打洞成功判定

- 收到探测包且 `reflected_port == my_port` → 计数 +1
- 连续收到 2 次匹配包 → 判定打洞成功
- 若已发送过确认探测包（`peer_port != 0`），此时 relay 被对方关闭，也视为成功

### 14.4 收包分流规则

本地 UDP 收包入口严格按以下顺序分流：

1. 长度 `== 1`：当作 keepalive，静默丢弃
2. 长度 `== 6`：当作 `udp_punch` 包；探测阶段按 §14.2-§14.3 处理，其他阶段静默丢弃
3. 长度 `< KCP 头最小长度` 且不属于上面两类：静默丢弃
4. 其余包：按 KCP 数据包处理

`provider` 与 `accessor` 的这套入口分流规则必须完全一致。

## 15. KCP 数据面

- KCP 是所有 flow 的统一传输层，relay 和 p2p 均走 KCP
- **KCP 输入前加密**：业务数据进入 KCP 前，统一经过 AES-256-CTR 对称加密，解密在 KCP 输出后进行。加密层与底层传输无关（TCP relay 或 P2P UDP 均使用同一加密路径）
- **密钥派生**：使用 HKDF-SHA256 从 `traffic_secret + flow_id` 派生 per-flow 对称密钥（`frp_derive_kcp_flow_key`）。双方 `flow_id` 相同，派生结果完全一致
- **控制面 UDP 包**（startup probe、flow endpoint probe）：使用 `flow_id=0` 派生的 key
- **数据面 KCP 流量**：使用对应 `flow_id` 派生的 per-flow key
- relay 阶段：加密后的 KCP output 通过 TCP relay 连接发送
- p2p 阶段：加密后的 KCP output 通过建联 UDP socket 发送
- 切换时 KCP 实例不重建，仅替换底层 output 路径；KCP 对未 ACK 的 segment 通过新路径重传
- KCP 使用非流式模式
- `flow_id` 直接作为 KCP `conv`
- 业务数据仍由上层读事件驱动
- 不新增额外的 P2P 成功业务握手包

## 16. 建立期控制面消息

### 16.1 flow 创建与传输选择

- `create_flow(service_name, transport)`（transport 固定为 `tcp_relay`）
- `prepare_flow(flow_id, service_name, transport)`
- `create_flow_result`

`create_flow_result` 最小结果集合：

- `accepted`
- `p2p_unavailable`（保留，用于服务器拦截不可行的 p2p upgrade 请求）
- `rejected`

### 16.2 p2p upgrade 协调

- `p2p_upgrade_request(flow_id)`（accessor/provider → server，relay 就绪后发起）
- `flow_endpoint_ready(flow_id, external_ip, external_port)`
- `flow_p2p_peer(flow_id, peer_ip, peer_port, peer_nat_type)`

打洞确认不再经过公共服务器：双方通过 UDP 直连探测包中的 `peer_port` 字段完成互相确认（详见 §14）。

### 16.3 flow terminal result

- `flow_ready(flow_id)`
- `flow_failed(flow_id, reason, message)`

### 16.4 设计原则

- 建立期控制消息默认采用最小消息体设计
- 诊断信息优先通过日志暴露，而不是扩张消息体
- 日志既承担诊断职责，也承担本地原型验收的证据职责

## 17. flow_failed 原因全集

第一版最小全集固定为：

- `relay_channel_open_failed`
- `flow_endpoint_probe_timeout`
- `punch_timeout`
- `backend_connect_failed`

`flow_failed.message` 只作为人类可读诊断信息，不参与程序分支逻辑。

## 18. 会话期数据面对象

### 18.1 顶层会话对象

- `proxy_data_channel`（`frp_proxy_data_channel`）：统一封装 relay 和 p2p 两条底层链路

### 18.2 会话内部组件

- `accepted tcp connection`（accessor 侧）
- `backend tcp connection`（provider 侧）
- `kcp session`（贯穿 relay 和 p2p 阶段）
- TCP relay 连接（relay 阶段底层）
- UDP socket（p2p 阶段底层，upgrade 成功后替换 TCP relay）
- `keepalive timer`（p2p 阶段）

### 18.3 生命周期规则

- `flow_ready` 之前创建的所有数据面对象都是建立期临时对象
- 若最终进入 `flow_failed`，这些临时对象必须全部销毁
- `flow_ready` 同步发送完成后，相关对象进入会话期自治状态
- 进入会话期后，不再产生任何 `flow_failed`
- p2p upgrade 成功后，TCP relay 连接在 KCP output 切换完成后 release

## 19. 状态机

### 19.1 startup_probe

- `disabled_by_config`
- `probing_udp_port_1`
- `probing_udp_port_2`
- `detected_full`
- `detected_symmetric`
- `probe_failed`（超时，视为 disabled）

### 19.2 accessor flow

- `waiting_transport_decision`（已移除，始终 tcp_relay）
- `waiting_relay_ready`
- `established`（relay 运行中）
- `upgrading_to_p2p`（可选，relay 运行中同时进行 upgrade）
- `closed`

### 19.3 provider flow

- `waiting_prepare`
- `waiting_relay_ready`
- `waiting_backend_connect`
- `established`（relay 运行中）
- `upgrading_to_p2p`（可选，relay 运行中同时进行 upgrade）
- `closed`

### 19.4 p2p upgrade（proxy_data_channel 内部）

- `idle`（未发起）
- `waiting_flow_endpoint`
- `waiting_punch_result`
- `upgraded`（底层已切换为 UDP）
- `upgrade_failed`（静默，relay 继续）

## 20. 日志规则

### 20.1 建立成功日志

`provider` 与 `accessor` 都必须输出一次性建立成功日志，统一字段顺序：

- `transport`
- `flow_id`
- `service_name`
- `local`
- `remote`

要求：

- relay 建立时 `transport=tcp_relay`
- p2p upgrade 成功时额外输出一条 `transport=p2p` 的建立成功日志
- `service_name` 必须非空
- `local/remote` 一律指当前数据面 socket 自身端点

### 20.2 P2P 特定日志

`p2p` 主线应能观测到：

- startup `udp echo probe` 结果日志（含 `detected_nat_type`）
- `udp_punch` 成功日志
- p2p upgrade 成功日志（`transport=p2p`）

### 20.3 迟到消息

- server 对过期 `flow_id` 的迟到 signal/UDP 包逐条输出 `WARN`
- 本地 `provider/accessor` 对探测阶段结束后的迟到 6 字节包静默丢弃
- `provider` 对 1 字节 keepalive 静默丢弃，不产生日志

## 21. 失败与回退规则

- relay 建立失败：`flow_failed(relay_channel_open_failed)`，accessor 不重试
- p2p upgrade 失败（probe 超时或 punch 超时）：静默放弃，relay 继续运行，触发 `on_p2p_upgrade_failed`
- relay 本身不再设计额外重试

## 22. 原型链路验收

### 22.1 tcp_relay 主线

前提：

- 至少一侧 `nat_type=disabled`，或双方均为 `symmetric`

成功判据：

- 明确观测到 `transport=tcp_relay` 建立成功日志
- 本地 client 经 accessor 访问 backend 成功
- 无 p2p upgrade 发起（条件不满足时）

### 22.2 p2p upgrade 主线

前提：

- 双方 `nat_type != disabled`
- 双方 startup probe 成功
- 不同时为 `symmetric`

成功判据：

- 明确观测到 startup `udp echo probe` 结果日志（`detected_nat_type`）
- 明确观测到 `transport=tcp_relay` 建立成功日志（relay 先建立）
- 明确观测到 `udp_punch` 成功日志
- 明确观测到 `transport=p2p` upgrade 成功日志
- 本地 client 经 accessor 访问 backend 成功，数据经 p2p 通道传输

### 22.3 验收环境

- 不使用 Docker
- 使用本地固定角色拓扑：
  - `public_server`
  - `provider`
  - `accessor`
  - `backend`
  - `client`
- backend 保持最小化，可使用简单 TCP 回显或简单 HTTP 服务
