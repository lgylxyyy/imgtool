#!/bin/bash
# run_all.sh — 一键跑全部健壮性/回归测试（改完代码先跑它）
#   用法: tests/run_all.sh
# 覆盖：
#   cmd_test.sh          命令表 27 条（成功/失败/参数校验/check_size/宿主空间/尺寸估算/日志）
#   limits-test.py       尺寸口径（PART-SIZE-BAD）/ extra-mb / part.add·remove·resize
#   compact-label-test.py 紧凑另存（img.compact）/ 只改卷标（ntfs.label·fat.label）
#   format_twice_test   同进程连续两次 fs.format（mkntfs 静态状态，崩了就是 P0）
#   parttable-test.py   part.export / part.apply / 空 MBR 表
#   corrupt-chain-test.sh 反向：环状 FAT 必须快速报错，不许死循环
#   fd_test              fd 生命周期（坏 fd/所有权/提前关闭）
#   thread_test          并发（日志线程安全/两镜像并行/同 ctx 串行）
#   cancel_test          取消（不留半成品/不漏簇/不污染下一次/覆盖写时旧文件完好）
#   race_test            全局锁并发（首次初始化竞态/环形缓冲读写竞争）
#   bcd_test             BCD 读写（需 libhivex）
#   ebr_test             MBR 扩展/逻辑分区：EBR 链写回读、链尾 entry 全零
#   mbr_bcd_test         BIOS BCD 全对象批量改写（签名+偏移，5 处设备）
#   reg-test.sh          注册表通用编辑（读/写/列/删/事务）
#   winver-registry-test.sh  win.version 的四个"Windows 自己写"的字段（REQ-72）
#   diskfmt-test.sh      raw / VHD / VMDK 读写与 qemu 互验
#   vhd-test.py          VHD 建盘、多块写入、重开与 qemu 互验
#   vmdk-test.py         VMDK 建盘、GD 落盘、跨进程重开与 qemu 互验
#   compact-fmt-test.py  img.compact 对 raw/qcow2/vhd/vmdk 四种源都真的搬数据（D2）
#   move-rename-test.sh  move 的 moved_by=rename|copy（raw 会不会被实体化）
#   wim-capture-test.sh  wim.capture 端到端（含临时目录清理）
#   style-check.py       用户可见文本符合 STYLE-GUIDE
#   genbcd-test.sh       imggenbcd UEFI BCD 定制（GUID 映射/4 参/反例）
#   recovery_bcd_test    恢复条目（需 libhivex + 官方 ISO BCD 参考）
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
T="$ROOT/build/tools"
# 素材与依赖路径统一由 scripts/env.sh 解析
# shellcheck disable=SC1091
. "$ROOT/scripts/env.sh"
pass=0; fail=0; skipped=0
run(){ # $1=名字 $2...=命令
  local name="$1"; shift
  printf "\n════ %s ════\n" "$name"
  if "$@" > /tmp/runall.out 2>&1; then
    echo "  ✅ $name"; pass=$((pass+1))
    tail -3 /tmp/runall.out | sed 's/^/     /'
  elif [ $? -eq 77 ]; then
    # 77 = 缺素材主动跳过（不是失败）；详情见 /tmp/runall.out
    echo "  ⏭ $name（缺素材跳过）"; skipped=$((skipped+1))
    grep -m2 '⏭\|素材' /tmp/runall.out | sed 's/^/     /' || true
  else
    echo "  ❌ $name（失败，末尾输出↓）"; fail=$((fail+1))
    tail -12 /tmp/runall.out | sed 's/^/     /'
  fi
}

# 公共：一张小盘（供 fd/thread/cancel/corrupt 用）
W=$(mktemp -d /tmp/runallXXXXXX)
$T/imglayout mk "$W/a.qcow2" $((20*1024*1024*1024)) gpt --esp 200 --msr 16 >/dev/null 2>&1
$T/imgmkfs "$W/a.qcow2" --mkfs 2048 T >/dev/null 2>&1
$T/imglayout mk "$W/b.qcow2" $((20*1024*1024*1024)) gpt --esp 200 --msr 16 >/dev/null 2>&1
$T/imgmkfs "$W/b.qcow2" --mkfs 2048 T >/dev/null 2>&1
head -c 16777216 /dev/urandom > "$W/big.bin"      # 取消测试要够大的源（16MB）

