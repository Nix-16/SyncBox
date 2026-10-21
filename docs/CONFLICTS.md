# SyncBox 版本模型与冲突处理

| 项目 | 内容 |
|---|---|
| 文档版本 | v0.1（M0 基线） |
| 日期 | 2026-10-21 |
| 状态 | 已冻结，M2 定义类型、M5 实现解析器 |
| 关联文档 | [PRD.md](PRD.md) · [ARCHITECTURE.md](ARCHITECTURE.md) · [DATABASE.md](DATABASE.md) · [PROTOCOL.md](PROTOCOL.md) |

> **当前状态：无任何实现。** 本文档是 M5 冲突解析器的唯一行为规范。

---

## 1. 版本模型

### 1.1 不可变版本链

每个文件是一条只追加的版本链：

```
file_id = F
  revision 1  (create)   parent 0   hash H1   path p1
  revision 2  (update)   parent 1   hash H2   path p1
  revision 3  (rename)   parent 2   hash H2   path p2
  revision 4  (delete)   parent 3   hash H2   path p2   deleted=1
  revision 5  (restore)  parent 4   hash H3   path p2   deleted=0
```

规则：

| 规则 | 说明 |
|---|---|
| V-1 | `revision` 只能由服务端分配，严格 +1 |
| V-2 | `parent_revision` 必须等于前一个 revision（数据库不变量 DB-1） |
| V-3 | 版本行不可修改、不可删除（GC 除外，见 DATABASE §2.5） |
| V-4 | `file_id` 在重命名、删除、恢复后保持不变 |
| V-5 | `rename` 不改变 `content_hash`；`update` 不改变 `path`（移动+编辑 = rename + update 两条操作） |
| V-6 | `delete` 保留最后 `content_hash`，供冲突与复活判定使用 |

### 1.2 客户端如何得知自己的基线

客户端在 `local_entries.server_revision` 保存“最后一次成功同步的服务端版本”。任何本地修改都以该值为 `base_revision` 提交。

若客户端在等待提交期间又修改了文件：

1. 同一 `file_id` 只允许一个在途操作（INV-9），新修改合并进当前待提交操作（更新 `content_hash`、`staged_path`，保留原 `op_id` 与 `base_revision`）。
2. 若操作已发出且无法合并，则等待其结果后再生成新的 `op_id`。

---

## 2. 乐观并发控制（CAS）

### 2.1 服务端算法

```
commit(op):
    if op_id seen:               # 幂等
        replay stored response
    entry = load(op.file_id)     # create 跳过
    if op.kind != create and entry.missing: 404
    if op.kind != create and entry.revision != op.base_revision:
        return 409 REVISION_CONFLICT(current=entry)
    if op.kind == create and live_entry(path_key): 409 PATH_CASE_COLLISION
    if op.kind == create and tombstone(path_key).updated_seq > op.client_seq:
        return 409 RESURRECT_BLOCKED
    apply(op)                    # 单事务，见 DATABASE §2.4.1
```

### 2.2 为什么不用服务端合并

文件内容是任意字节，无法通用合并。因此采用**先提交者胜出 + 失败者保留副本**，把“合并”降级为“不丢数据”：

- 服务端从不静默丢弃客户端内容；
- 客户端在 409 后负责决定是“接受远端”“重放本地元数据”还是“生成冲突副本”；
- 每个决定都记录在 `conflicts` 表中，可审计、可在 GUI 中回看。

### 2.3 三个客户端基于同一旧版本的时序

```
base=7
A: commit update  → revision 8 (seq 100)   成功
B: commit update  → 409 (current=8)
C: commit update  → 409 (current=8)
B: 本地内容写入冲突副本 → create(new file_id) → revision 1 (seq 101)
B: 应用远端 revision 8 到原路径
C: 本地内容写入冲突副本 → create(new file_id) → revision 1 (seq 102)
C: 应用远端 revision 8 到原路径
```

结果：原路径为 A 的内容；B、C 的内容各自存在于冲突副本中，内容零丢失。

---

## 3. 冲突判定输入

客户端收到 409 后，需要四个信息：

