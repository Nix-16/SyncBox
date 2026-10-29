# AGENTS.md —— SyncBox 工程协作规则

> 本文件是**所有开发者与 AI 代理**进入本仓库后必须先读的操作手册。
> 它约束“怎么做”，不重复“做什么”。“做什么”以 `docs/` 为准。

---

## 0. 项目一句话

SyncBox：C++17 跨平台（Windows/Linux/macOS）双向文件同步工具，中心服务器架构，要求不丢数据、离线可用、崩溃可恢复。

---

## 1. 当前真实状态（每次任务前必读）

| 项 | 状态 |
|---|---|
| 当前里程碑 | **M5 进行中：一次性同步引擎与 CLI 已实现；冲突解决/持续监控/恢复未完成** |
| 业务代码 | core + store + fs + net + server + client（scan/outbox/push/pull）；冲突解决与持续监控未实现 |
| 构建系统 | CMake 3.22 + vcpkg manifest（storage/unicode/crypto/networking/json 已启用） |
| 测试 | 78 个测试（含守护/WS/恢复）；Linux Debug 与 ASan 通过 |
| Git 仓库 | 已初始化（`main`），远端 `origin` = `github.com/Nix-16/SyncBox` |

完整状态见 [docs/STATUS.md](docs/STATUS.md)。**在 M1 完成前，不要声称项目“可构建”“可运行”“测试通过”。**

---

## 2. 文档索引与阅读时机

| 文档 | 内容 | 什么时候必须读 |
|---|---|---|
| [docs/PRD.md](docs/PRD.md) | 产品目标、功能与非功能需求、范围边界、架构不变量 | 任何需求判断前 |
| [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) | 模块划分、依赖方向、数据流、关键设计决策 | 写任何跨模块代码前 |
| [docs/DATABASE.md](docs/DATABASE.md) | 表结构、索引、约束、事务、迁移、恢复矩阵 | 改 schema / 写 Repository 前 |
| [docs/PROTOCOL.md](docs/PROTOCOL.md) | HTTP/WS 端点、字段、错误码、幂等与游标 | 改网络层 / 客户端服务端交互前 |
| [docs/CONFLICTS.md](docs/CONFLICTS.md) | 版本模型、CAS、冲突策略、跨平台命名 | 改同步引擎 / 冲突逻辑前 |
| [docs/SECURITY.md](docs/SECURITY.md) | 威胁模型、认证、TLS、路径安全 | 改认证 / 路径处理 / 文件落盘前 |
| [docs/TEST_PLAN.md](docs/TEST_PLAN.md) | 测试用例、故障注入、性能与安全验收 | 写测试 / 判定完成前 |
| [docs/ROADMAP.md](docs/ROADMAP.md) | M0–M9 任务、依赖、验收、交付物 | 接任务 / 排期前 |
| [docs/STATUS.md](docs/STATUS.md) | 真实进度、差距、风险、待决设计 | 每次任务开始与结束 |

**阅读规则**：任务涉及哪个模块，就至少读该模块在 ARCHITECTURE 中的职责 + 对应专项文档。禁止“凭印象”实现。

---

## 3. 每次任务的固定流程

```
1. 读 STATUS.md，确认当前里程碑与允许范围
2. 读相关规范（见 §2）
3. 写计划：要改哪些文件、用什么数据结构、如何验证
4. 只实现当前里程碑内的最小可用改动
5. 实际编译（不得跳过）
6. 实际运行相关测试（不得跳过）
7. 更新 docs/STATUS.md（进度、差距、风险、未决问题）
8. 如行为变更，同步更新对应规范文档
9. 汇报：改了什么、编译结果、测试结果、未完成/未验证项
```

**任务范围**：用户指定“只做 M1”时，不得顺手实现 M2 的业务逻辑。可以创建占位接口，但不得假装完成。

---

## 4. 真实性规则（最高优先级）

1. **不得虚报测试结果。** 没运行就说“未运行”，不要说“应该通过”。
2. **不得编造命令输出。** 汇报构建/测试时给出实际命令与关键输出；失败就贴失败信息。
3. **不得把“设计”写成“已实现”。** 文档中的 schema、端点、算法都是目标，不是现状。
4. **不得用注释/TODO 冒充完成。** 函数体为空或抛 `not_implemented` 时，状态必须是“未实现”。
5. **不得为了让文档好看而删除未实现声明。** STATUS.md 必须持续反映差距。
6. **不得删除、绕过或弱化已有测试**来让 CI 变绿。修代码，不修测试（除非测试本身与规范冲突，且需说明理由）。
7. **不得静默更改需求。** 规范与实现冲突时，见 §6。

