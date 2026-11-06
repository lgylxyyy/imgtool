#!/usr/bin/env bash
# REQ-37：① 恢复分区 GPT 属性 0x8000000000000001（建盘默认 + populate 幂等）
#         ② wim.file-version 读映像里文件的 PE 版本（含 bootmgr 那种前面带引导壳的）
set -u
T="${IMGCMD:-$(cd "$(dirname "$0")/.." && pwd)/build/tools/imgcmd}"
W="$(mktemp -d)"; trap 'rm -rf "$W"' EXIT
pass=0; fail=0
ok(){ pass=$((pass+1)); echo "  ✅ $1"; }
no(){ fail=$((fail+1)); echo "  ❌ $1"; echo "      $2"; }
jget(){ python3 -c "import sys,json
d=json.load(sys.stdin)
try:
    print(eval(sys.argv[1]))
except Exception as e:
    print('ERR')" "$1" 2>/dev/null; }

echo "════ ① 建盘默认属性 ════"
$T img.create --out="$W/a.qcow2" --size-gb=8 --table=gpt --rec-mb=512 --sys-mb=4096 >/dev/null 2>&1
attr=$($T part.show --img="$W/a.qcow2" 2>/dev/null | jget '[p["attributes"] for p in d["data"]["pt"]["parts"] if p["typeGuid"].startswith("de94bba4")][0]')
[ "$attr" = "0x8000000000000001" ] && ok "img.create 的恢复分区直接是 0x8000000000000001" \
    || no "恢复分区默认属性" "got=$attr"

echo "════ ② populate 顺手纠正 + 幂等 ════"
[ "$(command -v wimlib-imagex)" ] || { echo "  ⏭ 没有 wimlib-imagex，跳过 ②③④"; }
if command -v wimlib-imagex >/dev/null 2>&1; then
  mkdir -p "$W/wsrc"
  head -c 2000000 /dev/urandom > "$W/wsrc/Winre.wim"
  wimlib-imagex capture "$W/wsrc" "$W/winre.wim" "RE" --compress=LZX >/dev/null 2>&1
  REC_LBA=8833024
  $T part.attr --img="$W/a.qcow2" --lba=$REC_LBA --attr=0x8000000000000000 >/dev/null 2>&1
  o=$($T recovery.populate --img="$W/a.qcow2" --rec-lba=$REC_LBA --winre-src="$W/winre.wim" --format=true 2>/dev/null)
  [ "$(jget "d['data']['attr_before']" <<<"$o")" = "0x8000000000000000" ] \
    && ok "attr_before 如实回填" || no "attr_before" "$o"
  [ "$(jget "d['data']['attr_after']" <<<"$o")" = "0x8000000000000001" ] \
    && ok "attr_after 已改成官方值" || no "attr_after" "$o"
  [ "$(jget "d['data']['attr_set']" <<<"$o")" = "True" ] \
    && ok "attr_set=true（这次真写了）" || no "attr_set" "$o"
  o=$($T recovery.populate --img="$W/a.qcow2" --rec-lba=$REC_LBA --winre-src="$W/winre.wim" 2>/dev/null)
  [ "$(jget "d['data']['attr_set']" <<<"$o")" = "False" ] \
    && ok "第二次 attr_set=false（幂等，不重复写）" || no "幂等" "$o"
  attr=$($T part.show --img="$W/a.qcow2" 2>/dev/null | jget '[p["attributes"] for p in d["data"]["pt"]["parts"] if p["typeGuid"].startswith("de94bba4")][0]')
  [ "$attr" = "0x8000000000000001" ] && ok "落盘后复读还是官方值" || no "复读" "got=$attr"
  # 2026-11-04：恢复分区检查整体搬去 recovery.status（boot.check 与 WinRE 解耦）
  st=$($T recovery.status --img="$W/a.qcow2" 2>/dev/null)
  [ "$(jget "d['data']['recovery_partition_attr_ok']" <<<"$st")" = "True" ] \
    && ok "recovery.status: rec 属性转绿" || no "recovery.status rec 属性" "got=$st"
  [ "$(jget "d['data']['winre_present']" <<<"$st")" = "True" ] \
    && ok "recovery.status: WinRE 卡片字段=显示" || no "winre_present" "got=$st"
fi

echo "════ ③ MBR：没有属性位，不报错也不改东西 ════"
$T img.create --out="$W/b.qcow2" --size-gb=8 --table=mbr --extra-mb=1024 >/dev/null 2>&1
if command -v wimlib-imagex >/dev/null 2>&1; then
  o=$($T recovery.populate-system --img="$W/b.qcow2" --rec-lba=1050624 \
        --winre-src="$W/winre.wim" --format=true 2>/dev/null)
  [ "$(jget "d['ok']" <<<"$o")" = "True" ] || jget "d['ok']" <<<"$o"
  [ "$(jget "d['data']['attr_supported']" <<<"$o")" = "False" ] \
    && ok "MBR → attr_supported=false" || no "MBR attr_supported" "$o"
  [ "$(jget "d['data'].get('attr_error','')" <<<"$o")" = "" ] \
    && ok "MBR 不报 attr_error" || no "MBR attr_error" "$o"
else
  echo "  ⏭ 没有 wimlib-imagex，跳过"
fi

echo "════ ④ wim.file-version ════"
if ! command -v wimlib-imagex >/dev/null 2>&1; then
  echo "  ⏭ 没有 wimlib-imagex，跳过"
else
  # 造一个最小 PE32+：只有一个 .rsrc，里面是 RT_VERSION → VS_VERSIONINFO → 固定版本 10.0.19045.1
  python3 - "$W" <<'PY'
import struct,sys,os
W=sys.argv[1]
def pe():
    rsrc=bytearray()
    def hdr(nname,nid): return struct.pack('<IIHHHH',0,0,4,0,nname,nid)
    rsrc+=hdr(0,1)                      # level1: type=16
    rsrc+=struct.pack('<II',16,0x80000000|0x18)
    rsrc+=hdr(0,1)                      # level2: name=1
    rsrc+=struct.pack('<II',1,0x80000000|0x30)
    rsrc+=hdr(0,1)                      # level3: lang=0x409
    rsrc+=struct.pack('<II',0x409,0x48) # 数据项
    # VS_VERSIONINFO
    key='VS_VERSION_INFO'.encode('utf-16le')+b'\0\0'
    fi=struct.pack('<IIIIIIIIIIIII',0xFEEF04BD,0x00010000,
                   (10<<16)|0,(19045<<16)|1,
                   (10<<16)|0,(19045<<16)|1,
                   0x3F,0,4,1,0,0,0)
    body=key
    while (6+len(body))%4: body+=b'\0'
    vi=struct.pack('<HHH',6+len(body)+len(fi),52,0)+body+fi
    off=0x58
    rsrc+=struct.pack('<IIII',0x1000+off,len(vi),1252,0)
    rsrc+=b'\0'*(off-len(rsrc))+vi
    rawsz=(len(rsrc)+0x1FF)//0x200*0x200
    rsrc+=b'\0'*(rawsz-len(rsrc))
    dos=bytearray(0x40); dos[0:2]=b'MZ'; struct.pack_into('<I',dos,0x3C,0x40)
    coff=struct.pack('<HHIIIHH',0x8664,1,0,0,0,0xF0,0x22)
    opt=bytearray(0xF0); struct.pack_into('<H',opt,0,0x20B)
    struct.pack_into('<I',opt,108,16)
    struct.pack_into('<II',opt,112+16,0x1000,len(rsrc))
    sec=struct.pack('<8sIIIIIIHHI',b'.rsrc\0\0\0',len(rsrc),0x1000,rawsz,0x200,0,0,0,0,0x40000040)
    head=dos+b'PE\0\0'+coff+bytes(opt)+sec
    return head+bytes(0x200-len(head))+bytes(rsrc)
p=pe()
open(os.path.join(W,'plain.exe'),'wb').write(p)
open(os.path.join(W,'stub.bin'),'wb').write(b'\xe9\xd5\x01\xeb\x04\x90'+b'\0'*(0x100-6)+p)
PY
  mkdir -p "$W/vsrc/Windows/Boot/PCAT" "$W/vsrc/Windows/Boot/EFI"
  cp "$W/plain.exe" "$W/vsrc/Windows/Boot/EFI/bootmgfw.efi"
  cp "$W/stub.bin"  "$W/vsrc/Windows/Boot/PCAT/bootmgr"
  echo not-a-pe > "$W/vsrc/readme.txt"
  wimlib-imagex capture "$W/vsrc" "$W/inst.wim" "Win" --compress=LZX >/dev/null 2>&1
  o=$($T wim.file-version --src="$W/inst.wim" --path='\Windows\Boot\EFI\bootmgfw.efi' 2>/dev/null)
  [ "$(jget "d['data']['file_version']" <<<"$o")" = "10.0.19045.1" ] \
    && ok "普通 PE：file_version=10.0.19045.1" || no "普通 PE 版本" "$o"
  [ "$(jget "d['data']['build']" <<<"$o")" = "19045" ] && ok "build 段单独给（App 直接比）" || no "build" "$o"
  o=$($T wim.file-version --src="$W/inst.wim" --path='/Windows/Boot/PCAT/bootmgr' 2>/dev/null)
  [ "$(jget "d['data']['file_version']" <<<"$o")" = "10.0.19045.1" ] \
    && ok "带引导壳（PE 不在偏移 0）：照样读出来" || no "引导壳 PE" "$o"
  [ "$(jget "d['data'].get('pe_offset',0)" <<<"$o")" = "256" ] && ok "pe_offset=256 如实回填" || no "pe_offset" "$o"
  o=$($T wim.file-version --src="$W/inst.wim" --path=/readme.txt 2>/dev/null)
  [ "$(jget "d['data']['has_version']" <<<"$o")" = "False" ] && ok "非 PE：has_version=false + reason" || no "非 PE" "$o"
  o=$($T wim.file-version --wim="$W/inst.wim" --path=/nope.bin 2>/dev/null)
  [ "$(jget "d['err']['error_code']" <<<"$o")" = "WIM-PATH-NOTFOUND" ] && ok "路径不存在 → WIM-PATH-NOTFOUND" || no "不存在路径" "$o"
  if command -v xorriso >/dev/null 2>&1; then
    mkdir -p "$W/isodir/sources"; cp "$W/inst.wim" "$W/isodir/sources/install.wim"
    xorriso -as mkisofs -quiet -o "$W/t.iso" "$W/isodir" 2>/dev/null
    o=$($T wim.file-version --src="$W/t.iso" --path='\Windows\Boot\EFI\bootmgfw.efi' 2>/dev/null)
    [ "$(jget "d['data']['file_version']" <<<"$o")" = "10.0.19045.1" ] \
      && ok "--src 直接给 ISO 也行" || no "ISO 源" "$o"
  else
    echo "  ⏭ 没有 xorriso，跳过 ISO 源"
  fi
fi

echo
echo "════ 结果：通过 $pass · 失败 $fail ════"
[ "$fail" -eq 0 ]