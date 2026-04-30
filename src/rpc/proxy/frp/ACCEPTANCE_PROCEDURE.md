# FRP 本地原理原型验收

## 1. 目标

当前验收只做本地模拟，不使用 Docker。  
本轮只验证两条主线：

- `tcp_relay`
- `p2p`

每条主线都必须同时证明两件事：

- 建链结果正确
- 业务响应正确

## 2. 本地模拟拓扑

本地最小拓扑固定为 5 个角色进程：

- `public_server`
- `provider`
- `accessor`
- `backend`
- `client`

说明：

- 端口和地址属于部署参数，必须通过配置提供，不写死为协议常量
- backend 保持最小化，可使用简单 TCP 回显服务或简单 HTTP 服务

## 3. 验收入口

保留以下本地脚本入口：

- `verify-relay-local.sh`
- `verify-p2p-local.sh`
- `run_prototype_validation.sh`

不再使用 Docker 验收入口。

## 4. tcp_relay 主线

### 前提

- 至少一侧 `nat_type=disabled`

### 通过标准

必须同时满足：

1. 建链结果正确
   - 实际 transport 为 `tcp_relay`
   - 出现统一格式的建立成功日志：
     - `transport`
     - `flow_id`
     - `service_name`
     - `local`
     - `remote`
2. 业务响应正确
   - `client -> accessor -> provider -> backend` 请求成功
   - backend 响应内容正确

### 运行

```bash
bash src/rpc/proxy/frp/verify-relay-local.sh
```

## 5. p2p 主线

### 前提

- `provider.nat_type != disabled`
- `accessor.nat_type != disabled`
- 双方 startup probe 成功

### 通过标准

必须同时满足：

1. 建链结果正确
   - 出现 startup `udp echo probe` 结果日志
   - 出现 `udp_punch` 成功日志
   - 实际 transport 为 `p2p`
   - 出现统一格式的建立成功日志：
     - `transport`
     - `flow_id`
     - `service_name`
     - `local`
     - `remote`
2. 业务响应正确
   - `client -> accessor -> provider -> backend` 请求成功
   - backend 响应内容正确

### 运行

```bash
bash src/rpc/proxy/frp/verify-p2p-local.sh
```

## 6. 一键验收

```bash
bash src/rpc/proxy/frp/run_prototype_validation.sh
```

通过标准：

- relay 主线通过
- p2p 主线通过
- 脚本退出码为 `0`

## 7. 非主线项

`P2P -> tcp_relay` fallback 仍保留在实现模型里，但本轮不是主线验收项。

它在本轮只要求：

- 代码路径自洽
- 日志语义正确
- 不污染两条主线状态机
