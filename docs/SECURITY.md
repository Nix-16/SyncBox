# SyncBox 安全设计

| 项目 | 内容 |
|---|---|
| 文档版本 | v0.1（M0 基线） |
| 日期 | 2026-10-21 |
| 状态 | 已冻结，M4/M8 实现与验证 |
| 关联文档 | [PRD.md](PRD.md) · [ARCHITECTURE.md](ARCHITECTURE.md) · [PROTOCOL.md](PROTOCOL.md) · [TEST_PLAN.md](TEST_PLAN.md) |

> **当前状态：无任何安全相关实现。** 本文档是 M4（基础认证/TLS/路径校验）与 M8（加固与验证）的验收依据。

---

## 1. 安全目标与资产

| 资产 | 机密性 | 完整性 | 可用性 |
|---|---|---|---|
| 用户文件内容 | 高（传输中；服务端静态存储为明文） | 极高 | 高 |
| `device_token` | 极高 | 极高 | — |
| 服务端数据库 | 高 | 极高 | 高 |
| 版本历史 / 变更日志 | 中 | 极高 | 中 |
| 设备身份与撤销状态 | 中 | 极高 | 高 |

**明确假设**：v1 **不提供端到端加密**。服务端可读取文件内容；服务端主机与磁盘必须由运维者保护。E2EE 列入 v2，见 PRD §7。

---

## 2. 威胁模型

### 2.1 攻击者画像

| 编号 | 攻击者 | 能力 |
|---|---|---|
| A1 | 网络中间人 | 可拦截/篡改/重放公网流量 |
| A2 | 被盗设备 | 持有有效 `device_token` 与本地文件 |
| A3 | 被撤销但仍持有旧 token 的设备 | 可发起请求 |
| A4 | 恶意同步客户端 | 可构造任意 JSON 与路径 |
| A5 | 服务端本地低权限进程 | 可读取服务端文件系统 |
| A6 | 粗心用户 | 误删、误改、误配置 |

### 2.2 威胁与缓解

| 编号 | 威胁 | 影响 | 缓解 | 验证 |
|---|---|---|---|---|
| T-01 | 中间人窃听文件内容 | 机密性 | 强制 TLS 1.2+，禁止明文 HTTP（回环除外） | TC-SEC-01 |
| T-02 | 中间人篡改上传内容 | 完整性 | TLS + SHA-256 双向校验；服务端重算哈希 | TC-SEC-02 |
| T-03 | 重放提交请求 | 重复版本/篡改 | `op_id` 幂等 + `request_hash` + CAS | TC-SEC-03 |
| T-04 | 路径穿越写文件（`../../etc/passwd`） | 任意写 | 路径规范化 + 拒绝规则 + 服务端根目录 `weakly_canonical` 复核 | TC-SEC-10 |
| T-05 | 绝对路径 / 盘符 / UNC | 任意写 | 显式拒绝 | TC-SEC-11 |
| T-06 | 符号链接穿越 | 越界读写 | 默认不跟随符号链接；扫描时跳过并记录 | TC-SEC-12 |
| T-07 | token 泄露 | 冒用设备 | 仅存哈希；文件权限 0600；可撤销；日志脱敏 | TC-SEC-20 |
| T-08 | 被盗设备持续同步 | 数据泄露 | 设备撤销立即生效（每次请求查状态） | TC-SEC-21 |
| T-09 | 恶意客户端耗尽磁盘 | DoS | 配额、`max_file_size`、限流、blob GC | TC-SEC-30 |
| T-10 | 恶意客户端伪造哈希 | 内容不一致 | 服务端重算 SHA-256，不信任声明 | TC-SEC-31 |
| T-11 | 资源耗尽（大 JSON / 慢速攻击） | DoS | 请求体上限 1 MiB（JSON）、读超时、连接数上限 | TC-SEC-32 |
| T-12 | 日志泄露敏感信息 | 机密性 | 不记录 token、内容、文件正文；路径可配置脱敏 | TC-SEC-40 |
| T-13 | 时序攻击比较 token | 身份冒用 | 使用恒定时间比较（`CRYPTO_memcmp`） | 代码审查 |
| T-14 | 数据库被本地进程读取 | 机密性 | 文件权限 0600；文档声明不防本地 root | 部署文档 |

