# fh-fundamental 网络层整体改造计划

> 状态：草案（2026-08-03 初版，基于全模块崩溃风险诊断）
> 目标：消除网络层频繁崩溃，以 asio2（zhllxt/asio2）的成熟模式为参照
> 参照物：asio2 的四状态机 + CAS 迁移、this_ptr 全程持有、事件队列串行化、组件化

---

## 0. 诊断方法论（可复用的检查清单）

诊断任何异步网络代码，按以下 6 个方面扫描，每条发现都要落到 `file:line`：

1. **对象生命周期**：回调捕获裸 this？weak_ptr 哨兵只保护"进入时刻"？用户回调内部销毁对象后外层继续执行（重入 UAF）？循环引用？
2. **状态管理**：状态变量是否原子？谁在哪个线程读写？迁移有没有守卫（CAS）？
3. **线程模型**：单 io_context 线程还是多线程？用户线程是否直接调用内部状态修改函数（不经 post/dispatch）？同一 socket/对象的操作是否可能并发？
4. **清理路径**：destroy/stop 的完整序列；成员定时器回调捕获什么；pending 操作是否取消；清理路径里是否同步调用用户回调。
5. **缓冲所有权**：传给异步操作的 buffer 归谁管、何时释放、写回调何时才能 pop。
6. **其他**：空指针、double free、重复回调、异常路径（bad_alloc 等抛入 io 线程）、未检查错误码。

---

## 1. 诊断结论汇总

### 1.1 全局 TOP 崩溃点（按严重度排序）

| # | 位置 | 问题 | 类型 |
|---|------|------|------|
| 1 | `src/rpc/connection.h:172,466`、`rpc_client.hpp:328,1442` | `const any_io_executor& executor_` 绑定临时对象，**构造结束即悬垂**；每次 `asio::post(executor_, ...)` 都是 heap-use-after-free | 生命周期（确定崩溃） |
| 2 | `src/rpc/rpc_client.hpp:353-358,394-400` vs `938-989` | 用户线程直接 `async_resolve`/`deadline_.expires_after`，与 io 线程 `async_reconnect`/定时器重设并发操作同一 asio 对象 | 线程模型（触发即崩） |
| 3 | `src/http/http_response.cpp:408,442-449` | 双并发写同一 socket：守卫只挡 size>1；两个完成回调并发 pop 空 deque → UB | 状态/清理 |
| 4 | `src/http/http_server.cpp:64,69,83` vs `http_response.cpp:153-159` | 用户/线程池线程直接改 `response_`（headers_ 等），与 io 线程写回调并发 → 写悬垂内存 | 线程模型 |
| 5 | `src/network/rudp/asio_rudp.cpp` 四处 | ① `status` 非原子跨线程读写 ② `rudp_handle::destroy()` 用户线程直接执行 ③ 回调捕获裸 this + weak 哨兵 ④ `set_status(CONNECTED)` 先调 `connected_cb` 再继续执行（重入 UAF） | 全部四类 |
| 6 | applications（frp）多处 | 中继通道跨 io_context 竞争（`frp_accessor.cpp:298 vs 335` 等）；同一 socket 并发 `async_write_raw`/并发 receive（`frp_client.cpp:201-212`、`frp_punch_engine.cpp:372-400`） | 线程模型 |
| 7 | `src/http/http_request.hpp:121` | Content-Length 无界 `reserve` → bad_alloc 抛入 io 线程 → FASSERT/terminate，**远程可触发** | 其他（安全） |

### 1.2 按模块明细

**基础层 `src/network/io_context_pool`**
- `next_io_context_` 非原子轮询（io_context_pool.cpp:94-96），多线程并发调用有竞争
- io 线程内任何异常 → FASSERT 终止进程（io_context_pool.cpp:50-54），bad_alloc 等远程可触发
- `stop()` 只清 work guard，不等待线程退出、不等待 pending 操作完成
- 架构缺陷：round-robin 把连接分散到多个 io_context，各协议又在"信号线程"与"轮询 io_context 线程"之间跨线程读写连接成员——**这是 frp 大量数据竞争的根源**。asio2 的做法：每个连接终身绑定一个 io_context（线程亲和性），跨线程操作一律 post。

