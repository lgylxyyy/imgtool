# NTFS 改名/移动：规范写了什么、开源怎么做、我的代码差在哪

日期：2026-10-23 ｜ 作者：Icdqcow2 侧取证 ｜ 结论一句话：
**"跨父目录搬移被 Windows 判负"是误判。真正的缺陷是我方"写完不自盘上重开就报成功"，把一个【幽灵目录】当成了成功。**

---

## 0. 本文的证据来源（都是原文，不是我推测）

| # | 来源 | 拿到什么 |
|---|---|---|
| A | **[MS-FSA] File System Algorithms**（用户提供的 2025-09-09 官方 docx，已全文抽取） | §2.1.5.15.12 `FileRenameInformation` 改名/移动的完整算法；§2.1.1.3/2.1.1.4 每 File/每 Link 的持久字段；§2.1.4.19 更新"重复信息"；§2.1.5.6.3.4 目录查询对 `.` / `..` 的规定；附录 `<65>` |
| B | **libntfs-3g 源码**（本地 `/workspace/third_party/ntfs-3g`，2026.9 系） | `src/ntfs-3g.c: ntfs_fuse_rename()`；`libntfs-3g/dir.c: ntfs_link_i()/ntfs_delete()/ntfs_create_inode()`；`include/ntfs-3g/layout.h` 各结构偏移；`libntfs-3g/mst.c` 多扇区保护 |
| C | **现场盘**：`.tmp/w10b.qcow2` → `qemu-img convert -O raw` | 这张就是用户 VM 启动过的那张盘（卷号 `2B00-726F` 与 TXT 逐字符相同）在"我搬完、Windows 未动手前"的状态 |
| D | **独立解析器** `tests/ntfs-inventory.py`（纯 Python，不链接 imgtool/libntfs-3g） | 三方对账：目录索引项 ↔ MFT 记录 ↔ `$MFT` 的 `$BITMAP` |

★ 按 AGENT-MEMORY 规则：**证明 Windows 行不行必须用真盘**；本文所有"盘上事实"都来自 C/D，不来自我自己造的验证盘。

---

## 1. 规范：改名/移动时"到底改了哪些东西"

### 1.1 [MS-FSA] §2.1.5.15.12 的算法骨架（原文顺序）

输入是 `FILE_RENAME_INFORMATION`（`ReplaceIfExists` / `RootDirectory` / `FileName`）。规范要求的动作，按原文顺序：

1. **参数与访问检查**：`Open.GrantedAccess` 必须含 `DELETE`；文件名为 0/奇数字节/超长/远程却带绝对路径 ⇒ `STATUS_INVALID_PARAMETER`。
2. **打开目标目录**：`DesiredAccess = FILE_ADD_FILE`（文件）或 `FILE_ADD_SUBDIRECTORY`（目录）；跨卷 ⇒ `STATUS_NOT_SAME_DEVICE`。
3. **目录改名前置检查**：若 `Open.File.FileType is DirectoryFile`，必须先跑 §2.1.4.2"determine whether Open.File contains open files"；**目录子树里有打开的文件 ⇒ `STATUS_ACCESS_DENIED`**（原文 `<186>`）。
4. **同目录 + 大小写完全相同 ⇒ 立刻 `STATUS_SUCCESS`**（原文："If NewLinkName is a case-sensitive exact match with Open.Link.Name, the operation MUST return STATUS_SUCCESS at this point"）。
5. **目标名已存在**：按 `ReplaceIfExists`、是否同一 File、是否只是改大小写，分成 `TargetExistsSameFile / ExactCaseMatch / OverwriteSourceLink / RemoveSourceLink` 几种组合（原文给了完整判定表）。
6. **先挂 USN**：`USN_REASON_RENAME_OLD_NAME`。
7. **移除源链接**（`RemoveSourceLink`）：把 `Open.Link` 从 `Open.File.LinkList` 和 `Open.Link.ParentFile.DirectoryList` 摘掉，并写入 **TunnelCache**（父目录+文件名+短名+创建时间+ObjectId 四元组）。
8. **添加目标链接**（`AddTargetLink`）：新建 `NewLink{File, ParentFile}` ⇒ **套用 TunnelCache（创建时间/ObjectId 会被"隧道"回来）** ⇒ 需要时**生成 8.3 短名**（必须在目标目录内唯一）⇒
   > `The object store MUST update the duplicated information as specified in section 2.1.4.19 with Link equal to NewLink.`
   ⇒ 把 NewLink 加进 `DestinationDirectory.DirectoryList`。