| 信息 | 来源 |
|---|---|
| 本地意图 | `pending_ops.kind`、本地 `content_hash`、本地 `path` |
| 本地基线 | `pending_ops.base_revision` |
| 远端当前状态 | 409 响应 `details.current`（`revision`、`deleted`、`content_hash`、`path`、`editor_device`） |
| 本地是否已见过远端这次变更 | `client_seq` 与当前游标 |

判定顺序（短路）：

```
1. 本地内容哈希 == 远端内容哈希        → 无实质冲突，接受远端，丢弃本地操作
2. 本地意图 = delete 且 远端 deleted   → 双方都想删，幂等完成
3. 本地意图 = delete 且 远端存在       → 删除失败：修改胜出（P-D）
4. 本地意图 != delete 且 远端 deleted  → 远端删除失败：本地修改胜出，restore（P-R）
5. 本地意图 = rename 且 远端仅改内容   → 可合并，重放 rename（P-M）
6. 本地意图 != delete/rename 且 远端仅改名（内容未变）→ 采用远端路径后重放 update（P-RN）
7. 本地意图 = rename 且 远端已改名     → 路径冲突，远端路径胜出（P-N）
8. 其他（双方都修改了内容）            → 冲突副本（P-C）
```

---

## 4. 解决策略

### 4.1 P-C：双方修改内容（modify/modify）

1. 记录 `conflicts(kind='modify_modify', resolution='conflict_copy')`。
2. 将**本地内容**以新文件形式提交：
   - `kind = create`，`file_id = null`，`base_revision = 0`；
   - `path = <冲突副本名>`（§5）；
   - `content_hash` = 本地内容哈希；
   - `conflict = true`；
   - 新 `op_id`；
   - 本地 `local_entries.conflict_of = 原 file_id`。
3. 原路径应用远端版本（下载 `current.content_hash`）。
4. 若冲突副本提交失败（极端情况，如路径再次被占用），按 §5.3 追加序号重试，最多 5 次；仍失败则把本地内容保留在 `.syncbox/conflicts/` 目录并标记 `manual_pending`。

**保证**：本地内容在磁盘上（原路径的临时副本 → 冲突副本文件）与服务端（新版本）各有一份，直到替换成功才删除临时副本。

### 4.2 P-D：本地删除 vs 远端修改

**策略：修改胜出，删除放弃。**

- 丢弃本地的 delete 操作（`pending_ops.state = done`）。
- 记录 `conflicts(kind='modify_delete', resolution='remote_wins')`。
- 本地文件保留（若已被删除则按远端版本恢复）。
- GUI/CLI 中展示“该删除被放弃，因为其他设备修改了文件”。

理由：删除是唯一会减少数据的操作；让删除输给修改，符合“不丢数据”总目标。用户若仍要删除，可再次操作（此时基于新 revision）。

### 4.3 P-R：本地修改 vs 远端删除

**策略：修改胜出，文件恢复。**

1. 以 `kind = restore`、`base_revision = current.revision`（tombstone 的 revision）、`path = 原路径`、`content_hash = 本地内容哈希`、`conflict = true`、新 `op_id` 提交。
2. 服务端校验 `deleted=1` 且 `base_revision == current_revision` 后，revision+1，`op='restore'`，`deleted=0`。
3. 记录 `conflicts(kind='modify_delete', resolution='restored')`，`detail` 中写入 `{"restored":true,"restored_path":"..."}`。

4. 若 restore 再次返回 `409`（其他设备又删/改），重新进入判定。

**为什么不用冲突副本**：原路径此时空闲（已被删除），恢复不覆盖任何现有数据，且保留路径语义更符合用户预期。

### 4.4 P-M：本地重命名 vs 远端内容修改（可合并）

- 远端内容更新（revision n+1，路径不变）与本地重命名是**正交**的。
- 客户端以 `kind = rename`、`base_revision = current.revision`、`path = 本地新路径`、新 `op_id` 重放。
- 若新路径未被占用 → 成功，revision n+2，路径为新路径，内容为远端内容。
- 若新路径已被占用 → `409 PATH_CASE_COLLISION` → 转 §4.6 或冲突副本。
- 属安全合并，不产生冲突副本，也不写 `conflicts` 行。

### 4.5 P-RN：本地内容修改 vs 远端重命名（可合并）

