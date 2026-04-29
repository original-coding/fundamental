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
  会话建立时选定的正式传输类型，正式取值仅有 `p2p` 与 `tcp_relay`。
- `udp echo probe`
  使用加密 JSON UDP 回显协议向 `public_server` 获取某个 UDP socket 外网 `ip:port` 的流程。
- `startup_probe`
  启动期全局 `udp echo probe`，用于判断当前运行时是否具备 P2P 能力。
- `flow_endpoint_probe`
  单个 `p2p flow` 建立期的 `udp echo probe`，用于获取建联 UDP socket 的外网 `ip:port`。
- `udp_punch`
  双方建联 UDP socket 间的直连打洞探测流程，详见 `udp_p2p_flow.md`。
- `flow`
  建立期控制单元，由 `flow_id` 标识；生命周期止于 `flow terminal result`。
- `flow terminal result`
  建立期终结结果，只包含 `flow_ready` 与 `flow_failed`。
- `session`
  `flow_ready` 之后进入会话期的自治数据面对象，不再受 `flow_id` 控制面驱动。
- `relay_channel`
  `tcp_relay` 路径下的会话期顶层数据面对象。
- `p2p session`
  `p2p` 路径下的会话期顶层数据面对象。
- `kcp session`
  `p2p session` 的内部组成，不是顶层会话概念。

## 3. 废弃词

以下词汇不再作为当前原型协议的正式用语：

- `enable_p2p`
- `supports_p2p`
- `prefer_p2p`
- `relay`（作为正式 transport 枚举值）
- `xtcp`
- `stcp`
- `low_ttl_probe`

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

- startup probe 结果：`probe_succeeded` / `probe_failed`
- provider 对外 `join.nat_type`
- 单个 `flow` 的建立期状态
- `flow_ready` 后的会话期自治对象

运行时状态仅存在于内存对象与日志中，不写回配置，不做状态持久化或查询接口。进程退出后全部丢失。

## 6. 协议常量与部署参数边界

- 协议常量可以先硬编码
- 地址、端口、`service_name`、密钥、`nat_type` 都属于部署参数，必须通过配置提供

## 7. 启动期 udp echo probe

### 7.1 目标

在 signal 建立前，用同一个 UDP socket 对 `public_server` 的两个 UDP 端口做回显探测，判断当前运行时是否支持 P2P。

### 7.2 协议

- 使用加密 JSON UDP 报文
- 复用现有基于 `traffic_secret` 的 UDP AEAD 封装
- 使用临时 `probe_uuid`
- `probe_uuid` 与 runtime `uuid`、`flow_id` 完全无关

### 7.3 流程

1. 若 `nat_type=disabled`，跳过探测，本次运行视为 relay-only。
2. 创建一个 UDP socket。
3. 生成 `probe_uuid`。
4. 向 server UDP 端口 1 发送 `udp echo probe`，每 `200ms` 重发，最多 `10` 次，直到收到回包。
5. 使用同一个 socket 对 server UDP 端口 2 重复同样流程。
6. 比较两次回包中的外网 `ip:port`。
7. 一致则 `probe_succeeded`；否则 `probe_failed`。
8. 探测结束后，才进入后续 signal 流程。

### 7.4 日志

启动探测结束后，`provider` 与 `accessor` 都要输出统一总结日志，至少包含：

- `probe_result_1=<ip:port>`
- `probe_result_2=<ip:port>`
- `p2p_probe_result=succeeded|failed`

## 8. transport 选择规则

`accessor` 是否选择 `transport=p2p`，只由以下条件共同决定：

- accessor 配置 `nat_type != disabled`
- accessor startup probe 成功
- 服务目录里的 `provider_nat_type != disabled`

否则直接选择 `transport=tcp_relay`。

`create_flow` 使用显式 `transport`，不再保留任何“偏好”字段。

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

## 10. tcp_relay 建立期流程