run "cmd_test（命令表/校验/日志/建盘流水线 67 条）"  bash "$ROOT/tests/cmd_test.sh"
run "limits-test（尺寸口径/extra-mb/part.add·remove·resize/win-mb/extended/mbr-fill/auto/lba/w128 126 条）" python3 "$ROOT/tests/limits-test.py"
run "compact-label-test（紧凑另存 img.compact + 只改卷标 48 条）" python3 "$ROOT/tests/compact-label-test.py"
run "trim-test（REQ-66：空闲位图原地释放簇 + 门禁 + 打洞；NTFS/FAT32）" python3 "$ROOT/tests/trim-test.py"
run "format_twice_test（同进程连续两次格式化 NTFS，不许崩）" "$ROOT/build/tests/format_twice_test"
[ -x "$ROOT/build/tests/wimcodes_test" ] && run "wimcodes_test（wimlib 错误码 → 枚举名/中文；REQ-48 §1）" "$ROOT/build/tests/wimcodes_test"
[ -x "$ROOT/build/tests/progress_test" ] && run "progress_test（进度/ETA 口径：换阶段清零、字节优先；REQ-68）" "$ROOT/build/tests/progress_test"
run "parttable-test（分区表导出/应用 + 空 MBR 30 条）" python3 "$ROOT/tests/parttable-test.py"
run "iso-wim-test（ISO 直灌 + 多 install.wim + 恢复分区 + arch + wim.ls 40 条）" python3 "$ROOT/tests/iso-wim-test.py"
run "soload-test（共享库必须 dlopen 得动；REQ-18 门禁）" python3 "$ROOT/tests/soload-test.py"
run "boot-populate-test（一条命令铺完引导 29 条；REQ-20）" python3 "$ROOT/tests/boot-populate-test.py"
run "stack-test（1 MB 栈线程里跑吃栈命令；REQ-21）" bash "$ROOT/tests/stack-test.sh"
run "winre-test（WinRE 检测可信 + 裸 winre.wim；REQ-24/25）" bash "$ROOT/tests/winre-test.sh"
run "fs-page-test（sort/mkdir/stat/批量时间 + copy 跳过回收站；REQ-30/32）" bash "$ROOT/tests/fs-page-test.sh"
run "archive-test（压缩包只读：格式/列目录/抽文件/抽目录/加密/坏文件；REQ-31）" bash "$ROOT/tests/archive-test.sh"
run "attr-version-test（恢复分区属性 + wim.file-version；REQ-37）" bash "$ROOT/tests/attr-version-test.sh"
run "archive-write-test（archive.create/du/写进包/多源；REQ-39/40/41/42）" bash "$ROOT/tests/archive-write-test.sh"
run "iso-hardening-test（UDF 列目录 SIGSEGV 回归 + 截断模糊；REQ-45）" bash "$ROOT/tests/iso-hardening-test.sh"
run "fat-alias-test（FAT 8.3 短名 ↔ 长名：exists/stat/rm/覆盖；REQ-47）" python3 "$ROOT/tests/fat-alias-test.py"
run "rar-test（RAR solid 列目录/抽取；REQ-50；没夹具就跳过）" bash "$ROOT/tests/rar-test.sh"
run "7z-solid-test（7z 固实标志；REQ-51 A；没 7z 命令就跳过）" bash "$ROOT/tests/7z-solid-test.sh"
run "bootrepair-arg-test（--vbr-tpl=builtin/default 不再当参数错误；REQ-48 §3）" bash "$ROOT/tests/bootrepair-arg-test.sh"
# 场景：REQ-31 的 index_token 生命周期（要一个 zip；没有 zip 工具就跳过）
ARC_ZIP=""
if command -v zip >/dev/null 2>&1; then
  mkdir -p "$W/arcsrc/sub" && echo a > "$W/arcsrc/a.txt" && echo b > "$W/arcsrc/sub/b.txt"
  ( cd "$W/arcsrc" && zip -qr "$W/t.zip" . ) && ARC_ZIP="$W/t.zip"
