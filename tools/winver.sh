#!/bin/bash
# winver.sh — 一条命令看"这个文件到底是哪个 Windows"
#   用法: tools/winver.sh <xxx.iso|xxx.esd|xxx.wim|xxx.qcow2>
#   · ISO/ESD/WIM：读里面的映像清单（不解包）
#   · qcow2：自动找 Windows 分区，读【内核 PE + 注册表 + SKU】多方法交叉验证
set -u
T=$(cd "$(dirname "$0")/.." && pwd)/build/tools
F="${1:-}"
[ -f "$F" ] || { echo "用法: $0 <iso|esd|wim|qcow2>"; exit 2; }

case "$F" in
  *.qcow2|*.QCOW2)
    # 找 Windows 分区：优先最大的 NTFS 分区
    LBA=$("$T/imgcmd" part.show --img="$F" --ro --progress=none 2>/dev/null | python3 -c "
import json,sys
d=json.load(sys.stdin)
if not d.get('ok'): raise SystemExit(1)
ps=[p for p in d['data']['pt']['parts'] if p.get('used',True)]
cand=[p for p in ps if p['typeGuid'].startswith('ebd0a0a2')] or \
     [p for p in ps if p['mbri'] in (7,0x07)]
cand.sort(key=lambda p: -p['sectors'])
print(cand[0]['first'] if cand else 0)" 2>/dev/null)
    [ "${LBA:-0}" -gt 0 ] || { echo "❌ 找不到 NTFS/Windows 分区"; exit 1; }
    echo "文件 : $F"
    echo "分区 : LBA $LBA"
    "$T/imgcmd" win.version --img="$F" --lba="$LBA" --progress=none 2>/dev/null | python3 -c "
import json,sys
d=json.load(sys.stdin)
if not d.get('ok'): print('❌',d.get('err')); raise SystemExit(1)
x=d['data']
g=lambda k: x.get(k,'-')
print('结论 : build %s  |  %s  |  edition %s'%(g('consensus_build'),g('registry.product_name'),g('registry.edition_id')))
if g('registry.display_version')!='-': print('版本 : %s (build %s)'%(g('registry.display_version'),g('registry.current_build')))
print('证据 :')
for e in x.get('evidence',[]): print('   %-12s %-6s %s'%(e['source'],e['build'],e['version']))
if x.get('skus'): print('SKU  : %s'%x['skus'][:110])
print('判定 : %s'%('⚠ 证据互相矛盾，镜像被改过' if x.get('mismatch') else '✅ 证据一致'))
if x.get('note'): print('备注 : %s'%x['note'])
"
    ;;
  *)
    echo "文件 : $F"
    "$T/imgcmd" wim.identify --src="$F" --progress=none 2>/dev/null | python3 -c "
import json,sys
d=json.load(sys.stdin)
if not d.get('ok'): print('❌',d.get('err')); raise SystemExit(1)
x=d['data']
if x.get('volume'): print('卷标 : %s'%x['volume'])
print('容器 : %s%s'%(x['container'], ('  内路径 '+x['inner_path']) if x.get('inner_path') else ''))
print('映像 : %d 个'%x['count'])
print('  idx  %-34s %-12s %-6s %s'%('名称','版本','架构','edition'))
for i in x['images']:
    arch={'0':'x86','9':'x64','5':'arm','12':'arm64'}.get(i['arch'],i['arch'])
    print('  %-4s %-34s %-12s %-6s %s'%(i['index'],i['name'][:34],i['version'],arch,i['edition_id']))
"
    ;;
esac