9. **时间戳**：`MoveToNewDir` ⇒ 目标目录的 `LastModificationTime/LastAccessTime/LastChangeTime` 三个 **MUST updated**；`RemoveSourceLink` ⇒ 源目录同样三个 MUST updated；并且 `The object store MUST update Open.File.LastChangeTime`；**文件（DataFile）改名还要置 `FILE_ATTRIBUTE_ARCHIVE`**。
10. 目录变更通知（`FILE_ACTION_REMOVED/RENAMED_OLD_NAME/RENAMED_NEW_NAME`）+ 父句柄 oplock break。

### 1.2 §2.1.4.19「更新重复信息」= 索引项里那份缓存必须重刷

```
CreationTime / LastAccessTime / LastModificationTime / LastChangeTime
AllocationSize / FileSize / FileAttributes / ExtendedAttributesLength / ReparseTag
```
= **全部从 File 对象抄一份到 Link（目录项）里**。这解释了盘上"同一个名字的时间戳出现在两处、必须一起改"。

### 1.3 §2.1.1.3 / §2.1.1.4：目录**只许有一个链接**

> `A DirectoryFile MUST have exactly one element in LinkList.`

⇒ 搬目录的正确终态 = **一条** 指向新父的 Link；中途出现两条是过渡态，绝不能留下两条。

### 1.4 ★ `.` 和 `..` 是**查询时合成**的，不是盘上必须存在的索引项

[MS-FSA] §2.1.5.6.3.4 原文：

> `If FirstQuery is TRUE or RestartScan is TRUE, the object store MUST set the "." and ".." file names as the first two records returned`（根目录/模式为 `.`/含通配符时除外）

附录 `<65>`：

> `The NTFS file system on versions earlier than Windows 11 … always set the FileID field to zero in the ".." entry.`

⇒ **`.`/`..` 由对象存储现场合成返回**，因此"搬目录必须同步子目录 `..` 索引项"这个前提是错的；libntfs-3g 的 `ntfs_readdir()` 也正是这么做的（`dir.c:836 static const ntfschar dotdot[3]`，第 1154/1171 行用 `filldir(dotdot, …)` **现场合成**，不是从索引里读）。

⇒ 搬目录真正只需两件事：**新父的索引项** + **被搬记录自己的 `$FILE_NAME` 父引用**。

### 1.5 on-disk 的最小集（NTFS 特有，[MS-SPEC]/libntfs-3g `layout.h`）

一个"名字"在盘上有**两份**，必须同时成立：

```
父目录 $I30 索引项 INDEX_ENTRY{ indexed_file=(ino,seq), key=FILE_NAME_ATTR{ parent_directory, 4×时间, alloc, size, attrs, ns, name } }
     ↕ 必须互证
被命名对象的 MFT 记录里的 $FILE_NAME 属性{ parent_directory=(父ino,父seq), … , name }
```
外加：`MFT_RECORD.link_count` 要等于 `$FILE_NAME` 属性条数；每条 MFT/INDX 记录受**多扇区保护**约束（`mst.c: is_valid_record` ⇒ `usa_cnt == 1 + size/512`，每扇区末 2 字节必须等于 `usa[0]`）；有 `$INDEX_ALLOCATION` 就必须有 `$BITMAP`。

**chkdsk Stage 2「Examining file name linkage」的判据就是这两份互证。** 任何一份缺位 ⇒ 该记录成孤儿 ⇒ `1 unindexed files recovered to lost and found` + `found.000\*.chk`。

---

## 2. 开源实现怎么做（本地 libntfs-3g 源码，逐行）

* **libntfs-3g 没有 `ntfs_rename()`。** 全仓唯一一处提及是 `dir.c:2196` 的 FIXME：
  > `NOTE: At present we allow creating hardlinks to directories, we use them in a temporary state during rename. But it's defenitely bad idea to have hard links to directories as a result of operation.`
  > `FIXME: Create internal __ntfs_link that allows hard links to a directories and external ntfs_link that do not. Write ntfs_rename that uses __ntfs_link.`

* 真正的实现在 FUSE 层 `src/ntfs-3g.c:2679 ntfs_fuse_rename()`，做法就是**两步**：
  ```
  ret = ntfs_fuse_link(old_path, new_path);   /* 新位置加链接：索引项 + $FILE_NAME 属性 */
  ret = ntfs_fuse_unlink(old_path);           /* 旧位置删链接：删索引项 + 删那条 $FILE_NAME */
  ```
  目标已存在时走 `ntfs_fuse_rename_existing_dest()`（先 `ntfs_check_empty_dir`，再 rename 到临时名再删）。

