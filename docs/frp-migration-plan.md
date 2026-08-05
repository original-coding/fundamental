# FRP 模块改造实施计划（修订版 v2）

> 2026-08-03 · v2：基于 `docs/asio-async-standards.md`（规范 §0–§10）对 FRP 全量源码审计后的修订。
> 审计范围：frp_client / frp_signal_client / frp_accessor / frp_provider / frp_punch_engine / frp_server / kcp_channel，所有发现均落到 `file:line`。
> v1 执行情况：commit `5fca48b` 完成了「probe socket/timer 成员化」「release_obj 投递化」「relay close 释放 punch_engine」三项；**其余条目全部未动**（1.1/1.2 仍在，见 §1）。
> 相关文档：`docs/asio-async-standards.md`（规范）、`docs/refactoring-plan.md`（总体改造计划）、`docs/migration-impact-analysis.md`（影响分析）

## 实施状态（2026-08-04 全部完成）

| 阶段 | 内容 | 提交 | 验收 |
|---|---|---|---|
| 阶段 A | 单 executor 收敛（3.0）+ 止血（1.1/1.2/1.3/1.4/1.5/1.6/1.7）+ relay 三层化 | `5461d3e` | 4 脚本通过，退出段错误消除 |
| 阶段 B | 写串行化（2.1/2.2/2.3）+ 传输错误收敛（§7.2） | `4394078` | 4 脚本通过 |
| 阶段 C | 服务端 3.1/3.7 + server/session reg_object/reg_timer + P2-19 | `e65364f` | 4 脚本通过 |
| 阶段 D | 3.3 共享所有权 + 3.8 错误收敛 + P3-22/23/24 死代码清理 | `805dc83` | 4 脚本通过 |
| 最终验收 | release + ASAN debug 两套构建 | — | 4/4 脚本全绿，ASAN 无内存错误、无泄漏 |

- 因 3.0 单线程收敛，3.4（relay 布尔）/3.6（nat 成员）/P3-25（双模块分发）/P1-17（弱哨兵）随单线程模型自然消除；P2-18（services_by_key_）经 post 建立 happens-before 后无实际竞争，未加锁（文档留痕）。
- 遗留观察项：P0-26 服务端 unknown 命令 → release_obj 的重连风暴嫌疑（有日志可查，未改协议行为）。

## 执行环境

```bash
# 构建
./test-gen-linux.sh && cd build-linux && make -j$(nproc)
# ASAN 调试构建（验证崩溃用）
./test-gen-linux-debug.sh && cd build-linux-debug && make -j$(nproc)
# 运行（frp 联调）
./build-linux/applications/frp_proxy_server/frp_proxy_server -c <server.json>
./build-linux/applications/frp_proxy_client/frp_proxy_client -c <client.json>
```

**每完成一项：commit 一次，保留可回滚点。**

---

## 0. 审计结论速览

按规范 §10 清单扫描后的**完整发现清单**（★ = v1 计划未覆盖的新发现）：

