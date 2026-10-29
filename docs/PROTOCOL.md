# SyncBox 通信协议

| 项目 | 内容 |
|---|---|
| 文档版本 | v0.1（M0 基线） |
| 协议版本 | `1` |
| 日期 | 2026-10-21 |
| 状态 | 已冻结，M4 实现服务端、M5 实现客户端 |
| 关联文档 | [ARCHITECTURE.md](ARCHITECTURE.md) · [DATABASE.md](DATABASE.md) · [CONFLICTS.md](CONFLICTS.md) · [SECURITY.md](SECURITY.md) |

> **当前状态：没有任何网络实现。** 本文件定义 M4/M5 必须实现的契约。

---

## 1. 通用规则

### 1.1 传输

| 项 | 规定 |
|---|---|
| 协议 | HTTPS（HTTP/1.1）+ WSS |
| 默认端口 | `8443` |
| 编码 | 请求/响应体 `application/json; charset=utf-8`；分块上传为 `application/octet-stream` |
| 请求体上限 | JSON 端点 1 MiB；分块上传单块 16 MiB |
| 时间 | 所有时间字段为 Unix 毫秒（int64） |
| 哈希 | 64 位小写十六进制 SHA-256，无前缀 |
| ID | UUIDv7 字符串 |
| 路径 | UTF-8、NFC、`/` 分隔的相对路径，规则见 [DATABASE.md](DATABASE.md) §1.1 |
| TLS | 生产环境强制；`127.0.0.1` 调试可关闭（配置显式开启） |

### 1.2 必需请求头

| 头 | 值 | 说明 |
|---|---|---|
| `Authorization` | `Bearer <device_token>` | 除 `/v1/enroll`、`/v1/health` 外必需 |
| `X-Sync-Protocol` | `1` | 不兼容时返回 `426 PROTOCOL_UNSUPPORTED` |
| `X-Sync-Client` | `syncbox/0.1.0 (linux)` | 客户端版本，用于审计 |
| `X-Request-Id` | UUIDv7 | 链路追踪；缺失时服务端生成 |
| `Content-Type` | 见上表 | |

### 1.3 响应头

| 头 | 说明 |
|---|---|
| `X-Request-Id` | 回显或服务端生成 |
| `X-Sync-Server-Seq` | 当前空间最新 `seq`（尽力而为，仅提示） |
| `X-Idempotent-Replay` | `true` 表示该响应来自幂等回放 |
| `Retry-After` | 429/503 时给出秒数 |

### 1.4 错误信封

所有非 2xx 响应统一为：

```json
{
  "error": {
    "code": "REVISION_CONFLICT",
    "message": "base_revision 7 does not match current revision 9",
    "retryable": false,
    "request_id": "0192f0c4-7a3e-7c11-9b2a-5f6d0e1a2b3c",
    "details": {
      "file_id": "0192f0c4-1111-7aaa-8bbb-000000000001",
      "current": {
        "revision": 9,
        "deleted": false,
        "path": "docs/spec.md",
        "content_hash": "9f86d081884c7d65...",
        "size": 20480,
        "editor_device": "0192f0c4-2222-7ccc-8ddd-000000000002"
      }
    }
  }
}
```

字段说明：

| 字段 | 类型 | 说明 |
|---|---|---|
| `code` | string | 稳定错误码，见 §1.5 |
| `message` | string | 人类可读，不保证稳定，不用于分支判断 |
| `retryable` | bool | 客户端是否应在退避后重试 |
| `request_id` | string | 与响应头一致 |
| `details` | object? | 错误相关的结构化上下文 |

### 1.5 错误码表