### 2.3 非目标

- 防御拥有服务端 root 权限的攻击者；
- 防御物理接触设备并提取内存的攻击者；
- 浏览器端 XSS/CSRF（无 Web 前端）；
- 供应链攻击（依赖锁定与校验列入 M8 加分项，非 P0）。

---

## 3. 认证与授权

### 3.1 注册流程

```
管理员（本地 CLI）: syncboxd admin enroll --space <id> [--ttl 24h]
   → 生成 32 字节随机 enrollment token，仅打印一次
   → 数据库存 SHA-256(token)

客户端: POST /v1/enroll {enrollment_token, device_name, platform}
   → 服务端恒定时间比较哈希、检查未使用未过期
   → 事务内标记 used_by/used_at
   → 生成 32 字节 device_token（base64url，无填充）
   → 数据库存 SHA-256(device_token)，返回明文一次
```

| 规则 | 说明 |
|---|---|
| EN-1 | enrollment token 单次使用，TTL 默认 24 小时，过期即失效 |
| EN-2 | 每个 token 绑定一个 `space_id` |
| EN-3 | 注册失败不区分“不存在/已使用/已过期”，统一 `401 AUTH_REQUIRED`，避免枚举 |
| EN-4 | 注册与认证请求记录审计日志（不含 token 明文） |

### 3.2 请求认证

```
Authorization: Bearer <device_token>
```

服务端处理顺序：

1. 提取 token；缺失或格式错 → 401。
2. `SHA-256(token)` 查 `devices.token_hash`（唯一索引）。
3. 恒定时间比较（`CRYPTO_memcmp`）。
4. 检查 `status='active'`；否则 403 `DEVICE_REVOKED`。
5. 检查 token 所属 `space_id` 与 URL 中 `space_id` 一致；否则 403 `SPACE_FORBIDDEN`。
6. 更新 `last_seen_at`（异步、节流到每分钟一次）。

**token 设计**：

- 32 字节 CSPRNG（`RAND_bytes`），base64url 编码后 43 字符；
- 高熵随机值无需加盐，直接 SHA-256 存储即可（防离线爆破无意义）；
- 明文只出现在注册响应中一次；客户端写入 `0600` 文件，权限不符时拒绝启动并告警。

### 3.3 授权模型

v1 只有两种角色：

| 角色 | 权限 |
|---|---|
| 设备（device） | 读写自己所属 space 的条目与 blob；不能管理其他设备 |
| 管理员（本地 CLI） | 创建 space、签发/撤销 enrollment token、撤销设备、执行 GC/fsck |

**没有**跨 space 访问；所有查询强制带 `space_id` 且与设备绑定。

### 3.4 设备撤销

- `DELETE /v1/devices/{id}`（M8；v1 通过本地 CLI `syncboxd admin revoke`）。
- 撤销写 `status='revoked'`、`revoked_at`、`revoked_reason`。
- 不删除设备行（保留审计与游标历史）。
- 已建立的 WebSocket 连接在下次心跳时被关闭。
- 撤销后：401/403 立即返回，客户端停止同步并提示重新注册（需新 enrollment token）。

---

## 4. 传输安全（TLS）

| 项 | 要求 |
|---|---|
| 最低版本 | TLS 1.2；推荐 1.3 |
| 套件 | 仅 AEAD（AES-GCM / ChaCha20-Poly1305），禁用 CBC、RC4、3DES |
| 证书 | 服务端必须配置受信证书；客户端默认校验链与主机名 |
| 自签名 | 仅允许显式配置 `ca_file` 或 `insecure_skip_verify=false` 的调试模式，并在日志中告警 |
| 重协商 | 禁用客户端发起的重协商 |
| 会话 | 允许 TLS 会话复用以减少握手开销 |
| 回环 | `127.0.0.1`/`::1` 可配置明文，仅用于本地集成测试，启动时打印警告 |
| HSTS | 无浏览器，不适用 |

**证书固定（pinning）**：v1 不默认启用；提供 `server.cert_fingerprint_sha256` 可选配置，M8 实现。

WebSocket 使用 WSS，复用同一 TLS 配置；认证使用 `Authorization` 头（非 query 参数，避免日志泄露）。

---

## 5. 路径安全

### 5.1 入站校验（服务端，必须）