* `ntfs_link_i()`（`dir.c:2200`）加链接时写了什么：
  `fn->parent_directory = MK_LE_MREF(dir_ni->mft_no, dir_ni->mrec->sequence_number)`、时间戳**从 `ni` 现取**（等价于 §1.2 的"重复信息"）、目录则 `data_size/allocated_size = 0` 并带 `FILE_ATTR_I30_INDEX_PRESENT`；先 `ntfs_index_add_filename(dir_ni, fn, MK_MREF(ni->mft_no, ni->mrec->sequence_number))` 写父索引，再 `ntfs_attr_add(ni, AT_FILE_NAME, …)` 写记录；然后 `link_count++`。**索引失败会回滚记录**（`ntfs_index_remove`）。

* `ntfs_delete()`（`dir.c:1904`）删链接时：按"父 MFT 号必须相等"筛 `$FILE_NAME`；`ntfs_check_unlinkable_dir()`；**保留最后一条名字不删**（"Keep the last name in place, this is useful for undeletion (Windows also does so)"）——这跟 Windows 的行为一致：改名不是"改字符串"，而是**删一条 + 加一条**。

⇒ **我用 `ntfs_link` + `ntfs_delete` 实现搬移，与上游 FUSE 层的做法同构，规范上也允许。** 上轮我据此判"能力本身有问题"，是错的。

---

## 3. 现场取证：`w10b.qcow2` 上真正坏的是什么

用独立解析器（D）读原始字节，卷号 `2B00-726F` 与用户 TXT 完全一致：

```
根目录索引 28 项 → 27 项三方互证 ✅  唯一 ❌：
   'MovedByTool' → INDEX_ENTRY.indexed_file = (ino 99790, seq 1)
   但 ino 99790 的记录：flags=0x1（不是目录）、link=1、名字 = 'AppxBlockMap.xml'、父 = 99780
   99780 的索引里也有 ('AppxBlockMap.xml' → 99790)   ⇒ 同一个记录被两处索引引用
$MFT $BITMAP: bit(99790)=1（该记录在用，属于 AppxBlockMap.xml）
全 MFT 字节扫描 'MovedByTool'（UTF-16LE）：命中 0 处 ⇒ 这个目录的记录【从来没落盘】
被我搬进去的 LogFiles：$FILE_NAME parent = (99790, seq1)  ← 指向那个根本不存在目录号
```

**因果链**：

1. `ntfs.move` 目标父目录不存在 ⇒ 调 `img_ntfs_mkdir_p()` 建 `/MovedByTool`；
2. 建目录**只把父索引项写上了盘，被建目录自己的 MFT 记录没写**（记录槽位仍是别人的旧内容 99790）；
3. 工具紧接着用**内存里**的 inode 继续 `ntfs_link(LogFiles → 99790)`，并回读"新位置在" ⇒ **报 `ok:true`**；
4. Windows 挂载后：按根索引找到 `MovedByTool` ⇒ 打开 99790 ⇒ 那是个**文件** ⇒ `cd C:\MovedByTool\LogFiles` = "找不到路径"；
5. `chkdsk /scan` 做 §1.5 的双向互证：`LogFiles` 的 `$FILE_NAME` 说父亲是 99790，而 99790 没有 `$I30` ⇒ 没有任何索引项指向它 ⇒ **`1 unindexed files recovered to lost and found`**。

⇒ **用户 TXT 里那两条现象（找不到路径 + found.000）被这一条完整解释，且与"跨父搬移"这个能力无关。**

⇒ 同父改名（`appcompat → AppCompat-Renamed`）在真盘上完全正常（记录 1748 名字正确、父 1747 正确、无报怨），也印证了这条路径没问题。

⇒ 受控复现（干净 2 GiB NTFS + `ntfs.rename --names=BigDir --dst=MovedByTool`，跨父搬目录）：独立三方对账 **0 条违背**，`ntfs.parent` 一致，内容逐字节还在。

---

## 4. 按规范逐条审我的代码（imgtool/src/img_ntfs_fs.c）

