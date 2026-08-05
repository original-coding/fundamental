# FRP 异步资源访问与管理 —— 调研任务

## 问题域

FRP 模块中的对象（`relay_data_channel`, `kcp_channel`, `frp_tcp_channel`, `frp_signal_channel` 等）持有 asio I/O 资源（socket/timer），其公共接口可能被非 io 线程调用。当前通过以下机制保护：

- `network_data_reference` — 标记对象已释放，回调变 no-op
- `enable_shared_from_this` — 延长对象生命周期至异步操作完成
- `asio::post(executor, ...)` — 将清理操作投递到 io 线程

但这套机制在退出时存在时序问题：取消 → 投递 handler → poll → 释放。需要调研更优雅的方案。

## 核心矛盾

```
异步操作的完成通知  ←→  同步的资源销毁顺序
```

取消一个异步操作（timer/socket/signal）本身产生一个新的异步事件（`operation_aborted`），必须被 poll 到才能释放其持有的 `shared_ptr`。但销毁时调度器先于资源管理器停止。

## 调研方向

### 1. 访问模型分类

哪些接口需要线程安全，哪些不需要：

| 调用者 | 典型操作 | 是否需要 post |
|---|---|---|
| io 线程回调 | on_frame, on_connected, timer 回调 | 已在 io 线程 |
| 外部线程通过 public API | send(), close(), release_obj() | 需要 post 或同步保护 |
| 退出路径 | release_obj() 链 | 当前 post，但调度器已停 |

### 2. `asio::post` 的使用边界

- 哪些 `release_obj()` 中的 post 是真正必要的（外部线程可能调用）
- 哪些可以改为 assert 当前已在 io 线程（因为调用方保证）
- shutdown 路径是否可以用 `poll()` drain 替代 post

### 3. 退出协调机制

调研业界模式：

- **asio `ssl::stream` 模式**：`async_shutdown` + 手动 poll 直到完成
- **boost::beast**：`close()` 取消 + `timer` 保证超时清理
- **用户态 RCU**：延迟释放，不依赖异步完成
- **strand 模式**：所有回调经 strand 序列化，取消后 `dispatch` 保证执行

### 4. 候选方案

**A. poll-drain 模式**

```
release_obj():
  timer.cancel()  → 投递 operation_aborted
  socket.close()  → 投递 operation_aborted  
  drain_poll()    → 在当前线程 poll 直到队列空
```

问题：`drain_poll()` 需要在 `stop()` 之前执行，且 poll 哪个 io_context 需要明确。

**B. 无 post 取消模式**

```
release_obj():
  reference_.release()  → 标记死亡
  timer.cancel()        → 同步取消，handler 稍后触发时检查 reference_ → no-op
  socket.close()        → 同步关闭
  // 不 post，不 poll。handler 中的 shared_ptr 在未来某次 poll 时自然释放
```

问题：socket.close() 与 async_read 的竞态在某些 asio 版本中未定义。

**C. 分层关闭**

```
Phase 1: cancel all → poll until drained
Phase 2: stop io_context
Phase 3: join threads
Phase 4: destroy objects
```

需要全局协调器。

## 输出要求

- 明确 FRP 各类的线程安全契约（哪些方法可跨线程调用）
- 统一 `release_obj()` 的退出模式
- 解决 SIGTERM 崩溃
