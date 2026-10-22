# SyncBox 当前状态

| 项目 | 内容 |
|---|---|
| 报告日期 | 2026-10-21 |
| 当前里程碑 | **M2 完成（三平台 CI 全绿）；M3 未开始** |
| 代码状态 | core 模型/路径/哈希 + 两库 schema/Repository；**无同步引擎** |
| 构建状态 | **可构建**：CMake 3.22 + vcpkg（storage/unicode/crypto features） |
| 测试状态 | 34 个单元测试；CI 7/7 job 通过（三平台 Debug/Release + ASan/UBSan） |
| 版本控制 | Git 已初始化，远端 `github.com/Nix-16/SyncBox` |

> 本文档是项目真实状态的唯一依据。任何“已完成”的声明必须在此处有对应条目与证据（文件路径、命令输出、测试名称）。**当前没有任何同步功能可用。**

---

## 1. 仓库实际内容

M0 开始时的检查结果：仓库仅有一个 0 字节的 `AGENTS.md`，无 README、构建系统、源码、测试、`docs/`，也不是 Git 仓库。

M0 交付后的内容：

| 路径 | 说明 |
|---|---|
| `README.md` | 项目简介、状态声明、文档索引 |
| `AGENTS.md` | 工程协作规则（M0 重写） |
| `.gitignore` | 构建产物与 IDE 文件忽略 |
| `docs/PRD.md` | 需求基线 |
| `docs/ARCHITECTURE.md` | 架构设计 |
| `docs/DATABASE.md` | 数据库设计 |
| `docs/PROTOCOL.md` | 协议契约 |
| `docs/CONFLICTS.md` | 版本与冲突策略 |
| `docs/ROADMAP.md` | 里程碑计划 |
| `docs/TEST_PLAN.md` | 测试计划 |
| `docs/SECURITY.md` | 安全设计 |
| `docs/STATUS.md` | 本文件 |
| `CMakeLists.txt` | 顶层构建（M1） |
| `CMakePresets.json` | `dev` / `release` / `asan` 预设 |
| `vcpkg.json` | vcpkg manifest，baseline 锁定 |
| `cmake/CompilerWarnings.cmake` | 编译警告策略 |
| `include/syncbox/core/` | `version`、`error`、`result`、`uuid`、`version_model`、`path`、`hash` |
| `src/core/` | 上述 core 模块实现 |
| `include/syncbox/store/` | `database`、`migrations`、`server_store`、`client_store` |
| `src/store/` | SQLite 封装、两库迁移、服务端/客户端 Repository |
| `apps/syncbox/`、`apps/syncboxd/` | CLI 骨架（`--version`/`--help`） |
| `tests/unit/` | 34 个 GoogleTest 用例（core + store + 崩溃注入） |
| `tests/helpers/` | `crash_writer`（FI-16 子进程夹具） |
| `.clang-format` | 代码格式化规则（M1 遗留项，M2 补上） |
| `.github/workflows/ci.yml` | 三平台 CI（M1 时 7/7 通过；M2 变更尚未运行） |

**仍然不存在**：`proto/`、`fs/`、`net/`、`client/`、`server/` 模块，Qt GUI，任何同步业务代码。

Git 仓库已初始化（`main` 分支），远端 `origin` 指向 `github.com/Nix-16/SyncBox`。

---

## 2. 功能状态总表

状态定义：

- **未实现**：没有任何代码。
- **设计完成**：规范已冻结，代码不存在。
- **部分实现**：有代码但不满足验收。
- **已实现**：有代码 + 自动化测试通过。
- **已验证**：已实现 + 对应测试在 CI 三平台通过。