- 远端只改了路径（`current.content_hash == 本地基线哈希`），本地只改了内容，两者正交。
- 步骤：
  1. 本地把文件移动到 `current.path`（先确保该路径可用；若被无关文件占用 → P-C）。
  2. 以 `kind = update`、`base_revision = current.revision`、`path = current.path`、`content_hash = 本地内容哈希`、新 `op_id` 提交。
  3. 记录 `conflicts(kind='rename_conflict', resolution='remote_wins')`（路径采用了远端的）。
- 不产生冲突副本，内容零丢失。

### 4.6 P-N：双方都重命名

**策略：先提交者路径胜出，后提交者采用胜出路径；内容不变，不产生副本。**

1. 取 409 响应中的 `current.path`。
2. 本地删除自己的旧路径目录项（文件内容仍在本地，hash 相同）。
3. 本地重命名为 `current.path`（若磁盘上该路径被占用且内容不同 → 转 P-C）。
4. 记录 `conflicts(kind='rename_conflict', resolution='remote_wins')`。
5. 同步 `local_entries.path` 与 `server_revision = current.revision`。

**若本地同时改了内容**：先按 P-C 保留内容为冲突副本，再采用远端路径。

### 4.7 create 路径碰撞

本地离线新建 `a.txt`，服务端已有其他 `file_id` 的 `a.txt`（或大小写折叠后同名）：

| 情况 | 处理 |
|---|---|
| 内容哈希相同 | 视为同一文件：把本地 `file_id` 改为服务端的 `file_id`，`server_revision = current.revision`，丢弃 create |
| 内容不同 | 将本地文件改名为冲突副本（§5）并 create；记录 `conflicts(kind='path_collision', resolution='conflict_copy')` |
| 后者是大小写变体（`A.txt` vs `a.txt`） | 同上，记录 `case_collision` |

### 4.8 双方删除

`409` 且 `current.deleted = true`：本地 delete 视为完成，不记录冲突，不重试。

### 4.9 重命名检测失败（本地视角）

客户端把“删除 + 新建”误判为两个操作时，服务端看到 delete(F) 与 create(新路径)。这仍然正确，只是丢失了 `file_id` 连续性。检测优先级：

1. `fs_ino + fs_dev` 相同（POSIX）或文件 ID 相同（Windows）；
2. 删除与新建在同一去抖批次内，且 `content_hash` 相同；
3. 时间窗内（默认 2 秒）路径相似度（`rename_candidates` 表兜底）。

无法判定时按删除+新建处理，正确性不受影响，仅版本历史可读性下降。

---

## 5. 冲突副本

### 5.1 命名

```
<stem> (conflict <device-short> <UTC yyyyMMdd-HHmmss>).<ext>
```

- `device-short`：`device_id` 前 8 位（十六进制小写）。
- 时间取**本地冲突处理时刻**的 UTC，秒级。
- 无扩展名时省略后缀点。
- 例：`notes (conflict 0192f0c4 20261021-143005).txt`。
- 超过 255 字节的段按 UTF-8 边界截断 `stem`。

### 5.2 位置

与原文件同目录。若原目录不存在（例如远端已删除整个目录），放在同步根目录。

### 5.3 名称再次冲突

追加 `-2`、`-3`……直到可用：`notes (conflict ...)-2.txt`。最多尝试 100 次，之后写入 `.syncbox/conflicts/` 保留本地副本。

### 5.4 语义

- 冲突副本是**普通文件**，在服务端有独立 `file_id`，参与正常同步。
- 客户端 `local_entries.conflict_of` 记录来源 `file_id`，仅用于 UI 关联，不影响同步。
- 冲突副本不会被自动删除；用户处理后在 GUI 中标记。

---

## 6. 删除语义

| 规则 | 说明 |
|---|---|
| D-1 | 删除只写 tombstone（`deleted=1`），不物理删除 `entries` 行 |
| D-2 | `delete` 必须携带 `base_revision` 与当前 `path`，任一对不上 → 409 |
| D-3 | tombstone 保留最后 `content_hash` 与 `path` |
| D-4 | 删除不删除 blob 引用计数以外的内容；blob 由 GC 管理 |
| D-5 | 旧设备重连时，`client_seq` 早于 tombstone 的 `updated_seq` → `RESURRECT_BLOCKED`（见 ARCHITECTURE §4.5） |
| D-6 | 已见 tombstone 的设备可以显式 `create` 同路径（用户主动重建），服务端分配新 `file_id` |
| D-7 | tombstone GC 后，旧的 create 不再被阻断（见 DATABASE §2.5 保留期） |
| D-8 | 重命名到已释放的 tombstone 路径是允许的（`file_id` 不同，不构成复活）；仅 create 受 `RESURRECT_BLOCKED` 约束 |

