# low_ttl_probe 补充说明

本文档是 [frp_upgrade_design.md](frp_upgrade_design.md) 的补充说明，只解释 `p2p` 建立期里的 `low_ttl_probe` 原理与最小协议，不重复主文档中的完整状态机与角色职责。

## 1. 定位

`low_ttl_probe` 是 `p2p` 建立期的第二阶段。

在它开始前，双方已经完成：

1. `create_flow(transport=p2p)`
2. `prepare_flow(p2p)`
3. 建联 UDP socket 创建
4. flow 级 `udp echo probe`
5. `flow_endpoint_ready`
6. `flow_p2p_peer`

因此 `low_ttl_probe` 的唯一目标只有一个：

- 让双方建联 UDP socket 完成直连探测，并决定是否进入 `p2p` 成功分支

## 2. 成功与失败

唯一成功条件：

- 任意一方建联 UDP socket 收到对端发来的 5 字节探测包

唯一失败条件：

- 全部既定 TTL 轮次完成后，双方仍都没有收到对端 5 字节探测包

失败结果映射为：

- `low_ttl_timeout`

## 3. 5 字节探测包格式

`low_ttl_probe` 使用固定 5 字节明文包：

- 前 `4` 字节：小端 `flow_id`
- 后 `1` 字节：实际 `ttl_value`

示意：

```text
+----------------------+-----------+
| 4 bytes flow_id LE   | 1 byte TTL|
+----------------------+-----------+
```

这里直接放实际 TTL 值，而不是轮次编号。当前 TTL 序列固定为：

- `2`
- `3`
- `4`
- `5`
- `6`
- `7`
- `128`

每个 TTL 固定发送 `5` 次。

## 4. 轮次驱动

`low_ttl_probe` 的轮次推进由 `accessor` 驱动。

分工固定如下：

- `accessor`
  - 收到 `flow_p2p_peer` 后开始第一轮
  - 在收到 provider 的本轮完成回报后，立即请求下一轮
- `provider`
  - 被动执行当前轮次发送
  - 当前轮次发送完成后，经 signal 中转 `round_done(flow_id, ttl_value)`
- `public_server`
  - 只中转控制消息
  - 不驱动轮次
  - 不产生成功结论

## 5. 建议时序

```text
accessor                  public_server                  provider
    |                           |                           |
    |<------ flow_p2p_peer -----|----- flow_p2p_peer ----->|
    |                           |                           |
    |-- send TTL=2 x5 --------->|                           |
    |                           |<-------- send TTL=2 x5 ---|
    |                           |<------ round_done(2) -----|
    |<------ round_done(2) -----|                           |
    |----- next_round(3) ------>|----- next_round(3) ------>|
    |                           |                           |
    | ... 重复直到成功或轮次耗尽 ...                          |
```

任意一侧一旦先收到对端的 5 字节探测包：

1. 本地立即判定 `low_ttl_probe` 成功
2. 本地发送 `p2p_connected(flow_id)` 给 `public_server`
3. `public_server` 立即向另一侧中转 `peer_p2p_connected(flow_id)`
4. 双方停止 low-TTL 发送，进入成功分支

## 6. 成功分支本地动作

成功分支的最小动作顺序固定为：

1. 停止 low-TTL 发送定时器
2. 停止等待下一轮控制消息
3. `accessor` 启动 1 字节 UDP keepalive
4. 切入 KCP 数据面
5. `provider` 继续 backend connect
6. `accessor` 继续等待 `flow_ready`

注意：

- 不要求另一侧也必须先本地观察到 5 字节探测包
- 收到 `peer_p2p_connected(flow_id)` 后，必须立即进入成功分支

## 7. 与 UDP 小包协议的关系

`low_ttl_probe` 5 字节包属于“UDP 小包协议”的一种。

建立期与会话期的本地 UDP 收包分流顺序固定为：

1. 长度 `== 1`：keepalive，静默丢弃
2. 长度 `== 5`：`low_ttl_probe` 包
3. 长度 `< KCP 头最小长度` 且不属于前两类：静默丢弃
4. 其余包：进入 KCP 数据面

探测阶段结束后：

- 本地再收到迟到的 5 字节探测包，静默丢弃
- `public_server` 若收到过期 `flow_id` 的迟到 UDP 探测包，输出 `WARN`

## 8. 为什么使用 low TTL

当前原型不是把“收到候选地址后直接发正常 KCP 包”作为第一步，而是先做 `low_ttl_probe`，原因是：

- 先用极小、明文、明确语义的探测包完成直连判定
- 不让 low-TTL 探测与 KCP 数据面混在一起
- 让“收到 5 字节探测包即成功”的成功条件保持单一、可观测、可日志化

这份原型文档不继续展开不同 NAT 类型下的产品化推导。当前阶段只关注：

- 协议路径最小化
- 状态机边界清晰
- 本地原理原型可验证