| 规范要求 | 我的实现 | 判定 |
|---|---|---|
| 目标目录访问权限/存在性先检查 | `lookup(ddir)` 不存在则 `mkdir_p` | ⚠ 存在，但**不校验建出来的目录是否真在盘上**（← 事故点） |
| 目录改名前"子树有打开文件即拒绝" | 无（离线单写者，没有打开句柄概念） | ✅ 场景不适用，可接受 |
| 同目录同名大小写完全一致 ⇒ 直接成功 | `dref==sref` 提前返回 + 大小写专测 | ✅ |
| 目标存在 ⇒ 按 ReplaceIfExists 分支 | `overwrite` 参数 + 目录目标一律 `IMG_ESTATE` 拒绝 | ✅ 比规范更保守 |
| 目录终态只许一条链接 | `ntfs_link` 后 `ntfs_delete`，`link_count` 回到 1 | ✅ |
| §2.1.4.19 重复信息刷新 | 由 `ntfs_link_i()` 从 `ni` 现取四个时间/大小/属性 | ✅（依赖上游，正确） |
| 源/目标目录 + File 的 LastChange/Modify 时间更新 | 上游 `ntfs_link/ntfs_delete` 会标脏并落盘，但我**没有显式更新三个父目录时间戳** | ⚠ **与规范有差异，不影响可见性**；记为待办 |
| DataFile 改名置 ARCHIVE | 上游按 `ni->flags` 抄，不额外置位 | ⚠ 同上 |
| USN 记录 | 不写（盘上 `$UsnJrnl` 保持原样） | ⚠ 有意为之；chkdsk 不因此报错 |
| 多扇区保护 / `$INDEX_ALLOCATION` ⇒ 必须有 `$BITMAP` | 上游负责；**我方原本无任何事后校验** | ❌ **这是缺的那一环** |

### 结论：缺陷不在"搬目录的语义"，在"写完不自盘上重开就报成功"

具体两条：

* **P0**：`img_ntfs_mkdir_p()` 用 `ntfs_create()` 返回的**内存 inode** 当作已存在，从不**按名字重新从盘解析并核对 MFT 记录**。一旦记录写入丢失（本例：记录槽位还是别人的旧内容），后果是造出**幽灵目录**，后续所有写入都挂在一个不存在的父上 —— 而所有返回值都是 `ok:true`。
* **P1**：写后自检 `img_ntfs_dir_parent_check()` 只看"`..` 与父引用是否自洽"，恰好**看不见"父本身是假的"**；所以它当时报了 `consistent=true`。

---

## 5. 修法（本轮**已实现并合入**，`imgtool` 603bba4）

1. **`img_ntfs_mkdir_p()` 建完/命中已存在，一律"从盘上重开验明正身"**
   （`verify_dir_on_disk()`）：① 父索引里按名字查得到引用；② 该 ino 的记录能打开；
   ③ 记录带 `MFT_RECORD_IS_DIRECTORY`；④ 记录里存在"同名 **且** 父引用指回这一级父"的
   `$FILE_NAME`。任一不过 ⇒ `IMG_ECORRUPT` → 命令层 **`NTFS-DIR-PHANTOM`**。
   中间某一级是文件也当场拒（以前只会报一个含义不明的 EIO）。
2. **搬移之后 `verify_move_result()`**：新位置查得到、记录就是原来那条 inode、
   目录仍是目录、**子记录的 `$FILE_NAME` 指回新父**（= chkdsk Stage 2 的判据）。
3. 两个实现坑（都写在注释里，别再犯）：
   * 自证必须放在 `ntfs_inode_close(父)` **之后** —— 循环里父索引还只在内存；
   * **"旧位置查不到"只能 WARN**：NTFS 大小写不敏感 + libntfs-3g 的 NIDATA 会缓存
     整份索引快照，拿它硬判会把好盘报成坏盘。
4. **`tools/ntfs-inventory.py`**：不链接 imgtool/libntfs-3g 的**独立三方对账器**，
   `索引项 ↔ MFT 记录 ↔ $MFT $BITMAP`；"工具说成功"与"Windows 说坏了"之间它是仲裁者。
   `tests/ntfs-inventory-test.py`（22 条）把它钉进回归：正常流程 0 违背 → 手工注入
   "目录项指向文件记录"精确复刻事故 → 裁判报警 **且** imgtool 拒写。
5. 跨父目录搬移的锁**暂时保留**，但注释/手册已改判为"证据不足"，解禁条件见 §7。

---

## 6. 作废清单（本文献正式推翻的旧结论）