| HTTP | `code` | 可重试 | 含义 / 客户端动作 |
|---|---|---|---|
| 400 | `BAD_REQUEST` | 否 | JSON 解析失败或字段缺失 |
| 401 | `AUTH_REQUIRED` | 否 | 缺少/无效 token |
| 401 | `TOKEN_EXPIRED` | 否 | 重新注册或刷新（v1 无刷新，重新 enroll） |
| 403 | `DEVICE_REVOKED` | 否 | 设备被撤销，停止同步并提示 |
| 403 | `SPACE_FORBIDDEN` | 否 | 设备不属于该空间 |
| 404 | `FILE_NOT_FOUND` | 否 | 对不存在的 `file_id` 做非 create 操作 |
| 404 | `BLOB_NOT_FOUND` | 否 | 请求的 blob 不存在 |
| 405 | `METHOD_NOT_ALLOWED` | 否 | — |
| 409 | `REVISION_CONFLICT` | 否 | CAS 失败；进入冲突处理（[CONFLICTS.md](CONFLICTS.md)） |
| 409 | `PATH_CASE_COLLISION` | 否 | `path_key` 已被其他活跃条目占用；改名后重试 |
| 409 | `RESURRECT_BLOCKED` | 否 | 旧设备试图重建已被删除的路径 |
| 409 | `BLOB_MISSING` | 否 | 提交引用的 blob 未完成；先完成上传 |
| 409 | `BLOB_INCOMPLETE` | 是 | 分块不全；按 `missing_chunks` 续传 |
| 410 | `CURSOR_EXPIRED` | 否 | 游标早于保留窗口；执行全量快照核对 |
| 413 | `PAYLOAD_TOO_LARGE` | 否 | 超过 `max_file_size` 或 JSON 上限 |
| 422 | `PATH_INVALID` | 否 | 路径不符合 §1.1 / DB §1.1 规则 |
| 422 | `BLOB_HASH_MISMATCH` | 否 | 内容哈希与服务端计算结果不符 |
| 422 | `CHUNK_HASH_MISMATCH` | 是 | 单块哈希不符，重传该块 |
| 422 | `IDEMPOTENCY_MISMATCH` | 否 | 同一 `op_id` 携带不同请求体；客户端 bug |
| 422 | `INVALID_OPERATION` | 否 | `kind` 与字段组合非法（如 delete 带 content_hash） |
| 426 | `PROTOCOL_UNSUPPORTED` | 否 | 协议版本不兼容 |
| 429 | `RATE_LIMITED` | 是 | 按 `Retry-After` 退避 |
| 500 | `INTERNAL` | 是 | 服务端内部错误 |
| 503 | `UNAVAILABLE` | 是 | 过载或维护中 |

### 1.6 幂等规则

- 所有写操作（`POST /ops`、`POST /blobs`、`PUT chunk`、`POST complete`）必须幂等。
- `op_id` 由客户端生成（UUIDv7），在**重试时保持不变**，仅在生成**新的逻辑操作**时改变。
- 服务端以 `operations.op_id` 为主键；命中且 `request_hash` 相同 → 回放首次响应并带 `X-Idempotent-Replay: true`。
- `request_hash` = 对请求体按“字典序键 + 无空白”规范序列化后的 SHA-256。
- 幂等记录保留 7 天；过期后由 CAS（`base_revision`）兜底。

### 1.7 游标规则

- 游标是服务端**全局** `seq`，非每空间独立编号。
- 客户端只保存一个 `cursor`（该空间的 `applied_seq`），只增不减。
- `GET /changes?since=cursor` 返回 `seq > cursor` 的变更，按 `seq` 升序。
- 客户端必须**按顺序应用**，应用成功后推进游标，再 `POST /ack`。
- `since < min_retained_seq` → `410 CURSOR_EXPIRED`，客户端执行全量快照。
- 空结果不是错误；客户端继续轮询或等待 WS 提示。

### 1.8 分页

统一使用 `limit`（默认 200，最大 1000）与 `page_token`（不透明字符串，客户端不得解析）。变更流使用 `since` 游标而非 `page_token`。

---

## 2. 端点总览

| 方法 | 路径 | 用途 | 里程碑 |
|---|---|---|---|
| GET | `/v1/health` | 健康检查 | M4 |
| POST | `/v1/enroll` | 设备注册 | M4 |
| GET | `/v1/me` | 当前设备信息 | M4 |
| GET | `/v1/spaces/{space_id}/changes` | 增量变更 | M4 |
| GET | `/v1/spaces/{space_id}/snapshot` | 全量快照 | M4 |
| GET | `/v1/spaces/{space_id}/entries/{file_id}` | 单条目查询 | M4 |
| POST | `/v1/spaces/{space_id}/ops` | 提交操作（增删改重命名） | M4 |
| POST | `/v1/devices/{device_id}/ack` | 确认游标 | M4 |
| POST | `/v1/blobs` | 初始化上传 / 命中已有 | M4 |
| PUT | `/v1/blobs/{hash}/chunks/{index}` | 上传分块 | M4 |
| GET | `/v1/blobs/{hash}/status` | 查询分块状态 | M4 |
| POST | `/v1/blobs/{hash}/complete` | 完成并校验整体哈希 | M4 |
| GET | `/v1/blobs/{hash}` | 下载内容（支持 Range） | M4 |
| WS | `/v1/ws` | 变更通知 | M4 |
| DELETE | `/v1/devices/{device_id}` | 撤销设备（管理员，v1 走本地 CLI） | M8 |

