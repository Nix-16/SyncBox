# SyncBox 数据库设计

| 项目 | 内容 |
|---|---|
| 文档版本 | v0.1（M0 基线） |
| 日期 | 2026-10-21 |
| 状态 | 已冻结，M2 实现 |
| 关联文档 | [ARCHITECTURE.md](ARCHITECTURE.md) · [PROTOCOL.md](PROTOCOL.md) · [CONFLICTS.md](CONFLICTS.md) |

> **当前状态：本文档描述的是目标 schema，数据库中不存在任何表。** 见 [STATUS.md](STATUS.md)。

---

## 1. 共同约定

| 约定 | 规则 |
|---|---|
| 存储引擎 | SQLite ≥ 3.35（需要 `RETURNING` 与部分索引） |
| 日志模式 | `PRAGMA journal_mode=WAL` |
| 外键 | `PRAGMA foreign_keys=ON` |
| 忙碌等待 | `PRAGMA busy_timeout=5000` |
| 同步级别 | 服务端 `synchronous=FULL`；客户端 `synchronous=NORMAL` |
| 时间 | 全部为 **Unix 毫秒 UTC**（`INTEGER`），列名以 `_at` 结尾 |
| 布尔 | `INTEGER` + `CHECK(col IN (0,1))` |
| ID | UUIDv7 字符串（36 字符，含连字符），时间有序便于索引 |
| 哈希 | 64 位小写十六进制 `TEXT`（SHA-256）；token 哈希用 `BLOB(32)` |
| 路径 | UTF-8、NFC、`/` 分隔、相对路径；`path_key` 为折叠后的比较键 |
| 游标 | `seq` 为服务端**全局**单调自增整数，客户端游标只增不减 |
| 迁移 | `PRAGMA user_version` + `schema_migrations` 表，只向前 |

### 1.1 路径规范化规则（`core/path`，客户端与服务端共用一份实现）

```
给定输入 p（来自客户端或本地扫描）：
1. 拒绝：空串、绝对路径、以 '/' 结尾、含 '\'、含 NUL/控制字符
2. 按 '/' 切分，拒绝空段、"." 、".."
3. 每段：拒绝 Windows 保留名（CON/PRN/AUX/NUL/COM1-9/LPT1-9，含带扩展名形式）
4. 每段：拒绝 '<' '>' ':' '"' '|' '?' '*'，拒绝以空格或 '.' 结尾
5. 每段长度 ≤ 255 字节；整路径 ≤ 1024 字节
6. Unicode NFC 规范化 → 得到规范 path
7. path_key = unicode_casefold(path)（简单大小写折叠，按码点）
```

服务端与客户端使用同一实现，保证 `path_key` 一致。

---

## 2. 服务端数据库（`server.db`）

### 2.1 实体关系

```
spaces 1──n devices
spaces 1──n entries 1──n versions
spaces 1──n change_log        (seq 全局)
spaces 1──n operations        (op_id 全局唯一)
blobs 1──n blob_chunks
spaces 1──n enrollment_tokens
```

### 2.2 DDL