**RUDP（`src/network/rudp/asio_rudp.cpp`）**
- `status` 非原子（用户线程读 `operator bool`/`is_closed`，执行器线程写 `set_status`）
- 所有回调 `[this, ref = weak_from_this()]` 哨兵只保护进入时刻；`health_check`/`flush_data` 等链式回调中途可能被用户回调（closed_cb/connected_cb 释放最后 handle）销毁
- `rudp_handle::destroy()` 用户线程直接执行：改 status、cancel 定时器、清 pending 队列
- `set_status(CONNECTED)` 在 `update_active_time()/health_check()` 之前同步调 `connected_cb`——用户回调销毁对象后继续执行

**rpc（`src/rpc`）**
- `executor_` 悬垂引用（见 1.1 #1）——最确定崩溃点
- `last_err_` 非原子（connection.h:123），用户线程 GetLastError/Finish 与 io 线程 set_status 并发
- 用户线程直接操作 asio 对象（connect、enable_timeout_check）
- read_body 错误路径只发 `on_net_err_` 不释放连接 → 半死连接
- do_write 出错时 `close()` 不释放 `reference_`，`error_callback` 随即重建 socket_，与在途旧回调交错
- `FASSERT(message.size() < MAX_BUF_LEN)`：release 下大包直接 abort
- io 线程内用户 handler 同步执行，`server->stop()` 链式同步释放全部连接（rpc_server.hpp:253）——重入风险面大

**http（`src/http`）**
- 双并发写 + 完成回调并发 pop 空 deque（见 1.1 #3）
- `response_` 跨线程直接修改（见 1.1 #4）
- `router.get_table` 锁内返回 map 内部引用、锁外使用（http_router.cpp:24-29）→ 悬垂
- 连接持 `weak_ptr<http_server>` + server 成员 `router_` 引用：server 销毁后连接 UAF
- 超时定时器只在自身回调内重设，从未首次启动 → 慢连接永不超时，fd 缓慢耗尽
- 写错误路径 `release_obj()` 不调 `finish_cb` → ref_data 用户内存生命周期契约破坏
- `max_body_size==0` 即 release 与用户异步 fill 存在时序竞争

**applications（frp/代理）**
> 2026-08-03 全量审计结论（按 `docs/asio-async-standards.md` §10 清单）：详见 `docs/frp-migration-plan.md`（修订版 v2）。核心结论：
- **中继通道实际是 4 个 executor 交叉**（比 v1 判断更严重）：relay 绑 A（signal_->get_executor()）、tcp_ 绑 C（另取 get_io_context()）、backend_udp_socket_ 绑 D、全部驱动代码（on_client_command/on_subscribe）跑在信号线程 B——同 socket 的 async op 从错误线程发起
- **ikcp 状态机被多线程并发访问**（feed/send 来自 B/C/D，tick 定时器在 A）——最活跃的崩溃源
- 同 socket 并发写三处：`async_write_raw` 无队列（frp_client.cpp:201-212）、kcp on_frame 直写 backend/local（:386-404）、（backend 写队列 handle_backend_write_queue 存在但无调用者）
- `channels_` 在 range-for 迭代中被 `on_release_` erase → 迭代器失效（frp_accessor.cpp:1042、frp_provider.cpp:1017）
- 信号客户端自身双 executor（frp_signal_client.cpp:36-38 vs 60-62）：定时器在 A、signal channel 在 B
- punch engine `start_punch_at` 未取消 echo 循环的 pending receive → 同 socket 双接收者（frp_punch_engine.cpp:369 vs 708）
- `frp_signal_session::release_obj` 任意线程直接 cancel 定时器（frp_server.cpp:256-264）；backend 读循环错误无限重挂自旋（frp_provider.cpp:249,267）
- relay 的 `closed_/writing_/backend_connected_` 普通 bool 跨线程裸读写（frp_client.hpp:229-234）
- frp 模块弱哨兵 + reference_ CAS 模式比 RUDP 成熟，但生命周期层不完整（relay close 直接清理、析构同步关 socket、frp_tcp_channel::release_obj 无幂等）

---

## 2. 架构层改造（跨协议公共基础设施）

### 2.1 线程亲和性（最高优先级）
**原则**：每个连接/会话对象终身绑定一个 io_context，其所有成员只能在绑定线程访问；跨线程访问一律 `asio::post` 到绑定线程。