### 6.1 防止“删除被旧设备撤销”的完整链路

```
时刻 T1: A 删除 a.txt        → tombstone revision 5, seq 500
时刻 T2: B 离线，游标仍为 480，本地有 a.txt (revision 4)
时刻 T3: B 上线，扫描发现 a.txt 存在但索引未变 → 不产生操作（hash 与索引一致）
时刻 T3': 若 B 修改了 a.txt → 提交 update(base=4) → 409（不是 5）
          → 客户端拉取到删除 → 进入 P-R（本地修改胜出，restore）
时刻 T3'': 若 B 误删后又重建 → create(client_seq=480)
          → tombstone.updated_seq=500 > 480 → 409 RESURRECT_BLOCKED
          → B 先拉取变更，看到删除；若仍有文件则按 P-R 显式 restore
```

关键点：**旧设备不能“无意识地”复活文件**；任何复活都必须是客户端已知删除后的显式 `restore`/`create`。

---

## 7. 跨平台路径冲突

### 7.1 大小写与 Unicode

| 场景 | 服务端 | 客户端 |
|---|---|---|
| `A.txt` 与 `a.txt` 同时提交 | 第二个 `409 PATH_CASE_COLLISION` | 改名冲突副本，记录 `case_collision` |
| NFC 与 NFD 形式（`é`） | 服务端先 NFC 化，`path_key` 相同 → 冲突 | 同上 |
| Windows 大小写不敏感目录 | — | 扫描时若发现 `A.txt`/`a.txt` 共存（不可能），以服务端为准 |

`path_key = casefold(NFC(path))`，比较单位是码点。计划使用 utf8proc 的 `utf8proc_NFC` 与 `utf8proc_tolower`（简单折叠），服务端与客户端链接同一实现以避免平台差异；最终选型在 M1 决策并记录于 [STATUS.md](STATUS.md)（O-02）。

### 7.2 平台不可表示的路径

下行到 Windows 时可能出现：

| 问题 | 处理 |
|---|---|
| 保留名（`CON`、`NUL`、`COM1`…） | 写入 `.syncbox/quarantine/<file_id>/<原路径>`，记录 `path_collision` |
| 非法字符 `< > : " \| ? *` | 同上 |
| 尾随空格或点 | 同上 |
| 路径超过 `MAX_PATH`（未启用长路径） | 同上并告警，建议启用长路径支持 |
| 文件被其他进程独占 | 重试 3 次（间隔 200/500/1000 ms），仍失败 → `retry` |

隔离区文件不参与同步，用户在 GUI 中可见并可手动处理。

### 7.3 原子替换差异

| 平台 | 实现 | 注意 |
|---|---|---|
| Linux | `rename(2)` + 父目录 `fsync` | 跨文件系统 rename 会失败，临时目录必须在同一根内 |
| macOS | `rename(2)` + `F_FULLFSYNC`（尽力） | `FSEvents` 延迟高，依赖全量核对兜底 |
| Windows | `ReplaceFileW`；失败回退 `MoveFileExW(MOVEFILE_REPLACE_EXISTING\|MOVEFILE_WRITE_THROUGH)` | 目标被占用时返回 `ERROR_SHARING_VIOLATION`，进入重试 |

---

## 8. 冲突记录与用户处理

### 8.1 `conflicts` 表字段（见 DATABASE §3.1）

| 字段 | 说明 |
|---|---|
| `kind` | `modify_modify` / `modify_delete` / `rename_conflict` / `path_collision` / `case_collision` / `hash_mismatch` |
| `resolution` | `conflict_copy` / `remote_wins` / `restored` / `manual_pending` |
| `local_path` / `remote_path` | 双方路径 |
| `detail` | JSON：双方 hash、revision、device、是否 restore |

### 8.2 自动解决 vs 人工介入