```sql
-- ============ 迁移与元信息 ============
CREATE TABLE schema_migrations (
  version     INTEGER PRIMARY KEY,
  applied_at  INTEGER NOT NULL
);

CREATE TABLE server_meta (
  key   TEXT PRIMARY KEY,
  value TEXT NOT NULL
);  -- 例：('min_retained_seq','12000'), ('blob_gc_watermark','...')

-- ============ 空间 ============
CREATE TABLE spaces (
  space_id    TEXT PRIMARY KEY,
  name        TEXT NOT NULL,
  created_at  INTEGER NOT NULL,
  state       TEXT NOT NULL DEFAULT 'active'
              CHECK (state IN ('active','archived'))
);

-- ============ 设备 ============
CREATE TABLE devices (
  device_id      TEXT PRIMARY KEY,
  space_id       TEXT NOT NULL REFERENCES spaces(space_id),
  display_name   TEXT NOT NULL,
  platform       TEXT NOT NULL CHECK (platform IN ('linux','windows','macos')),
  token_hash     BLOB NOT NULL UNIQUE,          -- SHA-256(device_token)
  status         TEXT NOT NULL DEFAULT 'active'
                 CHECK (status IN ('active','revoked')),
  app_version    TEXT,
  created_at     INTEGER NOT NULL,
  last_seen_at   INTEGER,
  last_ack_seq   INTEGER NOT NULL DEFAULT 0,    -- 该设备已确认应用到的游标
  revoked_at     INTEGER,
  revoked_reason TEXT
);
CREATE INDEX ix_devices_space  ON devices(space_id, status);
CREATE INDEX ix_devices_ack    ON devices(space_id, last_ack_seq);

-- ============ 条目（当前状态物化视图） ============
CREATE TABLE entries (
  space_id          TEXT NOT NULL REFERENCES spaces(space_id),
  file_id           TEXT NOT NULL,
  path              TEXT NOT NULL,
  path_key          TEXT NOT NULL,
  current_revision  INTEGER NOT NULL CHECK (current_revision >= 1),
  content_hash      TEXT,                       -- deleted=1 时保留最后内容哈希
  size              INTEGER,
  mtime_ms          INTEGER,                    -- 客户端上报的最后修改时间，仅供参考
  deleted           INTEGER NOT NULL DEFAULT 0 CHECK (deleted IN (0,1)),
  editor_device     TEXT,
  updated_seq       INTEGER NOT NULL,
  updated_at        INTEGER NOT NULL,
  PRIMARY KEY (space_id, file_id)
);

-- 活跃条目路径唯一；大小写/规范化后的 path_key 唯一（见 §1.1）
CREATE UNIQUE INDEX ux_entries_live_path
  ON entries(space_id, path_key) WHERE deleted = 0;
CREATE INDEX ix_entries_seq  ON entries(space_id, updated_seq);
CREATE INDEX ix_entries_path ON entries(space_id, path);

-- ============ 版本（不可变，追加写） ============
CREATE TABLE versions (
  space_id        TEXT NOT NULL,
  file_id         TEXT NOT NULL,
  revision        INTEGER NOT NULL CHECK (revision >= 1),
  parent_revision INTEGER NOT NULL CHECK (parent_revision >= 0),
  op              TEXT NOT NULL
                  CHECK (op IN ('create','update','delete','rename','restore')),
  path            TEXT NOT NULL,
  content_hash    TEXT,
  size            INTEGER,
  mtime_ms        INTEGER,
  device_id       TEXT NOT NULL,
  op_id           TEXT NOT NULL,
  created_at      INTEGER NOT NULL,
  PRIMARY KEY (space_id, file_id, revision),
  FOREIGN KEY (space_id, file_id) REFERENCES entries(space_id, file_id)
);
CREATE INDEX ix_versions_op     ON versions(space_id, op_id);
CREATE INDEX ix_versions_device ON versions(space_id, device_id, created_at);

-- ============ 变更日志（全局 seq） ============
CREATE TABLE change_log (
  seq          INTEGER PRIMARY KEY AUTOINCREMENT,
  space_id     TEXT NOT NULL,
  file_id      TEXT NOT NULL,
  revision     INTEGER NOT NULL,
  op           TEXT NOT NULL,
  path         TEXT NOT NULL,
  content_hash TEXT,
  size         INTEGER,
  deleted      INTEGER NOT NULL CHECK (deleted IN (0,1)),
  conflict     INTEGER NOT NULL DEFAULT 0 CHECK (conflict IN (0,1)),
  device_id    TEXT NOT NULL,
  created_at   INTEGER NOT NULL
);
CREATE INDEX ix_changelog_space_seq ON change_log(space_id, seq);
CREATE INDEX ix_changelog_file      ON change_log(space_id, file_id, seq);

-- ============ 幂等操作记录 ============
CREATE TABLE operations (
  op_id         TEXT PRIMARY KEY,               -- 客户端生成的 UUIDv7
  space_id      TEXT NOT NULL,
  device_id     TEXT NOT NULL,
  request_hash  TEXT NOT NULL,                  -- 规范化请求体 SHA-256
  response_code INTEGER NOT NULL,
  response_body TEXT NOT NULL,                  -- 首次成功响应的 JSON 原文
  seq           INTEGER NOT NULL,
  created_at    INTEGER NOT NULL
);
CREATE INDEX ix_operations_created ON operations(created_at);

-- ============ 内容寻址存储 ============
CREATE TABLE blobs (
  content_hash TEXT PRIMARY KEY,
  size         INTEGER NOT NULL,
  chunk_size   INTEGER NOT NULL,
  chunk_count  INTEGER NOT NULL,
  state        TEXT NOT NULL CHECK (state IN ('uploading','complete')),
  ref_count    INTEGER NOT NULL DEFAULT 0,
  created_at   INTEGER NOT NULL,
  completed_at INTEGER
);
CREATE INDEX ix_blobs_state ON blobs(state, created_at);

CREATE TABLE blob_chunks (
  content_hash TEXT NOT NULL REFERENCES blobs(content_hash) ON DELETE CASCADE,
  chunk_index  INTEGER NOT NULL,
  chunk_hash   TEXT NOT NULL,
  size         INTEGER NOT NULL,
  received_at  INTEGER,
  PRIMARY KEY (content_hash, chunk_index)
);

-- ============ 注册令牌 ============
CREATE TABLE enrollment_tokens (
  token_hash BLOB PRIMARY KEY,                  -- SHA-256(token)
  space_id   TEXT NOT NULL REFERENCES spaces(space_id),
  created_by TEXT NOT NULL,                     -- 管理员/CLI 标识
  expires_at INTEGER NOT NULL,
  used_by    TEXT,                              -- device_id
  used_at    INTEGER
);
CREATE INDEX ix_enroll_expiry ON enrollment_tokens(expires_at);
```