| 级别 | # | 位置 | 问题 | 规范条款 |
|---|---|---|---|---|
| P0 | 1 | `frp_signal_client.cpp:191,267` | `probe_state.executor` 默认构造 = **null executor**，域名 host 时 resolver 崩溃（1.1 未修） | §1.1 |
| P0 | 2 | `frp_signal_client.cpp:122-123` | register_services_resp 失败分支缺 `read_next_command` → 信号读链停摆（1.2 未修） | §7.2 |
| P0 | 3★ | `frp_signal_client.cpp:36-38 vs 60-62` | **信号客户端自身双 executor**：定时器绑 context A（构造时 round-robin），signal channel 绑 context B（connect_signal_channel 时再取）；B 线程回调直接操作 A 上的 probe_socket_/probe_timer_/reconnect_timer_ | §3.1/3.3 |
| P0 | 4★ | `frp_accessor.cpp:1042-1044`、`frp_provider.cpp:1017-1019` | `close()` 的 range-for 迭代 `channels_` 时，`ch->close()` → `on_release_` → **迭代中 erase 同一 map** → 迭代器失效 UB | §4.3 |
| P0 | 5★ | `frp_client.cpp:344-347` 起 | **中继通道 4 个 executor 交叉**：relay 绑 A（signal_->get_executor()），`tcp_` 绑 C（accessor/provider 另取 get_io_context()），`backend_udp_socket_` 绑 D，而全部驱动代码跑在信号线程 B（on_client_command/on_subscribe）。同一 socket 的 async op 从错误线程发起 | §3.1/3.3 |
| P0 | 6★ | `frp_client.cpp:377-404`、`frp_accessor.cpp:329,501`、`frp_provider.cpp:251,269` | **ikcp 状态机被多线程并发访问**（feed_encrypted/send_plaintext 来自 B/C/D 线程，tick 定时器在 A）——ikcp 非线程安全，这是最活跃的崩溃源 | §3.1 |
| P0 | 7★ | `frp_client.cpp:201-212`、`386-404` | **同 socket 并发写**：`async_write_raw` 无队列直接 async_write；kcp `on_frame` 对 backend/local socket 直写（多帧并发）。原计划只覆盖了前者，后者遗漏 | §3.4 |
| P0 | 8 | `frp_server.cpp:682,728-729,733-759` | register_data_channel 在 src 线程直接操作 dst_session（enable_timeout/upgrade/start_data_forward_read_loop）→ 跨线程定时器 + 跨线程 async_read_some（3.1 未修） | §3.1 |
| P0 | 9★ | `frp_punch_engine.cpp:369-397 vs 708-747` | **双接收者**：`start_punch_at` 启动 punch 读循环时，endpoint echo 循环的 pending receive 未取消（同 socket 双 async_receive_from） | §3.5 |
| P0 | 10★ | `frp_server.cpp:256-264` | `frp_signal_session::release_obj` 在任意线程直接 `timeout_timer_.cancel()`（remove_session 可能在 acceptor 线程调它）→ 跨线程定时器操作；缺三层模型 | §4.1 |
| P0 | 11★ | `frp_provider.cpp:249,267` | backend 读循环错误路径**无限重挂自旋**：socket 出错（非 close）→ 重挂 → 再错 → 不收敛到关闭序列（半死连接 + CPU 自旋） | §7.2/7.3 |
| P0 | 12★ | `frp_client.cpp:358-372`、`349-352` | relay_data_channel 无三层模型：`close()` 直接执行全部清理（任意线程可调）、`closed_` 普通 bool、析构函数同步关 kcp/p2p socket（违反析构兜底约定） | §4.1-4.4 |
| P1 | 13 | 全部对象 | **reg_object / reg_timer 未接入任何对象**（1.3/1.4 未做）——退出时定时器存活、动态对象无兜底释放 | §9.2/9.3 |
| P1 | 14★ | `frp_client.cpp:51-54` | `frp_tcp_channel::release_obj` **无幂等守卫**：`reference_.release()` 返回值未检查，重复调用重复 post | §4.1 |
| P1 | 15 | `frp_accessor.cpp:479` + `reconcile_listeners:222` | `local_udp_socket_` 裸指针悬垂（3.3 未修）：listener 关闭并 reset udp_socket 后，kcp on_output 解引用悬垂指针 | §5.3 |
| P1 | 16 | `frp_client.hpp:229-234`、`frp_signal_client.hpp:83-85` | 普通 bool / nat 成员跨线程裸读写（3.4/3.6 未修） | §2.1 |
| P1 | 17★ | `frp_client.cpp:299-309`、`frp_accessor.cpp:290-304` | 「weak token 入口哨兵 + 全程裸 this」模式（signal Connect token 过期检查后仍用 this） | §1.3 |
| P2 | 18★ | `frp_provider.hpp:45` | `services_by_key_` 主线程 set / io 线程读，无保护（当前时序无并发，标记待加固） | §2.1 |
| P2 | 19★ | `frp_server.cpp:833-834` | session `uuid_/nat_type_/groups` 在锁外写（session 线程），`list_services_for_subscriber` 锁内读 → 竞争 | §2.1 |
| P2 | 20★ | `frp_client.cpp:118-123` | `frp_signal_channel::send_command` 任意线程读 `tcp_`（io 线程写）且不查 reference_ | §3.2 |
| P3 | 21★ | `frp_signal_client.cpp:373-395` | time_sync 完成后 `probe_socket_` 只 close 未 reset（保持到 release_obj） | 清理一致性 |
| P3 | 22★ | `frp_client.hpp:126` | `switch_to_raw_read` 声明未定义（死声明，一旦调用即链接错误） | 死代码 |
| P3 | 23★ | `frp_provider.cpp:280` | `handle_backend_write_queue` 无任何调用者（写队列基建存在但未接线，实际写路径是 kcp on_frame 直写） | 死代码 |
| P3 | 24★ | `frp_signal_client.cpp:315-316,325` | sync_state 的 `executor`/`socket`/`send_timer` 死字段（已被成员 probe_socket_/probe_timer_ 取代） | 死代码 |
| P3 | 25★ | `frp_client.cpp:524-528` | P2P 命令 default 分支同时投递给 accessor+provider（靠 channels_ 归属恰好正确，脆弱） | 架构 |
| 待验证 | 26 | `frp_server.cpp:472-475` | 服务端 unknown 命令 → release_obj → 疑似触发客户端重连风暴 | 协议 |

