# SyncBox 架构设计

| 项目 | 内容 |
|---|---|
| 文档版本 | v0.1（M0 基线） |
| 日期 | 2026-10-21 |
| 状态 | 已冻结，M1 起按此实现 |
| 关联文档 | [PRD.md](PRD.md) · [DATABASE.md](DATABASE.md) · [PROTOCOL.md](PROTOCOL.md) · [CONFLICTS.md](CONFLICTS.md) · [ROADMAP.md](ROADMAP.md) |

> 本文档描述**目标架构**。当前仓库无任何实现，见 [STATUS.md](STATUS.md)。

---

## 1. 总体结构

```
┌──────────────────────────── 客户端（每台设备） ────────────────────────────┐
│                                                                            │
│  ┌────────────┐   ┌─────────────┐   ┌──────────────┐   ┌────────────────┐  │
│  │  GUI (Qt)  │   │  CLI        │   │  Daemon      │   │  Config        │  │
│  │   M7       │   │  syncbox    │   │  syncbox-cli │   │  TOML/JSON     │  │
│  └─────┬──────┘   └──────┬──────┘   └──────┬───────┘   └───────┬────────┘  │
│        └─────────── IPC / 进程内接口 ────────┘                 │           │
│                              │                                 │           │
│                    ┌─────────▼─────────────────────────────────▼────────┐  │
│                    │              SyncEngine（同步引擎）                   │  │
│                    │  ┌──────────┐ ┌──────────┐ ┌──────────┐ ┌─────────┐ │  │
│                    │  │ Scanner  │ │ Outbox   │ │ Puller   │ │ Applier │ │  │
│                    │  │ 扫描/去抖 │ │ 待办队列  │ │ 拉取变更  │ │ 落盘    │ │  │
│                    │  └────┬─────┘ └────┬─────┘ └────┬─────┘ └────┬────┘ │  │
│                    │       │            │            │            │      │  │
│                    │  ┌────▼────────────▼────────────▼────────────▼────┐ │  │
│                    │  │  ConflictResolver · VersionModel · Validator  │ │  │
│                    │  └─────────────────────────┬──────────────────────┘ │  │
│                    └───────────┬────────────────┼────────────────────────┘  │
│                                │                │                           │
│             ┌──────────────────▼──┐   ┌─────────▼──────────┐                │
│             │ FileSystemAdapter   │   │ LocalStore (SQLite)│                │
│             │ inotify/RDCW/FSEvents│   │ 索引/队列/游标       │                │
│             └─────────────────────┘   └────────────────────┘                │
│                                │                                            │
│             ┌──────────────────▼──────────────────────────┐                 │
│             │ NetClient (Beast HTTP + WebSocket + TLS)    │                 │
│             └──────────────────┬──────────────────────────┘                 │
└────────────────────────────────┼────────────────────────────────────────────┘
                                 │ HTTPS / WSS
┌────────────────────────────────▼────────────────────────────────────────────┐
│                          中心服务器 syncboxd                                 │
│  ┌──────────────┐  ┌───────────────┐  ┌──────────────┐  ┌────────────────┐  │
│  │ HTTP Router  │─▶│ AuthService   │  │ OpService    │  │ BlobService    │  │
│  │ Beast        │  │ token/撤销     │  │ CAS+版本+日志 │  │ 分块/校验/GC    │  │
│  └──────┬───────┘  └───────────────┘  └──────┬───────┘  └───────┬────────┘  │
│         │                                    │                  │           │
│         │        ┌───────────────────────────▼──────────────────▼──────┐    │
│         └───────▶│              ServerStore (SQLite, WAL, 单写者)        │    │
│                  │  spaces/devices/entries/versions/change_log/blobs   │    │
│                  └─────────────────────────────────────────────────────┘    │
│         ┌──────────────────────────┐                                        │
│         │ Notifier (WebSocket Hub) │                                        │
│         └──────────────────────────┘                                        │
│         ┌──────────────────────────┐                                        │
│         │ Blob FS (内容寻址目录)      │                                        │
│         └──────────────────────────┘                                        │
└─────────────────────────────────────────────────────────────────────────────┘
```

## 2. 模块职责与依赖

### 2.1 目标目录结构（M1 建立）

