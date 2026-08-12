# Glossary

网络层改造的领域词汇。术语以本文件为准，不要漂移成同义词。

## 构建验收

- **零警告验收**：整个构建输出（库 + applications + samples）无任何编译器警告；不改编译选项（不加 `-Wno-*`、不加 `-Werror`），只改源码。见 ADR-0002。
- **验收矩阵**：4 种构建组合——gcc/RelWithDebInfo、gcc/Debug、clang/RelWithDebInfo、clang/Debug，全部满足零警告验收。
- **警告修复范围**：`src/` 与 applications/samples 的 `src/` 目录；third-parties 源码不修改。
- **LTO 策略**：LTO 只在 Release 模式 + gcc 启用；clang 一律不启用（LLVM bitcode 依赖 LTO 插件，环境不保证可用）。见 ADR-0002。

- **对外暴露的接口**：库的公共 C++ API（方法签名、回调签名、类层次）与跨进程 wire 格式的合称。二者都是部署环境下的契约。
- **接口冻结（冻结 A）**：对外暴露的接口签名与语义完全不变，只改内部实现的约束。见 ADR-0001。
- **崩溃修复豁免**：把"崩溃 / UB / 挂死"修复为"错误返回 / 正常关闭"不属于接口语义变化，不受冻结约束。见 ADR-0001。
- **协议任务**：以一个协议为粒度的工作单元，流程固定为 审计 → 改造 → 补测试 → 验收，每任务独立 commit。
- **测试载体**：某协议测试所归属的测试可执行。RPC 核心 / WebSocket 转发 / SOCKS5 共用 TestRpc；netlink 用其独立 netlink_test；protocal_pipe 用独立新建测试。
- **审计发现**：按 `docs/asio-async-standards.md` §10 清单扫描后的具体问题，落到 `file:line`，记录为 `.scratch/<task>/issues/` 下的 ticket。
- **验收标准**：一个协议任务完成的定义——相关测试全绿（未被测试覆盖的改造必须新建用例）、release 构建通过、ASAN 无内存错误/泄漏、测试进程退出不挂死。