### 2.3 关键约束说明

| 约束 | 目的 |
|---|---|
| `ux_entries_live_path(space_id, path_key) WHERE deleted=0` | 同一空间内活跃路径唯一，且跨平台大小写安全；tombstone 释放路径供新建使用 |
| `versions(space_id, file_id, revision)` 主键 | 同一文件版本号不可重复，CAS 的最终防线 |
| `operations.op_id` 主键 | 幂等；重复请求不会产生第二个版本 |
| `devices.token_hash UNIQUE` | token 不落明文 |
| `FOREIGN KEY(space_id,file_id)` on `versions` | 版本必须挂在已存在的条目上 |

### 2.4 服务端核心事务

#### 2.4.1 提交操作（`POST /v1/spaces/{id}/ops`）

```sql
BEGIN IMMEDIATE;

-- 1) 幂等检查
SELECT request_hash, response_code, response_body
  FROM operations WHERE op_id = :op_id;
--   命中且 request_hash 相同 → COMMIT 后直接回放响应
--   命中且不同        → ROLLBACK, 422 IDEMPOTENCY_MISMATCH

-- 2) 设备与空间校验（可由连接上下文预取）
SELECT status FROM devices WHERE device_id = :device AND space_id = :space;
--   非 active → ROLLBACK, 403 DEVICE_REVOKED

-- 3) 读取当前条目（CAS）
SELECT current_revision, path, path_key, content_hash, deleted, updated_seq
  FROM entries WHERE space_id = :space AND file_id = :file_id;
--   不存在且 kind != create → 404 FILE_NOT_FOUND
--   current_revision != base_revision → 409 REVISION_CONFLICT
--   kind=restore 还要求 deleted=1；kind=delete 还要求 path 与当前 path 一致

-- 4) 复活阻断（仅 create）
SELECT updated_seq FROM entries
 WHERE space_id = :space AND path_key = :path_key AND deleted = 1;
--   updated_seq > client_seq → 409 RESURRECT_BLOCKED

-- 5) blob 校验（有内容时）
SELECT state, size FROM blobs WHERE content_hash = :hash;
--   不存在 / state != 'complete' / size != :size → 409 BLOB_MISSING

-- 6) 先写变更日志，由 AUTOINCREMENT 分配全局 seq
INSERT INTO change_log(space_id,file_id,revision,op,path,content_hash,size,
                       deleted,conflict,device_id,created_at)
VALUES (:space,:file,:rev,:op,:path,:hash,:size,:deleted,:conflict,:dev,:now);
-- seq := last_insert_rowid()；这是全局序号的唯一来源

-- 7) 写物化视图（create 为 INSERT，其余为 UPDATE）
--    必须在 versions 之前：versions 有外键指向 entries
INSERT INTO entries(space_id,file_id,path,path_key,current_revision,content_hash,
                    size,mtime_ms,deleted,editor_device,updated_seq,updated_at)
VALUES (:space,:file,:path,:path_key,:rev,:hash,:size,:mtime,:deleted,:dev,:seq,:now);
-- 非 create：
-- UPDATE entries SET path=:path, path_key=:path_key, current_revision=:rev,
--        content_hash=:hash, size=:size, mtime_ms=:mtime, deleted=:deleted,
--        editor_device=:dev, updated_seq=:seq, updated_at=:now
--  WHERE space_id=:space AND file_id=:file;

-- 8) 写版本（满足 versions → entries 外键）
INSERT INTO versions(space_id,file_id,revision,parent_revision,op,path,
                     content_hash,size,mtime_ms,device_id,op_id,created_at)
VALUES (:space,:file,:rev,:base,:op,:path,:hash,:size,:mtime,:dev,:op_id,:now);

-- 9) blob 引用计数（仅当 content_hash 变化时）
UPDATE blobs SET ref_count = ref_count + 1 WHERE content_hash = :hash;

-- 10) 幂等记录
INSERT INTO operations(op_id,space_id,device_id,request_hash,
                       response_code,response_body,seq,created_at)
VALUES (...);

COMMIT;
```

