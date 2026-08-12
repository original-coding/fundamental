# ADR-0002: 构建零警告验收口径 + LTO 策略

- 状态：已接受（2026-08-12）
- 背景：`warn-cleanup` 分支（从 `main` 切出）上消除全部编译警告的清理任务。

## 决策

1. **验收矩阵 = 4 种组合全部零警告**：gcc/RelWithDebInfo（`test-gen-linux.sh`）、gcc/Debug（`test-gen-linux-debug.sh`）、clang/RelWithDebInfo、clang/Debug。先用 gcc/RelWithDebInfo 修完，再逐个验证其余组合。
2. **零警告的定义**：整个构建输出（库 + applications + samples）无任何编译器警告；不新增/删除 `-Wno-*`、不加 `-Werror`，只通过修改源代码消除。clang-tidy 的 diagnostic 不计入"零警告"验收。
3. **修改范围**：`src/` + `applications/*/src` + `samples/*/src`；`third-parties/` 的源码不修改（其构建配置仅在必要时调整，见第 4 条）。
4. **LTO 策略**：LTO 只在 Release 模式且使用 gcc 时启用（项目级 `-flto=auto` 与 quickjs 的 IPO 一致）；clang 一律不启用 LTO——clang 的 `-flto` 产出 LLVM bitcode，GNU ld 无 LLVMgold 插件时无法链接，且会在 clang 构建中引入错误/警告。

## 背景（为什么这么做）

- 仓库在 main 上 gcc/RelWithDebInfo 有 8 类警告，clang 构建还有编译错误（`parallel.hpp` 缺 `<functional>`、`jsscript_visitor.h` 多余 `template` 关键字）和一批 clang 专属警告，且 quickjs 强制 IPO 导致 clang 链接失败。
- 只修 applications/samples 修不掉库头文件里的警告（如 `frp_signal_client.hpp:42` 不完整类型），因此范围必须包含 `src/`。
- `third-parties/` 源码不动，但其 quickjspp CMake 无条件开启 IPO、给 gcc 加 `-Wall`，这两处构建配置影响验收结果，故在 CMakeLists 层面调整。

## 后果

- 后续改动引入新警告时，按本口径应修复后合并；新增 third-party 依赖若默认开 LTO/警告，需在 CMake 层面对齐本策略。
- `archive_main` 分支不处理；本决策只约束 `main`（及从其切出的分支）。
