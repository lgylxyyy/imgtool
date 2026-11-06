#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
compact-label-test —— 紧凑另存（img.compact）+ 只改卷标（ntfs.label / fat.label）
对应 REQ-20261025-06。

覆盖：
  · img.compact 全部保留：源 md5 前后不变、虚拟大小/分区表/分区数一致、
    bytes_after ≤ bytes_before、verify 通过
  · img.compact --keep 只保留一个分区：parts_copied=1，其余分区留空（表还在）
  · out 已存在 → CMD-ARG-BAD@out；--force 覆盖；src==out → CMD-ARG-BAD
  · src 不存在 → IMG-OPEN-FAIL；空盘（table=none）→ CMD-ARG-BAD@src
  · ntfs.label：卷标改了、used_bytes 不变、文件还能列出来
  · fat.label：BPB + 根目录都改（小写转大写）
  · 反例：对 FAT 用 ntfs.label → PART-FS-BAD；超长/非法字符 → CMD-ARG-BAD@label
"""
import hashlib, json, os, subprocess, sys, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
IMGCMD = os.path.join(ROOT, 'build', 'tools', 'imgcmd')
W = tempfile.mkdtemp(prefix='compact')
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


def md5(path):
    h = hashlib.md5()
    with open(path, 'rb') as f:
        for b in iter(lambda: f.read(1 << 20), b''):
            h.update(b)
    return h.hexdigest()


print('════ 造一张有内容的盘（8 GiB GPT，NTFS 4 GiB + 8 MiB 文件）════')
BIG = os.path.join(W, 'big.qcow2')
run('img.create', out=BIG, **{'size-gb': 8, 'table': 'gpt', 'esp-mb': 200,
                              'msr-mb': 16, 'sys-mb': 4096, 'limits': 'off'})
ps = parts(BIG)
WIN = [p for p in ps if p['sectors'] > 1000000][0]['first']
ESP = ps[0]['first']
check('建盘成功（3 块）', len(ps) == 3, json.dumps(ps)[:200])

d = run('fs.format', img=BIG, lba=WIN, fs='ntfs', label='SYSTEM')
check('格式化 NTFS（4 GiB）', d.get('ok'), json.dumps(d)[:160])
blob = os.path.join(W, 'blob.bin')
with open(blob, 'wb') as f:
    f.write(os.urandom(8 << 20))
d = run('ntfs.put', img=BIG, lba=WIN, src=blob, dst='/blob.bin')
check('写一个 8 MiB 文件', d.get('ok'), json.dumps(d)[:160])
d = run('fs.format', img=BIG, lba=ESP, fs='fat32', label='ESP')
check('格式化 ESP（FAT32）', d.get('ok'), json.dumps(d)[:160])
scan0 = data(run('part.scan', img=BIG, lba=WIN))
used0 = scan0.get('used_bytes') or 0
check('part.scan 读到卷标 SYSTEM', scan0.get('label') == 'SYSTEM', json.dumps(scan0)[:160])

print('════ img.compact（全部保留）════')
before_md5 = md5(BIG)
SMALL = os.path.join(W, 'small.qcow2')
d = run('img.compact', src=BIG, out=SMALL)
dd = data(d)
check('compact 成功 + verify 通过', d.get('ok') and dd.get('verified') is True, json.dumps(d)[:220])
check('源文件 md5 前后不变（源只读）', md5(BIG) == before_md5)
check('结果不比源大', (dd.get('bytes_after') or 0) <= (dd.get('bytes_before') or 1),
      '%s -> %s' % (dd.get('bytes_before'), dd.get('bytes_after')))
check('分区数一致（3 块）', len(parts(SMALL)) == 3, json.dumps(parts(SMALL))[:200])
check('虚拟容量一致', data(run('img.info', img=SMALL)).get('size_bytes') == 8 * 1024 ** 3,
      json.dumps(data(run('img.info', img=SMALL)))[:200])
check('拷了 3 个分区', dd.get('parts_copied') == 3, str(dd.get('parts_copied')))
check('结果里的卷标还在（数据真的拷过去了）',
      data(run('part.scan', img=SMALL, lba=WIN)).get('label') == 'SYSTEM')
check('结果里的文件还能列出来',
      (data(run('ntfs.list', img=SMALL, lba=WIN)).get('count') or 0) == 1,
      json.dumps(data(run('ntfs.list', img=SMALL, lba=WIN)))[:200])

print('════ img.compact --keep（只保留系统分区）════')
KEEP1 = os.path.join(W, 'keep1.qcow2')
d = run('img.compact', src=BIG, out=KEEP1, **{'keep': str(WIN)})
check('parts_copied=1', data(d).get('parts_copied') == 1, json.dumps(d)[:200])
check('分区表还在（3 块，其余留空）', len(parts(KEEP1)) == 3, str(len(parts(KEEP1))))
esp_scan = data(run('part.scan', img=KEEP1, lba=ESP))
check('没保留的 ESP 现在是空的（fs=none / level<=1 / 没有文件）',
      esp_scan.get('fs') in (None, 'none', 'unknown') or (esp_scan.get('level') or 0) <= 1,
      json.dumps(esp_scan)[:160])
check('保留的 NTFS 分区数据在', data(run('part.scan', img=KEEP1, lba=WIN)).get('label') == 'SYSTEM')

print('════ img.compact 的反例 ════')
d = run('img.compact', src=BIG, out=SMALL)
check('out 已存在 → CMD-ARG-BAD@out',
      (d.get('err') or {}).get('error_code') == 'CMD-ARG-BAD'
      and (d.get('err') or {}).get('at') == 'out', json.dumps(d)[:200])
d = run('img.compact', src=BIG, out=SMALL, force='true')
check('--force 覆盖成功', d.get('ok'), json.dumps(d)[:160])
d = run('img.compact', src=BIG, out=BIG)
check('src==out → CMD-ARG-BAD@out',
      (d.get('err') or {}).get('at') == 'out', json.dumps(d)[:160])
d = run('img.compact', src=os.path.join(W, 'nope.qcow2'), out=os.path.join(W, 'x.qcow2'))
check('src 不存在 → IMG-OPEN-FAIL',
      (d.get('err') or {}).get('error_code') == 'IMG-OPEN-FAIL', json.dumps(d)[:160])
d = run('img.compact', src=BIG, out=os.path.join(W, 'k.qcow2'), **{'keep': '12345'})
check('keep 里给了不存在的 LBA → CMD-ARG-BAD',
      (d.get('err') or {}).get('error_code') == 'CMD-ARG-BAD', json.dumps(d)[:160])
EMPTY = os.path.join(W, 'empty.qcow2')
run('img.create', out=EMPTY, **{'size-gb': 4, 'table': 'none'})
d = run('img.compact', src=EMPTY, out=os.path.join(W, 'e2.qcow2'))
check('空盘（table=none）→ PART-READ-FAIL（REQ 里列的就是它）',
      (d.get('err') or {}).get('error_code') == 'PART-READ-FAIL', json.dumps(d)[:200])

print('════ ntfs.label（只改卷标）════')
d = run('ntfs.label', img=BIG, lba=WIN, label='Data')
check('改卷标成功', d.get('ok') and data(d).get('label') == 'Data', json.dumps(d)[:200])
scan1 = data(run('part.scan', img=BIG, lba=WIN))
check('part.scan 读到新卷标 Data', scan1.get('label') == 'Data', json.dumps(scan1)[:160])
check('used_bytes 没变（没动数据）', (scan1.get('used_bytes') or 0) == used0,
      '%s vs %s' % (scan1.get('used_bytes'), used0))
check('文件还在（ntfs.list 正常）',
      (data(run('ntfs.list', img=BIG, lba=WIN)).get('count') or 0) == 1)
d = run('ntfs.label', img=BIG, lba=WIN, label='')
check('空卷标 → 清掉卷标（part.scan 不再报 label）',
      d.get('ok') and not data(run('part.scan', img=BIG, lba=WIN)).get('label'),
      json.dumps(d)[:160])
d = run('ntfs.label', img=BIG, lba=WIN, label='Windows')
check('再设回去（Windows）', d.get('ok'), json.dumps(d)[:160])

print('════ fat.label（BPB + 根目录）════')
d = run('fat.label', img=BIG, lba=ESP, label='boot')
check('小写自动转大写（BOOT）', d.get('ok') and data(d).get('label') == 'BOOT', json.dumps(d)[:160])
check('part.scan 读到 BOOT',
      data(run('part.scan', img=BIG, lba=ESP)).get('label') == 'BOOT')
check('ESP 里还能列目录（没写坏）',
      run('fat.ls', img=BIG, lba=ESP, path='/').get('ok') is True)

print('════ 反例 ════')
d = run('ntfs.label', img=BIG, lba=ESP, label='x')
check('对 FAT32 用 ntfs.label → PART-FS-BAD',
      (d.get('err') or {}).get('error_code') == 'PART-FS-BAD', json.dumps(d)[:160])
d = run('fat.label', img=BIG, lba=WIN, label='x')
check('对 NTFS 用 fat.label → PART-FS-BAD',
      (d.get('err') or {}).get('error_code') == 'PART-FS-BAD', json.dumps(d)[:160])
d = run('fat.label', img=BIG, lba=ESP, label='123456789012')
check('FAT 卷标 >11 → CMD-ARG-BAD@label',
      (d.get('err') or {}).get('error_code') == 'CMD-ARG-BAD'
      and (d.get('err') or {}).get('at') == 'label', json.dumps(d)[:160])
d = run('fat.label', img=BIG, lba=ESP, label='a/b')
check('FAT 非法字符 → CMD-ARG-BAD@label',
      (d.get('err') or {}).get('at') == 'label', json.dumps(d)[:160])
d = run('ntfs.label', img=BIG, lba=WIN, label='x' * 40)
check('NTFS 卷标 >32 → CMD-ARG-BAD@label',
      (d.get('err') or {}).get('at') == 'label', json.dumps(d)[:160])

print('════ fs.format --label 与 label 命令同口径（REQ-11）════')
p = os.path.join(W, 'fmt_label.qcow2')
run('img.create', out=p, **{'size-gb': 20, 'table': 'gpt', 'esp-mb': 200,
                            'msr-mb': 16, 'sys-mb': 16384})
WIN = 444416
ESP = 2048

d = run('fs.format', img=p, lba=WIN, fs='ntfs', quick='true', label='我的数据')
check('NTFS 中文卷标：格式化成功', d.get('ok') and data(d).get('label') == '我的数据',
      json.dumps(d)[:240])
scan = data(run('part.scan', img=p, lba=WIN))
check('读回来还是「我的数据」（UTF-16 真写进去了）', scan.get('label') == '我的数据',
      json.dumps(scan)[:200])
check('fs=NTFS 且 level>=1', scan.get('fs') == 'NTFS' and (scan.get('level') or 0) >= 1,
      json.dumps(scan)[:160])

d = run('fs.format', img=p, lba=ESP, fs='fat32', quick='true', label='启动盘')
e = d.get('err') or {}
check('FAT32 中文卷标 → CMD-ARG-BAD@label（不再谎报成功）',
      e.get('error_code') == 'CMD-ARG-BAD' and e.get('at') == 'label', json.dumps(d)[:240])
check('错误信封带 hint（App 没有文案时也能给人话）', bool(e.get('hint')),
      json.dumps(e)[:200])
scan = data(run('part.scan', img=p, lba=ESP))
check('被拒之后盘上是干净空分区（不许 unknown/-1）',
      scan.get('fs') != 'unknown' and (scan.get('level') or 0) >= 0, json.dumps(scan)[:160])

d = run('fs.format', img=p, lba=WIN, fs='ntfs', label='x' * 40)
check('NTFS 卷标 >32 → CMD-ARG-BAD@label',
      (d.get('err') or {}).get('at') == 'label', json.dumps(d)[:200])
d = run('fs.format', img=p, lba=ESP, fs='fat32', label='123456789012')
check('FAT32 卷标 >11 → CMD-ARG-BAD@label',
      (d.get('err') or {}).get('at') == 'label', json.dumps(d)[:200])

d = run('fs.format', img=p, lba=ESP, fs='fat32', quick='true', label='boot')
check('FAT32 小写 ASCII → 转大写 BOOT',
      d.get('ok') and data(d).get('label') == 'BOOT', json.dumps(d)[:200])
check('part.scan 读到 BOOT', data(run('part.scan', img=p, lba=ESP)).get('label') == 'BOOT')

d = run('fs.format', img=p, lba=ESP, fs='fat32', quick='true')
check('不传 label 也能格式化', d.get('ok'), json.dumps(d)[:160])

print()
print('════ 结果：通过 %d · 失败 %d ════' % (ok, bad))
print('调试目录：%s' % W)
sys.exit(0 if bad == 0 else 1)