**规则**：
- 事务内不做网络 I/O、不做文件拷贝、不做哈希计算（这些在事务外完成）。
- 任一步失败必须 `ROLLBACK`，不得部分提交。
- `seq` 以 `INSERT change_log` 的返回值作为唯一来源，不预分配。

#### 2.4.2 读取变更（`GET /changes`）

```sql
SELECT seq,file_id,revision,op,path,content_hash,size,deleted,conflict,
       device_id,created_at
  FROM change_log
 WHERE space_id = :space AND seq > :since
 ORDER BY seq ASC
 LIMIT :limit;
```
只读，使用只读连接，不加写锁。`limit` 默认 200，最大 1000。

#### 2.4.3 全量快照（`GET /snapshot`）

```sql
-- 在单个读事务中执行，保证 snapshot_seq 与 entries 一致
BEGIN;
SELECT COALESCE(MAX(seq),0) AS snapshot_seq FROM change_log WHERE space_id=:space;
SELECT file_id,path,current_revision,content_hash,size,deleted,updated_seq
  FROM entries
 WHERE space_id = :space AND (path_key > :after OR (:after IS NULL))
 ORDER BY path_key ASC
 LIMIT :limit;
COMMIT;
```

### 2.5 GC 与保留策略

| 对象 | 规则 |
|---|---|
| `operations` | 保留 7 天；删除不影响正确性（CAS 兜底） |
| `change_log` | 仅删除 `seq < min_retained_seq` 的行 |
| `tombstone`（`entries.deleted=1`） | 与 change_log 同阈值；GC 前写入 `server_meta.min_retained_seq` |
| `blobs` | `ref_count = 0` 且 24 小时未被引用 → 删除文件与行 |
| `blob_chunks` | 随 blob 级联删除；上传超过 24 小时未完成 → 清理 |

**保留水位定义**（服务端唯一口径）：

```
oldest_unacked = MIN(last_ack_seq) over devices WHERE status='active'
horizon_seq    = 当前 seq 中“创建时间早于 90 天”的最大 seq（无则为 0）
min_retained_seq = MIN(oldest_unacked, horizon_seq)
```

即：至少保留 90 天；只要还有活跃设备未 ACK，就继续保留得更久。只有 `seq < min_retained_seq` 的 `change_log` 与 tombstone 可被 GC。

GC 使 `min_retained_seq` 前进后，`GET /changes?since=<min_retained_seq-1>` 返回 `410 CURSOR_EXPIRED`。

---

## 3. 客户端数据库（`client.db`）

### 3.1 DDL

