#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
limits-test —— 分区尺寸口径 + 追加分区 + part.add/remove/resize（2026-10-25）

对应 REQ：
  REQ-20261025-01  img.create --extra-mb 一次建出额外分区
  REQ-20261025-02  part.add / part.remove / part.resize
  REQ-20261025-03  PART-SIZE-BAD + cmd.list 暴露 min/max/role

覆盖：
  · 越界 → PART-SIZE-BAD（不是 CMD-ARG-BAD），err.at 是参数名，
    失败信封里带 data.{role,min,max,unit,got}，且【不留半个镜像】
  · 边界值必须通过（esp 128 / rec 512 / sys 16384 ...）
  · --limits=off 放行（测试/实验专用口子）
  · --extra-mb：GPT 顺序 + 1 MiB 对齐 + 总数；MBR 主分区满 → 扩展容器 + 逻辑分区
  · 放不下 → PART-NOSPACE（at=extra-mb），且不留文件
  · part.add / remove / resize：改表不改数据，空间不够报 PART-NOSPACE
  · cmd.list：limits 总表 + 每个尺寸参数带 min/max/unit/role
"""
import json, os, subprocess, sys, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
IMGCMD = os.path.join(ROOT, 'build', 'tools', 'imgcmd')
W = tempfile.mkdtemp(prefix='limits')
ok = bad = 0


def run(cmd, **kw):
    args = [IMGCMD, cmd] + ['--%s=%s' % (k, v) for k, v in kw.items()]
    p = subprocess.run(args, capture_output=True, text=True)
    js = [l for l in p.stdout.splitlines() if l.startswith('{')]
    return json.loads(js[-1]) if js else {'ok': False, 'raw': (p.stdout + p.stderr)[-300:]}


def data(d):
    return d.get('data') or {}


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


def bad_size(name, code, at, **kw):
    """通用：断言失败码/at、data 里有范围、文件没留下（img.create 用）。"""
    out = os.path.join(W, 'no.qcow2')
    if os.path.exists(out):
        os.unlink(out)
    d = run('img.create', out=out, **kw)
    e = d.get('err') or {}
    dd = data(d)
    check('%s → %s@%s' % (name, code, at),
          (not d.get('ok')) and e.get('error_code') == code and e.get('at') == at,
          json.dumps(d)[:200])
    check('%s：失败也带 role/min/max/unit' % name,
          dd.get('role') and 'min' in dd and 'max' in dd and dd.get('unit') and 'got' in dd,
          json.dumps(dd)[:160])
    check('%s：没留下半个镜像' % name, not os.path.exists(out))


print('════ 尺寸口径：越界必须 PART-SIZE-BAD 且不留文件 ════')
bad_size('esp-mb=8', 'PART-SIZE-BAD', 'esp-mb',
         **{'size-gb': 64, 'table': 'gpt', 'esp-mb': 8})
bad_size('esp-mb=2048', 'PART-SIZE-BAD', 'esp-mb',
         **{'size-gb': 64, 'table': 'gpt', 'esp-mb': 2048})
bad_size('msr-mb=32', 'PART-SIZE-BAD', 'msr-mb',
         **{'size-gb': 64, 'table': 'gpt', 'msr-mb': 32})
bad_size('rec-mb=100', 'PART-SIZE-BAD', 'rec-mb',
         **{'size-gb': 64, 'table': 'gpt', 'rec-mb': 100})
bad_size('gpt sys-mb=1（Windows 分区 <128 MiB）', 'PART-SIZE-BAD', 'sys-mb',
         **{'size-gb': 64, 'table': 'gpt', 'sys-mb': 1})
bad_size('mbr sys-mb=60（系统保留太小）', 'PART-SIZE-BAD', 'sys-mb',
         **{'size-gb': 8, 'table': 'mbr', 'sys-mb': 60})
bad_size('extra-mb=8（碎片）', 'PART-SIZE-BAD', 'extra-mb',
         **{'size-gb': 64, 'table': 'gpt', 'sys-mb': 16384, 'extra-mb': 8})

d = run('img.create', out=os.path.join(W, 'g0.qcow2'), **{'size-gb': 0})
check('size-gb=0 → CMD-ARG-BAD@size-gb',
      (d.get('err') or {}).get('error_code') == 'CMD-ARG-BAD'
      and (d.get('err') or {}).get('at') == 'size-gb', json.dumps(d)[:160])
d = run('img.create', out=os.path.join(W, 'g5.qcow2'), **{'size-gb': 2048})
check('size-gb=2048 → CMD-ARG-BAD@size-gb',
      (d.get('err') or {}).get('error_code') == 'CMD-ARG-BAD'
      and (d.get('err') or {}).get('at') == 'size-gb', json.dumps(d)[:160])

print('════ 边界值必须通过 ════')
p = os.path.join(W, 'edge.qcow2')
d = run('img.create', out=p, **{'size-gb': 20, 'table': 'gpt', 'esp-mb': 128,
                                'msr-mb': 16, 'rec-mb': 512, 'sys-mb': 16384})
ps = parts(p)
check('esp=128 / msr=16 / rec=512 / sys=16384 建得出来（4 个分区）',
      d.get('ok') and len(ps) == 4, json.dumps(d)[:200])
check('恢复分区确实 512 MiB', any(x['sectors'] // 2048 == 512 for x in ps),
      str([x['sectors'] for x in ps]))
check('不再自动多塞一块"吃掉剩余空间"的数据分区', len(ps) == 4)

p = os.path.join(W, 'edge2.qcow2')
d = run('img.create', out=p, **{'size-gb': 20, 'table': 'gpt', 'rec-mb': 2048})
check('rec-mb=2048（上限）通过', d.get('ok'), json.dumps(d)[:200])
p = os.path.join(W, 'edge3.qcow2')
d = run('img.create', out=p, **{'size-gb': 4, 'table': 'mbr', 'sys-mb': 1024})
check('mbr sys-mb=1024（上限）通过', d.get('ok'), json.dumps(d)[:200])

p = os.path.join(W, 'off.qcow2')
d = run('img.create', out=p, **{'size-gb': 1, 'table': 'gpt', 'esp-mb': 32,
                                'msr-mb': 16, 'limits': 'off'})
check('--limits=off 放行（测试口子）', d.get('ok'), json.dumps(d)[:200])
d = run('img.create', out=os.path.join(W, 'bad.qcow2'), **{'size-gb': 1, 'limits': 'maybe'})
check('--limits=maybe → CMD-ARG-BAD@limits',
      (d.get('err') or {}).get('error_code') == 'CMD-ARG-BAD'
      and (d.get('err') or {}).get('at') == 'limits', json.dumps(d)[:160])

print('════ extra-mb：GPT 顺序/对齐/总数 ════')
p = os.path.join(W, 'ex_gpt.qcow2')
d = run('img.create', out=p, **{'size-gb': 64, 'table': 'gpt', 'layout': 'windows',
                                'esp-mb': 200, 'msr-mb': 16, 'rec-mb': 0,
                                'sys-mb': 20480, 'extra-mb': '4096,8192'})
ps = parts(p)
check('parts_total=5（ESP+MSR+Windows+4096+8192）',
      d.get('ok') and data(d).get('parts_total') == 5 and len(ps) == 5, json.dumps(d)[:200])
check('顺序 = ESP → MSR → Windows → 4096 → 8192',
      [x['sectors'] // 2048 for x in ps] == [200, 16, 20480, 4096, 8192],
      str([x['sectors'] // 2048 for x in ps]))
check('每块起点 1 MiB 对齐', all(x['first'] % 2048 == 0 for x in ps), str([x['first'] for x in ps]))
last = ps[-1]
check('最后一块没越过盘尾', last['first'] + last['sectors'] <= 64 * 1024 * 1024 * 2,
      str(last))
check('额外分区是 Basic Data（GPT）',
      len(ps[2:]) == 3 and all(x['typeGuid'].startswith('ebd0a0a2') for x in ps[2:]),
      str([x['typeGuid'][:8] for x in ps]))

print('════ extra-mb：MBR 主分区满 → 扩展容器 + 逻辑分区 ════')
p = os.path.join(W, 'ex_mbr.qcow2')
d = run('img.create', out=p, **{'size-gb': 32, 'table': 'mbr', 'layout': 'windows',
                                'sys-mb': 512, 'rec-mb': 800, 'extra-mb': '2048,4096'})
ps = parts(p)
ext = [x for x in ps if x['mbri'] == 0x0f]
log = [x for x in ps if x.get('logical')]
check('建出来了（系统+Windows+扩展容器+恢复+2 逻辑 = 6 项）',
      d.get('ok') and len(ps) == 6 and len(ext) == 1 and len(log) == 2,
      json.dumps([(x['first'], x['mbri'], x.get('logical')) for x in ps]))
check('逻辑分区大小 = 2048 / 4096 MiB', [x['sectors'] // 2048 for x in log] == [2048, 4096],
      str([x['sectors'] // 2048 for x in log]))
check('主分区没超过 4 个', len([x for x in ps if not x.get('logical')]) <= 4,
      str([(x['first'], x['mbri']) for x in ps]))

print('════ extra-mb：放不下 → PART-NOSPACE@extra-mb，不留文件 ════')
p = os.path.join(W, 'ex_big.qcow2')
d = run('img.create', out=p, **{'size-gb': 20, 'table': 'gpt', 'sys-mb': 16384,
                                'extra-mb': 8192})
check('20 GiB 里塞 16384+8192 → PART-NOSPACE@extra-mb',
      (d.get('err') or {}).get('error_code') == 'PART-NOSPACE'
      and (d.get('err') or {}).get('at') == 'extra-mb', json.dumps(d)[:200])
check('给出了 avail_mb 让 App 提示用户', 'avail_mb' in data(d), json.dumps(data(d))[:160])
check('没留下半个镜像', not os.path.exists(p))

d = run('img.create', out=os.path.join(W, 'ex_sys0.qcow2'),
        **{'size-gb': 64, 'table': 'gpt', 'sys-mb': 0, 'extra-mb': 4096})
check('sys-mb=0 + extra-mb → CMD-ARG-BAD@sys-mb（不静默丢分区）',
      (d.get('err') or {}).get('error_code') == 'CMD-ARG-BAD'
      and (d.get('err') or {}).get('at') == 'sys-mb', json.dumps(d)[:200])
d = run('img.create', out=os.path.join(W, 'ex_bad.qcow2'),
        **{'size-gb': 64, 'table': 'gpt', 'sys-mb': 16384, 'extra-mb': '4096,,8192'})
check('空元素 1,,2 → CMD-ARG-BAD@extra-mb',
      (d.get('err') or {}).get('error_code') == 'CMD-ARG-BAD'
      and (d.get('err') or {}).get('at') == 'extra-mb', json.dumps(d)[:200])

print('════ part.add / remove / resize ════')
p = os.path.join(W, 'pa.qcow2')
run('img.create', out=p, **{'size-gb': 32, 'table': 'gpt', 'esp-mb': 200,
                            'msr-mb': 16, 'sys-mb': 16384})
d = run('part.add', img=p, **{'size-mb': 8192, 'type': 'data', 'label': 'DATA'})
dd = data(d)
check('part.add 回 lba/sectors/index/parts_total',
      d.get('ok') and dd.get('lba') and dd.get('sectors') == 8192 * 2048
      and dd.get('index') == 4 and dd.get('parts_total') == 4, json.dumps(d)[:240])
check('新分区起点 1 MiB 对齐', dd.get('lba') % 2048 == 0, str(dd.get('lba')))
ps = parts(p)
check('part.show 看到 4 块，最后一块是 DATA',
      len(ps) == 4 and ps[-1].get('name') == 'DATA', json.dumps(ps)[-200:])
check('备份 GPT 表写好（img.info 能再打开）', run('img.info', img=p).get('ok'))

d = run('part.add', img=p, **{'size-mb': 999999})
check('空间不够 → PART-NOSPACE@size-mb',
      (d.get('err') or {}).get('error_code') == 'PART-NOSPACE'
      and (d.get('err') or {}).get('at') == 'size-mb', json.dumps(d)[:200])
check('失败后表没变（还是 4 块）', len(parts(p)) == 4)

d = run('part.add', img=p, **{'size-mb': 8})
check('size-mb=8 → PART-SIZE-BAD@size-mb',
      (d.get('err') or {}).get('error_code') == 'PART-SIZE-BAD'
      and (d.get('err') or {}).get('at') == 'size-mb', json.dumps(d)[:200])

e = os.path.join(W, 'empty.qcow2')
run('img.create', out=e, **{'size-gb': 8, 'table': 'none'})
d = run('part.add', img=e, **{'size-mb': 1024})
check('空盘 part.add → CMD-ARG-BAD@img（不是 PART-READ-FAIL）',
      (d.get('err') or {}).get('error_code') == 'CMD-ARG-BAD'
      and (d.get('err') or {}).get('at') == 'img', json.dumps(d)[:200])

lba = parts(p)[-1]['first']
d = run('part.remove', img=p, **{'lba': lba})
check('part.remove 删掉最后一块', d.get('ok') and data(d).get('parts_total') == 3,
      json.dumps(d)[:200])
d = run('part.add', img=p, **{'size-mb': 8192})
check('删完能再加回来（复用同一段空间）',
      d.get('ok') and data(d).get('lba') == lba, json.dumps(d)[:200])

d = run('part.resize', img=p, **{'lba': lba, 'size-mb': 4096})
check('part.resize 缩到 4096 MiB', d.get('ok') and data(d).get('sectors') == 4096 * 2048,
      json.dumps(d)[:200])
d = run('part.resize', img=p, **{'lba': lba, 'size-mb': 8192})
check('part.resize 再扩回 8192 MiB', d.get('ok') and data(d).get('sectors') == 8192 * 2048,
      json.dumps(d)[:200])
first = parts(p)[0]['first']
d = run('part.resize', img=p, **{'lba': first, 'size-mb': 512})
check('非末位分区 resize 被拒（不许挪别人的起点）',
      (d.get('err') or {}).get('error_code') == 'CMD-ARG-BAD'
      and (d.get('err') or {}).get('at') == 'lba', json.dumps(d)[:200])

print('════ MBR win-mb（指定 Windows 分区大小）════')
p = os.path.join(W, 'm1.qcow2')
d = run('img.create', out=p, **{'size-gb': 64, 'table': 'mbr', 'sys-mb': 512, 'win-mb': 40960})
ps = parts(p)
check('win-mb=40960 → 两块（系统保留 + Windows）', d.get('ok') and len(ps) == 2,
      json.dumps(d)[:200])
check('Windows 分区正好 40960 MiB', ps[1]['sectors'] // 2048 == 40960 if len(ps) > 1 else False,
      str([x['sectors'] // 2048 for x in ps]))
check('剩余空间没有被自动切成数据分区', len(ps) == 2)

d = run('img.create', out=os.path.join(W, 'm2.qcow2'),
        **{'size-gb': 64, 'table': 'mbr', 'win-mb': 8})
e = d.get('err') or {}
check('win-mb=8 → PART-SIZE-BAD@win-mb',
      e.get('error_code') == 'PART-SIZE-BAD' and e.get('at') == 'win-mb', json.dumps(d)[:200])
check('data.role=win，min=128（REQ-12），max=当前可用（不是 0）',
      data(d).get('role') == 'win' and data(d).get('min') == 128
      and (data(d).get('max') or 0) > 128, json.dumps(data(d))[:200])

p = os.path.join(W, 'm3.qcow2')
d = run('img.create', out=p, **{'size-gb': 64, 'table': 'mbr', 'sys-mb': 512,
                                'win-mb': 40960, 'extra-mb': '10240,8192'})
ps = parts(p)
log = [x for x in ps if x.get('logical')]
check('Windows 显式 + 额外分区（第三块起进扩展容器）',
      d.get('ok') and len(ps) == 5 and len(log) == 2, json.dumps(d)[:200])
check('Windows 仍然 40960 MiB', ps[1]['sectors'] // 2048 == 40960 if len(ps) > 1 else False,
      str([x['sectors'] // 2048 for x in ps]))
check('额外分区落在 Windows 之后（没有重叠）',
      len(ps) > 2 and ps[2]['first'] >= ps[1]['first'] + ps[1]['sectors'],
      json.dumps([(x['first'], x['sectors']) for x in ps]))

p = os.path.join(W, 'm4.qcow2')
d = run('img.create', out=p, **{'size-gb': 64, 'table': 'mbr'})
ps = parts(p)
check('不给 win-mb → 老行为（Windows 吃满剩余）',
      d.get('ok') and len(ps) == 2 and ps[-1]['first'] + ps[-1]['sectors'] >= 64 * 1024 * 1024 * 2 - 4096,
      json.dumps([(x['first'], x['sectors']) for x in ps]))

d = run('img.create', out=os.path.join(W, 'm5.qcow2'),
        **{'size-gb': 8, 'table': 'mbr', 'win-mb': 65536})
e = d.get('err') or {}
check('win-mb 超过盘容量 → PART-NOSPACE@win-mb + avail_mb',
      e.get('error_code') == 'PART-NOSPACE' and e.get('at') == 'win-mb'
      and 'avail_mb' in data(d), json.dumps(d)[:200])

d = run('img.create', out=os.path.join(W, 'm6.qcow2'),
        **{'size-gb': 64, 'table': 'gpt', 'sys-mb': 16384, 'win-mb': 40960})
check('GPT 传 win-mb → CMD-ARG-BAD@win-mb（GPT 用 sys-mb）',
      (d.get('err') or {}).get('error_code') == 'CMD-ARG-BAD'
      and (d.get('err') or {}).get('at') == 'win-mb', json.dumps(d)[:200])

print('════ PART-NOSPACE 带 avail_mb（part.add / part.resize）════')
p = os.path.join(W, 'av.qcow2')
run('img.create', out=p, **{'size-gb': 32, 'table': 'gpt', 'esp-mb': 200,
                            'msr-mb': 16, 'sys-mb': 16384})
d = run('part.add', img=p, **{'size-mb': 99999})
check('part.add 放不下 → data.avail_mb = 最大空闲', (data(d).get('avail_mb') or 0) > 15000,
      json.dumps(d)[:200])
d = run('part.add', img=p, **{'size-mb': 4096})
lba = data(d).get('lba')
d = run('part.resize', img=p, **{'lba': lba, 'size-mb': 99999})
e = d.get('err') or {}
check('part.resize 放不下 → PART-NOSPACE@size-mb + avail_mb',
      e.get('error_code') == 'PART-NOSPACE' and e.get('at') == 'size-mb'
      and (data(d).get('avail_mb') or 0) > 15000, json.dumps(d)[:200])

print('════ part.add type=logical（可选需求）════')
p = os.path.join(W, 'lg.qcow2')
run('img.create', out=p, **{'size-gb': 64, 'table': 'mbr', 'sys-mb': 512,
                            'win-mb': 20480, 'extra-mb': '4096,4096'})
log = [x for x in parts(p) if x.get('logical')]
d = run('part.remove', img=p, **{'lba': log[-1]['first']})
d = run('part.add', img=p, **{'size-mb': 4096, 'type': 'logical'})
check('type=logical 往扩展容器尾部加（logical:true）',
      d.get('ok') and data(d).get('logical') is True and data(d).get('lba') == log[-1]['first'],
      json.dumps(d)[:240])

d = run('part.add', img=os.path.join(W, 'm4.qcow2'), **{'size-mb': 1024, 'type': 'logical'})
check('没有扩展容器时 type=logical → CMD-ARG-BAD@type',
      (d.get('err') or {}).get('error_code') == 'CMD-ARG-BAD'
      and (d.get('err') or {}).get('at') == 'type', json.dumps(d)[:200])
d = run('part.add', img=os.path.join(W, 'av.qcow2'), **{'size-mb': 1024, 'type': 'logical'})
check('GPT 上 type=logical → CMD-ARG-BAD@type',
      (d.get('err') or {}).get('error_code') == 'CMD-ARG-BAD'
      and (d.get('err') or {}).get('at') == 'type', json.dumps(d)[:200])

print('════ part.add type=extended（建空扩展容器，不自动塞逻辑分区）════')
p = os.path.join(W, 'ex_ext.qcow2')
run('img.create', out=p, **{'size-gb': 32, 'table': 'mbr', 'win-mb': 16384})
d = run('part.add', img=p, **{'type': 'extended'})
dd = data(d)
ps = parts(p)
check('type=extended 省略 size-mb → 吃满空闲区',
      d.get('ok') and dd.get('type') == 'extended' and (dd.get('size_mb') or 0) > 15000,
      json.dumps(d)[:200])
check('扩展容器建出来是 0x0f，且里面【没有】逻辑分区',
      len(ps) == 3 and ps[-1]['mbri'] == 0x0f and not any(x.get('logical') for x in ps),
      json.dumps([(x['first'], x['mbri'], x.get('logical')) for x in ps]))
d = run('part.add', img=p, **{'type': 'logical', 'size-mb': 4096})
check('有了容器后再加逻辑分区（logical:true，在容器里）',
      d.get('ok') and data(d).get('logical') is True
      and data(d).get('lba') > parts(p)[-2]['first'],
      json.dumps(d)[:200])
d = run('part.add', img=p, **{'size-mb': 1024})
check('有容器后 type=data 也会进容器当逻辑分区（主分区槽已封）',
      d.get('ok') and data(d).get('logical') is True, json.dumps(d)[:200])
d = run('part.add', img=p, **{'type': 'extended'})
check('MBR 只允许一个扩展容器（第二个 → CMD-ARG-BAD@type）',
      (d.get('err') or {}).get('error_code') == 'CMD-ARG-BAD'
      and (d.get('err') or {}).get('at') == 'type', json.dumps(d)[:200])

# 空容器可以删（建错了不用整盘重建）；有逻辑分区就不能删
p = os.path.join(W, 'ex_ext2.qcow2')
run('img.create', out=p, **{'size-gb': 32, 'table': 'mbr', 'win-mb': 16384})
d = run('part.add', img=p, **{'type': 'extended'})
ext_lba = data(d).get('lba')
d = run('part.remove', img=p, **{'lba': ext_lba})
check('空的扩展容器可以删（建错了能撤销）',
      d.get('ok') and data(d).get('parts_total') == 2, json.dumps(d)[:200])
run('part.add', img=p, **{'type': 'extended'})
run('part.add', img=p, **{'type': 'logical', 'size-mb': 2048})
d = run('part.remove', img=p, **{'lba': ext_lba})
check('容器里还有逻辑分区 → 拒绝删容器（CMD-ARG-BAD@lba）',
      (d.get('err') or {}).get('error_code') == 'CMD-ARG-BAD'
      and (d.get('err') or {}).get('at') == 'lba', json.dumps(d)[:200])

d = run('part.add', img=os.path.join(W, 'ex_ext.qcow2'), **{'type': 'extended'})
check('（重复）第二个容器仍然被拒',
      (d.get('err') or {}).get('at') == 'type', json.dumps(d)[:160])

p = os.path.join(W, 'ex_4pri.qcow2')
run('img.create', out=p, **{'size-gb': 64, 'table': 'mbr', 'win-mb': 16384})
run('part.add', img=p, **{'size-mb': 8192})
run('part.add', img=p, **{'size-mb': 8192})
d = run('part.add', img=p, **{'type': 'extended'})
check('4 个主分区槽已满 → 建不了扩展容器（CMD-ARG-BAD@img）',
      (d.get('err') or {}).get('error_code') == 'CMD-ARG-BAD'
      and (d.get('err') or {}).get('at') == 'img', json.dumps(d)[:200])
d = run('part.add', img=p, **{'size-mb': 1024})
check('（同上）type=data 也加不了（@img）',
      (d.get('err') or {}).get('at') == 'img', json.dumps(d)[:160])

d = run('part.add', img=os.path.join(W, 'av.qcow2'), **{'type': 'extended'})
check('GPT 上 type=extended → CMD-ARG-BAD@type',
      (d.get('err') or {}).get('at') == 'type', json.dumps(d)[:160])
d = run('part.add', img=os.path.join(W, 'av.qcow2'), )
check('不给 size-mb（type=data）→ CMD-ARG-BAD@size-mb',
      (d.get('err') or {}).get('error_code') == 'CMD-ARG-BAD'
      and (d.get('err') or {}).get('at') == 'size-mb', json.dumps(d)[:200])

print('════ mbr-fill：主分区槽策略（REQ-07）════')
p = os.path.join(W, 'fill_p.qcow2')
d = run('img.create', out=p, **{'size-gb': 64, 'table': 'mbr', 'sys-mb': 512,
                                'win-mb': 20480, 'extra-mb': '8192,8192',
                                'mbr-fill': 'primary'})
ps = parts(p)
check('primary：4 块主分区、没有 0x0F',
      d.get('ok') and len(ps) == 4 and not any(x['mbri'] == 0x0f for x in ps)
      and not any(x.get('logical') for x in ps),
      json.dumps([(x['first'], x['mbri'], x.get('logical')) for x in ps]))

p = os.path.join(W, 'fill_r.qcow2')
d = run('img.create', out=p, **{'size-gb': 64, 'table': 'mbr', 'sys-mb': 512,
                                'win-mb': 20480, 'extra-mb': '8192,8192'})
ps = parts(p)
check('reserve（默认）：0x27 + 0x07 + 0x0F + 2 逻辑（行为不变）',
      d.get('ok') and any(x['mbri'] == 0x0f for x in ps)
      and len([x for x in ps if x.get('logical')]) == 2,
      json.dumps([(x['first'], x['mbri'], x.get('logical')) for x in ps]))

p = os.path.join(W, 'fill_bad.qcow2')
if os.path.exists(p): os.unlink(p)
d = run('img.create', out=p, **{'size-gb': 64, 'table': 'mbr', 'mbr-fill': 'primary',
                                'extra-mb': '8192,8192,8192'})
check('primary 放不下 N=3 → PART-NOSPACE@extra-mb',
      (d.get('err') or {}).get('error_code') == 'PART-NOSPACE'
      and (d.get('err') or {}).get('at') == 'extra-mb', json.dumps(d)[:200])
check('带上 avail_primary_slots=2', data(d).get('avail_primary_slots') == 2,
      json.dumps(data(d))[:120])
check('没留下半个镜像', not os.path.exists(p))

d = run('img.create', out=os.path.join(W, 'fill_rec.qcow2'),
        **{'size-gb': 64, 'table': 'mbr', 'mbr-fill': 'primary', 'rec-mb': 800,
           'extra-mb': '8192,8192'})
check('有恢复分区时只剩 1 个槽（avail_primary_slots=1）',
      data(d).get('avail_primary_slots') == 1, json.dumps(d)[:200])

d = run('img.create', out=os.path.join(W, 'fill_x.qcow2'),
        **{'size-gb': 8, 'table': 'mbr', 'mbr-fill': 'weird'})
check('mbr-fill 非法值 → CMD-ARG-BAD@mbr-fill',
      (d.get('err') or {}).get('error_code') == 'CMD-ARG-BAD'
      and (d.get('err') or {}).get('at') == 'mbr-fill', json.dumps(d)[:200])

p = os.path.join(W, 'fill_gpt.qcow2')
d = run('img.create', out=p, **{'size-gb': 32, 'table': 'gpt', 'sys-mb': 16384,
                                'extra-mb': 4096, 'mbr-fill': 'primary'})
check('GPT 传 mbr-fill → 忽略（照常建盘）', d.get('ok'), json.dumps(d)[:160])

print('════ part.resize 对容器内逻辑卷（REQ-07 §6.1）════')
p = os.path.join(W, 'lg_resize.qcow2')
run('img.create', out=p, **{'size-gb': 64, 'table': 'mbr', 'win-mb': 20480,
                            'extra-mb': '8192,8192'})
ps = parts(p)
last = max(ps, key=lambda x: x['first'])
d = run('part.resize', img=p, **{'lba': last['first'], 'size-mb': 4096})
check('逻辑卷可以缩（容器内）', d.get('ok') and data(d).get('sectors') == 4096 * 2048,
      json.dumps(d)[:200])
d = run('part.resize', img=p, **{'lba': last['first'], 'size-mb': 8192})
check('逻辑卷可以扩回（容器内）', d.get('ok'), json.dumps(d)[:160])
d = run('part.resize', img=p, **{'lba': last['first'], 'size-mb': 20000})
check('超出扩展容器 → PART-NOSPACE@size-mb + avail_mb',
      (d.get('err') or {}).get('error_code') == 'PART-NOSPACE'
      and (d.get('err') or {}).get('at') == 'size-mb'
      and (data(d).get('avail_mb') or 0) > 0, json.dumps(d)[:200])

print('════ Windows 分区下限 128 MiB（REQ-12）════')
p = os.path.join(W, 'w128.qcow2')
d = run('img.create', out=p, **{'size-gb': 20, 'table': 'gpt', 'esp-mb': 200,
                                'msr-mb': 16, 'sys-mb': 128})
ps = parts(p)
check('GPT sys-mb=128 建得出来（3 块）', d.get('ok') and len(ps) == 3, json.dumps(d)[:200])
check('Windows 分区正好 128 MiB（262144 扇区）',
      len(ps) == 3 and ps[2]['sectors'] == 128 * 2048, str([x['sectors'] for x in ps]))
p = os.path.join(W, 'w128m.qcow2')
d = run('img.create', out=p, **{'size-gb': 20, 'table': 'mbr', 'sys-mb': 512, 'win-mb': 128})
ps = parts(p)
check('MBR win-mb=128 建得出来（2 块）', d.get('ok') and len(ps) == 2, json.dumps(d)[:200])
check('MBR Windows 分区正好 128 MiB',
      len(ps) == 2 and ps[1]['sectors'] == 128 * 2048, str([x['sectors'] for x in ps]))
for v in (200, 512, 1024):
    d = run('img.create', out=os.path.join(W, 'w%d.qcow2' % v),
            **{'size-gb': 20, 'table': 'gpt', 'sys-mb': v})
    check('GPT sys-mb=%d 也放行' % v, d.get('ok'), json.dumps(d)[:160])
p = os.path.join(W, 'w0.qcow2')
d = run('img.create', out=p, **{'size-gb': 20, 'table': 'gpt', 'sys-mb': 0})
check('sys-mb=0 仍然 = 吃满剩余（没被下限改动带偏）',
      d.get('ok') and parts(p)[-1]['sectors'] > 1024 * 2048, json.dumps(d)[:160])
d = run('img.create', out=os.path.join(W, 'w_m1.qcow2'),
        **{'size-gb': 20, 'table': 'mbr', 'win-mb': 1})
check('MBR win-mb=1 仍拒（min=128）',
      (d.get('err') or {}).get('error_code') == 'PART-SIZE-BAD', json.dumps(d)[:160])

print('════ mbr-fill=auto：3 主 + 容器（REQ-08）════')
for n, want in ((0, '0x27 0x07'), (1, '0x27 0x07 0x07'),
                (2, '0x27 0x07 0x07 0x0f+L'), (3, '0x27 0x07 0x07 0x0f+L+L')):
    p = os.path.join(W, 'auto_%d.qcow2' % n)
    kw = {'size-gb': 64, 'table': 'mbr', 'win-mb': 20480, 'mbr-fill': 'auto'}
    if n: kw['extra-mb'] = ','.join(['8192'] * n)
    d = run('img.create', out=p, **kw)
    ps = parts(p)
    # 逻辑分区在 part.show 里是独立条目；把容器和后面的逻辑合并成 0x0f+L 便于比对
    toks = []
    for x in ps:
        if x['mbri'] == 0x0f:
            toks.append('0x0f')
        elif x.get('logical'):
            if toks and toks[-1].startswith('0x0f'):
                toks[-1] = '0x0f+L' if toks[-1] == '0x0f' else toks[-1] + '+L'
            else:
                toks.append('0x07+L')
        else:
            toks.append('0x%02x' % x['mbri'])
    check('auto N=%d → %s' % (n, want), d.get('ok') and ' '.join(toks) == want,
          ' '.join(toks))

p = os.path.join(W, 'auto_2.qcow2')
d = run('part.add', img=p, **{'size-mb': 4096, 'type': 'logical'})
check('auto 建完还能继续加逻辑卷（容器吃满剩余空间）',
      d.get('ok') and data(d).get('logical') is True, json.dumps(d)[:200])

print('════ part.add --lba：点哪块建哪块（REQ-10）════')
p = os.path.join(W, 'lba.qcow2')
run('img.create', out=p, **{'size-gb': 8, 'table': 'gpt', 'layout': 'empty', 'limits': 'off'})
run('part.add', img=p, **{'size-mb': 1024})
run('part.add', img=p, **{'size-mb': 1024})
run('part.remove', img=p, **{'lba': 2048})          # 前面空出 1024 MiB 的洞

d = run('part.add', img=p, **{'size-mb': 2048, 'lba': 2048})
check('洞里只要 1024，要 2048 → PART-NOSPACE@size-mb',
      (d.get('err') or {}).get('error_code') == 'PART-NOSPACE'
      and (d.get('err') or {}).get('at') == 'size-mb', json.dumps(d)[:200])
check('avail_mb 是【这块洞】的大小（1024），不是全盘最大空洞',
      data(d).get('avail_mb') == 1024, json.dumps(data(d))[:120])

d = run('part.add', img=p, **{'size-mb': 512, 'lba': 2048})
check('lba 落在洞里 → 就从洞里起（first=2048）',
      d.get('ok') and data(d).get('lba') == 2048, json.dumps(d)[:200])

d = run('part.add', img=p, **{'size-mb': 16, 'lba': 2099200})
check('lba 打在已有分区上 → CMD-ARG-BAD@lba',
      (d.get('err') or {}).get('error_code') == 'CMD-ARG-BAD'
      and (d.get('err') or {}).get('at') == 'lba', json.dumps(d)[:200])
d = run('part.add', img=p, **{'size-mb': 16, 'lba': 999999999})
check('lba 超出磁盘 → CMD-ARG-BAD@lba',
      (d.get('err') or {}).get('at') == 'lba', json.dumps(d)[:160])
d = run('part.add', img=p, **{'size-mb': 16, 'lba': 1050625})
check('未对齐的 lba → 起点对到 1 MiB',
      d.get('ok') and (data(d).get('lba') or 0) % 2048 == 0, json.dumps(d)[:200])
d = run('part.add', img=p, **{'size-mb': 16, 'where': 'tail'})
check('不给 lba 时 where 照旧可用', d.get('ok'), json.dumps(d)[:160])

print('════ cmd.list：范围是数据，不是前端常量 ════')
d = run('cmd.list')
tbl = data(d).get('table') or {}
lim = tbl.get('limits') or {}
check('cmd.list 带 limits 总表', bool(lim), json.dumps(lim)[:200])
check('esp 口径 = 128~1024 MiB',
      lim.get('esp', {}).get('min') == 128 and lim.get('esp', {}).get('max') == 1024
      and lim.get('esp', {}).get('unit') == 'MiB', json.dumps(lim.get('esp')))
check('windows 下限 128 MiB（REQ-12：不再是 16 GiB）',
      lim.get('windows', {}).get('min') == 128, json.dumps(lim.get('windows')))
check('sys-mb.gpt 下限也是 128（两处别分叉）',
      lim.get('sys-mb', {}).get('gpt', {}).get('min') == 128,
      json.dumps(lim.get('sys-mb', {}).get('gpt')))
check('mbr-win 下限也是 128',
      lim.get('mbr-win', {}).get('min') == 128, json.dumps(lim.get('mbr-win')))
check('sys-mb 分表给（gpt/mbr 两套数字）',
      lim.get('sys-mb', {}).get('gpt', {}).get('min') == 128
      and lim.get('sys-mb', {}).get('mbr', {}).get('max') == 1024, json.dumps(lim.get('sys-mb')))
check('mbr-win 项在（App 用它在不在判断 so 支不支持）',
      lim.get('mbr-win', {}).get('min') == 128
      and lim.get('mbr-win', {}).get('zero') == 'fill', json.dumps(lim.get('mbr-win')))

args = {a['name']: a for c in tbl.get('cmds') or [] if c['name'] == 'img.create'
        for a in c.get('args') or []}
check('img.create 每个尺寸参数带 min/max/unit/role',
      all(k in args.get(n, {}) for n in ('size-gb', 'esp-mb', 'msr-mb', 'rec-mb', 'extra-mb')
          for k in ('min', 'max', 'unit', 'role')), json.dumps(sorted(args))[:300])
check('rec-mb 标了 zero_ok（0=不建）', args.get('rec-mb', {}).get('zero_ok') is True)
check('存在 part.add 命令', any(c['name'] == 'part.add' for c in tbl.get('cmds') or []))

print()
print('════ 结果：通过 %d · 失败 %d ════' % (ok, bad))
print('调试目录：%s' % W)
sys.exit(0 if bad == 0 else 1)