1. accessor 基于本地条件选择 `transport=tcp_relay`
2. accessor 通过 signal 发送 `create_flow(service_name, tcp_relay)`
3. server 返回 `accepted(flow_id)` 并向 provider 下发 `prepare_flow(flow_id, service_name, tcp_relay)`
4. provider 与 accessor 各自新建一条 relay TCP 数据连接
5. 双方用 `flow_id + runtime uuid` 做握手
6. server 将两条 TCP 连接配对成 `relay_channel`
7. provider 在 `relay_channel` 成立后连接 backend
8. provider 发送 `flow_ready(flow_id)` 或 `flow_failed(flow_id, reason, message)`
9. server 将 terminal result 同步发送给 accessor 后销毁该 `flow_id`
10. `relay_channel` 进入自治运行

说明：

- relay 建立成功日志统一打印：`transport / flow_id / service_name / local / remote`
- `relay_channel` 可保留只读元信息：
  - `flow_id`
  - `provider_uuid`
  - `accessor_uuid`
  - `service_name`
  - `transport`
- 这些元信息只用于日志与诊断，不参与后续控制逻辑

## 11. p2p 建立期流程

1. accessor 基于本地条件选择 `transport=p2p`
2. accessor 通过 signal 发送 `create_flow(service_name, p2p)`
3. server 返回 `accepted(flow_id)` 并向 provider 下发 `prepare_flow(flow_id, service_name, p2p)`
4. provider 与 accessor 各自创建建联 UDP socket
5. 双方执行 `flow_endpoint_probe`
6. server 在双方都 `flow_endpoint_ready` 后，分别下发 `flow_p2p_peer(flow_id, peer_ip, peer_port, peer_nat_type)`
7. 双方执行 `udp_punch`（详见 `udp_p2p_flow.md`）
8. 任意一方收到对端 6 字节探测包即判成功，向 server 发送 `p2p_connected(flow_id, matched_peer_local_port)`
9. server 立即向另一侧中转 `peer_p2p_connected(flow_id, matched_peer_local_port)`
10. 双方停止 `udp_punch`，进入成功分支
11. accessor 立即启动 1 字节 UDP keepalive，双方切入 KCP 数据面
12. provider 在 KCP 数据面可用后连接 backend
13. provider 发送 `flow_ready(flow_id)` 或 `flow_failed(flow_id, reason, message)`
14. server 将 terminal result 同步发送给 accessor 后销毁该 `flow_id`
15. 已建立 `p2p session` 自治运行

## 12. udp echo probe

### 12.1 通用协议

`udp echo probe` 是统一的加密 JSON UDP 回显协议，复用现有 UDP AEAD 封装。

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

- `symmetric` 一侧本地 socket 数：`32`（`xxx` + 31 个随机端口，范围 `[xxx-128, xxx+128]`，合法范围 `[1024, 65535]`）
- `full` 一侧第 0 轮扫描端口数：`64`（`[yyy, yyy+63]` 随机顺序）
- `full` 一侧后续每轮扫描端口数：`128`，从 `[23, 65535]` 随机选取，不重复
- 轮间隔：`1s`，计时器驱动，双方独立
- 最大轮次：`32`

## 14. UDP 小包协议

只定义两种非 KCP 小包：

- `1` 字节 keepalive
- `6` 字节 `udp_punch` 探测包

本地 UDP 收包入口严格按以下顺序分流：

1. 长度 `== 1`：当作 keepalive，静默丢弃
2. 长度 `== 6`：当作 `udp_punch` 包；探测阶段处理，其他阶段静默丢弃
3. 长度 `< KCP 头最小长度` 且不属于上面两类：静默丢弃
4. 其余包：按 KCP 数据包处理

`provider` 与 `accessor` 的这套入口分流规则必须完全一致。

## 15. KCP 数据面

- `transport=p2p` 的数据面运行在建联 UDP socket 上
- KCP 使用非流式模式
- `flow_id` 直接作为 KCP `conv`
- 业务数据仍由上层读事件驱动
- 不新增额外的 P2P 成功业务握手包

## 16. 建立期控制面消息

### 16.1 flow 创建与传输选择