**v1 已完成项（无需再做）**：probe socket/timer 成员化（1.1 的一半）、release_obj 投递化（frp_unified_client/frp_signal_client）、relay close 释放 punch_engine 断环。

---

## 1. 阶段 1：止血（小改动，每项独立验证）

### 1.1 probe_state.executor null executor（**未修，改法更新**）

- **位置**：`frp_signal_client.cpp:178-193`（`probe_state` 的 `executor` 字段）、`:267`（`resolve_udp_endpoint(state->executor, ...)`）
- **现状**：v1 commit 已把 socket/timer 成员化，但 **`state->executor` 字段和 :267 调用残留**——字段默认构造 = null executor，host 为域名时 `udp::resolver` 用 null executor → 崩溃（IP 走 make_address 绕过，时好时坏）
- **改法**：删掉 `probe_state::executor` 字段，`:267` 改用局部变量 `executor`（`run_nat_probe` 里已有，见 :168）；顺带删除 `sync_state` 的死字段（:315-316, 325）
- **验证**：配置 host 用域名跑 frp_proxy_client，nat probe 不崩

### 1.2 register_services_resp 失败路径信号通道停摆（**未修**）

- **位置**：`frp_signal_client.cpp:120-127`
- **现状**：`from_json` 失败（:122）和 `!resp.ok`（:123）两个分支 `return` 前**没有 `signal_->read_next_command()`**——读链停止，注册失败后客户端失联
- **改法**：所有出口统一 `read_next_command()`（对照 :125 成功分支写法）
- **验证**：错误 register_key 启动 client，失败后仍能收到后续响应

### 1.3 reg_object 接入（1.3 未做，模板见 v1）

| 对象 | 注册点 | 注销点 |
|---|---|---|
| `frp_public_server` | `start()`（frp_server.cpp:21） | `release_obj()` 开头 |
| `frp_signal_session` | `start()`（frp_server.cpp:245） | `release_obj()` 开头 |
| `relay_data_channel` | 构造函数 | `close()` 开头 |
| `frp_unified_client`（可选，make_guard 已管） | `start()` | `release_obj()` 开头 |

```cpp
void frp_public_server::start() {
    io_context_pool::Instance().reg_object(this,
        [self = shared_from_this()] { self->release_obj(); });
    ...
}
```

### 1.4 reg_timer 接入（1.4 未做）

| 对象 | 定时器 |
|---|---|
| `frp_signal_client` | `reconnect_timer_`、`poll_timer_`、`probe_timer_` |
| `frp_signal_session` | `timeout_timer_` |
| `relay_data_channel` | `idle_timer_`、`handshake_timer_` |
| `kcp_channel` | `timer_` |
| `frp_punch_engine` | `endpoint_probe_timer_`、`deadline_timer_`、`punch_timer_` |

- **风险**：析构必须 `unreg_timer`，否则池停止时 cancel 已销毁定时器 → UB