```sql
PRAGMA journal_mode=WAL;
PRAGMA synchronous=NORMAL;
PRAGMA foreign_keys=ON;
PRAGMA busy_timeout=5000;

CREATE TABLE schema_migrations (
  version    INTEGER PRIMARY KEY,
  applied_at INTEGER NOT NULL
);

-- 元信息：schema_version, device_id, space_id, cursor, root_path, protocol
CREATE TABLE meta (
  key   TEXT PRIMARY KEY,
  value TEXT NOT NULL
);

-- 本地与服务端的联合视图
CREATE TABLE local_entries (
  file_id          TEXT PRIMARY KEY,           -- 服务端分配；未提交 create 暂用本地临时 UUID
  path             TEXT NOT NULL,
  path_key         TEXT NOT NULL,
  server_revision  INTEGER NOT NULL DEFAULT 0, -- 0 表示尚未在服务端存在
  content_hash     TEXT,                       -- NULL 表示未知/未下载
  size             INTEGER,
  mtime_ms         INTEGER,
  deleted          INTEGER NOT NULL DEFAULT 0 CHECK (deleted IN (0,1)),
  dirty            INTEGER NOT NULL DEFAULT 0 CHECK (dirty IN (0,1)),
  fs_ino           INTEGER,                    -- POSIX inode / Windows file index
  fs_dev           INTEGER,
  applied_seq      INTEGER NOT NULL DEFAULT 0, -- 本地已应用到的服务端 seq
  conflict_of      TEXT,                       -- 若为冲突副本，指向原 file_id
  updated_at       INTEGER NOT NULL
);
CREATE INDEX ix_local_path_key ON local_entries(path_key);
CREATE INDEX ix_local_path     ON local_entries(path);

-- 待提交操作（持久化 Outbox）
CREATE TABLE pending_ops (
  op_id          TEXT PRIMARY KEY,             -- UUIDv7，重试时不变
  file_id        TEXT,                         -- create 时为 NULL，服务端分配后回填
  kind           TEXT NOT NULL
                 CHECK (kind IN ('create','update','delete','rename','restore')),
  base_revision  INTEGER NOT NULL,
  path           TEXT NOT NULL,
  new_path       TEXT,
  content_hash   TEXT,
  size           INTEGER,
  staged_path    TEXT,                         -- .syncbox/tmp/<op_id>.part
  client_seq     INTEGER NOT NULL,             -- 入队时的本地游标，用于 RESURRECT_BLOCKED
  state          TEXT NOT NULL
                 CHECK (state IN ('staged','uploading','committing','retry',
                                  'conflict','done','dead')),
  attempts       INTEGER NOT NULL DEFAULT 0,
  next_attempt_at INTEGER,
  last_error     TEXT,
  created_at     INTEGER NOT NULL,
  updated_at     INTEGER NOT NULL
);
CREATE INDEX ix_pending_state ON pending_ops(state, next_attempt_at);
CREATE INDEX ix_pending_file  ON pending_ops(file_id);

-- 同一文件的在途操作只允许一个（INV-9）；NULL file_id 不受约束
CREATE UNIQUE INDEX ux_pending_active_file
  ON pending_ops(file_id)
  WHERE file_id IS NOT NULL
    AND state IN ('staged','uploading','committing','retry');

-- 落盘意图日志（崩溃恢复）
CREATE TABLE apply_journal (
  id            INTEGER PRIMARY KEY AUTOINCREMENT,
  file_id       TEXT NOT NULL,
  target_path   TEXT NOT NULL,
  temp_path     TEXT NOT NULL,
  expected_hash TEXT,
  expected_size INTEGER,
  state         TEXT NOT NULL
                CHECK (state IN ('downloading','staged','replacing','done','failed')),
  created_at    INTEGER NOT NULL,
  updated_at    INTEGER NOT NULL
);
CREATE INDEX ix_apply_state ON apply_journal(state);
CREATE INDEX ix_apply_file  ON apply_journal(file_id);

-- 冲突记录（展示与处理）
CREATE TABLE conflicts (
  conflict_id TEXT PRIMARY KEY,
  file_id     TEXT NOT NULL,
  kind        TEXT NOT NULL
              CHECK (kind IN ('modify_modify','modify_delete','rename_conflict',
                              'path_collision','case_collision','hash_mismatch')),
  local_path  TEXT,
  remote_path TEXT,
  resolution  TEXT NOT NULL
              CHECK (resolution IN ('conflict_copy','remote_wins','restored','manual_pending')),
  detail      TEXT,                            -- JSON：hash、revision、device
  created_at  INTEGER NOT NULL,
  resolved_at INTEGER
);
CREATE INDEX ix_conflicts_pending ON conflicts(resolved_at);

-- 自身写入抑制窗口
CREATE TABLE write_suppressions (
  path_key TEXT PRIMARY KEY,
  until_ms INTEGER NOT NULL,
  reason   TEXT NOT NULL
);

-- 下载分块进度（断点续传）
CREATE TABLE download_parts (
  content_hash TEXT NOT NULL,
  chunk_index  INTEGER NOT NULL,
  temp_path    TEXT,
  received     INTEGER NOT NULL DEFAULT 0 CHECK (received IN (0,1)),
  updated_at   INTEGER NOT NULL,
  PRIMARY KEY (content_hash, chunk_index)
);

-- 本地重命名检测的候选窗口（inode 复用兜底）
CREATE TABLE rename_candidates (
  path_key   TEXT PRIMARY KEY,
  fs_ino     INTEGER,
  fs_dev     INTEGER,
  content_hash TEXT,
  seen_at    INTEGER NOT NULL
);
```

