# 05-netlink-stop-hang

Type: task
Status: resolved

## 问题

`enable_test=1 ./build-linux/src/rpc/netlink_test 3`（release 或 debug 均可）所有用例跑完后进程不退出：主线程卡在 `io_context_pool::stop()` 轮询。

## 复现

```bash
cd build-linux/src/rpc
timeout 90 env enable_test=1 ./netlink_test 3
# 期望：跑完所有用例后进程退出；实际：玩家全部 finished 后进程残留
```

## 已完成（提交 6146751 + 56adc35，勿重复）

- 42 个定时器全部 reg_timer（池停统一取消）
- 半关闭语义重构（56adc35）：EOF 驱动拆除 + FIN 传播（proxy_upstream_interface::shutdown_send）+ 滑动空闲定时器（arm 一次、每包 expires_after）+ 转发连接/socks5 会话接入 reg_object
- test_ws_pipe_half_close 通过（负向验证有效）

## 已排除的根因

- 定时器残留：cancel_registered_timers count:42，全部取消后仍挂
- 500ms 强关：已移除
- 池停时定时器被取消打断级联：已由 EOF 级联 + reg_object 替代
- is_stopping 递归崩溃：已消除

## 当前状态与下一步

- **半关闭修复后仍挂死**：玩家全完成、池停启动（日志 "recv signo:0 Operation aborted"）后进程残留
- 诊断线索（历史 fdinfo）：客户端↔9000 直连已关闭，自环转发链（pipe → socks5 跳点 → 9000）的 ESTABLISHED 连接残留——**EOF 级联在自环拓扑下仍有一环未收敛**
- 候选环节：socks5 跳点连接的 server 侧连接、ws_forward 链的释放顺序、reg_object 驱动顺序（unordered_map 无序）
- 建议：跑 gdb 抓残留线程栈（`timeout 90 env enable_test=1 gdb -batch -ex run -ex bt --args ./netlink_test 3`，debug 构建），或对每个候选环节加临时日志定位哪一环没关

## 验收

- netlink_test 3/4 人局（release + ASAN debug）跑完且进程正常退出
- TestRpc 49+ 例无回归

## Answer（2026-08-07 根因定论 + 修复 + 验证）

### 根因：rpc_client::reset_deadline_timer 取消后无条件重挂

- gdb 全线程栈 + reactor 状态定位：main 卡在 `t.join()`（netlink_test.cpp:105），
  Application::Loop 线程在处理 exitStarted 事件时同步调用 `io_context_pool::stop()`，
  stop() 在等待 io_context run() 返回（io_context_pool.cpp:110 轮询）。
- 两个 io_context 的 reactor 定时器堆里各有 4/2 个挂起定时器，全部是
  `rpc_client::reset_deadline_timer` 的 wait_handler。
- `reset_deadline_timer`（rpc_client.hpp）的 handler 在**取消（operation_aborted）后仍无条件
  重新 expires_after + async_wait**——池停 `cancel_registered_timers()` 取消它后立刻又被重挂，
  每次取消都持有一个 work 单元，io_context::run() 永不返回 → stop() 永不返回 → 退出链挂死。
  （此前记录的"42 个定时器取消后仍挂"即此原因；其余定时器均已修复为取消即返回，仅此一处漏网。）

### 修复

- rpc_client.hpp `reset_deadline_timer` handler：`if (ec) return;`——取消（池停）时不重挂，
  正常超时路径行为不变（保持心跳/keepalive 语义）。

### 验证

- netlink_test 3/4 人局：debug + release 均 EXIT=0 正常退出（修复前永久挂死，SIGTERM 都无法终止）。
- 验证期间顺带修复 3 处同类的脆时序测试断言（与 ticket 01 同类型）：
  test_auto_reconnect（50ms 墙钟 vs 45ms 强制 sleep，改功能性重连验证）、
  test_reconnect_after_server_kill（末尾单发 echo 竞态，改有界重试）、
  test_download/test_upload（1ms connect 超时，改 5000ms）+ 移除全套其余脆墙钟 guard。
- TestRpc 全套连跑 4 轮 50/50 通过、进程正常退出。