- `io_context_pool::get_io_context()` 原子化轮询（`std::atomic<std::size_t>`）
- 新增约定：连接对象构造时记录 `executor_`（**按值存储**，修复 1.1 #1），之后所有操作 post 到该 executor
- 改造点：frp 中继通道（tcp/kcp 状态绑定单一 context）、signal 通道、http server accept 分发

### 2.2 生命周期基础设施：this_ptr 全程持有
**原则**：内部每个异步操作从发起点到完成点持有 `shared_ptr`（this_ptr），而不是入口 weak 哨兵；用户回调触发时，发起链仍持有所有权，用户销毁 handle 不会造成中途 UAF。

- 公共工具：`safe_post(executor, weak, fn)` / `post(executor, selfptr, fn)`（自动捕获 selfptr 的投递封装，参照 asio2 `post_cp::post`）
- 改造点：RUDP 全部回调链、rpc/http 的用户回调触发点

### 2.3 状态机基础设施
**原则**：连接状态用 `std::atomic<state>` + CAS 迁移；迁移在触发用户回调**之前**完成；迁移函数可重入安全（重复 close 是 no-op）。

- 公共类型：`connection_state`（如 `closed / starting / started / stopping`，参照 asio2 `state_t`）
- 公共守卫：`try_transition(expected, desired)` 模板
- 改造点：RUDP `set_status`、rpc `reference_` 释放、http `response_pack_status`

### 2.4 统一关闭序列
**原则**：stop/close 只投递事件，不在调用者线程做事；真正的清理在 io 线程按固定顺序执行：

1. CAS 迁移状态（started→stopping，重复调用直接返回）
2. 取消所有定时器、清 pending 队列（在 io 线程内）
3. 触发用户回调（此时状态已迁移，回调内再调 stop 是 no-op）
4. 释放资源

- 改造点：RUDP `destroy()`、rpc `release_obj()`、http `release_obj()`

### 2.5 定时器所有权规则
- 定时器是成员 → 回调必须捕获强引用 this_ptr（不是 weak 哨兵）
- 或统一用公共的延迟任务封装（参照 asio2 `post_cp` 的 timed_tasks_ 注册表 + `stop_all_timed_events`）

### 2.6 基础层修复清单（先做）
- [x] `executor_` 改为按值成员（connection.h、rpc_client.hpp，round-2 commit 6146751）
- [x] `next_io_context_` 原子化（2026-08-03 已改）
- [x] io 线程异常不 FASSERT：记录日志 + 尝试恢复/优雅退出（Signal/解析/序列化/用户回调
      逐层兜底，run() 最后防线改为记录 + restart 恢复，commit 323104d）
- [x] `io_context_pool::stop()` 优雅序列：幂等守卫 → 唤醒 wait_stop → 取消已登记定时器 → 释放 work guard → 阻塞等待全部 io_context 排空（2026-08-03 已改；线程由 BlockTimePool 管理，无法 join，以"等待 io_context stopped"替代）
- [x] 新增 `wait_stop()`：阻塞直到 stop() 被调用（asio2 wait_stop_timer_ 同款机制）
- [x] 新增定时器注册表 `reg_timer/unreg_timer`：stop 时统一取消（解决"stop 后定时器存活导致无法退出"）
- [x] 新增 `running_in_io_thread()`：线程亲和性断言基础设施
- [x] 新增对象注册表 `reg_object/unreg_object`：stop() 统一驱动已登记对象释放（保证"池停 → 所有对象必然被 stop"；与顶层对象的 make_guard 互补，覆盖 server 动态创建的 session）
- [x] `stop()` 顺序修正：先驱动对象释放 → 再取消定时器 → 再等待 io 排空（2026-08-03 已改）
- [x] http Content-Length 上限校验（防远程 bad_alloc，commit 40cc76d）
- [x] rpc `FASSERT(message.size() < MAX_BUF_LEN)` 改为错误返回（pack 端
      `>= MAX_BUF_LEN` 拦截走错误通道，rpc_client.hpp:510/802；write() 内 FASSERT 仅为
      不可达防御，commit 6146751）
- [x] 各模块定时器创建点接入 `reg_timer/unreg_timer`（frp_signal_client 三个定时器、
      relay_data_channel 两个定时器等，frp 阶段 A/C + round-2 已完成）
- [x] 动态对象（server accept 的 session/通道）接入 `reg_object/unreg_object`
      （frp_signal_session、relay_data_channel 等，frp 阶段 C + round-2 已完成）

---

## 3. 各协议实现层改造