| 需求 | 功能 | 设计 | 实现 | 测试 | 结论 |
|---|---|---|---|---|---|
| FR-01 | 设备注册与配置 | ✅ PRD/SECURITY/PROTOCOL | ❌ | ❌ | 未实现 |
| FR-02 | 中心服务器通信 | ✅ ARCHITECTURE/PROTOCOL | ❌ | ❌ | 未实现 |
| FR-03 | 文件监控与索引 | ✅ ARCHITECTURE §4.1 | ❌ | ❌ | 未实现 |
| FR-04 | 双向同步（增改删重命名） | ✅ CONFLICTS | ❌ | ❌ | 未实现 |
| FR-05 | 元数据持久化 | ✅ DATABASE | ⚠️ schema + Repository | ✅ UT-DB | 部分实现（无同步引擎使用） |
| FR-06 | 版本管理与幂等 | ✅ CONFLICTS/PROTOCOL | ⚠️ 模型与约束 | ✅ 模型/幂等约束测试 | 部分实现（服务端 CAS 未实现） |
| FR-07 | 变更日志与游标 | ✅ DATABASE/PROTOCOL | ⚠️ 日志表与游标 | ✅ seq/游标测试 | 部分实现（拉取端点未实现） |
| FR-08 | 冲突检测与副本 | ✅ CONFLICTS | ❌ | ❌ | 未实现 |
| FR-09 | 分块、校验、续传、重试 | ✅ PROTOCOL §3.9 | ❌ | ❌ | 未实现 |
| FR-10 | 持久化队列与恢复 | ✅ DATABASE §3.2/ARCH §3.3 | ❌ | ❌ | 未实现 |
| FR-11 | 临时文件与安全替换 | ✅ ARCHITECTURE §4.7 | ❌ | ❌ | 未实现 |
| FR-12 | 三平台文件系统适配 | ✅ ARCHITECTURE §2 | ❌ | ❌ | 未实现 |
| FR-13 | Qt 桌面界面 | ✅ ROADMAP M7（概要） | ❌ | ❌ | 未实现 |
| FR-14 | 认证/TLS/路径安全/撤销 | ✅ SECURITY | ❌ | ❌ | 未实现 |
| FR-15 | 测试体系 | ✅ TEST_PLAN | ⚠️ 34 个单元测试 | ✅ 单元层 | 部分实现（无集成/性能测试） |
| FR-16 | 构建/文档/演示 | ✅ 部分（M0 文档） | ⚠️ 构建骨架 | ✅ 34 项 | 部分实现（无演示） |

**没有任何同步功能可以声称“可用”。**

---

## 3. 代码资产清单

| 类别 | 数量 | 说明 |
|---|---|---|
| C++ 源文件 | 13 | core 6、store 4、apps 2、测试夹具 1 |
| 头文件 | 12 | core 7、store 4、`test_support.hpp` 1 |
| CMake 文件 | 3 | 顶层、`tests/`、警告模块 |
| 测试文件 | 8 | 单元测试 8 个 `.cpp`，共 34 个用例 |
| CI 配置 | 1 | GitHub Actions 三平台矩阵 + sanitizer |
| 数据库 schema | 2 套 | 内嵌于 `migrations.cpp`（服务端/客户端 v1） |
| 可执行产物 | 3 | `syncbox`、`syncboxd`、测试用 `syncbox_crash_writer` |

### M1 验证证据（2026-10-21，Linux aarch64）

| 项 | 实际命令 | 实际结果 |
|---|---|---|
| 配置 | `VCPKG_FORCE_SYSTEM_BINARIES=1 VCPKG_ROOT=~/vcpkg cmake --preset dev` | 成功；vcpkg 安装 spdlog 1.17.0、gtest 1.17.0 |
| 编译 | `cmake --build --preset dev` | 成功，9 个构建步骤 |
| 测试 | `ctest --preset dev --output-on-failure` | 4/4 通过 |
| ASan/UBSan | `cmake --preset asan && cmake --build --preset asan && ctest --preset asan` | 4/4 通过 |
| 版本命令 | `./build/dev/syncbox --version` / `./build/dev/syncboxd --version` | `SyncBox 0.1.0` / `SyncBox server 0.1.0` |

> 环境说明：本机 vcpkg 版本要求 CMake 4.3.2，而 GitHub/Kitware 下载被限速到 ~50 KB/s，无法在合理时间内完成下载。因此本地验证使用 `VCPKG_FORCE_SYSTEM_BINARIES=1` 复用系统 CMake 3.22，并且只安装 M1 所需的 spdlog/gtest。CI 使用 `lukka/run-vcpkg` 正常路径。