对每个 `path` 字段按 [DATABASE.md](DATABASE.md) §1.1 执行：

1. UTF-8 合法性校验（拒绝非法字节序列、代理对、NUL）；
2. 拒绝绝对路径（`/`、`\`、盘符 `C:`、UNC `\\`）；
3. 拒绝 `.`、`..` 段（在 NFC 之后检查，防止 Unicode 变体绕过）；
4. 拒绝空段、尾随 `/`；
5. 拒绝控制字符（U+0000–U+001F、U+007F）；
6. 拒绝 Windows 保留名与非法字符（`< > : " | ? *`）；
7. 拒绝段尾空格/点；
8. 长度限制：段 ≤ 255 字节，整路径 ≤ 1024 字节；
9. NFC 规范化后计算 `path_key`。

任一条失败 → `422 PATH_INVALID`，不写库。

### 5.2 出站校验（客户端，必须）

即使服务端已校验，客户端在落盘前仍执行：

1. 同样规则校验；
2. `std::filesystem::weakly_canonical(root / path)` 后必须仍以 `canonical(root)` 为前缀（防符号链接与 `..` 绕过）；
3. 目标路径的父目录逐级创建时，不得穿过符号链接（`create_directories` 前检查）；
4. 平台特有校验（Windows 保留名、长路径）。

失败 → 不落盘，写入隔离区并记录 `path_collision`。

### 5.3 路径穿越测试集（M8 必须全拒）

```
../etc/passwd
..\..\windows\system32\config\sam
/absolute/path
C:\Windows\System32
\\server\share\file
a/../../b
a/./b
a//b
a/ (trailing slash)
..%2f..%2fetc/passwd        (作为字面量，不是 URL 解码后)
a\x00b
CON
con.txt
NUL
COM1
aux.
file.
file 
a:b
a|b
a*b
a?b
（超过 1024 字节的路径）
（NFC 与 NFD 混合的 ".." 变体）
```

预期：全部 `422 PATH_INVALID`；客户端侧全部进入隔离/拒绝路径。

### 5.4 符号链接

- 扫描默认**不跟随**符号链接，记录 `warning` 事件。
- 不将符号链接作为文件条目同步（v1 范围外，见 PRD §7）。
- 落盘时不通过符号链接写入：打开文件使用 `O_NOFOLLOW`（POSIX）或检查 `FILE_ATTRIBUTE_REPARSE_POINT`（Windows）。

---

## 6. 数据完整性

| 环节 | 机制 |
|---|---|
| 上传 | 每块 `X-Chunk-SHA256`；整体 `POST complete` 重算 SHA-256 |
| 提交 | 校验 `blobs.state='complete'` 且 `content_hash`/`size` 一致 |
| 下载 | 流式计算 SHA-256，与变更记录比对；不符则重试 3 次后报警 |
| 落盘 | 临时文件 + `fsync` + 原子替换；替换后可选二次校验 |
| 索引 | SQLite WAL + 事务；`integrity_check` 定期执行（M9） |
| 服务端 | 事务内写 `versions`/`entries`/`change_log`；`fsck --rebuild-entries` 可重建物化视图 |

**原则**：服务端与客户端都不信任对方声明的哈希，均自行计算。

---

## 7. 可用性与滥用防护

| 控制 | 值 |
|---|---|
| JSON 请求体上限 | 1 MiB（`/ops`、`/blobs` 初始化） |
| 分块上传上限 | 单块 16 MiB |
| 单文件上限 | 默认 8 GiB（可配） |
| 单 space 存储配额 | 默认 100 GiB（M8 可配） |
| 连接数上限 | 每设备 8，全局 1024 |
| 请求超时 | 读 30 s，写 30 s，空闲 60 s |
| 慢速攻击 | Beast `read_some` 超时 + 每连接最大持续时间 |
| 分块上传并发 | 每设备 8 |
| 限流 | 见 [PROTOCOL.md](PROTOCOL.md) §6 |
| 失败上传清理 | 24 小时未完成 → 删除块与元数据 |

服务端在内存中维护每设备计数器（令牌桶），超限返回 `429`。限流只影响请求速率，不影响已入队数据的最终同步。

---

## 8. 日志与审计

### 8.1 禁止记录