- `create_flow(service_name, transport)`
- `prepare_flow(flow_id, service_name, transport)`
- `create_flow_result`

`create_flow_result` 最小结果集合：

- `accepted`
- `p2p_unavailable`
- `rejected`

### 16.2 endpoint probe

- `flow_endpoint_ready(flow_id, external_ip, external_port)`
- `flow_p2p_peer(flow_id, peer_ip, peer_port, peer_nat_type)`

### 16.3 udp_punch 结果通知

- `p2p_connected(flow_id, matched_peer_local_port)`
- `peer_p2p_connected(flow_id, matched_peer_local_port)`

### 16.4 flow terminal result

- `flow_ready(flow_id)`
- `flow_failed(flow_id, reason, message)`

### 16.5 设计原则

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

- `relay_channel`
- `p2p session`

### 18.2 会话内部组件

- `accepted tcp connection`
- `backend tcp connection`
- `kcp session`
- `keepalive timer`

### 18.3 生命周期规则

- `flow_ready` 之前创建的所有数据面对象都是建立期临时对象
- 若最终进入 `flow_failed`，这些临时对象必须全部销毁
- `flow_ready` 同步发送完成后，相关对象进入会话期自治状态
- 进入会话期后，不再产生任何 `flow_failed`

## 19. 状态机

### 19.1 startup_probe

- `disabled_by_config`
- `probing_udp_port_1`
- `probing_udp_port_2`
- `probe_succeeded`
- `probe_failed`

### 19.2 accessor flow

- `waiting_transport_decision`
- `waiting_flow_endpoint`
- `waiting_punch_result`（仅 P2P）
- `waiting_final_result`
- `established`
- `closed`

### 19.3 provider flow

- `waiting_prepare`
- `waiting_flow_endpoint`
- `waiting_punch_result`（仅 P2P）
- `waiting_backend_connect`
- `established`
- `closed`

## 20. 日志规则

### 20.1 建立成功日志

`provider` 与 `accessor` 都必须输出一次性建立成功日志，统一字段顺序：

- `transport`
- `flow_id`
- `service_name`
- `local`
- `remote`

要求：

- `transport` 正式取值仅有 `p2p` 与 `tcp_relay`
- `service_name` 必须非空
- `local/remote` 一律指当前数据面 socket 自身端点

### 20.2 P2P 特定日志

`p2p` 主线应能观测到：

- startup `udp echo probe` 结果日志
- `udp_punch` 成功日志
- `p2p` 建立成功日志

### 20.3 迟到消息

- server 对过期 `flow_id` 的迟到 signal/UDP 包逐条输出 `WARN`
- 本地 `provider/accessor` 对探测阶段结束后的迟到 6 字节包静默丢弃
- `provider` 对 1 字节 keepalive 静默丢弃，不产生日志

## 21. 失败与回退规则

- `p2p_unavailable` 触发同一条本地 TCP 连接上的新 `tcp_relay flow`
- `p2p -> tcp_relay` fallback 保留在实现中，但不是本轮主线验收项
- fallback 只要求代码路径自洽、日志语义正确、不污染主线状态机
- relay 本身不再设计额外重试

## 22. 原型链路验收

### 22.1 tcp_relay 主线

前提：

- 至少一侧 `nat_type=disabled`

成功判据：

- 明确观测到 `transport=tcp_relay`
- 明确观测到建立成功日志
- 本地 client 经 accessor 访问 backend 成功

### 22.2 p2p 主线

前提：

- 双方 `nat_type != disabled`
- 双方 startup probe 成功

成功判据：

- 明确观测到 startup `udp echo probe` 结果日志
- 明确观测到 `udp_punch` 成功日志
- 明确观测到 `transport=p2p`
- 明确观测到建立成功日志
- 本地 client 经 accessor 访问 backend 成功

### 22.3 验收环境

- 不使用 Docker
- 使用本地固定角色拓扑：
  - `public_server`
  - `provider`
  - `accessor`
  - `backend`
  - `client`
- backend 保持最小化，可使用简单 TCP 回显或简单 HTTP 服务