| 旧结论 | 出处 | 现状 |
|---|---|---|
| 「NTFS 目录跨父搬移被真 Windows 判负，已收回」 | 2026-10-23 上轮 HANDOFF/手册 §21.11.1/测试计划 T1 | **作废**：判负证据实际是"幽灵目录"，不是搬移语义；受控跨父搬移三方对账 0 违背 |
| 「Windows 卷的 `$I30` 里没有 `.`/`..` 索引项，所以搬目录不用管 `..`」（上轮已作废过一次） | 上轮作废理由=循环论证 | **重新成立，但换了合法依据**：[MS-FSA] §2.1.5.6.3.4 + `<65>` 明文规定 `.`/`..` 是**查询时合成**；libntfs-3g `dir.c:1154/1171` 同样合成。不再依赖"我自己造的盘" |
| 「`dotdot_present=false` 所以 Windows 不靠 `..` 索引项」 | 上轮 | 依据换成规范条文，结论保留 |

---

## 7. ★ 灌入/写入工具在"文件本体以外"写了什么（真盘实测对照）

同一张真 Windows 盘（`w10b`，卷号 2B00-726F）上取三类记录，逐属性对比：

| 谁建的 | 样本 | `$FILE_NAME` 条数 / 命名空间 | 安全描述符 | `$SI` vs `$FN` 时间 | 本体以外的其它属性 |
|---|---|---|---|---|---|
| **Windows 运行时** | `/pagefile.sys`、`/$Recycle.Bin`、`/Windows/Temp`、`C:\System Volume Information` | 各 1 条 | **无内联 `$SD`** ⇒ 走 `$SI` 里的 SecurityID 引用 `$Secure:$SDS` | **多处不一致**（pagefile/Temp/SVI 的 `$SI` 修改时间 ≠ `$FN`） | 目录：`$INDEX_ROOT`（Temp 还带 `$INDEX_ALLOCATION`+`$BITMAP`）；`$Reparse`/`$ObjId` 另计 |
| **wimlib（我们产线第一步）** | `cmd.exe`、`explorer.exe`、`config/SAM` | **2 条**（长名 + 8.3 短名，`link_count=2`），ns=0 | 无内联 `$SD`（同上引用 `$SDS`） | 全部**一致** | `SAM` 带 `$EA_INFORMATION(0xd0)`+`$EA(0xe0)`；`explorer.exe` 带第二条 `$DATA` + **`$LOGGED_UTILITY_STREAM(0xc0)`**（CompactOS/WIM 指针流） |
| **imgtool 建的目录** | `/ProbeDir`、`/Rec1` | 1 条，ns=0，**没有 8.3 短名** | ★ **内联 `$SECURITY_DESCRIPTOR(0x50)`，内容是 everyone 全通 ACL** | 一致 | 只有 `$INDEX_ROOT`，没有 `.`/`..` 项（符合规范，见 §1.4） |

**读出来的三条结论**：

1. **时间戳不用逐条同步是合规的**：Windows 自己就让 `$SI` 与 `$FN` 不一致
   （libntfs-3g `layout.h` 的 `$FILE_NAME` 注释也写着"除父引用外，这些字段只在改名时更新，
   在此之前允许与现实不同步"）⇒ 我们没必要为"两份时间"焦虑。
2. **8.3 短名是可选项**：`fsutil` 在系统卷上默认开启短名，但 [MS-FSA] 只在
   "短名已启用且目标没有同短名冲突"时要求生成；我们建的对象没短名，Windows 会自己按需生成
   ⇒ 不是 found.000 的原因。
3. ★ **真正的差异在 ACL：我们建的目录带的是"内联 `$SD` + everyone 全通"**，
   而 Windows/wimlib 建的都是"引用 `$Secure:$SDS`"。
   ⇒ `win.recover` 造出来的 `Windows.old` 这类目录，**权限与系统原生不同**（人人可写）。
   `chkdsk` Stage 3 不报怨（内联 `$SD` 合法），但这是**产品级问题**：已记入未解决清单（P1）。

⇒ 也就是说：用户让我"去吃一下灌入的工具"，吃出来的东西**不是** found.000 的原因
   （原因是 §3 的幽灵目录），但吃出了**另一个真问题**（ACL 模型不一致）。

---

### 7.1 ✅ 第 3 条（ACL）本轮已修（`imgtool` 7a9ddd4）
新建对象不再传 `securid=0`（那会触发 ntfs-3g 的 `ntfs_sd_add_everyone()`，给记录塞一条
**内联 `$SECURITY_DESCRIPTOR` = everyone 全通**），改为把**父目录 `$SI` 偏移 52 的 SecurityID**
原样传给 `ntfs_create()` —— 等价于 Windows 的"继承父目录 ACL"，且不再新增内联 `$SD`。

