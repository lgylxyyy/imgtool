#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
parttable-test —— 分区表导出/应用（part.export / part.apply，REQ-20261026-14）
+ 空 MBR 表（REQ-20261026-13）。

覆盖：
  · 导出 → 应用到同尺寸空盘：parts[] 逐字段一致（含 partGuid/typeGuid/attributes）
  · MBR + 逻辑分区：EBR 链重建，logical 标志保住
  · 盘太小 → PART-NOSPACE + need_sectors/need_gb，且不留半截表
  · 已有表 + 没 force → PT-NOT-EMPTY(parts_total)；force → 覆盖
  · 坏文件：kind 不对 / 表类型不认识 / sectorSize / 重叠 / 重复 partGuid / MBR 5 主分区
  · 空 MBR：建得出、读得回（table=MBR, 0 块）、还能直接 part.add
  · apply 不动数据（先写文件，apply 后还在）
"""
import json, os, subprocess, sys, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
IMGCMD = os.path.join(ROOT, 'build', 'tools', 'imgcmd')
W = tempfile.mkdtemp(prefix='pttable')
ok = bad = 0


def run(cmd, **kw):
    args = [IMGCMD, cmd] + ['--%s=%s' % (k, v) for k, v in kw.items()]
    p = subprocess.run(args, capture_output=True, text=True)
    js = [l for l in p.stdout.splitlines() if l.startswith('{')]
    return json.loads(js[-1]) if js else {'ok': False, 'raw': (p.stdout + p.stderr)[-300:]}


def data(d):
    return d.get('data') or {}


def err(d):
    return d.get('err') or {}


def check(name, cond, extra=''):
    global ok, bad
    if cond:
        ok += 1
        print('  ✅ %s' % name)
    else:
        bad += 1
        print('  ❌ %s  %s' % (name, extra))


def parts(img):
    return data(run('part.show', img=img)).get('pt', {}).get('parts') or []


def same_parts(a, b):
    ka = sorted([(p['first'], p['sectors'], p['mbri'], p['logical'], p['typeGuid'],
                  p['partGuid'], p['name'], p['attributes']) for p in parts(a)])
    kb = sorted([(p['first'], p['sectors'], p['mbri'], p['logical'], p['typeGuid'],
                  p['partGuid'], p['name'], p['attributes']) for p in parts(b)])
    return ka == kb, (ka, kb)


print('════ 空 MBR 表（REQ-13）════')
p = os.path.join(W, 'emptymbr.qcow2')
d = run('img.create', out=p, **{'size-gb': 20, 'table': 'mbr', 'layout': 'empty'})
check('建空 MBR 盘成功（原来 PART-WRITE-FAIL）', d.get('ok'), json.dumps(d)[:200])
pt = data(run('part.show', img=p)).get('pt') or {}
check('读回 table=MBR、0 块', pt.get('table') == 'MBR' and len(pt.get('parts') or []) == 0,
      json.dumps(pt)[:200])
check('pt 里有 diskSignature / hasBootCode / sectorSize',
      pt.get('diskSignature', '').startswith('0x') and pt.get('hasBootCode') is False
      and pt.get('sectorSize') == 512, json.dumps(pt)[:200])
d = run('part.add', img=p, **{'size-mb': 1024})
check('空 MBR 上能直接加分区', d.get('ok') and data(d).get('parts_total') == 1,
      json.dumps(d)[:200])

p = os.path.join(W, 'del_last.qcow2')
run('img.create', out=p, **{'size-gb': 20, 'table': 'mbr', 'layout': 'single'})
d = run('part.remove', img=p, **{'lba': 2048})
check('MBR 单分区删到 0 块（原来也是死路）',
      d.get('ok') and data(d).get('parts_total') == 0, json.dumps(d)[:200])
check('删完还是合法 MBR 表', (data(run('part.show', img=p)).get('pt') or {}).get('table') == 'MBR')

print('════ GPT：导出 → 应用到同尺寸空盘 ════')
src = os.path.join(W, 'src.qcow2')
run('img.create', out=src, **{'size-gb': 20, 'table': 'gpt', 'esp-mb': 200, 'msr-mb': 16,
                              'rec-mb': 512, 'sys-mb': 16384, 'extra-mb': 1024})
js = os.path.join(W, 't.json')
d = run('part.export', img=src, out=js)
check('part.export 写出文件 + 回 parts_total',
      d.get('ok') and data(d).get('parts_total') == 5 and os.path.exists(js),
      json.dumps(d)[:200])
with open(js, encoding='utf-8') as f:
    spec = json.load(f)
check('文件头是规范格式（kind/v/table/parts）',
      spec.get('kind') == 'icdq.parttable' and spec.get('v') == 1
      and spec.get('table') == 'GPT' and isinstance(spec.get('parts'), list),
      json.dumps(spec)[:200])

dst = os.path.join(W, 'dst.qcow2')
run('img.create', out=dst, **{'size-gb': 20, 'table': 'none'})
d = run('part.apply', img=dst, **{'in': js})
check('part.apply 到空盘成功', d.get('ok') and data(d).get('parts_total') == 5,
      json.dumps(d)[:240])
same, detail = same_parts(src, dst)
check('两块表逐字段一致（含 partGuid / typeGuid / attributes）', same, str(detail)[:300])
check('磁盘 GUID 也保住了',
      (data(run('part.show', img=src)).get('pt') or {}).get('diskGuid')
      == (data(run('part.show', img=dst)).get('pt') or {}).get('diskGuid'))
check('img.info 能正常读新盘（备份 GPT 表写了）', run('img.info', img=dst).get('ok'))

print('════ MBR + 逻辑分区：EBR 链要重建 ════')
m = os.path.join(W, 'm.qcow2')
run('img.create', out=m, **{'size-gb': 20, 'table': 'mbr', 'sys-mb': 512,
                            'win-mb': 16384, 'extra-mb': 1024, 'mbr-fill': 'reserve'})
mj = os.path.join(W, 'm.json')
run('part.export', img=m, out=mj)
m2 = os.path.join(W, 'm2.qcow2')
run('img.create', out=m2, **{'size-gb': 20, 'table': 'none'})
d = run('part.apply', img=m2, **{'in': mj})
check('MBR 表应用成功', d.get('ok'), json.dumps(d)[:200])
same, detail = same_parts(m, m2)
check('MBR + 逻辑分区逐字段一致（logical 保住了）', same, str(detail)[:300])
check('读表仍然正常（EBR 链没写坏）', run('part.show', img=m2).get('ok'))
src_pt = data(run('part.show', img=m)).get('pt') or {}
dst_pt = data(run('part.show', img=m2)).get('pt') or {}
check('MBR 磁盘签名往返一致（BCD 记着它）',
      src_pt.get('diskSignature') == dst_pt.get('diskSignature'),
      '%s vs %s' % (src_pt.get('diskSignature'), dst_pt.get('diskSignature')))
check('hasBootCode 也一致', src_pt.get('hasBootCode') == dst_pt.get('hasBootCode'),
      '%s vs %s' % (src_pt.get('hasBootCode'), dst_pt.get('hasBootCode')))

print('════ 容量 / force / 覆盖 ════')
small = os.path.join(W, 'small.qcow2')
run('img.create', out=small, **{'size-gb': 8, 'table': 'none'})
d = run('part.apply', img=small, **{'in': js})
check('盘太小 → PART-NOSPACE', err(d).get('error_code') == 'PART-NOSPACE', json.dumps(d)[:240])
check('带 need_sectors / need_gb',
      'need_sectors' in data(d) and data(d).get('need_gb', 0) >= 17,
      json.dumps(data(d))[:160])
check('失败后不留半截表（空盘还是空盘）',
      (data(run('part.show', img=small)).get('error_code') == 'PART-READ-FAIL')
      or len(parts(small)) == 0, json.dumps(data(run('part.show', img=small)))[:160])

d = run('part.apply', img=dst, **{'in': js})
check('已有表 + 无 force → PT-NOT-EMPTY（带 parts_total）',
      err(d).get('error_code') == 'PT-NOT-EMPTY' and data(d).get('parts_total') == 5,
      json.dumps(d)[:200])
d = run('part.apply', img=dst, **{'in': js, 'force': 'true'})
check('force=true 覆盖成功', d.get('ok'), json.dumps(d)[:160])

print('════ 坏文件 / 非法表 ════')


def write_spec(name, obj):
    p = os.path.join(W, name)
    with open(p, 'w', encoding='utf-8') as f:
        json.dump(obj, f, ensure_ascii=False)
    return p


base = dict(spec)
d = run('part.apply', img=dst, **{'in': write_spec('bad_kind.json', dict(base, kind='nope'))})
check('kind 不对 → PT-FILE-BAD@kind',
      err(d).get('error_code') == 'PT-FILE-BAD' and err(d).get('at') == 'kind',
      json.dumps(d)[:200])
d = run('part.apply', img=dst, **{'in': write_spec('bad_table.json', dict(base, table='APM'))})
check('table 不认识 → PT-FILE-BAD', err(d).get('error_code') == 'PT-FILE-BAD',
      json.dumps(d)[:160])
d = run('part.apply', img=dst, **{'in': write_spec('bad_ss.json', dict(base, sectorSize=4096))})
check('sectorSize=4096 → PT-FILE-BAD@sectorSize', err(d).get('at') == 'sectorSize',
      json.dumps(d)[:160])

p1 = {'first': 2048, 'sectors': 4096, 'typeGuid': 'ebd0a0a2-b9e5-4433-87c0-68b6b72699c7',
      'partGuid': '11111111-2222-3333-4444-555555555555', 'name': 'A', 'mbri': 7}
p2 = dict(p1, first=4096, partGuid='66666666-7777-8888-9999-aaaaaaaaaaaa', name='B')
d = run('part.apply', img=dst, **{'in': write_spec('overlap.json', dict(base, parts=[p1, p2]))})
check('两条重叠 → PT-OVERLAP', err(d).get('error_code') == 'PT-OVERLAP', json.dumps(d)[:200])

dup = dict(p1, first=8192)
d = run('part.apply', img=dst, **{'in': write_spec('dupguid.json', dict(base, parts=[p1, dup]))})
check('partGuid 重复 → PT-DUP-GUID', err(d).get('error_code') == 'PT-DUP-GUID',
      json.dumps(d)[:200])

five = [{'first': 2048 + i * 8192, 'sectors': 4096, 'mbri': 7, 'typeGuid': '',
         'partGuid': '', 'name': 'P%d' % i} for i in range(5)]
d = run('part.apply', img=dst, **{'in': write_spec('fiveprim.json',
        dict(base, table='MBR', parts=five, mbrCode=None))})
check('MBR 5 个主分区 → PT-TOO-MANY', err(d).get('error_code') == 'PT-TOO-MANY',
      json.dumps(d)[:200])

print('════ apply 不碰数据 ════')
dm = os.path.join(W, 'data.qcow2')
run('img.create', out=dm, **{'size-gb': 20, 'table': 'gpt', 'esp-mb': 200,
                             'msr-mb': 16, 'sys-mb': 16384})
run('fs.format', img=dm, lba=444416, fs='ntfs', label='DATA')
blob = os.path.join(W, 'b.bin')
with open(blob, 'wb') as f:
    f.write(os.urandom(2 << 20))
run('ntfs.put', img=dm, lba=444416, src=blob, dst='/hello.bin')
dj = os.path.join(W, 'data.json')
run('part.export', img=dm, out=dj)
d = run('part.apply', img=dm, **{'in': dj, 'force': 'true'})
check('对同一张盘应用它自己的表（force）成功', d.get('ok'), json.dumps(d)[:160])
lst = data(run('ntfs.list', img=dm, lba=444416))
check('分区里的文件还在（apply 只改表不动数据）', (lst.get('count') or 0) == 1,
      json.dumps(lst)[:200])

print()
print('════ 结果：通过 %d · 失败 %d ════' % (ok, bad))
print('调试目录：%s' % W)
sys.exit(0 if bad == 0 else 1)