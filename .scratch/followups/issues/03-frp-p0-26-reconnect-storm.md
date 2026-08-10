# 03-frp-p0-26-reconnect-storm

Type: task
Status: resolved

## 问题（待验证）

服务端 unknown 命令 → `release_obj()`（frp_server.cpp:500-502，authenticated 阶段 default 分支）疑似触发客户端重连风暴。原记录：docs/frp-migration-plan.md 待验证-26（该文档在 origin/main 上）。

## 下一步

1. 加日志验证：恶意/异常客户端发 unknown 命令 → 服务端释放 → 客户端（若开自动重连）重连 → 是否形成风暴
2. 若触发：改为忽略 + 计数熔断（wire 协议零改动约束下，ADR-0001）
3. 验收：异常命令场景下服务端稳定，无风暴日志

关键位置：frp_server.cpp:500-502（authenticated 阶段 default 分支 → release_obj）。

## Answer（2026-08-07 修复 + 验证）

### 结论

- 客户端（frp_signal_client）本身断线重连带 2s 指数退避（2→4→…→60s），不会自风暴；
  但服务端对 unknown 命令直接 release_obj 会让服务端成为恶意/异常客户端的重连 churn
  主动方（accept→parse→release 循环），且对未知的新命令类型不兼容（直接断连）。

### 修复（wire 协议零改动，ADR-0001 约束内）

- frp_signal_session 新增 `bad_command_cnt_` 计数 + `kMaxBadCommandCount = 10` 阈值，
  `handle_unknown_command()` 统一处理 initial/authenticated 两个阶段的 default 分支：
  - 未知命令不再直接 release：计数 < 阈值时忽略并 `read_next_command()` 继续读
    （服务端不作为风暴主动方，同时兼容未来新增命令类型）；
  - 计数 >= 10 才 release（防恶意刷）。

### 验证

- 新增 test_frp_unknown_command_circuit_breaker：原始 TCP 连接发未知命令，
  前 3 条连接保持（poll 探测无 EOF），累计 10 条后服务端释放（recv==EOF）。
- 新用例 3/3 通过；全套 2 轮 52/52 通过、进程正常退出。
- 注：测试走 initial 阶段（免 auth）；authenticated 阶段与 initial 共用同一
  handle_unknown_command 路径，逻辑一致。
