#!/bin/bash
# ntfs-case-test.sh — NTFS 路径【大小写不敏感】
#
# 为什么要这条：Windows 路径不区分大小写，而 ntfs-3g 的
# ntfs_inode_lookup_by_name() 是精确匹配。实测 Win7 装完是
# \Windows\Boot、\Windows\System32；写 /Windows/boot 直接 FAT-LS-FAIL，
# 用户看到的是"文件不存在/版本读不出"，很难想到是大小写。
# 第三方精简镜像大小写更乱，所以 linux 侧必须兜一层（用 NTFS 自己的 upcase 表）。
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
T="$ROOT/build/tools"
W=$(mktemp -d /tmp/ntfscaseXXXXXX)
pass=0; fail=0
ok(){ if [ "$1" = "1" ]; then echo "  ✅ $2"; pass=$((pass+1)); else echo "  ❌ $2"; fail=$((fail+1)); fi; }
okb(){ if [ "$1" = "True" ]; then echo "  ✅ $2"; pass=$((pass+1)); else echo "  ❌ $2（得到 $1）"; fail=$((fail+1)); fi; }
q(){ "$T/imgcmd" "$@" --progress=none 2>/dev/null; }
jget(){ python3 -c "import json,sys; d=json.load(sys.stdin); v=d.get('data',{}); print($1)"; }

echo "════ NTFS 大小写不敏感 ════"
q img.create --out="$W/a.qcow2" --size-gb=1 --table=mbr --sys-mb=100 --limits=off >/dev/null
LBA=$(q part.show --img="$W/a.qcow2" | python3 -c "import json,sys; ps=json.load(sys.stdin)['data']['pt']['parts']; print(sorted(ps,key=lambda p:-p['sectors'])[0]['first'])")
q fs.format --img="$W/a.qcow2" --lba="$LBA" --fs=ntfs --label=Case >/dev/null

printf 'hello-case' > "$W/x.txt"
q ntfs.put --img="$W/a.qcow2" --lba="$LBA" --src="$W/x.txt" --dst=/Windows/Boot/Test.TXT >/dev/null
okb "$(q ntfs.exists --img="$W/a.qcow2" --lba="$LBA" --path=/Windows/Boot/Test.TXT | jget "v['exists']")" "原样路径能查到"

okb "$(q ntfs.exists --img="$W/a.qcow2" --lba="$LBA" --path=/windows/boot/test.txt | jget "v['exists']")" "全小写路径也能查到（★ 这条以前是 false）"
okb "$(q ntfs.exists --img="$W/a.qcow2" --lba="$LBA" --path=/WINDOWS/BOOT | jget "v['exists']")" "目录也能大小写不敏感"

# 列表 + 导出（走同一套 lookup）
CNT=$(q ntfs.list --img="$W/a.qcow2" --lba="$LBA" --path=/windows/boot | jget "len(v['names'])")
ok "$([ "$CNT" = "1" ] && echo 1 || echo 0)" "小写路径能列目录（$CNT 项）"

q ntfs.export --img="$W/a.qcow2" --lba="$LBA" --path=/WINDOWS/boot/test.TXT --dst="$W/out.bin" >/dev/null
ok "$([ "$(cat "$W/out.bin" 2>/dev/null)" = "hello-case" ] && echo 1 || echo 0)" "小写路径能导出，内容一致"

# 反例单独判（上面那行只是防止误判为成功）
if [ "$(q ntfs.exists --img="$W/a.qcow2" --lba="$LBA" --path=/Windows/Booz | jget "v['exists']")" = "False" ]; then
  echo "  ✅ 反例核对：/Windows/Booz → False"; pass=$((pass+1))
else
  echo "  ❌ 反例核对：/Windows/Booz 竟然存在"; fail=$((fail+1))
fi

echo
echo "════ 结果：通过 $pass · 失败 $fail ════"
if [ "$fail" = "0" ]; then rm -rf "$W"; else echo "调试目录：$W"; fi
[ "$fail" = "0" ]