fi
if [ -n "$ARC_ZIP" ]; then
  run "archive_chain（index_token 生命周期；REQ-34 §3）" "$ROOT/build/tests/archive_chain" "$ARC_ZIP"
fi
run "corrupt-chain-test（防死循环）"      bash "$ROOT/tests/corrupt-chain-test.sh"
run "timeout_test（I/O 看门狗：无进展→超时→fail-fast）" bash -c "./build/tests/timeout_test"
run "fd_test（fd 生命周期 15 条）"        "$ROOT/build/tests/fd_test" "$W/a.qcow2"
run "thread_test（并发 4 组）"            "$ROOT/build/tests/thread_test" "$W/a.qcow2" "$W/b.qcow2" 2048 2048 "$W/t.log"
# 场景③ 素材：一张最小 ISO（无 xorrisofs 就只跑前两个场景）
RACE_ISO=""
if command -v xorrisofs >/dev/null 2>&1; then
  mkdir -p "$W/isosrc" && echo hello > "$W/isosrc/a.txt"
  xorrisofs -o "$W/race.iso" "$W/isosrc" >/dev/null 2>&1 && RACE_ISO="$W/race.iso"
fi
if [ -n "$RACE_ISO" ]; then
  # 场景③ 要真读 ISO，沙箱里每次调用约 0.5s（proot syscall 代理开销，非库慢）
  run "race_test（全局锁/环形缓冲/无ctx命令 TLS 缓冲）" "$ROOT/build/tests/race_test" 5 "$RACE_ISO"
else
  run "race_test（全局锁/环形缓冲）" "$ROOT/build/tests/race_test" 40
fi
run "ntfs-move-test（盘内改名/移动/删除 端到端）" python3 "$ROOT/tests/ntfs-move-test.py"
run "ntfs-inventory-test（独立三方对账 + 幽灵目录必抓）" python3 "$ROOT/tests/ntfs-inventory-test.py"
run "copy-test（盘内复制/粘贴策略 + 删除目录，NTFS+FAT）" python3 "$ROOT/tests/copy-test.py"
run "xcopy-test（跨分区/跨镜像/宿主/ISO/WIM 复制）" python3 "$ROOT/tests/xcopy-test.py"
run "fileops-test（统一路径 + copy/move/rename/rm）" python3 "$ROOT/tests/fileops-test.py"
run "ads-test（命名数据流：ntfs.copy / file.copy 必须保留）" "$ROOT/tests/ads-test.sh"
run "vbr-tpl-test（内置引导码模板三方一致性）" python3 "$ROOT/tests/vbr-tpl-test.py"
run "fat-fs-test（FAT32 改名/移动/删除 34 条 + mtools 交叉验证）" python3 "$ROOT/tests/fat-fs-test.py"
run "cancel_test（取消 7 条）"            "$ROOT/build/tests/cancel_test" "$W/a.qcow2" 2048 "$W/big.bin"
# 微软参考件（reference/*.bin）不随仓库分发：没有就跳过，不是失败
. "$ROOT/tests/ref.sh"
. "$ROOT/tests/fixture.sh"
# 微软参考件不在仓库里：有就用原件，没有就用内置模板现造一份当输入。
# 两者的差别是「能否证明与微软逐字节一致」，读改写逻辑本身都能测到。
BCD_UEFI="$(ref_path bcd-reference-uefi.bin)"
if [ -z "$BCD_UEFI" ]; then
  BCD_UEFI="$W/fixture-uefi.bcd"; fixture_bcd_hive "$BCD_UEFI" gpt >/dev/null 2>&1 || BCD_UEFI=""
