# 基础设施改造对子模块的影响分析与迁移规划

> 2026-08-03 · 配套：`docs/refactoring-plan.md`（改造计划）、`docs/asio-async-standards.md`（规范）
> 目的：明确已完成的基础设施改造对 network/rpc/http/frp 各子模块的影响，支撑"一个子模块一个子模块迁移"的规划。

---

## 0. 改造基线（已完成）

| 改动 | 类型 | 破坏性 |
|---|---|---|
| `src/network/async_utils.hpp`（新增） | 纯新增 header-only | 无（无人 include，零影响） |
| `io_context_pool::stop()` 非阻塞 → **阻塞** | **行为变更** | **有（见 §2.1）** |
| `wait_stop()` / `running_in_io_thread()` / `reg_timer` / `reg_object` | 纯新增 API | 无 |
| `next_io_context_` 原子化 | 内部实现 | 无（接口不变） |

---

## 1. 影响总览

| 子模块 | 编译破坏 | 行为变更 | 迁移项（引用规范） | 依赖 |
|---|---|---|---|---|
| network/rudp | 无 | 无 | 状态机（§2）、三层释放（§4）、定时器登记（§9.2） | 无（可最先迁移） |
| rpc（connection/rpc_client/rpc_server/netlink） | 无 | 无 | executor_ 悬垂（P0）、post 化（§3.2）、last_err_ 原子化、定时器登记 | network |
| http | 无 | 无 | 写队列（§3.4/8.3）、response_ 跨线程（§3.2）、Content-Length 上限（§7.1）、超时定时器启动+登记 | network |
| frp（rpc/proxy/frp） | 无 | 无 | 对象登记（§9.3）、通道绑定单一 io_context（§3.3）、async_write_raw 写队列、local_udp_socket_ 悬垂 | rpc + network |
| ws_port_pipe | 无 | 无 | 同 http 类问题 | rpc + network |
| applications（frp_proxy_*、tcp_custom_proxy） | 无 | **退出行为**（依赖 §2.1） | 顶层对象接入 reg_object（§9.3） | 下层全部 |

**结论：没有任何子模块会被编译破坏。** 唯一的即时风险是 `stop()` 阻塞化带来的退出挂死（§2.1），与迁移与否无关——这是第一优先要处理的。

---

## 2. 立即生效的行为变更

### 2.1 `stop()` 阻塞化 —— 最高优先风险

**事实**：`stop()` 唯一调用点是 `init_io_context_pool` 挂在 `Application::exitStarted` 上的信号（io_context_pool.hpp:106）。现在它阻塞等待所有 io_context 排空。

**风险**：exit 时若仍有挂起操作（acceptor 的 `async_accept`、UDP `async_receive_from`、连接的读写循环），`run()` 不返回 → `stop()` 永久阻塞。**当前没有任何模块调用 `reg_object`**，池停时不会主动关闭任何 acceptor——排空完全依赖"对象 release 先于 pool stop"。

**现状评估（好消息）**：
- 顶层对象（`frp_public_server`、`frp_unified_client`）由 `make_guard` 持有，其 `auto_network_storage_instance` 挂在 `exitStarted` 上，release 会关闭 acceptor/读循环
- 事件顺序：`init_io_context_pool` 先连接 pool stop（append），`make_guard` 后连接 release（append）→ **release 后执行 → pool stop 先执行 → 有挂死风险**
- **已确认的修复**：工作区未提交改动里 `Connect([&]() { release(); }, false)` —— eventpp 语义 `append_mode=false` = **prepend（插队到最前）** → release 先于 pool stop 执行 → 顺序正确，挂死解除

**立即动作（M1.0，迁移任何子模块之前）**：
- [ ] 确认 `auto_network_storage_instance` 的 prepend 改动合入（或等价地：顶层对象接入 `reg_object`）
- [ ] 排查其他 exit 路径（显式 `stop()` 调用、SIGTERM 处理）是否满足"先 release 后 pool stop"
- [ ] 用 `frp_proxy_server` 跑一次"启动 → Ctrl+C"验证退出不挂死

### 2.2 `get_io_context()` 原子化 —— 无影响

仅修复了多线程并发轮询的竞争，接口不变，所有调用方（http_server、rpc_client/rpc_server、frp×4、ws_port_pipe、netlink）无需改动。

---

## 3. 子模块迁移项

### 3.1 network/rudp —— 推荐第一个试点

- **现状**：独立模块（只用 asio + 自有 context），四类风险全占（status 非原子、跨线程 destroy、重入 UAF、回调后继续执行）
- **迁移清单**：
  1. `rudp_connection_status` 拆双层：生命周期层 `std::atomic<connection_state>` + `try_close/try_mark_closed`；握手层留在 io 线程内部（§2）
  2. `rudp_handle::destroy()` → 三层模型：入口只投递（§4.2）、关闭序列八步（§4.3）、析构兜底 `post_and_wait`（§4.4）
  3. 三个定时器（update/status/health_check）→ `reg_timer/unreg_timer`（§9.2）
  4. 回调链 `[this, ref=weak]` 哨兵 → 强引用持有（§1.2）