```
SyncBox/
├── CMakeLists.txt
├── cmake/                      # 工具链、依赖、警告、sanitizer
├── include/syncbox/
│   ├── core/                   # 版本模型、路径、名称、哈希、错误、结果类型
│   ├── proto/                  # JSON 编解码、协议消息类型
│   ├── fs/                     # FileSystemAdapter 抽象 + 平台实现
│   ├── store/                  # SQLite 封装、迁移、Repository
│   ├── net/                    # HTTP/WS 客户端与服务端封装
│   ├── client/                 # SyncEngine 及其子组件
│   ├── server/                 # 服务端服务层
│   └── common/                 # 日志、配置、节流、重试、时间
├── src/                        # 与 include 对应的实现
├── apps/
│   ├── syncbox/                # 客户端 CLI（M1 起，M5 完整）
│   ├── syncboxd/               # 服务端
│   └── syncbox-gui/            # Qt6 Widgets（M7）
├── tests/
│   ├── unit/  integration/  fault/  bench/
├── docs/
└── scripts/                    # 演示、打包、演示数据生成
```

### 2.2 模块与依赖方向

| 模块 | 职责 | 允许依赖 | 禁止依赖 |
|---|---|---|---|
| `core` | `file_id/revision/op_id/seq` 值对象、路径规范化、SHA-256、错误码、状态枚举 | 标准库、OpenSSL、utf8proc | 网络、SQLite、平台 API |
| `proto` | JSON ↔ 协议 DTO、字段校验、错误信封 | `core`、nlohmann/json | 业务逻辑、数据库 |
| `store` | SQLite 封装、迁移、事务助手、Repository | `core` | 网络、Qt、文件监控 |
| `fs` | `IFileSystemAdapter`（扫描、监控、原子替换、临时文件） | `core` | `client`、网络 |
| `net` | HTTP 请求/响应、WS、TLS、重试 | `core`、`proto`、Boost.Asio/Beast | `client`、`store` |
| `client` | 同步引擎编排 | `core` `proto` `store` `fs` `net` | Qt |
| `server` | 服务端业务 | `core` `proto` `store` `net`（服务端） | `fs` 客户端实现、Qt |
| `gui` | Qt 展示层 | 仅 `client` 的只读接口 / IPC | `fs`、`store`、`net` 直接调用 |

**依赖规则**：`core` 不依赖任何上层；`client` 与 `server` 不互相依赖；GUI 不直接操作文件系统。违反规则的 PR 在 Code Review 阶段拒绝（见 [AGENTS.md](../AGENTS.md)）。

### 2.3 核心类草图（M2/M5 落地）

```cpp
// core/version_model.h
struct FileId   { std::string value; };          // UUIDv7
struct OpId     { std::string value; };          // UUIDv7
struct Revision { std::int64_t value; };         // >= 0, 0 = 不存在
struct Seq      { std::int64_t value; };         // 服务端全局单调
struct ContentHash { std::array<std::uint8_t,32> bytes; }; // SHA-256
enum class OpKind { Create, Update, Delete, Rename, Restore };

struct EntryState {          // 服务端 entries 的内存投影
  FileId fileId; Revision revision; std::string path;
  std::optional<ContentHash> hash; std::int64_t size;
  bool deleted; Seq updatedSeq;
};

// fs/platform.h
class IFileSystemAdapter {
public:
  virtual ScanResult         scan(const std::filesystem::path& root) = 0;
  virtual void               startWatch(const std::filesystem::path& root,
                                        IWatchSink& sink) = 0;
  virtual ApplyResult        atomicReplace(const std::filesystem::path& target,
                                           const std::filesystem::path& tempFile,
                                           const ContentHash& expected) = 0;
  virtual TempFile           createTempFile(const std::filesystem::path& root) = 0;
  virtual void               suppress(const std::string& pathKey, Duration) = 0;
  virtual PathSupport        pathSupport() const = 0;  // 大小写、保留名、长度
};
```

## 3. 关键数据流

### 3.1 本地变更上行

