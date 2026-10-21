# SyncBox 开发路线图（M0–M9）

| 项目 | 内容 |
|---|---|
| 文档版本 | v0.1（M0 基线） |
| 日期 | 2026-10-21 |
| 状态 | M0 已完成（2026-10-21），M1 未开始 |
| 关联文档 | [PRD.md](PRD.md) · [ARCHITECTURE.md](ARCHITECTURE.md) · [TEST_PLAN.md](TEST_PLAN.md) · [STATUS.md](STATUS.md) |

---

## 0. 里程碑总览

```
M0 需求与规范固化
 │
 ▼
M1 工程骨架与构建系统
 │
 ▼
M2 核心模型与数据库层
 │
 ├──────────────┐
 ▼              ▼
M3 文件系统与监控   M4 服务端 API 与版本存储
 │              │
 └──────┬───────┘
        ▼
M5 同步引擎（队列、拉取、落盘、冲突、恢复）
        │
        ▼
M6 传输完善（分块、续传、重试、TLS）
        │
        ▼
M7 Qt 桌面界面
        │
        ▼
M8 安全加固与运维
        │
        ▼
M9 性能、跨平台验收与交付
```

| 里程碑 | 主题 | 依赖 | 对应需求 |
|---|---|---|---|
| M0 | 需求与工程规范固化 | — | 全部（文档层） |
| M1 | 工程骨架与构建系统 | M0 | FR-16 |
| M2 | 核心模型与数据库层 | M1 | FR-05、FR-06 |
| M3 | 文件系统适配与监控 | M2 | FR-03、FR-11、FR-12 |
| M4 | 服务端 API 与版本存储 | M2 | FR-02、FR-06、FR-07、FR-09（服务端） |
| M5 | 同步引擎 | M3、M4 | FR-01、FR-04、FR-08、FR-10 |
| M6 | 传输完善 | M5 | FR-09 |
| M7 | Qt 桌面界面 | M5 | FR-13 |
| M8 | 安全加固与运维 | M4、M5 | FR-14、FR-15 |
| M9 | 性能、跨平台验收与交付 | M6、M7、M8 | FR-15、FR-16 |

> M3 与 M4 可并行；M8 的安全基础（路径校验、认证）在 M4 就必须落地，M8 负责加固与验证。

---

## 1. M0 —— 需求与工程规范固化（已完成）

### 目标
建立单一事实来源（single source of truth）：需求、架构、数据库、协议、冲突、安全、测试、路线图全部冻结，后续编码只按规范推进。

### 任务
- [x] 检查现有工程（结论：仅空 `AGENTS.md`，无实现）
- [x] `docs/PRD.md`
- [x] `docs/ARCHITECTURE.md`
- [x] `docs/DATABASE.md`
- [x] `docs/PROTOCOL.md`
- [x] `docs/CONFLICTS.md`
- [x] `docs/ROADMAP.md`
- [x] `docs/TEST_PLAN.md`
- [x] `docs/SECURITY.md`
- [x] `docs/STATUS.md`
- [x] 重写 `AGENTS.md`（文档索引、里程碑纪律、真实性要求）
- [x] 跨文档一致性自检

### 验收
- [x] 9 份文档存在且字段/错误码/状态机互相一致（自检报告见 STATUS §5）
- [x] `AGENTS.md` 能回答：做什么、先读什么、如何验证、禁止什么
- [x] 未实现的功能在所有文档中均标注为“目标/未实现”，无虚假完成声明

### 交付物
`docs/*.md`、`AGENTS.md`。

---

## 2. M1 —— 工程骨架与构建系统

> 状态：**Linux 本机已完成（2026-10-21）**；Windows/macOS 与 CI 待首次运行验证。

### 目标
任何一台干净机器都能一条命令构建、运行、跑测试；三平台 CI 绿。

### 任务
1. 目录结构（见 ARCHITECTURE §2.1）。
2. CMake ≥ 3.22（当前开发机为 3.22.1；若后续依赖要求更高再提升）：
   - 顶层 `CMakeLists.txt`、`cmake/`（CompilerWarnings、Sanitizers、Dependencies、Install）。
   - 目标：`syncbox_core`、`syncbox_proto`、`syncbox_store`、`syncbox_fs`、`syncbox_net`、`syncbox_client`、`syncbox_server`、`syncbox`(CLI)、`syncboxd`、`syncbox-gui`（M7 占位）。
   - C++17，无扩展（`CXX_EXTENSIONS OFF`）。