---

## 5. 禁止事项

| 禁止 | 原因 |
|---|---|
| 删除已有可用功能 | 用户明确要求保留现状 |
| 大规模重写已有代码 | 增量演进优先 |
| 跨里程碑实现业务功能 | 破坏可验证的推进节奏 |
| 擅自引入重量级依赖 | 需在 ARCHITECTURE 记录并说明理由 |
| 擅自 `git init` / 提交 / 推送 | 需用户明确授权 |
| 在同步目录中直接写临时文件 | 违反 INV-10，会触发事件循环 |
| 客户端自增 `revision` | 违反 INV-1 |
| 信任客户端声明的哈希 | 违反 INV-2，服务端必须重算 |
| 在事务内做网络 I/O 或文件拷贝 | 延长锁持有时间，易超时 |
| 物理删除 tombstone 行 | 违反 INV-5，会导致文件复活 |
| 用 `sleep` 作为测试同步手段 | 不稳定，改用 `waitFor` 条件轮询 |
| 在日志中输出 token 或文件内容 | 违反 SECURITY §8.1 |

---

## 6. 需求与实现不一致时的处理

发现规范与实现（或平台现实）不一致：

1. **停止**，不要按自己的理解继续写。
2. 在 `docs/STATUS.md` §5.2「未闭环设计问题」新增条目（编号 O-xx）。
3. 在汇报中明确写出：`规范 X 与实现/平台 Y 冲突，建议 Z`。
4. 需要改规范时，**同时**修改规范文档与受影响的其他文档，并在 STATUS 记录变更。
5. 需要改实现时，说明为什么现实现更合理，并补测试。
6. **不得**只改代码不改文档，或只改文档不改代码。

---

## 7. 里程碑纪律

| 里程碑 | 允许做的事 | 不允许 |
|---|---|---|
| M1 | 构建、依赖、骨架、CI、冒烟测试 | 业务逻辑、数据库 schema 实现 |
| M2 | core 模型、路径、哈希、两库 schema、Repository | 文件监控、网络、同步引擎 |
| M3 | 文件系统抽象、三平台监控、扫描、原子替换 | 服务端 API、冲突策略 |
| M4 | 服务端 API、认证、CAS、变更流、blob、WS | 客户端同步引擎 |
| M5 | 同步引擎、队列、拉取、落盘、冲突、恢复 | GUI |
| M6 | 分块、续传、重试、TLS 完善 | 新功能 |
| M7 | Qt GUI | 修改引擎语义 |
| M8 | 安全加固、运维工具 | 新业务功能 |
| M9 | 性能、打包、文档、演示 | 架构重构 |

跨里程碑依赖未完成时，**不要用假实现绕过**；在 STATUS 记录阻塞。

---

## 8. 文档同步矩阵（改代码时必须同步改文档）

| 改动 | 必须更新 |
|---|---|
| 新增/修改表、索引、迁移 | DATABASE.md、STATUS.md |
| 新增/修改端点、字段、错误码 | PROTOCOL.md、TEST_PLAN.md |
| 修改冲突策略或版本语义 | CONFLICTS.md、ARCHITECTURE.md、TEST_PLAN.md |
| 修改认证/路径/落盘安全 | SECURITY.md、TEST_PLAN.md |
| 新增模块或改变依赖方向 | ARCHITECTURE.md |
| 完成/推迟里程碑任务 | ROADMAP.md、STATUS.md |
| 新增已知限制 | PRD.md §7、STATUS.md |

---

## 9. 构建与测试（M1 已建立）

依赖使用 vcpkg manifest 模式，基线锁定在 `vcpkg.json` 的 `builtin-baseline`。

```bash
# 一次性：准备 vcpkg 与工具链
git clone https://github.com/microsoft/vcpkg.git ~/vcpkg
~/vcpkg/bootstrap-vcpkg.sh
export VCPKG_ROOT=~/vcpkg

# 配置 / 构建 / 测试
cmake --preset dev                 # 或 release / asan
cmake --build --preset dev
ctest --preset dev --output-on-failure
```