| 情况 | 自动 | 需要用户 |
|---|---|---|
| P-C 冲突副本 | 是（先保证数据安全） | 合并内容、删除多余副本 |
| P-D 删除放弃 | 是 | 确认是否仍要删除 |
| P-R 恢复 | 是 | 确认是否仍要删除 |
| P-N 采用远端路径 | 是 | 一般无需 |
| 隔离区文件 | 是（隔离） | 手动重命名/移动 |
| `hash_mismatch` 连续 3 次 | 否 | 必须人工（可能磁盘故障） |

### 8.3 GUI 展示（M7）

- 冲突列表按文件聚合：原文件 + 所有冲突副本。
- 提供“保留此版本”“删除副本”“稍后处理”。
- “保留”操作 = 以选中内容创建新 revision（update，base=当前 revision）；冲突副本随后可删除。

---

## 9. 示例

### 9.1 双设备并发修改

```
初始: F revision 7, hash H0, path notes.txt
A: update(H_A, base=7) → 200 rev 8 hash H_A
B: update(H_B, base=7) → 409 current{rev 8, hash H_A}
B: H_B != H_A → P-C
B: create(notes (conflict 0192f0c4 20261021-143005).txt, H_B) → 200 rev 1
B: 下载 H_A → notes.txt
结果: notes.txt = H_A; 冲突副本 = H_B; conflicts 1 行
```

### 9.2 删除 vs 修改

```
初始: F rev 7 notes.txt
A: delete(base=7) → 200 rev 8 deleted=1
B: update(H_B, base=7) → 409 current{rev 8, deleted=true}
B: P-R → restore(base=8, H_B, path=notes.txt) → 200 rev 9 deleted=0
结果: notes.txt = H_B; conflicts 1 行 (modify_delete, restored)
```

### 9.3 重命名 vs 修改

#### 9.3a 远端改名、本地改内容（P-RN）

```
初始: F rev 7, path=a/x.txt, hash=H0
A: rename(base=7, path=b/x.txt) → 200 rev 8, path=b/x.txt, hash=H0
B: update(H_B, base=7, path=a/x.txt) → 409 current{rev 8, path=b/x.txt, hash=H0}
B: 判定 current.path != op.path 且 current.content_hash == 本地基线哈希 H0
   → P-RN
B: 本地文件移动到 b/x.txt
B: update(base=8, path=b/x.txt, hash=H_B) → 200 rev 9
结果: b/x.txt = H_B; 无冲突副本; conflicts 1 行 (rename_conflict, remote_wins)
```

#### 9.3b 本地改名、远端改内容（P-M）

```
初始: F rev 7, path=a/x.txt, hash=H0
A: update(H_A, base=7, path=a/x.txt) → 200 rev 8, hash=H_A
B: rename(base=7, path=b/x.txt) → 409 current{rev 8, path=a/x.txt, hash=H_A}
B: 判定本地意图=rename 且 current.path == op 的旧路径、内容已变
   → P-M：重放 rename(base=8, path=b/x.txt)
   → 200 rev 9, path=b/x.txt, hash=H_A
结果: b/x.txt = H_A; 无冲突副本; 无冲突记录（路径与内容均安全保留）
```

### 9.4 三设备同时新建同名文件

```
A: create(a.txt, H_A) → 200 F1
B: create(a.txt, H_B) → 409 PATH_CASE_COLLISION(current F1)
   → 改名冲突副本 create → 200 F2
C: create(a.txt, H_A) → 409，H 相同 → 视为同一文件，采用 F1
结果: a.txt=F1(H_A); 冲突副本=F2(H_B); C 无新版本
```

---

## 10. 验收条件（M5）

- [ ] §9 的 5 个示例可作为集成测试逐条复现。
- [ ] 任意冲突场景中，双方内容哈希都能在磁盘或冲突副本中找到。
- [ ] P-C 在 5 个并发设备下有 4 个冲突副本，且命名互不相同。
- [ ] P-D/P-R 后 `conflicts` 表行数与预期一致。
- [ ] `RESURRECT_BLOCKED` 在旧游标 create 时触发，在已见删除的 create 时不触发。
- [ ] 大小写冲突在 Linux 客户端创建 `A.txt`/`a.txt` 时可复现并被处理。
- [ ] 冲突副本提交使用新 `op_id`，重放不产生第二个副本。
- [ ] 冲突处理后原文件与服务端 hash 一致。