---

## 3. 端点详细定义

### 3.1 `GET /v1/health`

无认证。响应 `200`：

```json
{"status":"ok","protocol":1,"server_time_ms":1792540800000,"min_retained_seq":12000}
```

### 3.2 `POST /v1/enroll`

无认证（凭 enrollment token）。**限流：每 IP 每分钟 10 次。**

请求：

```json
{
  "enrollment_token": "enr_3f9c...",
  "device_name": "nix-laptop",
  "platform": "linux",
  "app_version": "0.1.0"
}
```

响应 `201`：

```json
{
  "device_id": "0192f0c4-2222-7ccc-8ddd-000000000002",
  "device_token": "dt_9a8b7c6d5e4f3a2b1c0d9e8f7a6b5c4d3e2f1a0b",
  "space_id": "0192f0c4-3333-7eee-8fff-000000000003",
  "space_name": "Personal",
  "current_seq": 4310,
  "protocol": 1
}
```

规则：

- `enrollment_token` 单次使用、24 小时过期；无效/已用/过期 → `401 AUTH_REQUIRED`。
- `device_token` 只在此响应中出现一次，客户端存入权限 `0600` 的文件。
- `platform` 必须为 `linux|windows|macos`，否则 `400 BAD_REQUEST`。

### 3.3 `GET /v1/me`

响应 `200`：

```json
{
  "device_id": "0192f0c4-2222-...",
  "space_id": "0192f0c4-3333-...",
  "display_name": "nix-laptop",
  "platform": "linux",
  "status": "active",
  "last_ack_seq": 4300,
  "server_seq": 4310
}
```

### 3.4 `GET /v1/spaces/{space_id}/changes`

查询参数：

| 参数 | 类型 | 必填 | 默认 | 说明 |
|---|---|---|---|---|
| `since` | int64 | 是 | — | 返回 `seq > since` |
| `limit` | int | 否 | 200 | 最大 1000 |

响应 `200`：

```json
{
  "space_id": "0192f0c4-3333-...",
  "from_seq": 4301,
  "to_seq": 4310,
  "has_more": false,
  "changes": [
    {
      "seq": 4301,
      "file_id": "0192f0c4-1111-...",
      "revision": 8,
      "op": "update",
      "path": "docs/spec.md",
      "content_hash": "9f86d081884c7d65...",
      "size": 20480,
      "deleted": false,
      "conflict": false,
      "device_id": "0192f0c4-2222-...",
      "created_at": 1792540800000
    },
    {
      "seq": 4302,
      "file_id": "0192f0c4-4444-...",
      "revision": 3,
      "op": "delete",
      "path": "old/notes.txt",
      "content_hash": "e3b0c44298fc1c14...",
      "size": 128,
      "deleted": true,
      "conflict": false,
      "device_id": "0192f0c4-5555-...",
      "created_at": 1792540801000
    }
  ]
}
```

规则：

- `from_seq` 为 `since + 1`；无变更时 `changes: []`、`to_seq = since`。
- 客户端必须逐一应用；某条应用失败则**停止**，不推进游标。
- 变更项不包含文件内容，只包含 `content_hash` 与元数据。

错误：`410 CURSOR_EXPIRED`（`details.min_retained_seq` 给出下限）。

### 3.5 `GET /v1/spaces/{space_id}/snapshot`

用于全量核对。查询参数：`limit`（默认 500，最大 2000）、`page_token`。

响应 `200`：

```json
{
  "space_id": "0192f0c4-3333-...",
  "snapshot_seq": 4310,
  "entries": [
    {
      "file_id": "0192f0c4-1111-...",
      "path": "docs/spec.md",
      "revision": 8,
      "content_hash": "9f86d081884c7d65...",
      "size": 20480,
      "deleted": false,
      "updated_seq": 4301
    },
    {
      "file_id": "0192f0c4-4444-...",
      "path": "old/notes.txt",
      "revision": 3,
      "content_hash": "e3b0c44298fc1c14...",
      "size": 128,
      "deleted": true,
      "updated_seq": 4302
    }
  ],
  "next_page_token": null
}
```