### 1.5 frp_tcp_channel::release_obj 幂等（★新增）

- **位置**：`frp_client.cpp:51-54`
- **问题**：`reference_.release()` 返回值未检查，重复调用重复 post
- **改法**：`if (!reference_.release()) return;`

### 1.6 channels_ 迭代中删除（★新增）

- **位置**：`frp_accessor.cpp:1042-1044`、`frp_provider.cpp:1017-1019`
- **问题**：`for (auto& [_, ch] : channels_) close_data_channel(ch);` 中 `ch->close()` → `on_release_` → `channels_.erase(cid)` → **迭代中删除，迭代器失效 UB**
- **改法**：先搬出再关闭（同 `fail_pending` 思路）：
  ```cpp
  auto copy = std::move(channels_);
  channels_.clear();
  for (auto& [_, ch] : copy) close_data_channel(ch);
  ```

### 1.7 probe_socket_ 收敛（★新增，小）

- `frp_signal_client.cpp:373-375`：time_sync 结束分支 `close(ignore)` 后补 `probe_socket_.reset()`（与 :235/:261 的写法一致）

### 阶段 1 验收

- [ ] ASAN debug 构建通过
- [ ] frp_proxy_server + client 联调正常（注册/订阅/转发）
- [ ] Ctrl+C 优雅退出不挂死
- [ ] host 配域名跑通 nat probe / time sync

---

## 2. 阶段 2：写串行化（同一 socket 至多一个在途写）

### 2.1 frp_tcp_channel::async_write_raw 并发写

- **位置**：`frp_client.cpp:201-212`
- **问题**：每次直接 `asio::async_write`，无队列；KCP tick on_output 与收发路径并发调用 → 同 socket 并发写
- **改法（最小改动）**：并入现有写队列——`async_write_raw` 改走 `write_queue_`（post 后 push，size==1 时 do_write），删除直写分支。signal channel 用 framed、data channel 用 raw，**同一通道不会混用两种模式**，共用队列安全
- **验证**：ASAN 下 KCP 开启 + 大流量中继，无并发写崩溃

### 2.2 kcp on_frame 的 backend/local 写串行化（★新增，v1 遗漏）

- **位置**：`frp_client.cpp:386-404`（`init_kcp` 的 `on_frame` 对 `backend_socket_`/`backend_udp_socket_`/`local_socket_`/`local_udp_socket_` 直接 `async_write`/`async_send_to`）
- **问题**：recv_loop 可连续吐出多帧 → 同 socket 并发写
- **改法（二选一）**：
  - A：接入已存在的 `backend_pending_writes_` + `handle_backend_write_queue`（`frp_provider.cpp:280-326`，目前**无调用者**，把 `writing_` 标志 + 队列驱动接上）
  - B：直接用 `network::serialized_writer`（规范 §8.3）包 backend/local 写路径
- **注意**：on_frame 执行线程取决于调用方（见 P0-5），写队列接入后仍需配合阶段 3 的线程绑定
- **验证**：KCP 开启大流量中继 + 收发自旋，ASAN 无并发写报告

### 2.3 punch engine 双接收者（★新增）

- **位置**：`frp_punch_engine.cpp`，`start_punch_at`（:443-459）启动 `start_punch_read_loop`（:708）时，`start_endpoint_echo_loop`（:369）的 pending receive 未取消
- **改法**：`start_punch_at` 里先 `p2p_socket_->cancel()` 取消 echo 接收（echo handler 收到 operation_aborted 后 `!probing_` → 不再重挂，:375 已有该守卫），再启动 punch 读循环
- **验证**：P2P 打通 + 断连重连，ASAN 无并发接收

### 阶段 2 验收

- [ ] ASAN 下中继大流量（KCP 开）无并发写/读崩溃
- [ ] P2P 打通后数据持续传输

---

## 3. 阶段 3：线程亲和性（架构级，最大的工程）