3. 依赖管理：**已决策使用 vcpkg manifest 模式**（`vcpkg.json`，`builtin-baseline` 锁定提交）。
   - 默认依赖（M1 即启用）：`spdlog`、`gtest`。
   - 其余按里程碑通过 feature 启用：`storage`（sqlite3，M2）、`unicode`（utf8proc，M2）、`networking`（boost-asio，M4）、`json`（nlohmann-json，M4）、`crypto`（openssl，M2/M6）、`gui`（qtbase，M7）。
   - 启用方式：配置时加 `-DVCPKG_MANIFEST_FEATURES="storage;unicode"`（或 `--x-feature`，以所用 vcpkg 版本为准）。
   - 禁止未锁定的 `master` 分支；禁止在 M1 提前安装 Qt。
4. 日志：spdlog 封装 `common/log`，JSON sink 占位。
5. 错误模型：`Result<T, Error>` 或 `std::expected` 兼容封装；统一错误码枚举（与 PROTOCOL §1.5 对应）。
6. 配置：TOML 解析（toml++ 或手写子集），schema 校验。
7. CLI 骨架：`syncbox --version`、`syncboxd --version`、`syncboxd admin --help`。
8. 测试骨架：`ctest`，1 个冒烟测试；GoogleTest 接入。
9. CI：GitHub Actions 三平台矩阵，Debug/Release，ASan/UBSan（Linux Debug）。
10. `.clang-format`、`.clang-tidy`、`.editorconfig`、`.gitignore`。
11. 初始化 Git 仓库（**需用户明确授权**；当前未授权，见 STATUS）。

### 验收
- [x] Linux 环境按 README 构建成功（aarch64，CMake 3.22，vcpkg）。
- [ ] Windows/macOS 全新环境构建成功（待 CI）。
- [x] `ctest --output-on-failure` 全绿（Linux，4/4）。
- [x] `syncbox --version` 输出语义化版本（`SyncBox 0.1.0`）。
- [x] ASan/UBSan 构建下冒烟测试无报错（Linux）。
- [ ] CI 三平台绿，且日志可公开访问。

### 交付物
可构建骨架、CI 配置、README 初版、依赖锁定文件。

---

## 3. M2 —— 核心模型与数据库层

### 目标
版本模型、路径规则、哈希、两套 schema、迁移与 Repository 就绪，并有单元测试。

### 任务
1. `core/version_model`：`FileId`、`OpId`、`Revision`、`Seq`、`ContentHash`、`OpKind`、`EntryState`。
2. `core/path`：§DATABASE 1.1 完整实现（NFC、casefold、保留名、长度）。
3. `core/hash`：OpenSSL EVP 流式 SHA-256（`EVP_MD_CTX`），向量测试。
4. `core/uuid`：UUIDv7 生成（时间有序）与解析。
5. `store/sqlite`：连接封装、WAL、busy_timeout、prepared statement、事务 RAII。
6. `store/migrations`：服务端 v1、客户端 v1（DATABASE §2.2/§3.1 原样）。
7. Repository：`SpaceRepo`、`DeviceRepo`、`EntryRepo`、`VersionRepo`、`ChangeLogRepo`、`OperationRepo`、`BlobRepo`；客户端 `LocalEntryRepo`、`PendingOpRepo`、`ApplyJournalRepo`、`ConflictRepo`。
8. 不变量测试：DB-1..DB-8。

### 验收
- [ ] UT-CORE-*、UT-DB-* 全绿。
- [ ] 两库空库迁移 + 重复迁移幂等。
- [ ] 唯一索引能拒绝大小写冲突。
- [ ] 事务崩溃注入（FI-16 雏形）后 `integrity_check` 通过。

### 交付物
`libsyncbox_core`、`libsyncbox_store`、schema 与测试。

---

## 4. M3 —— 文件系统适配与监控

### 目标
三平台文件监控、扫描、原子替换、临时文件、自身写入抑制。

### 任务
1. `IFileSystemAdapter` + `PathSupport`（大小写敏感、保留名、长度）。
2. Linux：inotify（`inotify_init1`、watch 目录递归、overflow 处理）。
3. Windows：`ReadDirectoryChangesW`（重叠 I/O、缓冲大小、递归）。
4. macOS：FSEvents（`FSEventStreamCreate`、latency、sinceWhen）。
5. Scanner：去抖 300 ms、按 `path_key` 合并、排除 `.syncbox/`。
6. 指纹：size + mtime_ns + inode/file index +（可选）快速哈希。
7. 原子替换与临时文件（CONFLICTS §7.3 实现）。
8. 写抑制表与窗口。
9. 忽略规则（glob）。
10. 重命名检测（inode + 同批次 hash）。

