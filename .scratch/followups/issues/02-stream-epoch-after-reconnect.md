# 02-stream-epoch-after-reconnect

Type: task
Status: resolved

## 问题（观察项）

`upgrade_to_stream` 后的 `ClientStreamReadWriter`（rpc_client.hpp:185+）在客户端连接重建（enable_auto_reconnect）后仍绑定旧 socket 代次：

- 流的 `reference_` 独立于 client（仅流 release 时释放），连接重建不释放流
- 在途流读写（read_head/read_body/handle_write 的 async op）在重建后可能落到**新 socket** → 交叉写入新连接
- rpc_client 的 `connection_epoch_` 代次守卫（commit 6146751 的修复）只覆盖 client 级回调，未覆盖流

## 下一步

决定修复方案：

- A：流回调绑定 client 的 `connection_epoch_`——代次不符即 `set_status(failed)` + `release_obj()`
- B：重连时流自动终止（client 重建连接时通知活跃流）

验收：重连期间在途流写不落到新连接（ASAN + 用例：连接 → 升级流 → 服务端杀连接 → 重连 → 断言旧流写入失败/新连接无交叉）。

关键位置：rpc_client.hpp 的 ClientStreamReadWriter 实现（read_head :1677、read_body :1738、handle_write :1799）、connection_epoch_（:1443 附近）。

## Answer（2026-08-07 修复定论 + 验证）

### 两个叠加问题

1. **流模式断流后客户端从不重连**：upgrade 后客户端自身的 do_read 停止（rpc_stream 分支不续
   do_read），断流只由流的 read/write 回调感知 → set_status(failed) → release_obj →
   client_->close(true)，但从不触发 error_callback → enable_auto_reconnect 在流模式下从未生效。
2. **流未绑定连接代次**：流对象独立于 client 存活，其异步 op 走 client_->async_buffers_*，
   一旦连接重建（新 epoch），在途流 op 可能落到新 socket（流帧写入新 RPC 连接 / 双读）。

### 修复（方案 A：流绑定 connection_epoch_，并打通流模式重连）

- ClientStreamReadWriter::start() 捕获 client_->connection_epoch_（改为类外定义，
  类内时 rpc_client 尚不完整）。
- read_head/read_body/handle_write 入口 + 完成回调均加代次守卫：epoch_mismatch →
  abort_for_epoch_mismatch()（置 failed + 唤醒 Read/Write，不调 release_obj，避免误关新连接）。
- release_obj 的 posted 清理：close(true) 完成后若流是"连接错误失败"（status==failed），
  调 client_->error_callback() → enable_auto_reconnect 时自动重连；正常 Finish/用户主动
  release 不触发。

### 验证

- 新增 test_stream_epoch_after_reconnect：连接 → upgrade test_echo_kill_stream（echo 后服务端
  断流）→ 客户端自动重连 → 旧流 Write/Read 立即失败 → 新连接 500ms 稳定（无流帧污染/再断连）。
- 新用例单跑 3/3 通过；全套 2 轮 51/51 通过、进程正常退出。