规则：

- 同一 `page_token` 必须携带相同的 `snapshot_seq`；`next_page_token=null` 表示结束。
- 第一页返回 `snapshot_seq`；客户端完成全部页面后，以 `snapshot_seq` 作为新游标起点，再拉增量。
- 快照包含 tombstone（`deleted=true`），用于删除传播与复活判定。

### 3.6 `GET /v1/spaces/{space_id}/entries/{file_id}`

响应 `200` 为单个条目对象（同快照条目结构）。不存在 → `404 FILE_NOT_FOUND`。

### 3.7 `POST /v1/spaces/{space_id}/ops` —— 核心提交端点

请求体：

```json
{
  "op_id": "0192f0c4-6666-7aaa-8bbb-000000000006",
  "kind": "update",
  "file_id": "0192f0c4-1111-7aaa-8bbb-000000000001",
  "base_revision": 7,
  "path": "docs/spec.md",
  "new_path": null,
  "content_hash": "9f86d081884c7d659a2feaa0c55ad015a3bf4f1b2b0b822cd15d6c15b0f00a08",
  "size": 20480,
  "mtime_ms": 1792540799000,
  "client_seq": 4300,
  "conflict": false
}
```

字段语义：

| 字段 | 类型 | create | update | rename | delete | restore |
|---|---|---|---|---|---|---|
| `op_id` | string | 必填 | 必填 | 必填 | 必填 | 必填 |
| `kind` | enum | `create` | `update` | `rename` | `delete` | `restore` |
| `file_id` | string? | **必须为 null** | 必填 | 必填 | 必填 | 必填 |
| `base_revision` | int64 | 必须为 `0` | 必填 | 必填 | 必填 | 必填 |
| `path` | string | 必填（新路径） | 必填（当前路径） | 必填（新路径） | 必填（删除时路径，用于校验） | 必填（恢复路径） |
| `new_path` | string? | null | null | null（路径放 `path`） | null | null |
| `content_hash` | string? | 必填 | 必填 | null（沿用当前） | null | 必填 |
| `size` | int64? | 必填 | 必填 | null | null | 必填 |
| `mtime_ms` | int64? | 必填 | 必填 | 可选 | null | 必填 |
| `client_seq` | int64 | 必填 | 必填 | 必填 | 必填 | 必填 |
| `conflict` | bool | 可选 | 可选 | 可选 | 可选 | 可选 |

> 约定：`path` 始终表示“操作完成后该文件的路径”。`rename` 时 `path` 是新路径，`base_revision` 对应的旧路径由服务端读取。`new_path` 字段保留以兼容未来“移动 + 新建”组合，v1 必须为 null。

`conflict` 为可选布尔值（默认 `false`），由客户端在“冲突副本提交”或“restore”时置为 `true`。服务端仅将其记录到 `change_log.conflict`，**不影响** CAS、幂等或版本语义；其他客户端可据此展示“该变更源于一次冲突处理”。

`client_seq` 的含义：客户端发起该操作时已应用到的最后一个服务端 `seq`。服务端用它判断客户端是否见过某次删除（RESURRECT_BLOCKED）以及是否可能遗漏中间变更。

#### 成功响应 `200`

```json
{
  "op_id": "0192f0c4-6666-...",
  "file_id": "0192f0c4-1111-...",
  "revision": 8,
  "parent_revision": 7,
  "path": "docs/spec.md",
  "content_hash": "9f86d081884c7d65...",
  "size": 20480,
  "deleted": false,
  "conflict": false,
  "seq": 4311,
  "applied_at": 1792540802000
}
```

`create` 的响应额外包含服务端分配的 `file_id`；客户端必须用它替换本地临时 `file_id` 并更新 `local_entries`。

#### 失败响应