### 验收
- [ ] UT-FS-* 全绿。
- [ ] TC-100..105、TC-110、TC-113、TC-114 通过。
- [ ] FI-13、FI-14 通过（丢事件/溢出后核对补齐）。
- [ ] TC-XP-01/03/06 在各平台通过。
- [ ] 10,000 文件扫描 < 5 s（SSD，冷缓存除外）。

### 交付物
`libsyncbox_fs` + 平台后端 + 扫描器测试。

---

## 5. M4 —— 服务端 API 与版本存储

### 目标
完整的 HTTP 服务端：认证、提交、变更流、快照、blob、WS；事务与幂等正确。

### 任务
1. Beast HTTP 服务器：路由、连接生命周期、超时、请求体上限。
2. 认证中间件：Bearer、token 哈希、恒定时间比较、撤销检查。
3. `POST /enroll`、`GET /me`、`/health`。
4. `OpService`：CAS、恢复阻断、路径校验、版本/日志/幂等单事务。
5. 变更流 `GET /changes`、快照 `GET /snapshot`。
6. Blob 存储：内容寻址目录、manifest、分块上传、整体校验、下载 Range、去重、引用计数。
7. WebSocket Hub：认证、订阅、合并通知、心跳。
8. `POST /ack`。
9. 管理 CLI：创建 space、签发 enrollment token、撤销设备（HTTP 端点 M8）。
10. 服务端配置与启动校验（TLS、权限）。

### 验收
- [ ] PROTOCOL §7 全部验收项。
- [ ] TC-SEC-01/03/10/31/32 通过。
- [ ] FI-16、FI-17、FI-22 通过。
- [ ] 10 并发同基线提交：1 成功 9 冲突。
- [ ] 同一 `op_id` 100 次重放只有 1 个版本。
- [ ] `change_log` 与 `versions` 行数严格对应。

### 交付物
`syncboxd` 可独立运行；服务端集成测试。

---

## 6. M5 —— 同步引擎

### 目标
客户端闭环：扫描→入队→上传→提交→拉取→落盘→游标；冲突与崩溃恢复。

### 任务
1. `SyncEngine` 状态机与执行器（ARCHITECTURE §5.1）。
2. Outbox：入队、串行化、退避、重启复位。
3. Puller：游标、分页、按序应用、与在途操作互斥。
4. Applier：下载、校验、临时文件、原子替换、`apply_journal`。
5. `ConflictResolver`：CONFLICTS §3/§4 全部策略。
6. 冲突副本命名与提交。
7. 启动恢复（ARCHITECTURE §3.3）。
8. 全量核对（ARCHITECTURE §3.4）。
9. WS 客户端 + 轮询兜底。
10. 客户端配置与注册流程。
11. CLI：`login`、`sync`、`status`、`conflicts`、`retry`。

### 验收
- [ ] TC-200..213 全绿。
- [ ] FI-01..05、FI-19 通过。
- [ ] 断网 30 分钟 + kill -9 + 重启，无丢失、无重复。
- [ ] S1–S9 演示场景可复现。
- [ ] 冲突场景内容零丢失（哈希核对）。

### 交付物
`syncbox` CLI 可用；端到端集成测试。

---

## 7. M6 —— 传输完善

### 目标
大文件、弱网、中断续传、TLS、重试与限速达标。

### 任务
1. 分块上传/下载与断点续传（已有基础则补全边界）。
2. 并发块传输（默认 4）与背压。
3. 指数退避 + 抖动统一实现，最大 60 s，10 次后 `dead`。
4. TLS 客户端配置：CA、证书校验、可选 pinning。
5. HTTP Range 下载与并行恢复。
6. 带宽限制（可选，配置）。
7. 失败分类：可重试/不可重试/需人工。

### 验收
- [ ] TC-106、TC-107 通过。
- [ ] FI-06..10 通过。
- [ ] PF-02 达标（100 MiB ≤ 60 s）。
- [ ] 中断 3 次续传成功且最终哈希一致。
- [ ] TLS 关闭/证书错误时明确失败，不降级。

### 交付物
稳定的传输层与基准记录。

---

## 8. M7 —— Qt 桌面界面

### 目标
可用的桌面客户端：状态、进度、失败任务、冲突处理。

### 任务
1. Qt 6 Widgets 主窗口：设备/空间状态、同步进度、最近活动。
2. 任务列表：进行中、失败（含错误码）、重试。
3. 冲突列表：原文件与副本、保留/删除/稍后。
4. 设置：服务器、根目录、忽略规则、日志级别。
5. 与引擎解耦：通过 `EngineFacade`（进程内接口或本地 IPC）。
6. 托盘图标与通知（可选）。
7. 关闭窗口不停止同步；退出选项明确。

