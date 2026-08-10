# 04-pipe-upgrade-incremental-read

Type: task
Status: resolved

## 问题（观察项）

`pipe_connection_upgrade::handle_greeting_response`（pipe_connection_upgrade_session.hpp:58-81）对 need_more_data 的处理：`read_buffer.resize(len)` 后重新 `read_cb_` 覆盖读入——若响应分块到达（首读未取全），后续读覆盖缓冲，decode 永远凑不齐完整响应 → 潜在无限 need_more_data 循环。

实际影响：响应体极小（forward_response_context），通常一次读完成，未观测触发。

## 下一步

先确认 forward_pipe_codec 的 decode len 语义（消费字节数 vs 需求字节数）再决定修复；无复现则保持观察。

## Answer（2026-08-07 代码分析定论）

- decode 语义确认（`forward_pipe_codec.hpp`）：`forward_parse_need_more_data` 时返回的 `len` 是
  **还需读取的字节数**（`kFrameSizeStrLen + kMagicNumSize - parse_cache.size()`，或
  `frame_total - parse_cache.size()`）；成功时返回本次消费的字节数。handler 的
  `read_buffer.resize(len)` 用法与语义一致。
- 数据累积点在成员 `parse_cache`（decode 每次把入参数据 append 进去），`read_buffer` 只是单次读的
  临时载体。`resize(len)` 后重新读入覆盖的是临时缓冲，**已接收字节不会丢失**——ticket 假设的
  "后续读覆盖缓冲 → decode 永远凑不齐 → 无限 need_more_data"不成立。
- read 回调实现为 `downstream_async_buffer_read` → `asio::async_read`（填满请求字节数，
  rpc_forward_connection.hpp:127-135），因此每轮恰好读入所需增量 → parse_cache 精确到达帧长 →
  必然收敛；EOF/错误走 finish_cb 错误路径，不会形成死循环。服务端（ws_port_pipe_server 接受路径）
  与客户端共用同一 `pipe_connection_upgrade`，结论一致。
- 结论：**无实际 bug，无需修复**，保持现状。唯一理论边角是"单次 read 成功返回 0 字节"（async_read
  在 len≥1 下不会发生），不可达。
