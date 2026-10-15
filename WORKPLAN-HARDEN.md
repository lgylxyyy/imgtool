# 健壮性加固工作计划（P0→P2）

> 起因：用户问"有没有冗余设计/防卡死/进度可见"。查证后按**最坏影响**排优先级。
> 每阶段都要**自验**（正向对拍 + 反向造坏数据），独立实现交叉验证是硬要求。

---

## 现状勘察结论（先澄清两处）
```
qcow2  用 L1→L2 索引表（不是链），L2 只缓存一张表 → 无绕圈风险、无内存膨胀
NTFS   走 libntfs-3g，其内有自己的校验；我们自己的循环（1MiB 分块写）有界
FAT    真正的问题所在：多处 `while(c>=2 && c<FAT32_EOC)` 沿链走，
       环状 FAT 会永久死循环；read_file 有 size 界，其余没有
```
| 现有 | 位置 |
|---|---|
✅ EINTR 重试 | `img_io.c:90,106`
✅ `chain_nth` 步数上限 / `alloc_cluster` 扫描上限 | `img_fat32_fs.c:103,75`

## 优先级与验收标准

### P0-1 · 簇链遍历加界（防打开坏镜像死循环）
**最坏影响**：用户打开别人给的损坏/恶意 qcow2 → 永久死循环、只能杀进程。
```
改动  img_fat32_fs.c: dir_find / list_dir / dir_make_room / free_chain / read_file
      → 步数上限 = cluster_count+1（天然上界），超界返回 IMG_ECORRUPT
验收① 正向：完整 ESP 142 文件重灌 + mtools 抽回 md5 全一致（回归）
验收② 反向：脚本故意把目录簇 FAT 项改成自环 → `timeout 10` 跑 ls/读文件，
      必须 <1s 返回错误码（死循环时代会 timeout）
```

### P0-2 · 取消真的能停（避免"以为卡死→杀进程→坏镜像"）
**最坏影响**：点取消无反应；强杀时正在写盘 → 半成品/损坏镜像。
```
改动  在长循环插检查点：FAT/NTFS 每 1MiB、WIM 提取回调、格式化阶段、qcow2 大块写
      取消语义：返回 IMG_ECANCELLED，且【不写目录项】、释放已分配簇链
验收① 扩展 progress_test：回调里调 cancel → 断言后续循环返回 IMG_ECANCELLED
验收② FAT/NTFS 两条写入路径各测一次；取消后镜像仍能被 mtools/ntfsls 读（无残留）
```

### P1-3 · 命令行能看见进度与"正在干什么"
**最坏影响**：静默数分钟 → 误判卡死 → 杀进程 → 坏镜像（本次 0xc000000f 排查中就反复出现"静默像卡死"）。
```
改动  新增 img_prog_render_bar()（把 img_progress_t 渲染成一行人读文本）
      CLI 加 --progress=bar|json|none（默认 bar 若是 tty，否则 json），
      imgmkfs/imgntfsfs/imgfatfs/imgfixboot/imgwimls 接上回调
验收① tty 与非 tty 两种输出各抓一次；bar 模式不影响既有 JSON 消费者
验收② 跑 415MB winre.wim 写入，肉眼可见百分比+当前文件+ETA
```

### P1-4 · FAT 写文件改分块（去掉 OOM 风险 + 进度变流式）
**最坏影响**：往 ESP 拷 400MB+ 文件内存爆 → App 被系统杀（闪退）。
```
改动  img_fat32_put_file：不再 malloc 整个文件；预分配簇链 → 1MiB 分块读→按簇映射写
      → 末尾写目录项；进度按字节递增（与 NTFS 侧对齐）
验收① /usr/bin/time -v 看 MaxRSS：715MB 源文件应从 ~700MB 降到 <16MiB
验收② mtools 抽回逐字节 md5 一致；ESP 全树重灌回归
```

### P2-5 · 记录待办（本轮不做，避免范围失控）
```
⬜ I/O 超时（SAF 读远端/慢介质）—— 需要接口设计（异步/超时参数）
⬜ 库缺失时的降级路径（例：无 NTFS 库时禁用该功能并明确报错）
```

---
## 执行记录（边做边填）

| 阶段 | 状态 | 自验结果 |
|---|---|---|
| P0-1 簇链加界 | ✅ 已提交 `5d91c9e` | 反向用例 `tests/corrupt-chain-test.sh`：环状 FAT 从"永久死循环"变为 <2s 返回 `IMG_ECORRUPT`；顺带修出 2 个真缺陷（rm 丢 free_chain 错误码、空目录判断传 NULL 回调） |
| P0-附加 非法参数 | ✅ 已提交 `a71e0c3` `7c18145` | 见下表：130 条非法输入，修前 4 崩（含 1 处**毁镜像**），修后 **0 崩 0 毁盘** |
| P0-2 取消接线 | ⬜ 未做 | — |
| P1-3 CLI 进度可见 | 🟡 部分：事件流 + 人话行 + 失败卡片 + `.error.txt` 已通（`6261aef` `a2a35cc` `5914928` 及本次 0c 提交） | 成功路径 `▶/✔` 人话行、失败卡片（原因/位置/建议/事件尾部）实测见下 |
| P1-4 FAT 写分块 | ⬜ 未做 | — |
| 0e 尺寸估算 + check_size | ✅ 估算 + 开关都通 | `wim.size` 命令：真实 ESD → `data_bytes=6861906464`（与上一轮实测 6.86GB 一致）、`file_count=32707`（一致）、`est_ntfs_bytes=7483201632`（含元数据+8% 余量）；`fat.put --check_size=1` 提前拦下并给"剩余 196MB / 需要 286MB"；非法索引 → `WIM-IDX-INVALID` |
| C 命令表骨架 | ✅ `a2a35cc` | `--list`（App 生成界面用）/`--help` 自动生成/统一信封/22 条验收全绿 |
| B 并发 + fd 契约 | ✅ `5914928` | `fd_test` 15 条 · `thread_test` 4 组全绿；契约见 DESIGN-OUTPUT.md 第七节 |