- `device_token`、`enrollment_token` 明文（含其前缀/后缀）；
- 文件内容、文件正文片段；
- 完整请求体（可记录 `request_hash`、`op_id`、`file_id`、`path`）。

### 8.2 必须记录（审计）

| 事件 | 字段 |
|---|---|
| 设备注册成功/失败 | `device_id`, `platform`, `remote_ip`, `result` |
| 设备撤销 | `device_id`, `admin`, `reason`, `ts` |
| 认证失败 | `token_hash_prefix`（前 8 hex）, `remote_ip`, `reason` |
| 提交被拒 | `op_id`, `file_id`, `code`, `base_revision`, `current_revision` |
| 路径校验失败 | `device_id`, `path`（截断 256 字节）, `rule` |
| blob 哈希不符 | `content_hash`, `declared`, `computed` |
| GC 执行 | `min_retained_seq`, `deleted_rows`, `blob_bytes` |

### 8.3 日志安全

- 日志文件权限 `0600`；
- 支持 JSON sink，便于集中采集；
- 日志轮转（spdlog rotating sink，默认 50 MB × 5）；
- 路径字段可选脱敏（M8）。

---

## 9. 本地存储安全

| 对象 | 权限 | 说明 |
|---|---|---|
| `device.token` | `0600` | 客户端启动时检查；权限过宽则拒绝启动 |
| `client.db` | `0600` | 含文件路径与哈希，不含内容 |
| 同步目录 | 用户自有 | 不修改原有权限 |
| `.syncbox/` | `0700` | 临时文件、隔离区、日志 |
| `server.db` | `0600` | 服务端运维责任 |
| `blobs/` | `0700` | 内容寻址；文件名即哈希，不泄露原路径 |

**明文存储声明**：服务端 blob 与客户端同步目录均为明文。文档与 README 必须明确说明，避免用户误解为 E2EE。

---

## 10. 安全配置基线（服务端）

```toml
[tls]
enabled = true
cert_file = "/etc/syncbox/server.crt"
key_file  = "/etc/syncbox/server.key"
min_version = "1.2"

[limits]
max_json_bytes = 1048576
max_file_bytes = 8589934592
max_connections = 1024
request_timeout_s = 30

[auth]
require_tls = true
revocation_check = "per_request"

[storage]
db_path = "/var/lib/syncbox/server.db"
blob_root = "/var/lib/syncbox/blobs"
```

启动时校验：

- TLS 开启且证书可读；
- 数据库与 blob 目录权限不宽于 `0700`（否则警告，`strict_permissions=true` 时拒绝启动）；
- 非回环地址且 TLS 关闭 → 拒绝启动。

---

## 11. 残余风险

| 风险 | 等级 | 说明 | 后续 |
|---|---|---|---|
| 服务端可读明文 | 中 | v1 架构决定 | v2 E2EE |
| 设备被盗后 token 可用 | 中 | 依赖用户及时发现并撤销 | M8 增加 token 轮换 |
| SQLite 文件被本地 root 读取 | 中 | 不在威胁模型内 | 文档声明 + 磁盘加密建议 |
| 依赖库漏洞 | 中 | Boost/OpenSSL/SQLite 需跟踪 CVE | M8 依赖锁定与扫描 |
| 冲突副本可能包含敏感内容 | 低 | 同步语义必然结果 | 用户自行管理 |
| GC 后旧设备复活 | 低 | 保留期 + 日志，见 CONFLICTS §6 | 监控告警 |

---

## 12. 验收条件

- [ ] 明文 HTTP（非回环）配置启动失败。
- [ ] 过期/已用/伪造 enrollment token 均返回同一 401。
- [ ] 撤销设备后 1 秒内所有请求失败（含 WS 断开）。
- [ ] §5.3 测试集 100% 被拒绝，且服务端无文件落盘。
- [ ] 篡改分块后被 `CHUNK_HASH_MISMATCH` 拒绝，blob 不进入 `complete`。
- [ ] 声明哈希与内容不符时提交被拒（`BLOB_HASH_MISMATCH`）。
- [ ] 日志中搜索 token 明文为零命中。
- [ ] `device.token` 权限 0644 时客户端拒绝启动。
- [ ] 同一 `op_id` 重放不产生新版本（与 PROTOCOL §1.6 一致）。