- 可执行文件：`build/dev/syncbox`、`build/dev/syncboxd`（当前仅 `--version` / `--help`）。
- `SYNCBOX_SANITIZE=ON`（`asan` preset）仅在 GCC/Clang 下可用。
- 新增源文件后必须重新运行配置步骤。
- 修改 `vcpkg.json` 后必须提交新的 `builtin-baseline` 并说明原因。
- 依赖按里程碑通过 vcpkg feature 启用（当前 `dev`/`release`/`asan` preset 已启用
  `storage;unicode;crypto`）；`networking`/`json` 在 M4 启用。

要求：

- 提交前必须在本机完成 **配置 + 编译 + 相关测试**。
- 声称“跨平台可用”必须有 CI 证据；不能只在本机 Linux 通过就下结论。
- 性能声明必须给出硬件、数据集、命令与原始结果。

---

## 10. 目录与依赖规则

- 目录结构见 ARCHITECTURE §2.1。
- `core` 不得依赖网络、SQLite、平台 API、Qt。
- `client` 与 `server` 不得互相依赖。
- GUI 不得直接操作文件系统或数据库。
- 平台差异只在 `fs/` 后端与少量 `#ifdef` 中出现；业务逻辑禁止平台分支。
- 新依赖必须：固定版本、记录在 ARCHITECTURE、说明许可证、评估体积。

---

## 11. 代码规范

| 项 | 规定 |
|---|---|
| 标准 | C++17，禁用编译器扩展 |
| 格式化 | `.clang-format`（M1 引入，LLVM 基础，4 空格，120 列） |
| 命名 | 类型 `PascalCase`；函数/变量 `snake_case`；成员 `snake_case_`；常量 `kPascalCase` |
| 头文件 | `#pragma once` 或 include guard 二选一（M1 统一） |
| 错误处理 | 库层用 `Result<T, Error>`；不跨模块边界抛异常；`noexcept` 明确 |
| 所有权 | `std::unique_ptr` 默认；裸指针仅观测用 |
| 并发 | 共享状态用消息传递；必须用锁时写清锁顺序 |
| 日志 | 结构化字段，禁止字符串拼接敏感信息 |
| 注释 | 代码注释用英文；对外文档用中文；复杂算法附规范章节号（如 `// CONFLICTS §4.1`） |
| 测试命名 | `ModuleTest.CaseName`，与 TEST_PLAN 用例 ID 对应 |

---

## 12. 关键不变量速查（违反即 S1 缺陷）

| 编号 | 不变量 |
|---|---|
| INV-1 | `revision` 只能服务端 +1 |
| INV-2 | 服务端重算哈希，不信任客户端 |
| INV-3 | `versions`/`entries`/`change_log` 同一事务 |
| INV-4 | 有本地待提交操作时，不得直接覆盖该文件 |
| INV-5 | 删除只写 tombstone |
| INV-6 | 冲突时内容不得丢失 |
| INV-7 | 游标只增不减，失效走全量快照 |
| INV-8 | 自身写入事件必须抑制 |
| INV-9 | 同一 `file_id` 同时最多一个在途提交 |
| INV-10 | 临时文件不进同步目录，替换必须原子 |

---

## 13. 汇报模板（每次任务结束）

```markdown
## 完成内容
- <文件/模块>：<做了什么>

## 编译结果
- 命令：<实际命令>
- 结果：成功 / 失败（附关键错误）

## 测试结果
- 命令：<实际命令>
- 结果：<通过/失败数量>；未运行则写“未运行”

## 文档更新
- <文档>：<变更点>

## 未完成 / 未验证
- <项>：<原因>

## 风险与待决
- <O-xx 或新风险>
```

**禁止**在“测试结果”里写没有实际执行过的内容。

---

## 14. 授权边界

以下操作必须先获得用户明确同意：

- 初始化 Git 仓库、提交、推送、创建 PR；
- 访问网络下载依赖或安装软件包；
- 删除或重命名用户已有文件；
- 修改 CI/CD 或发布配置；
- 对外发布任何内容。

---

## 15. 快速上手（新会话）

1. 读本文件 §1、§2。
2. 读 `docs/STATUS.md`。
3. 读当前里程碑在 `docs/ROADMAP.md` 的章节。
4. 按 §3 流程执行。