### 3.2 客户端状态机

#### `pending_ops.state`

```
staged ──开始上传──▶ uploading ──上传完成──▶ committing ──200──▶ done
  │                     │                      │
  │                     │ 失败(可重试)          │ 409 REVISION_CONFLICT
  │                     ▼                      ▼
  │                   retry ◀────────────── conflict ──用户/自动解决──▶ 新 op
  │                     │ 超过 10 次
  │                     ▼
  │                    dead（需手动重试）
  └──放弃（源文件已消失且 hash 未变）──▶ done
```

| 状态 | 含义 | 重启后行为 |
|---|---|---|
| `staged` | 已入队，内容可能尚未暂存完成 | 校验 `staged_path`；缺失则重新扫描 |
| `uploading` | 正在分块上传 | 复位为 `retry`，用同一 `op_id` 重传 |
| `committing` | 已发出提交请求，结果未知 | 复位为 `retry`，重试同 `op_id`（服务端幂等） |
| `retry` | 等待退避 | 按 `next_attempt_at` 调度 |
| `conflict` | 收到 409/422，需解决 | 保持，等待 ConflictResolver |
| `done` | 成功 | 归档/删除 |
| `dead` | 超过重试上限 | 保持，GUI/CLI 提示 |

#### `apply_journal.state`

```
downloading ──校验通过──▶ staged ──rename 开始──▶ replacing ──成功──▶ done
     │                        │                        │
     └──校验失败/重试用尽──▶ failed              崩溃后由启动恢复处理
```

### 3.3 索引与查询

| 查询 | 使用索引 |
|---|---|
| 按路径查条目 | `ix_local_path_key` |
| 取待办（按时间） | `ix_pending_state` |
| 查某文件是否有在途操作 | `ux_pending_active_file` |
| 恢复未完成落盘 | `ix_apply_state` |
| 未解决冲突 | `ix_conflicts_pending`（`resolved_at IS NULL`） |

### 3.4 事务边界（客户端）

| 操作 | 事务内容 |
|---|---|
| 入队本地变更 | `INSERT pending_ops` + `UPDATE local_entries.dirty=1` |
| 提交成功 | `UPDATE local_entries(server_revision, content_hash)` + 删除 `pending_ops` 行 |
| 应用远端变更 | `UPDATE local_entries` + `DELETE write_suppressions(过期)` + `UPDATE meta.cursor` **仅在文件替换成功后** |
| 入队冲突副本 | `INSERT local_entries`（临时 file_id）+ `INSERT pending_ops` + `INSERT conflicts` |

**顺序规则**（防止索引与磁盘矛盾）：
- 上行：**先写文件 → 再写索引/队列**。
- 下行：**先落盘替换 → 再更新索引 → 最后推进游标**。
- 游标推进永远是事务的最后一步；否则崩溃会跳过变更。

---