- **验证**：独立单测 + ASAN 压力测试（连→发→断→重连循环）
- **价值**：对象小、无外部依赖，四类风险全中，是验证三层模型和工具函数的最佳试点

### 3.2 rpc（connection / rpc_client / rpc_server / netlink / proxy 公共类）

- **现状**：`executor_` 悬垂引用（P0 崩溃源）；用户线程直操 asio 对象（connect/timeout）；`last_err_` 非原子；io 线程内链式释放
- **迁移清单**：
  1. **P0**：`const any_io_executor& executor_` → 按值成员（connection.h、rpc_client.hpp）——一行改动，全项目最确定崩溃源
  2. `connect()`/`enable_timeout_check()` 用户线程直接调用 → `safe_post`（§3.2）
  3. `last_err_` → 原子化或线程内化（§2.1）
  4. `release_obj` → 三层模型统一（§4）；定时器（deadline_、reconnect_delay_timer_）→ reg_timer
  5. 大包 `FASSERT` → 错误返回（§7.1）
- **注意**：rpc_client 默认用 pool 的 io_context（`s_io_context_cb` 可覆盖）——迁移后行为受 §2.1 退出顺序影响

### 3.3 http

- **现状**：双并发写崩溃（P0）；`response_` 跨线程修改；Content-Length 无界 reserve（远程 terminate）；超时定时器从未首次启动
- **迁移清单**：
  1. **P0**：`serialized_writer` 写队列，消除双并发写（§8.3）
  2. `response_` 所有修改（set_status/set_content_type/add_header/stock_response/set_bytes_range）→ post 到 io 线程（§3.2）
  3. Content-Length 上限校验（§7.1）
  4. 超时定时器：`start()` 时首次启动 + `reg_timer`（§9.2）
  5. `router.get_table` 锁外引用 → 返回副本
- **验证**：文件下载/大 body 场景 + ASAN

### 3.4 frp（rpc/proxy/frp）

- **现状**：跨 io_context 竞争（架构级，M2）；`async_write_raw` 并发写；`local_udp_socket_` 悬垂；信号命令解析默认释放
- **迁移清单**：
  1. **M1**：`frp_signal_session`、`relay_data_channel` 等动态对象接入 `reg_object/unreg_object`（§9.3）——直接关系到 §2.1 的退出安全
  2. **M1**：`async_write_raw` → 写队列（§8.3）
  3. **M2**：中继通道绑定单一 io_context（信号线程与连接线程跨线程读写 ch 成员）；punch engine 与 relay 单一接收者
  4. **M2**：`local_udp_socket_` 裸指针 → 生命周期归属修正
- **注意**：frp 是跨模块聚合点，依赖 rpc + network 的迁移结果，放最后

### 3.5 ws_port_pipe / applications

- ws_port_pipe：同 http 类问题（acceptor + 转发管道），迁移项并入 http 批次
- applications：无代码改动，仅验证（frp_proxy_server/client 已 make_guard 托管）；`tcp_custom_proxy_server` 的栈对象 proxy 依赖 main 存活期，保持现状

---

## 4. 迁移顺序（每步独立可验证）

```
M1.0  退出顺序安全（§2.1 三项动作）           ← 先做，防挂死
M1.1  network/rudp 试点（三层模型全落地）      ← 验证方法论
M1.2  rpc 止血（executor_ + post 化 + 定时器登记）
M1.3  http（写队列 + 长度上限 + 超时定时器）
M1.4  frp 接入（reg_object + 写队列）
M2    frp/ws_port_pipe 架构级（线程亲和性、统一关闭序列）
M3    ASAN + soak test 全绿，规范 §10 清单复查
```

每一步验收标准：编译通过 + ASAN 下对应模块压力场景无崩溃 + 退出不挂死。

---

## 5. 风险清单

| # | 风险 | 等级 | 缓解 |
|---|---|---|---|
| 1 | 阻塞 stop() 挂死（未迁移就存在） | **高** | M1.0 退出顺序修复（已在 WIP 中，需合入验证） |
| 2 | 定时器未登记 → io 排空不完整 → stop 超时 | 中 | 各模块迁移时逐步接入 reg_timer（§9.2） |
| 3 | 动态对象未 reg_object → 池停不释放 | 中 | frp/ws_port_pipe 迁移时接入（§9.3） |
| 4 | `post_and_wait` 死锁（持锁/回调反向等待） | 中 | §4.5 三条 MUST，评审清单把关 |
| 5 | 迁移中途双轨运行（部分模块用 reference_、部分用状态机） | 低 | 规范明确"新代码用状态机，过渡期保留 reference_"，不做批量替换 |
