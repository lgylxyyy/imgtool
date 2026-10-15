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
| P0-1 | | |
| P0-2 | | |
| P1-3 | | |
| P1-4 | | |