| 场景 | 状态 | `code` | 客户端动作 |
|---|---|---|---|
| 基础版本过期 | 409 | `REVISION_CONFLICT` | 进入冲突处理，见 [CONFLICTS.md](CONFLICTS.md) |
| 路径被占用（大小写/规范化后） | 409 | `PATH_CASE_COLLISION` | 改名重提（新 `op_id`） |
| create 撞上未见过的 tombstone | 409 | `RESURRECT_BLOCKED` | 停止上行，先拉取变更；必要时改名或转冲突副本 |
| blob 未完成 | 409 | `BLOB_MISSING` | 完成上传后以**同一 `op_id`** 重试 |
| 路径非法 | 422 | `PATH_INVALID` | 修正路径或转隔离区 |
| 同 op_id 不同请求体 | 422 | `IDEMPOTENCY_MISMATCH` | 停止并报 bug（客户端保证唯一） |

`REVISION_CONFLICT` 响应示例：

```json
{
  "error": {
    "code": "REVISION_CONFLICT",
    "message": "base_revision 7 does not match current revision 9",
    "retryable": false,
    "details": {
      "current": {
        "file_id": "0192f0c4-1111-...",
        "revision": 9,
        "path": "docs/spec.md",
        "deleted": false,
        "content_hash": "abc123...",
        "size": 20000,
        "editor_device": "0192f0c4-2222-..."
      }
    }
  }
}
```

#### 服务端操作语义

| `kind` | 前置条件 | 结果 |
|---|---|---|
| `create` | 无同 `path_key` 活跃条目；无更新于 `client_seq` 的 tombstone | revision=1，op=`create` |
| `update` | `base_revision == current_revision` 且未删除 | revision+1，op=`update` |
| `rename` | 同上，且新路径未被占用 | revision+1，op=`rename`（`file_id` 不变） |
| `delete` | 同上且 `current path == path` | revision+1，op=`delete`，`deleted=1`，保留最后 hash |
| `restore` | 当前 `deleted=1` 且 `base_revision == current_revision` | revision+1，op=`restore`，`deleted=0` |

### 3.8 `POST /v1/devices/{device_id}/ack`

请求：

```json
{"last_seq": 4311}
```

响应 `204`。规则：服务端取 `max(existing, last_seq)`，不回退；用于 tombstone 与日志 GC 的水位计算。

### 3.9 Blob 上传

#### 3.9.1 `POST /v1/blobs`

请求：

```json
{
  "content_hash": "9f86d081884c7d65...",
  "size": 104857600,
  "chunk_size": 4194304,
  "chunk_hashes": ["aa11...", "bb22...", "..."]
}
```

规则：
- `chunk_count = ceil(size / chunk_size)`；`chunk_hashes` 长度必须等于 `chunk_count`；最后一块可小于 `chunk_size`。
- 小于 `8 MiB` 的文件允许 `chunk_count = 1`。
- 已存在且 `state=complete` → `200 {"content_hash":"...","status":"complete","missing_chunks":[]}`（去重命中）。
- 新建上传会话 → `201 {"content_hash":"...","status":"uploading","missing_chunks":[0,1,2,...]}`。

#### 3.9.2 `PUT /v1/blobs/{hash}/chunks/{index}`

- Body：原始字节，`Content-Type: application/octet-stream`。
- 头：`X-Chunk-SHA256: <hex>`（必填）。
- 响应 `204`。
- 校验：长度必须等于 manifest；哈希必须等于 `chunk_hashes[index]` 与 `X-Chunk-SHA256`；否则 `422 CHUNK_HASH_MISMATCH`。
- 幂等：同一块重复上传（内容相同）返回 `204`。

#### 3.9.3 `GET /v1/blobs/{hash}/status`

响应 `200`：

```json
{"content_hash":"9f86...","state":"uploading","chunk_size":4194304,
 "chunk_count":25,"missing_chunks":[3,7,8]}
```

#### 3.9.4 `POST /v1/blobs/{hash}/complete`

无 body。服务端校验所有块存在、整体 SHA-256 正确：

```json
{"content_hash":"9f86...","status":"complete","size":104857600}
```

失败：`409 BLOB_INCOMPLETE`（附 `missing_chunks`）或 `422 BLOB_HASH_MISMATCH`。

#### 3.9.5 `GET /v1/blobs/{hash}`

- 支持 `Range: bytes=start-end`，返回 `206` 与 `Content-Range`。
- 无 Range 返回 `200` 全量。
- 服务端按块顺序流式读取，不要求在磁盘上拼接整体文件。
- `ETag` = `content_hash`；`Cache-Control: private, max-age=31536000, immutable`（内容寻址不可变）。