> **前置决策（★P0-3 的新结论）**：v1 假设"信号线程与轮询 io_context 线程"两方交叉；审计确认实际是 **B（信号线程）驱动 A/C/D 三个 executor 上的对象**。因此所有阶段 3 项必须先做「信号客户端单 executor 收敛」（3.0），否则中继通道绑单 context 后仍会被信号线程跨线程驱动。

### 3.0 信号客户端单 executor 收敛（★新增，所有项的前提）

- **位置**：`frp_signal_client.cpp:36-38`（reconnect/poll/probe 定时器绑构造时取到的 context A）、`:60-62`（signal channel 绑新取的 context B）
- **问题**：`get_executor()` 返回 A，但全部命令处理（process_server_command/process_client_command → accessor/provider 回调）跑在 B；`schedule_reconnect`（:79-85）、`run_nat_probe`/`run_time_sync`（:165-417）操作 A 上的定时器/socket
- **改法**：`frp_signal_client` 构造时**固定一个 executor**（如 `io_context_pool::Instance().get_io_context()` 存成员 `executor_`），signal channel 创建复用同一 executor；所有定时器用 `executor_` 构造；`get_executor()` 返回 `executor_`
- **验证**：`running_in_io_thread()` 断言接入 `process_command` 入口，ASAN 无竞争报告

### 3.1 服务端 register_data_channel 跨线程操作 dst_session

- **位置**：`frp_server.cpp:682`（`dst_session->send_raw`，post 化已 OK）、`:728-729`（`dst_session->enable_timeout`）、`:733-756`（`dst_session->upgrade` 写 forward_cb_/release_cb_）、`:758`（`dst_session->start_data_forward_read_loop` → 跨线程 async_read_some）
- **改法**：对 dst_session 的操作整体 post 到其 executor：
  ```cpp
  network::post_keepalive(dst_ex, dst_session, [src_weak, ...](const auto& dst) {
      dst->enable_timeout(...);
      dst->upgrade(...);
      dst->start_data_forward_read_loop();
  });
  ```
- **验证**：ASAN 下并发多路中继建立/断开

### 3.2 中继通道绑定单一 io_context

- **位置**：`frp_accessor.cpp:282-284,317,356,366-378,418,471`、`frp_provider.cpp:105,174,200,381-383,434-446`
- **问题**（P0-5/6 的具体化）：relay 的 `tcp_`（C）、`backend_udp_socket_`（D）与 relay 自身 executor（A）不同；且全部驱动代码（读循环、kcp 调用、定时器操作）来自信号线程 B
- **改法**：
  1. 通道的**所有成员资源**（tcp_、backend_udp_socket_、backend_socket_、定时器、kcp）统一用 relay 的 `executor_` 构造——`frp_provider.cpp:156-180` 的 resolver/udp socket 改用 `signal_->get_executor()`
  2. accessor/provider 的所有 io 操作**在信号线程入口统一 post 到目标 executor**：on_client_command/on_subscribe 收到命令 → 解析 → 按 conn 找通道 → `post_keepalive(ch_exec, ch, ...)` 内执行读循环/close/kcp 调用
  3. 内部读循环（start_local_read_loop / start_data_forward_read_loop / start_backend_read_loop）**不再自行重挂**，由 post 进去的驱动函数负责——避免回调链散落在多线程
- **验证**：ASAN 下 P2P + 中继混合场景长时间运行

### 3.3 local_udp_socket_ 悬垂

- **位置**：`frp_accessor.cpp:479`（`ch->local_udp_socket() = lst->udp_socket.get()`）vs `reconcile_listeners`（:222 关闭 reset）
- **改法**：通道改持 `shared_ptr<udp::socket>`（listener 与通道共享所有权）；`reconcile_listeners` 先通知相关通道再释放
- **验证**：UDP 服务 + 配置热更新（增删 listener），ASAN 无 UAF

### 3.4 relay 布尔成员跨线程

- **位置**：`frp_client.hpp:229-234`（`closed_/writing_/p2p_success_` 等）
- **改法**：配合 3.2 线程绑定后自然消失；过渡期改 `std::atomic<bool>`；`is_closed()`/`is_p2p_active()` 调用点逐一核对

### 3.5 punch engine 与 relay 并发接收

