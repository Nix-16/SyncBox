# SyncBox 运维手册

| 项目 | 内容 |
|---|---|
| 文档版本 | v0.1（M8） |
| 日期 | 2026-11-03 |
| 状态 | 已实现 fsck / gc / backup；审计与完整 blob GC 待完成 |
| 关联文档 | [DATABASE.md](DATABASE.md) · [SECURITY.md](SECURITY.md) · [ROADMAP.md](ROADMAP.md) |

---

## 1. 服务端布局

```
<data-dir>/
├── server.db          SQLite（WAL：server.db-wal / server.db-shm）
├── blobs/             内容寻址块：<sha256>/<index>.chunk
└── server.toml        配置（M9）
```

服务端通过命令行参数指定路径：

```bash
syncboxd --db /var/lib/syncbox/server.db \
         --blob-root /var/lib/syncbox/blobs \
         --listen 0.0.0.0 --port 8443 \
         --tls-cert /etc/syncbox/server.crt \
         --tls-key  /etc/syncbox/server.key
```

---

## 2. 管理命令

所有管理命令直接操作数据库，**不要求服务端在线**；`backup` 可在服务端运行时执行（SQLite backup API）。

| 命令 | 作用 |
|---|---|
| `syncboxd admin create-space --db PATH --name NAME` | 创建空间，输出 `space_id` |
| `syncboxd admin enroll --db PATH --space ID [--ttl-hours N]` | 签发一次性注册令牌 |
| `syncboxd admin revoke --db PATH --device ID [--reason TEXT]` | 撤销设备（立即影响后续 HTTP 请求） |
| `syncboxd admin fsck --db PATH` | 完整性检查 |
| `syncboxd admin gc --db PATH [--blob-root PATH] [--operations-days N]` | 清理过期幂等记录与未引用 blob |
| `syncboxd admin backup --db PATH --out BACKUP` | 一致性备份 |

### 2.1 通过 HTTP 撤销设备

启动服务端时设置管理员令牌（未设置则端点禁用）：

```bash
export SYNCBOX_ADMIN_TOKEN="$(openssl rand -base64 32)"
syncboxd ...
```

```bash
curl -X DELETE https://sync.example.com:8443/v1/devices/<device_id> \
     -H "Authorization: Bearer $SYNCBOX_ADMIN_TOKEN"
# 204；该设备的 HTTP 请求立即 403，WebSocket 被关闭
```

建议仅从受信网络访问该端点。

---

## 3. fsck

```bash
syncboxd admin fsck --db /var/lib/syncbox/server.db
```

输出示例：

```
entries=42 versions=57 changes=57 blobs=30
ok
```

检查项：

1. `PRAGMA integrity_check`；
2. `PRAGMA foreign_key_check`；
3. 活跃条目引用的 blob 必须存在且 `state='complete'`。

退出码：`0` 正常，`1` 发现问题（问题逐行打印到 stderr）。

**限制**：当前不逐块重算 blob 内容的 SHA-256。深入校验需在 M9 增加 `--deep`（会读取全部块）。

---

## 4. GC

```bash
syncboxd admin gc --db /var/lib/syncbox/server.db \
                  --blob-root /var/lib/syncbox/blobs \
                  --operations-days 7
# operations_removed=123 blobs_removed=4
```

清理内容：

- `operations`：超过 `--operations-days`（默认 7 天）的幂等回放记录；
- `blobs`：`ref_count=0` 且创建超过 24 小时的 blob（DB 行 + 块文件）。

**尚未自动 GC**：`change_log` 与 tombstone。它们依赖所有活跃设备的
`last_ack_seq` 与 90 天水位（DATABASE §2.5），留待 M9。在此之前变更历史会持续增长，
请监控磁盘并保留备份。

---

## 5. 备份与恢复

### 5.1 在线备份

```bash
syncboxd admin backup --db /var/lib/syncbox/server.db \
                      --out /backup/server-$(date +%F).db
```

备份包含数据库快照，**不包含 `blobs/` 目录**。完整备份需同时归档 blob 目录：

```bash
tar -C /var/lib/syncbox -czf /backup/blobs-$(date +%F).tar.gz blobs
```

建议顺序：先 `backup` 数据库，再归档 blobs（blob 只增不改，晚归档不会破坏一致性）。

### 5.2 恢复

1. 停止 `syncboxd`；
2. 恢复数据库与 blobs 到原路径；
3. 运行 `syncboxd admin fsck --db ...`；
4. 启动服务端，观察客户端是否补齐。

> 恢复到新机器时，设备 token 仍有效；如需强制重新注册，逐台 `revoke` 后重新 `enroll`。

---

## 6. 日志与审计

审计日志为 JSON Lines，通过环境变量启用：

```bash
export SYNCBOX_AUDIT_LOG=/var/log/syncbox/audit.log
syncboxd ...
```

示例：

```json
{"event":"device_enrolled","device_id":"0192...","platform":"linux"}
{"event":"auth_failure","token_hash_prefix":"39a5c1f0","reason":"invalid device token"}
{"event":"device_revoked","device_id":"0192...","actor":"admin","reason":"..."}
```

- 只记录 token 哈希前缀，原始 token 不落盘；
- 由 logrotate/journald 轮转；权限建议 `0600`；
- 未设置 `SYNCBOX_AUDIT_LOG` 时输出到 stderr。

部署建议：定期检查 `syncboxd admin fsck` 与磁盘水位；审计日志与业务数据分开备份。

---

## 7. 故障处置

| 现象 | 排查 |
|---|---|
| 客户端 403 `DEVICE_REVOKED` | `admin revoke` 后未重新注册 |
| 客户端 410 `CURSOR_EXPIRED` | 变更日志已 GC；客户端应执行全量快照（已实现） |
| fsck 报告缺失 blob | 从备份恢复 blobs，或让客户端重新上传（服务端会拒绝缺失 blob 的提交） |
| 磁盘将满 | 运行 `admin gc` 回收未引用 blob；change_log 仍需手工归档/扩容 |

---

## 8. 待完成（M8/M9）

- 结构化审计日志与脱敏测试；
- WS 会话在设备撤销后立即断开；
- change_log/tombstone 的 GC（需设备 ACK 水位）；
- 路径 fuzz（缺 clang）与依赖 CVE 扫描；
- 监控指标与告警。
