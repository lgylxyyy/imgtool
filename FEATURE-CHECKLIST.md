# 新功能开发规范（Adding a Feature — 必读）

> 目的：以后每加一个新功能，**基础能力齐全、风格一致、不返工**。
> 状态：本规范来自已落地的实现（日志/进度/取消/失败卡片/命令表/多语言），不是设想。
> 引用：`DESIGN-OUTPUT.md`（对外契约）· `WORKPLAN-HARDEN.md`（已知缺口与性能点）· `THIRD_PARTY.md`（许可）
>
> **一句话总则**：**库给事实（码 + 数值），视图层出文案；一处打点，多处消费；每个功能自带正反两个用例。**

---

## 一、新功能必须具备的基础能力（缺一项不许合入）

| # | 能力 | 硬要求 | 位置 |
|---|---|---|---|
| 1 | **返回值** | 所有公开函数返回 `img_err_t`（`IMG_OK`=0，负值=错误）；**绝不静默失败、绝不崩** | 库 |
| 2 | **参数体检** | 开头判空/判范围；越界返回 `IMG_EINVAL` 或 `IMG_ERANGE`；**所有 `off+len` 类判定必须防加法溢出**（用 `off > size \|\| len > size-off`，不用 `off+len > size`） | 库 |
| 3 | **错误码** | 稳定短码（如 `FAT-ENOSPC`）；新增必须同时加进 CLI 的 `E_TEXT` 表（原因+建议，中/英两列） | 库 + CLI |
| 4 | **错误上下文** | 失败时给出"在哪个对象/什么位置"（路径/LBA/分区）；经 `img_result_fail()` 落地 | 库 |
| 5 | **进度** | 任何可能 >1 秒的操作必须发进度：`img_prog` 回调 **或** 事件 `begin/done`；**长操作拆段**（否则"开始/完成"隔十分钟没法定位） | 库 |
| 6 | **取消** | ≥1MiB 的循环里必须查 `img_cancelled()`；取消后**不写目录项 + 释放已分配簇**（不留半成品）。做不到回滚的（如 NTFS 直灌）必须在卡片里写清"该分区需重做" | 库 |
| 7 | **内存恒定** | **不许把整个文件读进内存**；用 1MiB 分块流式（Android 上整文件进内存 = OOM 闪退） | 库 |
| 8 | **并发** | 同一句柄串行（`img_ctx` 锁已内建）；库内全局状态用锁（`img_lock`）；**不许假设单线程** | 库 |
| 9 | **语言中立** | 库不出现任何用户可见文案，只发 `event/action/error_code` + 数值（`message` 仅作日志备注） | 库 |
| 10 | **命令表条目** | 在 `img_cmds.c` 里加一行（见第三节模板） | CLI |
| 11 | **帮助文本** | 命令 `help` + 每个参数 `help`；`--help=<命令>` 自动生成 | CLI |
| 12 | **里程碑日志** | `begin`（带参数）/ `done`（带耗时 + rc）；错误走 `E` 级（**任何级别都不漏**） | 库 |
| 13 | **用户输出** | 人话行（`▶`/`✔`/`✖`）+ 失败卡片（原因/建议/状态/事件尾部）+ `<镜像>.error.txt` | CLI |
| 14 | **文案表** | 中/英两列都进 CLI 的 `A_TEXT`/`E_TEXT` 表 | CLI |
| 15 | **测试** | 一条正用例 + **一条反用例**（坏参数 / 坏数据 / 取消），并加入 `tests/run_all.sh` | tests |
| 16 | **文档** | 若有新命令 → 记进本文第三节；新契约 → 记进 `DESIGN-OUTPUT.md` | docs |

### 为什么这 16 条（都是踩过的坑）
```
1/2  实测：off+len 回绕 → 越界写把镜像头部改成 UINT64_MAX，qemu-img 从此打不开
3    实测：新增错误码没有表项 → 卡片给不出建议，用户只能干瞪眼
4/13 实测：卡片里没有"位置/原因/建议"→ 用户只会说"卡住了"，我无法定位
5    实测：灌 WIM 若不拆段，日志上"卡在灌系统"与"灌完一半死了"分不出来
6    实测：取消不留痕 → 用户杀进程后留下半成品，比崩更糟
7    实测：FAT 写 415MB 时整文件进内存 → Android 顶到 OOM
8    实测：多线程日志会交错成半行/坏 JSON
9    实测：库内硬编码中文 → App 无法本地化，被要求拆分库/视图层
```

---

## 二、命令表条目模板（复制改字段即可）

