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
| `syncboxd admin gc --db PATH [--operations-days N]` | 清理过期幂等记录（默认 7 天） |
| `syncboxd admin backup --db PATH --out BACKUP` | 一致性备份 |

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
syncboxd admin gc --db /var/lib/syncbox/server.db --operations-days 7
# operations_removed=123
```

当前只清理 `operations`（幂等回放记录）。以下对象**尚未自动 GC**：

- `change_log` 与 tombstone：依赖设备 `last_ack_seq` 与 90 天水位（DATABASE §2.5），M9 实现；
- 未被引用的 blob：需要可靠引用计数/GC 水位，M9 实现。

在实现前，磁盘增长主要来自变更历史与 blob；请监控磁盘并保留备份。

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

## 6. 日志与审计（部分）

当前服务端尚未接入结构化审计日志（M8 待办）。部署时建议：

- 由 systemd/journald 或容器日志收集 stdout/stderr；
- 不要把 `device_token` 写入任何外部日志（SECURITY §8.1）；
- 定期检查 `syncboxd admin fsck` 与磁盘水位。

---

## 7. 故障处置

| 现象 | 排查 |
|---|---|
| 客户端 403 `DEVICE_REVOKED` | `admin revoke` 后未重新注册 |
| 客户端 410 `CURSOR_EXPIRED` | 变更日志已 GC；客户端应执行全量快照（已实现） |
| fsck 报告缺失 blob | 从备份恢复 blobs，或让客户端重新上传（服务端会拒绝缺失 blob 的提交） |
| 磁盘将满 | 暂停写入、扩容；当前 GC 不能回收 blob |

---

## 8. 待完成（M8/M9）

- 结构化审计日志与脱敏测试；
- WS 会话在设备撤销后立即断开；
- blob 与 change_log/tombstone 的完整 GC；
- 路径 fuzz 与依赖 CVE 扫描；
- 监控指标与告警。