CI 运行（run [39631445766](https://github.com/Nix-16/SyncBox/actions/runs/39631445766)，提交 `34ed78a`）：**7/7 job 成功** —— Ubuntu Debug/Release、Windows Debug/Release、macOS-14 Debug/Release、ASan+UBSan。三平台构建与冒烟测试均已真实验证。

> 首次运行（run `39630110288`）中 `macos-13` 排队 9 分钟未分配 runner，已切换为 `macos-14`；该旧 run 仍显示 queued，已被新提交取代。

### M2 验证证据（2026-10-22，Linux aarch64）

| 项 | 实际命令 | 实际结果 |
|---|---|---|
| 配置 | `VCPKG_FORCE_SYSTEM_BINARIES=1 VCPKG_ROOT=~/vcpkg cmake --preset dev` | 成功；vcpkg 安装 openssl 3.6.2、sqlite3 3.53.1、utf8proc 2.11.3 |
| 编译 | `cmake --build --preset dev` | 成功 |
| 测试 | `ctest --preset dev --output-on-failure` | **34/34 通过** |
| ASan/UBSan | `cmake --preset asan && cmake --build --preset asan && ctest --preset asan` | **34/34 通过** |
| 崩溃注入 | `StoreCrashTest.UncommittedTransactionDoesNotSurviveProcessDeath` | 通过（子进程事务内 `_Exit`，重启后行未提交、`integrity_check=ok`） |

> 环境说明：本机访问 GitHub 被限速/中断，OpenSSL 与 utf8proc 源码通过镜像下载后放入 vcpkg 下载缓存，由 vcpkg 校验哈希后使用。该操作不改变依赖版本与 manifest。

CI 运行（run [39813188841](https://github.com/Nix-16/SyncBox/actions/runs/39813188841)，提交 `ce19b11`）：**7/7 job 成功** —— Ubuntu Debug/Release、Windows Debug/Release、macOS-14 Debug/Release、ASan+UBSan。

> Windows 首轮失败定位过程：`StoreCrashTest` 在 cmd.exe 下因首尾引号被剥离而无法启动 helper；已通过整串加引号修复。CI 现会在失败时把测试结果输出为注解。

---

## 4. 已知风险

| 编号 | 风险 | 影响 | 当前处置 |
|---|---|---|---|
| R-01 | 仅有 core/store，同步功能未实现 | 无法演示同步、无法验证设计 | M3 起按里程碑实现 |
| R-02 | CI 无 vcpkg 二进制缓存 | 每次构建重复安装依赖，耗时较长 | 后续加 `actions/cache` 或 GHCR 缓存 |
| R-03 | 本机访问 GitHub 受限速/中断 | 依赖安装可能失败 | 预置 vcpkg 下载缓存；CI 网络正常 |
| R-04 | 三平台监控语义差异 | 事件丢失/重复 | 设计已含核对兜底；M3 专项测试 |
| R-05 | 冲突策略复杂 | 实现偏离规范 | 以 CONFLICTS §9 示例作为回归用例 |
| R-06 | Windows 仅有 CI 环境，无本地开发机 | 平台问题只能靠 CI 与注解定位 | 失败注解 + 小步提交 |
| R-07 | 文档量大 | 可能与实现漂移 | AGENTS.md 强制同步更新 + M9 审计 |
| R-08 | SQLite 单写者 | 服务端吞吐上限 | 短事务；M9 基准验证 |
| R-09 | 无 E2EE | 服务端可读明文 | 已在 PRD/SECURITY 明示，v2 议题 |
| R-10 | 性能指标未验证 | 目标可能不现实 | M6/M9 实测并记录偏差 |

---

## 5. M0 跨文档一致性自检

自检方法：人工交叉核对字段名、错误码、状态机、枚举值、数值阈值。

| 检查项 | 涉及文档 | 结果 |
|---|---|---|
| `file_id/revision/parent_revision/op_id/seq` 语义 | 全部 | ✅ 一致 |
| `seq` 为服务端全局单调（非每空间） | PRD/ARCH/DB/PROTOCOL | ✅ 一致 |
| 操作类型枚举 `create/update/delete/rename/restore` | DB/PROTOCOL/CONFLICTS | ✅ 一致 |
| 错误码表 | PROTOCOL/SECURITY/TEST | ✅ 引用一致 |
| `path_key` 唯一性仅约束活跃条目 | DB/ARCH/CONFLICTS | ✅ 一致 |
| Tombstone 与 `RESURRECT_BLOCKED` 规则 | ARCH/DB/PROTOCOL/CONFLICTS | ✅ 一致 |
| 冲突策略 P-C/P-D/P-R/P-M/P-N | CONFLICTS/ARCH | ✅ 一致（ARCH 仅概述） |
| 客户端状态机枚举 | DB/CONFLICTS/ARCH | ✅ 一致 |
| 性能指标数值 | PRD/TEST_PLAN/ROADMAP | ✅ 一致 |
| 安全验收项编号 | SECURITY/TEST_PLAN | ✅ 一致 |
| 里程碑与需求映射 | ROADMAP/PRD | ✅ 一致 |
| 文档声明“未实现” | STATUS/全部文档 | ✅ 一致 |

### 5.1 自检中修正的问题

| 问题 | 处理 |
|---|---|
| 初稿曾把“删除 vs 修改”写成冲突副本 | 统一为“修改胜出 + restore”，见 CONFLICTS §4.3 |
| 初稿 `pending_ops.file_id` 对 create 为 NULL，无法满足唯一索引 | 采用部分索引 `WHERE file_id IS NOT NULL`，见 DATABASE §3.1 |
| 初稿 `conflicts.resolution` 无“restore”取值 | 枚举增加 `restored`，见 DATABASE §3.1 与 CONFLICTS §4.3 |
| 初稿 `seq` 曾设计为每空间独立计数器 | 统一为全局 `AUTOINCREMENT`，简化并保证原子性 |
| 依赖管理方式未定 | M1 决策为 vcpkg manifest，基线锁定于 `vcpkg.json` |
| 初稿 `update` 允许同时改路径 | 改为“移动 + 编辑 = rename + update 两条操作”，见 CONFLICTS V-5 |

### 5.2 仍未闭环的设计问题（需后续里程碑决定）

| 编号 | 问题 | 影响范围 | 建议决策点 |
|---|---|---|---|
| O-02 | Unicode 折叠实现：utf8proc vs Boost.Locale/ICU | core、server | M1–M2 |
| O-03 | 客户端 create 在服务端分配 `file_id` 前使用何种本地临时 ID | client、DB | M2 |
| O-04 | 快照分页如何保证同一 `snapshot_seq` 的一致视图（临时表 vs 重复查询） | M4 | M4 |
| O-05 | 重命名检测时间窗与相似度阈值 | M3 | M3 实测后定 |
| O-06 | GUI 与引擎通信方式：进程内 facade vs 本地 socket | M7 | M7 |
| O-07 | blob GC 与在途上传的竞态处理（租约/水位） | M4/M8 | M4 |
| O-08 | 服务端 blob 是否整体拼接落盘，还是始终按块存储 | M4 | M4 |
| O-09 | Windows 长路径（`\\?\`）策略与打包要求 | M3/M9 | M3 |
| O-10 | 自签名 CA 的演示流程与文档 | M8/M9 | M8 |
| O-11 | 客户端 token 轮换机制（SECURITY 残余风险） | M8 | M8 |
| O-12 | M3 验收原引用 TC-100..114（需要服务端+同步引擎，M5 才具备） | ROADMAP/TEST_PLAN | M3 门禁改为 UT-FS/TC-FS；同步用例在 M5 回归（本次已修订 ROADMAP M3） |

> 这些问题不影响 M0 文档作为基线成立；实现时必须在对应文档中补充决策并更新本表。

---

## 6. 与需求的差距（明确记录，不掩盖）

| 需求 | 差距 |
|---|---|
| 可运行的同步工具 | 仅有 CLI 骨架，无同步能力 |
| 三平台支持 | 三平台构建与冒烟测试已由 CI 验证；平台文件监控尚未实现（M3） |
| 服务端 | 仅有 `--version` 骨架，无 HTTP 服务 |
| 数据库 | 两套 schema、迁移与 Repository 已实现并测试；尚未被同步引擎使用 |
| 协议 | 仅文档，无实现 |
| 测试 | 34 个单元测试通过；无集成/故障注入（除崩溃雏形）/性能测试 |
| 性能 | 无任何测量 |
| 安全 | 无实现、无审计 |
| 文档 | 设计文档、README、构建说明已有；演示脚本仍缺失 |

---

## 7. 下一步（M3）

M2 本机已完成；待办与 M3 任务：

1. 推送后检查 GitHub Actions 三平台结果；修复平台差异。
2. `fs/`：`IFileSystemAdapter`、`PathSupport`、临时文件与原子替换。
3. Linux inotify、Windows ReadDirectoryChangesW、macOS FSEvents 后端。
4. Scanner：去抖、按 `path_key` 合并、`.syncbox/` 排除、指纹比对。
5. 写抑制表与自写事件回环防护（ARCHITECTURE §4.1）。
6. 重命名检测（inode/file index + 同批次哈希）。
7. 对应 UT-FS-*、TC-100..105、TC-110、TC-113、TC-114、FI-13、FI-14 测试。

**在 M3 验收前，不得开始服务端 API、网络或同步引擎实现。**