fi
BCD_BIOS="$(ref_path bcd-reference-bios-win11.bin)"
if [ -z "$BCD_BIOS" ]; then
  BCD_BIOS="$W/fixture-bios.bcd"; fixture_bcd_hive "$BCD_BIOS" mbr >/dev/null 2>&1 || BCD_BIOS=""
fi
if [ -x "$ROOT/build/tests/bcd_test" ] && [ -n "$BCD_UEFI" ]; then
  run "bcd_test（BCD 读写）" "$ROOT/build/tests/bcd_test" "$BCD_UEFI" "$W/bcd_out.bin"
elif [ -x "$ROOT/build/tests/bcd_test" ]; then
  skipped=$((skipped+1)); echo "  ⏭ 跳过 bcd_test（无参考件且自造 hive 失败）"
else skipped=$((skipped+1)); echo "  ⏭ 跳过 bcd_test（没编进构建）"; fi
if [ -x "$ROOT/build/tests/mbr_bcd_test" ] && [ -n "$BCD_BIOS" ]; then
  run "mbr_bcd_test（BIOS BCD 设备对象批量改写）" \
      "$ROOT/build/tests/mbr_bcd_test" "$BCD_BIOS" "$W/mbr_bcd.bin"
elif [ -x "$ROOT/build/tests/mbr_bcd_test" ]; then
  skipped=$((skipped+1)); echo "  ⏭ 跳过 mbr_bcd_test（无参考件且自造 hive 失败）"
else skipped=$((skipped+1)); echo "  ⏭ 跳过 mbr_bcd_test（没编进构建）"; fi
if [ -x "$ROOT/build/tests/ebr_test" ]; then
  run "ebr_test（MBR 扩展/逻辑分区 EBR 链往返）" "$ROOT/build/tests/ebr_test"
else skipped=$((skipped+1)); echo "  ⏭ 跳过 ebr_test（没编进构建）"; fi
run "ntfs-case-test（NTFS 路径大小写不敏感 6 条）" bash "$ROOT/tests/ntfs-case-test.sh"
run "qcscan_test（qcow2 别名/越界 + kill -9 续写）" "$ROOT/build/tests/qcscan_test"
run "bootplan_test（分区表/固件兼容判定 19 条）" "$ROOT/build/tests/bootplan_test"
run "identify_test（版本多源投票 12 条）" "$ROOT/build/tests/identify_test"
if [ -x "$ROOT/build/tests/bcdcreate_test" ]; then
  run "bcdcreate_test（内置模板建 BCD）" "$ROOT/build/tests/bcdcreate_test"