### 0c 实测（人话行 + 失败卡片）

成功（`--progress=bar`，stderr 给人看）：
```
▶ 写入 FAT32  /EFI/s2.bin  5 字节
✔ put 完成（0 ms）
```
失败（同一命令、空间不够）：
```
════════ Icdqcow2 错误报告（整段复制即可）════════
工具     imgtool 2026.10.15 · Linux · 64-bit
命令     imgcmd fat.put
镜像     /tmp/fc/t.qcow2
失败位置 /EFI/big.bin
错误码   FAT-ENOSPC
原因     目标分区剩余 196 MB，要写入 286 MB
建议     换更小的文件，或先扩容该分区（imgresize）
日志     /tmp/fc/t.qcow2.imgtool.log
──────── 事件尾部（内存最近 N 条，机器格式）────────
{"t":5,"l":"E","m":"cmd","ev":"fail","rc":-10,...}
════════ 错误报告结束 ════════
```
同一份内容还会落盘成 `<镜像>.error.txt`（用户直接发这个文件即可）。

### 非法参数矩阵结果（回答"App 传参会不会炸"）

```
第一轮 NULL/空串/垃圾：90 条   修前 2 崩  →  修后 0 崩
第二轮 极端值（真实镜像）：40 条  修前 2 崩  →  修后 0 崩 · 0 毁数据
```
修掉的 4 处（都是真 bug，已提交）：
```
1. img_bcd_open_file(NULL)            NULL 直传 libhivex → 段错误
2. img_ev_emit(未 init 的结构)         段错误 → 加 magic 哨兵拒绝
3. img_qcow2_read_full(len 传错)       越界写调用方缓冲区 → 崩（改成先判范围、一字不写）
4. img_qcow2_expand(UINT64_MAX)       ★ 把镜像头部 size 改成 UINT64_MAX，
   img_qcow2_write(offset+len 回绕)      qemu-img 从此打不开（毁用户数据，比崩更糟）
```
复现命令：
```bash
# UBSan 版库（ASAN 在 proot 沙箱起不来：影子内存 mmap 受限）
cmake -B build-ub -DCMAKE_C_FLAGS="-fsanitize=undefined -fno-sanitize-recover=all -g" \
      -DIMG_WITH_NTFS=ON -DIMG_BUILD_TOOLS=OFF -DIMG_BUILD_TESTS=OFF && cmake --build build-ub -j4
gcc -fsanitize=undefined -g -Iinclude -DIMG_HAVE_ZLIB=1 -DIMG_WITH_NTFS=1 -o /tmp/argfuzz \
    tests/argfuzz.c src/img_bcd.c build-ub/libimgtool.a -lntfs-3g -lwim -lz -lhivex
/tmp/argfuzz        # → 90 条 / 崩溃 0

# 第二个矩阵需要一张模板盘 + 工作目录（每用例复制一张独立盘再跑）
T=build/tools; W=/tmp/afz; rm -rf $W; mkdir -p $W
$T/imglayout mk $W/tpl.qcow2 $((20*1024*1024*1024)) gpt --esp 200 --msr 16
LBA=$($T/imgpart show $W/tpl.qcow2 | python3 -c "import sys,json;d=json.load(sys.stdin);print([p['first'] for p in d['parts'] if p['typeGuid'].startswith('c12a7328')][0])")
$T/imgmkfs $W/tpl.qcow2 --mkfs $LBA TEST
/tmp/argfuzz_img $W/tpl.qcow2 $LBA $W     # → 40 条 / 崩溃 0 / 用例盘全部通过 qemu-img check
```

### 尚未覆盖（诚实清单，别当成已经安全）
```
⬜ 并发：库用全局状态（img_event 的 G、ntfs ctx）→ 同一时刻只能一个操作；
    App 多线程同时调会互相踩。需要"句柄化 + 锁"或明确契约（未做）
⬜ fd 生命周期：App 提前关 fd / 传错 fd（传了别的文件）→ 未测
⬜ 32 位 Android（arm32，size_t 32 位）→ 未测（当前只在 64 位下验过）
⬜ 超时/卡死：底层读卡住无法中断（P0-2 取消接线后可缓解）
⬜ 内存峰值：img_fat32_put_file 整文件进内存（P1-4）
```