### 3.1 RUDP（src/network/rudp）

> 2026-08-04 完成（commit `8ffc0ea`）：status 原子化 + exchange 幂等迁移；回调顺序修正（内部初始化先于 connected_cb）；flush 单在途发送 + RST 入队（规范 §3.4）；perform_read token 先检（§3.3）；accept_assign_executor 先 cancel 再迁移定时器并重新登记；closed_cb 强引用化；config_item 原子化；reg_timer 全量接入；删除死代码 get_id。release + ASAN 下 TestRudp 9/9 通过。

| 优先级 | 改造项 | 对应问题 |
|--------|--------|----------|
| P0 | `status` 改为 `std::atomic<rudp_connection_status>` + 迁移幂等（exchange 返回旧值） | 数据竞争 |
| P0 | `destroy()` 分两段：用户线程仅 erase storage + post；清理序列在 io 线程执行（已有结构保留） | 跨线程 destroy |
| P0 | 回调链强引用：进入时 lock 出的 strong 覆盖整个回调体（含用户回调触发链）——审计确认各链无中途 UAF | 重入 UAF |
| P0 | `set_status(CONNECTED)` 中 `connected_cb` 移到 `update_active_time()/health_check()/request_update_rudp_status()` **之后** | 回调重入后继续执行 |
| P0 | **flush_data 单在途发送（新发现）**：KCP 一次 update 多包 → 并发 async_send；`send_rst` 原绕过队列直发 → 统一入队 | 同 socket 并发写 |
| P1 | `rudp_handle::operator bool` 线程安全（close_flag 原子 + status 原子后自然解决） | 跨线程读 |
| P1 | 定时器回调捕获强引用 + 全部定时器 reg_timer/unreg_timer（accept_assign_executor 迁移时先 cancel 再重登记） | 生命周期/退出排空 |
| P1 | `rudp_config_item` 原子化（新发现）：用户线程 rudp_config 与 io 线程并发读写 | 数据竞争 |
| P1 | accept_connection 的 closed_cb 捕获 server 强引用而非裸 server_context（新发现） | 悬垂 |

### 3.2 rpc（src/rpc）

| 优先级 | 改造项 | 对应问题 |
|--------|--------|----------|
| P0 | `executor_` 按值存储（`asio::any_io_executor executor_`，非 const 引用） | 悬垂引用（#1 崩溃点） |
| P0 | `connect()`/`enable_timeout_check()` 全部 post 到 io 线程执行，禁止用户线程直接操作 socket_/resolver_/deadline_ | 并发操作 asio 对象 |
| P1 | `last_err_` 原子化或只在线程内使用 | 数据竞争 |
| P1 | read_body 错误路径走统一释放；do_write 错误后 reference_ 释放时序修正 | 半死连接/交错 |
| P1 | 大包 FASSERT → 错误返回 | abort |
| P2 | `server->stop()` 的链式同步释放 → 逐个 post 延迟释放 | 重入 |

### 3.3 http（src/http）

> 2026-08-04 完成（commit `40cc76d`）：单在途写标志串行 header/body（§3.4）；setter 全 post 化 + status/body 尺寸原子化（§3.2）；解析长度上限（method/uri/header/Content-Length，§7.1）；超时定时器启动 + reg_timer；finish_cb 关闭/错误路径兜底（§5.2）；get_table 返回副本；release_obj 幂等；handler 异常 → 500；SSL 明文禁用时关闭连接。curl 回归 + 原始 socket 上限测试 + 20 并发 + ASAN 全绿，ctest 71/71。

| 优先级 | 改造项 | 对应问题 |
|--------|--------|----------|
| P0 | 单连接写串行化：`sending_` 标志统一串行 header 写与 body 写链，完成回调驱动下一段；消除 size==1 时的并发写窗口与并发 pop | 双写崩溃 |
| P0 | 所有 `response_` 修改（set_status/set_content_type/set_raw_content_type/add_header/stock_response）post 到 io 线程 | 跨线程改 headers |
| P0 | Content-Length/method/uri/header 名值/header 数量全部上限校验，超限 400 拒绝（防远程 bad_alloc） | 远程 terminate |
| P1 | `router.get_table` 返回副本，不返回内部引用 | 锁外悬垂 |
| P1 | 超时定时器在 handle_read 每次读时重置（首次启动），reg_timer 接入 | 超时失效/退出排空 |
| P1 | 写错误/关闭路径 abort_pending：回掉全部未完成 ref_data 的 finish_cb | 用户内存契约 |
| P2 | 同步 handler 异常捕获 → 500（防 io 线程 terminate）；SSL 明文禁用时拒绝明文连接；连接/server reg_object | server 销毁 UAF/异常 |

