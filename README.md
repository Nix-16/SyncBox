# SyncBox

C++17 跨平台双向文件同步工具（Windows / Linux / macOS），中心服务器架构。

目标是：**不丢数据、离线可用、崩溃可恢复**。任何无法安全合并的修改都会保留冲突副本，绝不静默覆盖用户文件。

## 当前状态

> **M4 服务端进行中：HTTP/认证/提交/变更流已实现；Blob/WS 与同步客户端未完成。**

- 需求、架构、数据库、协议、冲突策略、安全、测试计划与路线图已冻结（见 `docs/`）。
- 已实现：版本模型、路径规范化（NFC/casefold）、流式 SHA-256、UUIDv7、
  两套 SQLite schema/迁移、Repository、文件扫描/差分、原子替换、
  inotify / ReadDirectoryChangesW / FSEvents 监控、服务端 HTTP/认证/提交/变更流/Blob/WebSocket。
- `syncboxd` 可启动 HTTP 服务并提供设备注册/提交/变更流；`syncbox` 仍为骨架。
- 61 个测试通过（含 HTTP 集成与进程崩溃注入）。
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

前置：CMake ≥ 3.22、Ninja、支持 C++17 的编译器、[vcpkg](https://github.com/microsoft/vcpkg)（manifest 模式，基线已在 `vcpkg.json` 锁定）。

```bash
git clone https://github.com/microsoft/vcpkg.git ~/vcpkg
~/vcpkg/bootstrap-vcpkg.sh

export VCPKG_ROOT=~/vcpkg
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
```

产物：`build/dev/syncbox`、`build/dev/syncboxd`。

当前里程碑为 M1，两个可执行文件仅支持 `--version` / `--help`，**尚不具备同步能力**。

Windows 下若未安装 Ninja，可改用 Visual Studio 生成器：

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" `
  -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
```

## 许可

尚未确定。