### 库侧 handler（`src/img_cmds.c`）
```c
/* ======== 命令：<模块>.<动作> ======== */
static const img_arg A_XXX[] = {
    {"img",  IMG_A_STR,  true,  NULL,  "镜像路径（qcow2）", NULL},
    {"lba",  IMG_A_U64,  true,  NULL,  "分区起始 LBA",     chk_lba},   /* 可复用的值级校验 */
    {"src",  IMG_A_STR,  true,  NULL,  "本地源文件路径",   NULL},
    {"dst",  IMG_A_STR,  true,  NULL,  "分区内目标路径（/ 开头）", NULL},
    {"check_size", IMG_A_BOOL, false, "false", "写入前校验空间（默认关：App 显式请求才校验）", NULL},
};
static int cmd_xxx(img_ctx *ctx, const img_args *in, img_result *out){
    /* ① 取参数（校验已由表完成，这里只取值） */
    const char *dst = img_args_get(in,"dst");
    uint64_t    lba = img_args_u64(in,"lba",0);

    /* ② 里程碑：begin（带参数） */
    img_span sp;
    img_span_prep(&sp,"<module>","<action>","<人话备注（只写进日志，可中文）>");
    img_span_kv_str(&sp,"path",dst);
    img_span_kv_u64(&sp,"lba",lba);
    img_span_begin(&sp);

    /* ③ 干活（这里才真正调库；长循环里检查 img_cancelled()） */
    int rc = do_the_work(ctx, lba, dst);

    /* ④ 收尾：先 end 里程碑，再报错或出结果 */
    if(rc != IMG_OK){
        img_span_end(&sp, rc);
        img_result_fail(out, rc, "<ERROR-CODE>", dst, NULL, NULL);  /* 文案由 CLI/App 查表 */
        return rc;
    }
    img_result_u64(out,"written_bytes", got);        /* 单位进键名，值用原始字节 */
    img_span_end(&sp, IMG_OK);
    return IMG_OK;
}
```

### 表里加一行
```c
const img_cmd IMG_CMD_TABLE[] = {
    /* ... */
    {"<模块>.<动作>", "1.0", "一句话说明（App 与 --help 都显示它）", A_X, (int)(sizeof A_X/sizeof A_X[0]), cmd_xxx},
};
```

### CLI 文案表加一行（中/英各一列）
```c
static const TXT E_TEXT[] = {   /* 错误码 → 原因 / 建议 */
    {"<ERROR-CODE>","中文原因","English cause", "中文建议","English suggestion"},
};
```

---

## 三、日志规范（`DESIGN-OUTPUT.md §二` 为准）

```c
/* 里程碑（必写） */
img_span sp; img_span_prep(&sp,"fat32","put","写入 FAT32");
img_span_kv_str(&sp,"path",dst); img_span_kv_u64(&sp,"size_bytes",len);
img_span_begin(&sp);        /* → time=... level=INFO logger=fat32 event=begin action=put path=... size_bytes=... */
/* ... */
img_span_end(&sp, rc);      /* → ... event=done rc=0 action=put duration_ms=76 */

/* 细节（debug 档才落盘，热路径里可省） */
img_ev e;
if(img_ev_init(&e, IMG_LV_DEBUG, "fat32", "entry", IMG_OK, "写入目录项")){
    img_ev_u64(&e,"cluster",clus); img_ev_u64(&e,"size_bytes",size);
    img_ev_emit(&e);
}
/* 失败（任何级别都写） */
img_result_fail(out, rc, "FAT-ENOSPC", dst, NULL, NULL);
```
**禁止**：在循环里打 `INFO` 级事件（会刷爆日志）；用日志代替返回值（失败必须 return rc）。

---

## 四、用户输出规范

**人话行**（`--progress=bar` 或 tty）
```
▶ 写入  /EFI/Boot/bootx64.efi  3008968 字节     ← begin：动作 + 对象 + 数量
✔ 写入 完成 (12 ms)                             ← done：动作 + 结果 + 耗时
✖ 写入 失败（空间不足）                         ← done(rc≠0)
… 仍在进行 灌入 (iterations=40960)               ← 心跳（仅"正常推进"时刷）
```
规则：**完成的事留在屏幕上**；进行中的**只有一行原地刷新**；失败不出人话行，**统一交给失败卡片**。

**失败卡片**（必须含 8 项，用户可整段复制 + 落 `<镜像>.error.txt`）
```
工具/版本/平台 · 命令 · 镜像 · 失败位置 · 错误码(rc) · 原因 · 建议 · [当前状态] · 日志路径 · 事件尾部
```

---

## 五、测试规范

