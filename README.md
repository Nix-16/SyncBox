# SyncBox

C++17 跨平台双向文件同步工具（Windows / Linux / macOS），中心服务器架构。

目标是：**不丢数据、离线可用、崩溃可恢复**。任何无法安全合并的修改都会保留冲突副本，绝不静默覆盖用户文件。

## 当前状态

> **M0 已完成，尚未开始编码。仓库中没有任何可运行代码。**

- 需求、架构、数据库、协议、冲突策略、安全、测试计划与路线图已冻结（见 `docs/`）。
- 构建系统、源代码与测试将在 M1 开始建立。
- 真实进度以 [docs/STATUS.md](docs/STATUS.md) 为准，本文档不重复其结论。

## 文档

| 文档 | 内容 |
|---|---|
| [docs/PRD.md](docs/PRD.md) | 产品需求、使用场景、非功能指标 |
| [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) | 模块划分、依赖关系、关键数据流 |
| [docs/DATABASE.md](docs/DATABASE.md) | 客户端/服务端数据库 schema 与事务 |
| [docs/PROTOCOL.md](docs/PROTOCOL.md) | HTTP/WebSocket 协议、错误码、幂等 |
| [docs/CONFLICTS.md](docs/CONFLICTS.md) | 版本模型与冲突处理策略 |
| [docs/SECURITY.md](docs/SECURITY.md) | 威胁模型、认证、TLS、路径安全 |
| [docs/TEST_PLAN.md](docs/TEST_PLAN.md) | 单元/集成/故障注入/性能测试 |
| [docs/ROADMAP.md](docs/ROADMAP.md) | M0–M9 里程碑与验收标准 |
| [docs/STATUS.md](docs/STATUS.md) | 当前真实状态与未决问题 |
| [AGENTS.md](AGENTS.md) | 工程协作规则（开发前必读） |

## 计划中的技术栈

C++17 · CMake · Boost.Asio/Beast · SQLite · OpenSSL · spdlog · GoogleTest · Qt 6（GUI）
文件监控：inotify（Linux）/ ReadDirectoryChangesW（Windows）/ FSEvents（macOS）

## 构建

M1 完成后补充。当前仓库没有 `CMakeLists.txt`，无法构建。

## 许可

尚未确定。