```
文件系统事件
   │
   ▼ (去抖 300ms + 按 path_key 合并)
Scanner：stat + 快速指纹（size, mtime_ns, ino/file_index）
   │
   ├─ 与 local_entries 一致 ──────────────▶ 丢弃（事件回环抑制）
   ├─ 在 write_suppressions 窗口内 ────────▶ 丢弃（自身写入）
   ▼
Outbox：生成 op_id，写 pending_ops（state=staged），内容暂存 .syncbox/tmp/<op_id>.part
   │  (INV-9: 同一 file_id 串行)
   ▼
Blob 上传（>8MiB 分块）→ 服务端校验 → blob complete
   │
   ▼
POST /v1/spaces/{id}/ops  (base_revision, op_id, client_seq)
   │
   ▼ 服务端事务：CAS → versions → entries → change_log(seq) → operations
   │
   ▼ 200 {revision, seq}
本地：更新 local_entries.revision，删除 pending_ops 行，删除暂存文件
```

### 3.2 远端变更下行

```
WS {"type":"change","seq":4311}  或  30s 轮询
   │  （仅提示，不携带内容）
   ▼
Puller: GET /v1/spaces/{id}/changes?since=cursor&limit=200
   │
   ▼ 按 seq 升序逐条：先查本地是否有同 file_id 的在途操作
   │   ├─ 有 → 暂停该文件的下行，等待上行结果（INV-4）
   │   └─ 无 → Applier
   ▼
Applier（每个文件一个 apply_journal 记录）：
   write_suppressions[path] = now + 2s
   下载/校验 blob（Range 续传，SHA-256 校验）
   写 temp → fsync → atomicReplace → 目录 fsync
   更新 local_entries（revision, hash, path）
   冲突时 → ConflictResolver（见 CONFLICTS.md）
   ▼
推进 cursor（local_entries 全部落盘并 COMMIT 后）
POST /v1/devices/{id}/ack  {last_seq}
```

### 3.3 启动与崩溃恢复

```
进程启动
 ├─ 打开 SQLite（WAL），执行迁移
 ├─ 读 apply_journal 中 state != done：
 │    · temp 存在且 hash 匹配 → 完成替换 → done
 │    · temp 存在且 hash 不匹配 → 删除 temp → 重新下载
 │    · 目标已是期望 hash → done
 ├─ 读 pending_ops 中 state in (uploading, committing) → 复位为 retry
 │    （op_id 不变，依赖服务端幂等）
 ├─ 读 state=staged 且 staged_path 缺失 → 重新扫描该路径
 └─ 启动引擎：先 Pull 到最新，再按依赖推送 Outbox
```

### 3.4 全量核对（Reconcile）

触发：启动后首次联网、游标失效（`410 CURSOR_EXPIRED`）、周期性（24h）、用户手动。

1. `GET /snapshot` 分页拉取（含 tombstone），记录 `snapshot_seq`。
2. 与本地 `local_entries` + 磁盘扫描结果做三方比对：
   - 服务端有、本地无 → 下载；
   - 本地有、服务端无 → 上传（create）；
   - 都在但 hash 不同 → 以 revision/时间判定并走冲突策略；
   - 服务端 tombstone、本地存在 → 若本地内容与 tombstone 最后 hash 相同则删除；不同则按 modify/delete 策略。
3. 推进游标至 `snapshot_seq`，之后继续增量拉取。

## 4. 关键设计决策

### 4.1 如何避免文件监控事件反馈循环

四层防护，全部在 `Scanner`/`Applier` 内实现：

1. **写前声明**：Applier 在写入前向 `write_suppressions` 插入 `(path_key, now+2s, reason=apply)`，替换完成并更新索引后保留到窗口结束。
2. **指纹比对**：事件到达时比较磁盘指纹（size、mtime、inode/file_index）与 `local_entries`；一致则丢弃。这覆盖“窗口过期后事件才到”的延迟场景。
3. **事件只触发扫描、不直接入队**：watcher 回调只做 `markDirty(path)`，真正决策在去抖后的 Scanner 中，避免事件风暴。
4. **自写文件名隔离**：临时文件一律位于 `<root>/.syncbox/tmp/`，该目录被 watcher 与 Scanner 显式排除；不存在“先写目标路径再改”的中间态。

另加保护：若某 `path_key` 在 5 秒内产生 > 50 次写入且 hash 未变，记录告警并临时提高去抖到 1 秒。

### 4.2 如何保证服务端版本提交与变更日志一致