### 3.4 applications（frp/代理）

> 详细可执行计划：`docs/frp-migration-plan.md`（修订版 v2，按阶段 1-4 组织，含 ★ 新增项）

| 优先级 | 改造项 | 对应问题 |
|--------|--------|----------|
| P0 | 信号客户端单 executor 收敛：frp_signal_client 构造固定 executor，signal channel 复用（frp_signal_client.cpp:36-38 vs 60-62） | 定时器与信号通道跨 context |
| P0 | 中继通道绑定单一 io_context：tcp_/backend_udp_socket_/kcp/定时器统一用通道 executor；信号线程驱动代码 post 化 | 4 executor 交叉 + ikcp 并发 |
| P0 | 同一 socket 写串行化：async_write_raw 入写队列；kcp on_frame 的 backend/local 写接入写队列（接线 handle_backend_write_queue 或 serialized_writer） | 并发组合 IO |
| P0 | channels_ 关闭先搬出再迭代（accessor/provider close()） | 迭代中删除 UB |
| P0 | 服务端 register_data_channel 对 dst_session 的操作 post 到其 executor | 跨线程定时器/读循环 |
| P1 | punch engine 双接收者：start_punch_at 先 cancel echo 接收再开 punch 读循环 | 同 socket 双 receive |
| P1 | frp_signal_session release_obj 三层化（只投递，cancel 移入 io 线程） | 跨线程定时器 cancel |
| P1 | backend 读循环错误收敛到关闭序列，消除自旋 | 半死连接/自旋 |
| P1 | relay 布尔成员原子化或线程内化 | 数据竞争 |
| P1 | UDP listener 关闭后停止读循环（检查 socket 是否 open），`local_udp_socket_` 改为共享所有权 | 自旋/悬垂 |
| P1 | relay_data_channel 三层模型：release_obj 入口 + 关闭序列 + 析构兜底；frp_tcp_channel::release_obj 幂等 | 生命周期不完整 |
| P2 | 命令帧解析：验证 default 释放是否造成重连风暴，明确各命令的合法接收者 | 协议缺陷 |
| P2 | on_release 时清理 channels_（先搬出再关，见 P0 行） | 泄漏/UB |

---

## 4. 改造顺序（里程碑）

> 逐子模块迁移的详细影响分析与顺序：见 `docs/migration-impact-analysis.md`

### M1 止血（消除确定性崩溃）
1. 基础层修复清单（2.6，尤其 `executor_` 悬垂）
2. http 双并发写串行化 + Content-Length 上限
3. rpc 用户线程直接操作 asio 对象 → post
4. RUDP status 原子化 + destroy 投递化
5. **验证**：ASAN debug 构建跑各模块压力测试（`./test-gen-linux-debug.sh` 已含 ASAN），收集崩溃日志逐条对照

### M2 结构（生命周期与线程模型）
1. 统一关闭序列（2.4）+ 定时器所有权规则（2.5）
2. this_ptr 全程持有（RUDP 回调链、rpc/http 回调触发点）
3. 线程亲和性：frp 中继通道、signal 通道绑定单一 context
4. **验证**：frp 全链路（信令 + 中继 + P2P 打洞）压力测试；断连/重连风暴场景

### M3 加固（长期稳定）
1. 事件队列（如需要，参照 asio2 event_queue_cp）
2. 全模块 ASAN + 长时间 soak test 全绿
3. 对照诊断清单复查一遍（第 0 节方法论）

---

## 5. 与教学课程的映射

| 课程 | 主题 | 覆盖的改造项 |
|------|------|--------------|
| 0001 | 异步对象生命周期（this_ptr 所有权） | 2.2、3.1 RUDP、3.2 rpc |
| 0002 | 状态机与 CAS 迁移 | 2.3、3.1 RUDP status、3.3 http |
| 0003 | 线程模型与事件队列 | 2.1、2.6、3.3、3.4 |
| 0004 | 统一关闭序列 | 2.4、3.2、3.4 |

每完成一课，在对应改造项上打勾，以"改造 + 验证"为一次学习闭环。