真 Windows 盘副本（`w10b` → `acltest`）实测前后对照：

| 对象 | SecurityID | 内联 `$SD` |
|---|---|---|
| `/Windows`（Windows 自建） | 260 | 无 |
| **改之前**建的 `/ProbeDir` | 无（`$SI` 只有 48 B） | ★有（everyone 全通） |
| **改之后**建的 `/AclNew`、`/AclNew/deep`、里面的文件 | **258 = 根目录的** | **无** |

★断言的教训（我第一版就写错了）：规则**不是"永远不许有内联 `$SD`"**，而是**跟着卷自己的惯例走** ——
`mkntfs` 快速格式化的卷 `$SI` 是 48 字节（NTFS 1.2），**连根目录都没有 SecurityID 可继承**
（根自己都带内联 `$SD`），此时退回原行为才是对的。所以回归测的是**条件不变量**：
`父有 sid ⇒ 子必须同 sid 且无内联 $SD；父无 sid ⇒ 保持原样`。

真机（arm64）复验：多级建目录 / 改名 / 批量搬（`win.recover` 那条路）全部成功；新建对象
`sid=None`（该卷是 1.2 格式，符合预期）；且**被 `NTFS-DIR-PHANTOM` 拒绝的那次写之后，
整盘三方对账仍 0 违背** —— 拒绝是干净的，没留半截数据。

## 8. 跨父目录搬移的解禁条件（下一步）

1. 选一张**独立裁判 0 违背**的 Windows 盘（`tools/ntfs-inventory.py` 先跑一遍）；
2. 用 imgtool 做一次跨父搬目录，**再跑一次裁判**（必须仍 0 违背）；
3. 复制到 Vectras 目录，真 Windows 开机：`dir` 能进能列、`chkdsk C: /scan` 零报怨；
4. 三条全过 ⇒ 去掉 `img_ntfs_move_path` 里的 `isdir && !same_dir` 拦截，
   并同轮复核 `img_ntfs_move_into`（`win.recover`/`ntfs.rename` 在用，它搬的正是
   `Windows`/`Program Files`/`Users` 进 `Windows.old` —— 现在这条路径**也有写后自证**了）。


### 8.1 T1b 盘已就绪（2026-10-23，等真 Windows 一票）

已经做完的（全部有数字）：

| 步骤 | 结果 |
|---|---|
| 母盘 | `images/Windows 10 Xlite.qcow2`（128 GiB 虚拟 / 8.9 GB 实占，MBR，卷号 **2236-A3D9**） |
| 操作前独立体检 | **0 条违背**（30,863 个目录、106,990 条在用记录；5,032 条如实记为"未判定"） |
| 跨父搬目录 ×2（目标父由工具新建） | `/Windows/System32/LogFiles` → `/MovedByTool/LogFiles`（ino **4368** 不变，8 个子项）；`/Windows/Temp` → `/MovedByTool/Temp`（ino **5767** 不变，3 个子项） |
| 操作后差值对账 | **新增 0 条**（`--base` 模式，退出码 0） |
| 内容校验 | `vdagent.log`(9,492 B) / `vdservice.log`(1,940 B) 与母盘原文件 **逐字节相同** |
| 其它 | `boot.check` 必需 **9/9**；`qc.verify` clean；`ntfs.parent` 两处均 `backlink_ok=true`、`parent_ino==parent_by_path` |

盘已放到手机 Vectras 的 BIOS 虚拟机目录（`p37757s36220748/Windows.qcow2`），
配置原本就指向这个文件名 ⇒ **用户不需要改任何配置就能开机**。

**Windows 侧要看的四件事**（判据写死，避免再"凭感觉判负"）：
1. `vol C:` 卷号必须是 **2236-A3D9**（别跑在别的盘上）；
2. `dir C:\MovedByTool` 能列出 `LogFiles` 与 `Temp` 两个目录；
3. `cd C:\MovedByTool\LogFiles` 进得去、`dir /ad` 里 8 个子项一个不少；
4. `chkdsk C: /scan` **不得出现** `unindexed files` / `found.000` / `Index entry`。
   全过 ⇒ 去掉默认锁（把 `allow_dir_move` 默认值翻成 true）并复核 `ntfs.rename`（`win.recover` 那条）。