- **现状确认**：`on_punch_success` 已 `p2p_socket_->cancel()`（frp_punch_engine.cpp:348-351）→ `accept_p2p` 后再开新读循环——**主路径已安全**；剩余缺口是 2.3（echo loop 双接收）
- **补充**：`relay_data_channel::accept_p2p`（frp_client.cpp:409-418）跨线程写 `tcp_` 成员（A 线程写 C 线程对象）——随 3.2 解决

### 3.6 probed_nat_type_/startup_rtt_ms_/server_clock_offset_us_ 跨线程

- **位置**：`frp_signal_client.hpp:83-85`
- **改法**：随 3.0 单 executor 收敛后这些字段只在 A 线程写、B 线程读——改为 `std::atomic`（最小改动）或读取处 post 取用

### 3.7 frp_signal_session 三层模型化（★新增）

- **位置**：`frp_server.cpp:256-264`
- **问题**（P0-10）：release_obj 在任意线程直接 `timeout_timer_.cancel()`；`release_cb_()` 在调用者线程执行（其内部 remove_session 再触发对端 release）
- **改法**：release_obj 只投递（CAS 守卫后 `post(executor_, ...)` 内做 cancel + remove_session + release_cb_ + close_socket），对齐规范 §4.2/4.3；`upgrade` 的 forward/release 回调写出改到 dst 线程（随 3.1）
- **验证**：断连/重连风暴场景 + Ctrl+C，ASAN 无报告

### 3.8 backend 读循环错误收敛（★新增）

- **位置**：`frp_provider.cpp:249,267`
- **问题**（P0-11）：错误路径无限重挂自旋
- **改法**：错误分两类——`operation_aborted`/closed → 停止（配合 3.2 的驱动化）；真实错误 → `close_data_channel(ch)` 收敛到关闭序列
- **验证**：后端服务中途断开，通道应自动关闭而非自旋

### 阶段 3 验收

- [ ] ASAN 长时间运行（中继 + P2P + 断连重连 + 配置热更新）无任何竞争/UAF 报告
- [ ] Ctrl+C 退出干净
- [ ] `running_in_io_thread()` 断言在关键入口全开、零触发

---

## 4. 阶段 4：加固与清理（P2/P3）

- [ ] P2-18：`services_by_key_` 加锁或改为不可变快照（set 后不再改）
- [ ] P2-19：session `uuid_/nat_type_/groups` 写入纳入 mutex_（frp_server.cpp:833-834）
- [ ] P2-20：`send_command` 检查 reference_ 并在 io 线程读 `tcp_`（frp_client.cpp:118-123）
- [ ] P3-21：见 1.7
- [ ] P3-22：删除 `switch_to_raw_read` 死声明（frp_client.hpp:126）
- [ ] P3-23：`handle_backend_write_queue` 接线（2.2 改法 A）或删除
- [ ] P3-24：sync_state 死字段清理
- [ ] P3-25：P2P 命令按连接归属分发（先查 accessor.channels() 再查 provider.channels()，命中者处理）
- [ ] 待验证-26：服务端 unknown 命令的 release_obj 行为——加日志验证是否触发重连风暴；若是，改为忽略 + 计数熔断

---

## 5. 全局验收清单（所有阶段完成后）

- [ ] `test-gen-linux.sh` release 构建 + 全量目标通过
- [ ] `test-gen-linux-debug.sh`（ASAN）下 frp 全链路压测 30 分钟+ 无崩溃
- [ ] 规范 `docs/asio-async-standards.md` §10 清单对 FRP 模块逐条自检通过
- [ ] 文档同步：本计划、refactoring-plan、migration-impact-analysis 的 FRP 项勾选完成

## 执行顺序提醒

- **1.1/1.2/1.5/1.6 最先做**（确定性问题，互不依赖）
- **3.0 必须先于 3.2 完成**（3.2 的"单一 context"依赖信号客户端先收敛）
- 2.x 可并行于 3.x（写队列是独立串行化，与线程绑定正交；但 2.2 与 3.2 有交互，注意验证）
- 1.3/1.4（reg_object/reg_timer）与退出安全互相依赖，尽早做