```bash
# 1) 正用例：功能成功、结果正确（用独立实现对拍：mtools / ntfsls / sgdisk / qemu-img / wimlib）
# 2) 反用例：必须能证明"不会崩 / 不死循环 / 不留半成品 / 不毁数据"
#    · 非法参数：NULL、空串、越界、负索引、超长路径、单位错（LBA vs 字节）
#    · 坏数据：环状 FAT、损坏 hive、非 WIM 文件、不存在的镜像
#    · 取消：中途取消 → 返回 IMG_ECANCELLED、无半成品、剩余空间回基线
# 3) 全部收口
bash tests/run_all.sh      # 7 套：cmd / corrupt-chain / fd / thread / cancel / bcd / recovery
```
**要求**：新功能至少补 1 条反用例；反用例必须走 `timeout`（死循环类）或检查"剩余空间/文件存在性"（半成品类）。

---

## 六、兼容与版本

- 命令表的 `since` 字段必填（如 `"1.0"`）；字段**只增不改语义**，App 按字段存在性判断能力
- 结果信封与日志字段名**只增不改**（键名见 `DESIGN-OUTPUT.md §二 对照表`）
- 破坏性改动必须同时：改 `DESIGN-OUTPUT.md` + 说明理由 + 在提交信息里标注 `BREAKING:`

---

## 七、提交信息（写清三件事）

```
<type>: <做了什么 —— 为什么 —— 怎么验证的>
例：fat32: ★ 簇链遍历加界（防坏镜像死循环）+ 修 rm 丢错误码
      - 打开环状 FAT 会永久死循环 → 加 FAT32_CHAIN_GUARD
      - free_chain 返回值被丢 → 断链半删除状态
      - 反向用例 tests/corrupt-chain-test.sh：4/4 通过（<2s 内报错，不再死循环）
```

---

## 八、开工前自检（打钩）

- [ ] 我有没有在库/CLI 里写死中文或英文？（只能有 CLI 的文案表）
- [ ] 所有公开函数失败都返回 `img_err_t` 且没崩？
- [ ] `off+len` 判定防溢出了吗？
- [ ] 长操作有 begin/done 里程碑、带参数和耗时？
- [ ] ≥1MiB 循环有取消检查点、取消不留半成品？
- [ ] 没有把整个文件读进内存？
- [ ] 命令表加条目 + 参数 help + 错误码表两列？
- [ ] 至少 1 条正用例 + 1 条反用例，并进 `run_all.sh`？
- [ ] 构建零警告？`tests/run_all.sh` 全绿？
- [ ] 提交信息写了"为什么"和"怎么验证的"？

---

## 九、结构类功能的额外要求（改盘上结构 / 文件系统元数据时）

只写"数据"的功能可以跳过；只要**碰结构**就逐条对照：

| 结构 | 铁律 | 已有实践 |
|---|---|---|
| GPT 分区表 | 主表 + **备份表必须同时写**；磁盘末尾保留 `128×128` 字节；CRC32 必须重算 | `write_gpt()` 已双写（反例：只写主表 → 换机后分区全丢）|
| FAT32 | **两份 FAT 必须同步**；簇链分配/释放成对；目录项 `.`/`..` 齐全；LFN 项的 type(12) 与簇号(26-27) 必须为 0 | `img_fat32_fs.c`（反例：0xFF 写进 LFN → Windows 丢弃整条长名 → `0xc000000f`）|
| NTFS | **不许碰 BPB**（参数是格式化时算好的）；引导码只写 `$Boot` 的前 8 个扇区；MFT 用 libntfs-3g，**禁止手写** | `img_fixboot` / `img_ntfs_*` |
| BCD / hive | 用 libhivex 读写，**禁止手拼 hive 字节**；改完必须重读校验 | `img_bcd.c` + `bcd_test` |
| qcow2 | 改头/扩盘后**必须能通过 `qemu-img check`**；稀疏文件的 `ftruncate` **不等于**占空间（要真占位用 `fallocate`） | `img_qcow2_write.c`（反例：`expand(UINT64_MAX)` 把头部写成 `UINT64_MAX` → 镜像报废）|

**验证门槛（结构类功能必须做到）**
1. 写前：算出并记录将被改动的偏移/长度（进日志 `begin` 参数）
2. 写后：**用独立实现复核**（`qemu-img check` / `mtools` / `ntfsls` / `sgdisk -v`），不得只信自己的读函数
3. 反用例：**故意损坏结构** → 必须快速报错，不许死循环、不许继续写
4. 幂等：同一操作跑两次结果一致（换盘重建、重复写 BCD 等）