### 验收
- [ ] GUI 冒烟测试通过。
- [ ] 关闭 GUI 后同步继续。
- [ ] 冲突列表与 `conflicts` 表一致。
- [ ] 失败任务可一键重试并成功。

### 交付物
`syncbox-gui` 可执行文件 + 截图。

---

## 9. M8 —— 安全加固与运维

### 目标
满足 SECURITY.md 全部验收；具备可运维性。

### 任务
1. `DELETE /v1/devices/{id}` 与撤销即时生效（含 WS 断开）。
2. 路径穿越测试集全量回归 + libFuzzer。
3. 限流、配额、慢速攻击防护。
4. 日志脱敏与审计事件完整。
5. 依赖 CVE 扫描与锁定校验。
6. 备份/恢复脚本、`fsck --rebuild-entries`、GC 命令。
7. 运维文档：部署、TLS、备份、监控。

### 验收
- [ ] TC-SEC-* 全绿。
- [ ] FUZZ 10 分钟无崩溃。
- [ ] 撤销后 1 秒内失效。
- [ ] GC 后旧游标返回 `410`，客户端可恢复。
- [ ] 备份可恢复到新机器并继续同步。

### 交付物
安全测试报告、运维手册。

---

## 10. M9 —— 性能、跨平台验收与交付

### 目标
达到 PRD 全部指标，形成可展示的完整项目。

### 任务
1. PF-01..09 基准与调优（SQLite 索引、批量、并发度）。
2. 三平台完整回归（TEST_PLAN §7）。
3. 打包：Linux `deb`/`tar.gz`、Windows `zip`/MSI（可选）、macOS `dmg`（可选）。
4. README、安装/运行文档、架构图、演示脚本、求职项目说明。
5. 最终一致性审计：文档声明 ↔ 代码 ↔ 测试证据。
6. 发布 v1.0.0 标签（需用户授权）。

### 验收
- [ ] P0 100%、P1 ≥ 95%。
- [ ] PF 全部达标或有明确豁免记录。
- [ ] DEMO-01..07 可一键复现。
- [ ] 文档中每个“已实现”都有测试或命令佐证。
- [ ] 干净机器 20 分钟内完成构建 + 演示。

### 交付物
发布包、演示视频/脚本、最终文档。

---

## 11. 估算与节奏

| 里程碑 | 相对工作量 | 说明 |
|---|---|---|
| M0 | 1 | 文档 |
| M1 | 2 | 构建与依赖最易踩坑 |
| M2 | 3 | 模型与 schema |
| M3 | 5 | 三平台监控差异大 |
| M4 | 5 | 事务与协议 |
| M5 | 6 | 冲突与恢复最复杂 |
| M6 | 3 | 传输边界 |
| M7 | 4 | GUI |
| M8 | 3 | 加固与验证 |
| M9 | 4 | 调优与交付 |

以“一次专注会话完成一个里程碑的子任务”为节奏；**不跨里程碑并行编码**，除非任务明确标注可并行（M3/M4）。

---

## 12. 风险与缓解

| 风险 | 影响 | 缓解 |
|---|---|---|
| 三平台监控语义差异导致事件丢失 | 同步延迟/漏同步 | 周期核对兜底；平台专项测试 |
| 冲突策略实现偏离文档 | 数据丢失 | 冲突用例作为回归门禁；文档即测试规格 |
| SQLite 写锁竞争 | 吞吐不达标 | 短事务、单写者、批量提交 |
| 依赖获取失败（离线环境） | 无法构建 | 提供 vendor 目录或镜像说明 |
| 范围蔓延（E2EE、移动端） | 无法交付 | PRD §7 明确不做 |
| 文档与实现漂移 | 项目可信度下降 | AGENTS.md 强制“改代码即改文档”；M9 审计 |

---

## 13. 下一步：M1 任务清单（可直接执行）

1. 与用户确认是否初始化 Git 仓库与首选依赖管理方式（FetchContent vs vcpkg）。
2. 创建目录骨架与顶层 `CMakeLists.txt`。
3. 接入 Boost、SQLite、OpenSSL、spdlog、GoogleTest、nlohmann/json、utf8proc。
4. 建立 `syncbox_core` 的最小编译单元（版本号、错误码）。
5. 建立 `syncbox` / `syncboxd` 可执行文件骨架（`--version`）。
6. 接入 GoogleTest + CTest，写 1 个冒烟测试。
7. 建立 CI 三平台矩阵。
8. 更新 `docs/STATUS.md` 与 `AGENTS.md` 的构建命令段。