- `versions`、`entries`、`change_log`、`operations`、`blob.ref_count` 的写入在**同一个 `BEGIN IMMEDIATE` 事务**中完成（INV-3）。
- `seq` 由 `change_log.seq INTEGER PRIMARY KEY AUTOINCREMENT` 在事务内分配，天然单调；回滚会丢弃该 seq（允许空洞，游标使用 `>` 语义）。
- 提交成功后才向 Notifier 投递通知；通知失败不影响一致性（客户端有轮询兜底）。
- `entries` 是 `versions` + `change_log` 的物化视图，可由管理命令 `syncboxd fsck --rebuild-entries` 从 `versions` 重算并比对。

### 4.3 多客户端基于同一旧版本提交

- 服务端以 `base_revision` 做 CAS（INV-1）：第一个提交把 revision 推到 n+1，其余收到 `409 REVISION_CONFLICT`，响应体携带当前服务端状态。
- 客户端拿到 409 后进入 `ConflictResolver`：
  - 本地无实质内容变化（仅元数据）→ 直接接受远端；
  - 双方内容不同 → 本地内容写入冲突副本（新 `file_id`，新 create）并提交，随后应用远端版本；
  - 详见 [CONFLICTS.md](CONFLICTS.md) §4。
- 冲突副本的 create 使用新的 `op_id`，因此不会被幂等重放误判。

### 4.4 离线客户端重连后补齐

- 客户端持久化 `cursor` 与 `pending_ops`；离线期间只是把新操作追加到队列。
- 重连顺序：认证 → 拉取 `changes?since=cursor`（先补齐服务端已发生的）→ 推送本地队列（带 `client_seq`）。
- 若服务端返回 `410 CURSOR_EXPIRED`（变更日志已 GC），执行 §3.4 全量核对。
- 拉取与推送都对同一 `file_id` 串行，保证不会出现“先推后拉”导致的重复冲突。

### 4.5 防止已删除文件被旧设备重新上传

三重机制：

1. **Tombstone**：删除不物理移除 `entries`，`deleted=1` 且 `revision` 递增，保留 `path` 与最后 hash。
2. **CAS**：旧设备的 update/delete 带过期 `base_revision` → 409，强制先同步。
3. **复活阻断（RESURRECT_BLOCKED）**：create 携带 `client_seq`（客户端最后已知游标）。若同 `path_key` 的 tombstone 满足 `tombstone.updated_seq > client_seq`，说明客户端尚未看到这次删除 → 拒绝 `409 RESURRECT_BLOCKED`。若 `tombstone.updated_seq <= client_seq`，说明客户端已见过删除，属于用户主动重建 → 允许。
4. **Tombstone 保留期**：`min_retained_seq = MIN(所有 active 设备 last_ack_seq 的最小值, 90 天前的 seq)`。即：至少保留 90 天，且只要有活跃设备未 ACK 就继续保留。`seq < min_retained_seq` 的行才可 GC；该阈值同时决定 `CURSOR_EXPIRED` 边界。

### 4.6 跨平台大小写、Unicode 与替换差异

- 线上路径统一为：UTF-8、NFC 规范化、`/` 分隔、相对路径、无尾随空格与点。
- 服务端维护 `path_key = casefold(NFC(path))`（Unicode 简单大小写折叠），在**未删除条目间唯一**。因此 `A.txt` 与 `a.txt` 不能同时存在于同一 space，从源头消除 Windows/macOS 无法表示的状态。
- 客户端扫描发现本地已有大小写冲突时，保留原文件并把后来者重命名为 `name (case-conflict N).ext`，记录 `conflicts` 行。
- 原子替换：POSIX 用 `rename()` + 目录 `fsync`；Windows 用 `ReplaceFileW`（回退 `MoveFileExW(MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)`）；macOS 同 POSIX。
- 保留名（`CON`、`NUL`、`COM1`…）、非法字符、尾随点/空格在服务端入站校验时拒绝（`422 PATH_INVALID`）；下行到 Windows 时若仍不可表示，进入 `.syncbox/quarantine/` 并生成冲突记录。

### 4.7 从临时文件、数据库事务与进程崩溃中恢复

- **先写临时、再原子替换**：任何目标文件都不会出现半截内容（INV-10）。
- **意图日志**：`apply_journal` 在下载前写入，替换后置 `done`；启动时依日志判定“已下载未替换”“已替换未记账”两种状态。
- **SQLite WAL + `synchronous=FULL`（服务端）/ `NORMAL`（客户端）**：客户端允许最后一次提交丢失但不得出现索引与磁盘矛盾，因此替换与索引更新顺序固定为：先替换文件 → 再更新索引 → 再推进游标。若在“替换后、记账前”崩溃，启动核对发现磁盘 hash 与索引不符，按磁盘为准修正。
- **幂等重提**：`pending_ops` 中 `uploading/committing` 状态在重启后直接以相同 `op_id` 重试（§4.3 的幂等表）。