### 3.10 WebSocket `/v1/ws`

- 握手：`GET /v1/ws?space_id=<id>`，带 `Authorization: Bearer <token>`（原生客户端可设头；不依赖 query token）。
- 子协议：`syncbox.v1`。

服务端 → 客户端：

```json
{"type":"welcome","space_id":"0192f0c4-3333-...","seq":4310,"heartbeat_s":25}
{"type":"change","space_id":"0192f0c4-3333-...","seq":4311}
{"type":"pong"}
{"type":"error","code":"RATE_LIMITED","message":"too many notifications"}
```

客户端 → 服务端：

```json
{"type":"ping"}
{"type":"ack","seq":4311}
```

规则：

- **通知只是提示**，不携带内容；收到 `change` 后客户端执行 `GET /changes?since=cursor`。
- 通知可合并、可丢失、可重复；正确性不依赖它。
- 服务端每秒最多推送 10 条 `change`，超出合并为最大 `seq`。
- 心跳：客户端每 25 秒 `ping`；服务端 60 秒无消息则断开。
- 断线重连采用指数退避（1s → 60s，±20% 抖动），重连后立即拉取变更。
- WS 不可用期间客户端每 30 秒轮询一次。

---

## 4. 典型交互时序

### 4.1 修改文件并同步

```
Client A                      Server                       Client B
   │  watcher 事件                │                             │
   │  debounce + 扫描             │                             │
   │  POST /v1/blobs ───────────▶│                             │
   │  PUT chunks ───────────────▶│                             │
   │  POST complete ────────────▶│ 校验 SHA-256                │
   │  POST /ops (base=7) ───────▶│ BEGIN IMMEDIATE             │
   │                             │  CAS 7→8                    │
   │                             │  versions+entries+log+op    │
   │◀── 200 {revision:8,seq:4311}│ COMMIT                      │
   │                             │──── WS change seq=4311 ────▶│
   │                             │◀── GET /changes?since=4310 ─│
   │                             │──── 200 [seq=4311] ────────▶│
   │                             │◀── GET /blobs/{hash} Range ─│
   │                             │◀── POST /ack {4311} ────────│
```

### 4.2 超时重试（幂等）

```
Client A                       Server
   │ POST /ops op_id=X ────────▶│ 已提交，seq=4311
   │        ✗ 响应超时            │
   │ POST /ops op_id=X ────────▶│ 命中 operations
   │◀── 200 同 seq=4311 ────────│ X-Idempotent-Replay: true
```

---

## 5. 兼容性与版本演进

| 变更类型 | 规则 |
|---|---|
| 新增可选字段 | 允许，客户端必须忽略未知字段 |
| 新增错误码 | 允许，客户端按 HTTP 状态兜底 |
| 修改字段语义 | 必须提升 `X-Sync-Protocol` |
| 删除字段 | 必须提升协议版本，并保留一个版本的兼容期 |
| 新端点 | 允许，旧客户端不调用即可 |

服务端在 `426 PROTOCOL_UNSUPPORTED` 的 `details` 中给出 `supported: [1]`。

---

## 6. 限流

| 端点 | 限制（每设备） |
|---|---|
| `POST /enroll` | 每 IP 10 次/分钟 |
| `POST /ops` | 60 次/分钟 |
| `PUT chunk` | 600 次/分钟 |
| `GET /changes` | 120 次/分钟 |
| WS 连接 | 每设备 2 条 |

超限返回 `429`，`Retry-After` 为建议等待秒数。限流不得导致数据丢失（客户端退避重试）。

---

## 7. 验收条件（M4）

- [ ] 所有端点有集成测试，错误码与 §1.5 一致。
- [ ] 同一 `op_id` 重复提交 100 次，`versions` 只增加 0 或 1。
- [ ] 10 个并发客户端基于同一 `base_revision` 提交，1 个成功、9 个收到 `409`。
- [ ] `since` 早于保留窗口返回 `410` 且带 `min_retained_seq`。
- [ ] 分块上传中断后可凭 `status` 续传并最终通过整体校验。
- [ ] WS 断开后，仅靠 30 秒轮询可在 60 秒内补齐变更。
- [ ] 路径穿越样本全部返回 `422 PATH_INVALID`（见 [SECURITY.md](SECURITY.md) §7）。