## 4. 迁移策略

1. 每个数据库维护 `schema_migrations(version, applied_at)`；版本号从 1 开始，只增不减。
2. 迁移脚本编译进二进制（`std::string_view` 数组），按版本顺序在事务中执行。
3. 启动时若 `user_version` 高于程序支持版本 → 拒绝启动（防止降级破坏数据）。
4. 迁移前对数据库文件做快速备份：`server.db.bak.<version>`（M2 实现）。
5. 迁移必须幂等可重入：失败回滚整个事务，不留下半迁移状态。

| 数据库 | 初始版本 | 迁移 1 内容 |
|---|---|---|
| 服务端 | 1 | §2.2 全部表与索引 |
| 客户端 | 1 | §3.1 全部表与索引 |

---

## 5. 一致性与不变量

| 编号 | 不变量 | 强制方式 |
|---|---|---|
| DB-1 | 每个 `versions` 行的 `parent_revision` 等于前一个 revision | 服务端 CAS 校验 + 事务 |
| DB-2 | `entries.current_revision` 等于该文件最大 `revision` | 同一事务内更新 |
| DB-3 | 每条版本提交恰好对应一条 `change_log`（`seq` 唯一） | 同一事务内 INSERT |
| DB-4 | `change_log.seq` 全局单调递增 | `AUTOINCREMENT` |
| DB-5 | 活跃条目 `path_key` 唯一 | 部分唯一索引 |
| DB-6 | 幂等：`operations.op_id` 唯一，且 `request_hash` 与响应绑定 | 主键 + 事务 |
| DB-7 | 客户端游标只增不减 | `UPDATE meta SET value=:new WHERE key='cursor' AND CAST(value AS INTEGER) <= :new` |
| DB-8 | 每个在途 `pending_ops` 的 `file_id` 唯一 | 部分唯一索引 |

---

## 6. 故障恢复矩阵

| 崩溃点 | 磁盘状态 | 数据库状态 | 恢复动作 |
|---|---|---|---|
| 暂存内容写入中 | 部分 `.syncbox/tmp/<op_id>.part` | `pending_ops.staged` | 校验 hash，不符则删除重扫 |
| 分块上传中 | 服务端 blob 部分块 | `uploading` | 查 `/blobs/{h}/status` 续传 |
| 提交已发出、响应丢失 | 服务端已提交 | `committing` | 用同 `op_id` 重试，回放响应 |
| 下载完成、未替换 | temp 完整 | `apply_journal.downloading` | 校验后继续替换 |
| 替换完成、索引未写 | 目标文件已是新内容 | `replacing` | 比对磁盘 hash，补写索引 |
| 索引已写、游标未推进 | 一致 | 游标落后 | 重放变更（操作幂等，重复应用无副作用） |
| 服务端事务中途崩溃 | — | 事务回滚 | 客户端超时重试同 `op_id` |

---

## 7. 容量估算

| 对象 | 单条大小 | 10 万文件规模 |
|---|---|---|
| `entries` | ~250 B | ~25 MB |
| `versions` | ~200 B | 每次修改 +200 B（可按策略归档） |
| `change_log` | ~180 B | 受保留策略约束 |
| 客户端 `local_entries` | ~300 B | ~30 MB |

SQLite 在 10 万条目、百万版本量级内性能足够；超过时按 `space_id + 时间` 分区归档 `versions`（v2 议题）。

---

## 8. 验收条件（M2）

- [ ] 两个 schema 可在空库上一次性创建，重复执行迁移无副作用。
- [ ] `PRAGMA foreign_key_check` 与 `PRAGMA integrity_check` 通过。
- [ ] 并发 50 个写线程（测试用）在 `busy_timeout` 内无 `SQLITE_BUSY` 泄漏。
- [ ] 模拟进程崩溃（事务中 `exit`）后重启，数据库可打开且无半提交。
- [ ] 同一 `op_id` 重放 100 次，`versions` 与 `change_log` 行数只增加 0 或 1。
- [ ] `path_key` 唯一索引能拒绝 `A.txt` / `a.txt` 同时存在。
- [ ] 客户端 kill -9 于 §6 每个崩溃点，重启后 60 秒内自愈。