### 4.8 避免重复请求导致重复版本

- `operations.op_id` 为主键；提交事务开头查询，命中且 `request_hash` 相同 → 直接返回首次存储的响应（`X-Idempotent-Replay: true`）。
- `request_hash` 不同 → `422 IDEMPOTENCY_MISMATCH`，防止客户端复用 op_id 提交不同内容。
- 即使幂等记录已被清理（保留 7 天），CAS 仍会因 `base_revision` 过期而拒绝第二次提交，双重保险。

### 4.9 验证最终内容与服务端哈希一致

- **上传侧**：客户端发送整体 `content_hash`；服务端在 `/blobs/{hash}/complete` 时对落盘字节重新计算 SHA-256，不一致 → `422 BLOB_HASH_MISMATCH`，blob 置为 `uploading` 并清空冲突块。
- **提交侧**：OpService 在事务内确认 `blobs.state='complete'` 且 `blobs.content_hash = request.content_hash` 且 `size` 匹配，否则 `409 BLOB_MISSING`。
- **下载侧**：下载完成后流式计算 SHA-256，与 change 记录中的 `content_hash` 比较；不一致则删除临时文件并重试（最多 3 次），仍失败 → `hash_mismatch` 冲突记录并告警。
- **落盘后**：替换完成后可选二次校验（默认开启，大文件可用 `fsync` + 采样）；核对流程再次全量比对。

## 5. 并发模型

### 5.1 客户端

| 线程/执行器 | 职责 |
|---|---|
| `io_context`（1–2 线程） | 网络读写、WS 心跳、定时器 |
| `fs_worker` 池（默认 2） | 哈希计算、扫描、下载落盘 |
| `db_thread`（1） | 所有 SQLite 写操作（串行，避免 `SQLITE_BUSY`） |
| `watcher` 线程 | 平台监控回调，仅投递到去抖队列 |

所有跨线程状态通过任务队列传递，`SyncEngine` 状态机由单一线程驱动，避免锁竞争。

### 5.2 服务端

- Beast 异步 accept/read，连接状态贴合 strand。
- `ServerStore` 单写连接（`BEGIN IMMEDIATE`，`busy_timeout=5000`）；只读查询使用独立只读连接。
- Blob 写入使用内容寻址临时文件 + `rename`，数据库事务只记录元数据。

## 6. 配置

客户端 `config.toml`（M1 定义完整 schema）：

```toml
[server]
url = "https://sync.example.com:8443"
verify_tls = true

[device]
id = ""                       # 注册后写入
token_file = "~/.config/syncbox/device.token"

[sync]
root = "~/SyncBox"
ignore = [".git/**", "*.tmp", ".syncbox/**"]
debounce_ms = 300
poll_interval_s = 30
full_reconcile_hours = 24
max_inflight_ops = 4
chunk_size = 4194304
max_file_size = 8589934592

[logging]
level = "info"
file = "~/.local/state/syncbox/syncbox.log"
```

服务端 `server.toml`：监听地址、TLS 证书、数据库路径、blob 根目录、保留策略、限流参数、管理员操作仅通过 CLI。

## 7. 可观测性

- 结构化日志（spdlog，JSON sink），字段固定：`ts, level, module, device_id, space_id, file_id, op_id, seq, event, error_code`。
- 指标（M9）：同步延迟直方图、队列长度、冲突计数、重试计数、blob 存储量。
- 审计事件：设备注册/撤销、提交被拒（409/422）、GC 执行。

## 8. 与文档的对应关系

| 架构关注点 | 详细文档 |
|---|---|
| 表结构、索引、事务 | [DATABASE.md](DATABASE.md) |
| 端点、字段、错误码、WS | [PROTOCOL.md](PROTOCOL.md) |
| 版本与冲突决策 | [CONFLICTS.md](CONFLICTS.md) |
| 威胁模型与加固 | [SECURITY.md](SECURITY.md) |
| 测试与验收 | [TEST_PLAN.md](TEST_PLAN.md) |
| 实现顺序 | [ROADMAP.md](ROADMAP.md) |