else skipped=$((skipped+1)); echo "  ⏭ 跳过 bcdcreate_test（没编进构建）"; fi
run "boot-repair-test（BIOS 一键修复）" bash "$ROOT/tests/boot-repair-test.sh"
run "genbcd-test（UEFI BCD 定制）"      bash "$ROOT/tests/genbcd-test.sh"
if [ -x "$ROOT/build/tests/recovery_bcd_test" ]; then
  # 官方 ISO 的 BCD（微软真值）：已抽出来放进 reference/，16 KiB，随仓库走
  ISOBCD="$(ref_path iso-bcd-win11.bin)"
  [ -f "$ISOBCD" ] || ISOBCD="$ROOT/../build/w128/iso/bcd"
  if [ ! -f "$ISOBCD" ]; then
    # 兜底：有官方 ISO 就现场抽 efi/microsoft/boot/bcd
    iso_dir="$IMGTOOL_IMAGES"
    ISO=$(ls "$iso_dir"/*.iso 2>/dev/null | head -1)
    if [ -n "${ISO:-}" ] && command -v 7z >/dev/null 2>&1; then
      mkdir -p "$W/iso"
      7z e -y -o"$W/iso" "$ISO" "efi/microsoft/boot/bcd" >/dev/null 2>&1
      [ -f "$W/iso/bcd" ] && ISOBCD="$W/iso/bcd"
    fi
  fi
  if [ -n "$ISOBCD" ] && [ -f "$ISOBCD" ] && [ -n "$BCD_UEFI" ]; then
    run "recovery_bcd_test（恢复条目）" "$ROOT/build/tests/recovery_bcd_test" \
        "$BCD_UEFI" "$W/rec.bin" '\Recovery\WindowsRE\Winre.wim' "$ISOBCD" \
        5616a432-a581-0f42-8302-cff19ec2690f 08cb2fb9-d284-f948-9892-70ef8e7d70cd
  else skipped=$((skipped+1)); echo "  ⏭ 跳过 recovery_bcd_test（缺官方 ISO BCD 参考，微软文件未随仓库分发）"; fi
else skipped=$((skipped+1)); echo "  ⏭ 跳过 recovery_bcd_test（没编进构建）"; fi

# 本轮新增（P2 / P3 / 文案规范）
run "reg-test（注册表通用编辑：读/写/列/删/事务）"            bash "$ROOT/tests/reg-test.sh"
run "winver-registry-test（REQ-72：DisplayVersion/ReleaseId/UBR/InstallationType 恒在）" bash "$ROOT/tests/winver-registry-test.sh"
run "diskfmt-test（raw / VHD / VMDK 读写 + qemu 互验）"      bash "$ROOT/tests/diskfmt-test.sh"
run "qcow2-zstd-test（qcow2 的 zstd 压缩簇：qemu-img 造、我们读）" python3 "$ROOT/tests/qcow2-zstd-test.py"
run "vhd-fault-test（N6-3：任意一步失败后映像仍可用、已提交数据不丢）" python3 "$ROOT/tests/vhd-fault-test.py"
run "vhd-test（VHD 建盘、多块写入、重开与 QEMU 互验）"         python3 "$ROOT/tests/vhd-test.py"
run "vmdk-test（VMDK 建盘、GD 落盘、跨进程重开与 qemu 互验）"   python3 "$ROOT/tests/vmdk-test.py"
run "compact-fmt-test（compact 四种源格式都真的搬数据；D2）"   python3 "$ROOT/tests/compact-fmt-test.py"
run "move-rename-test（move 的 moved_by=rename|copy；T3）"      bash "$ROOT/tests/move-rename-test.sh"
run "bigdir-wim-test（REQ-65：单目录 >256 条、宽树估算不截断）" python3 "$ROOT/tests/bigdir-wim-test.py"
run "wim-capture-test（捕获、临时目录清理、产物可读）"          bash "$ROOT/tests/wim-capture-test.sh"
run "wim-exclude-test（REQ-67：--exclude 同口径 + extract/pack/write 三段进度）" python3 "$ROOT/tests/wim-exclude-test.py"
run "provenance-test（内置模板 == 微软原件；缺原件则跳过）"   bash "$ROOT/tests/provenance-test.sh"
run "style-check（用户可见文本符合 STYLE-GUIDE）"           python3 "$ROOT/tests/style-check.py"

# 依赖裁剪（默认跳过：要重编两套约 30 s；要跑：DEGRADED=1 tests/run_all.sh）
if [ "${DEGRADED:-0}" = "1" ]; then
  run "degraded-build-test（关掉 WIM/NTFS 仍可构建且如实报不可用）" bash "$ROOT/tests/degraded-build-test.sh"
else
  skipped=$((skipped+1)); echo "  ⏭ 跳过 degraded-build-test（要跑：DEGRADED=1 tests/run_all.sh）"
fi

# 重型测试（默认跳过，需要时：WITH_WIM=1 tests/run_all.sh）
if [ "${WITH_WIM:-0}" = "1" ]; then
  run "wim-apply-test（WIM 直灌 + 中途取消，1~8 分钟）" bash "$ROOT/tests/wim-apply-test.sh"
else
  skipped=$((skipped+1)); echo "  ⏭ 跳过 wim-apply-test（重型；要跑：WITH_WIM=1 tests/run_all.sh）"
fi

echo
echo "════ 总计：通过 $pass · 失败 $fail · 跳过 $skipped ════"
if [ $fail -eq 0 ]; then echo "✅ 全量回归通过"; rm -rf "$W"; else echo "❌ 有失败（调试目录 $W 保留）"; fi
[ $fail -eq 0 ]
