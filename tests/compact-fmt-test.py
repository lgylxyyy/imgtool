#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""compact-fmt-test —— img.compact 对四种源格式都必须真的搬数据（D2 回归）

为什么单列一个用例：
  以前 img_qcow2_any_allocated() 只特判 raw，VHD/VMDK 落到 qcow2 的 L1/L2 上查，
  它们根本没有 L1 → 恒「无分配」→ img.compact 对这两种源产出空盘：
  bytes_written=0、输出 524288 B、fat.ls 报 FAT-MOUNT-FAIL，--verify=true 时
  因抽样不一致直接删产物。表面上"成功"，实际数据全丢。

断言：
  · 四种源（raw / qcow2 / vhd / vmdk）→ 输出 qcow2
  · bytes_written > 0；--verify 默认开：verified=true
  · 打开产物：fat.ls 能列出文件，名字与内容都对
  · 源文件 md5 前后不变（compact 绝不改源）
  · 空盘（只有分区表）四种格式都能成功且 bytes_written=0
"""
import hashlib
import json
import shutil
import os
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
IMGCMD = os.path.join(ROOT, 'build', 'tools', 'imgcmd')

# 默认 /tmp 可能是 tmpfs（写多少吃多少内存）；用 IMG_TEST_TMP 指到真实盘上
W = tempfile.mkdtemp(prefix='compactfmt', dir=os.environ.get('IMG_TEST_TMP') or None)
ok = bad = 0
fails = []


def run(cmd, **kw):
    args = [IMGCMD, cmd] + ['--%s=%s' % (k, v) for k, v in kw.items()]
    p = subprocess.run(args, capture_output=True, text=True)
    js = [l for l in p.stdout.splitlines() if l.startswith('{')]
    return json.loads(js[-1]) if js else {'ok': False, 'raw': (p.stdout + p.stderr)[-400:]}


def data(d):
    return d.get('data') or {}


def check(name, cond, extra=''):
    global ok, bad
    if cond:
        ok += 1
        print('  ✅ %s' % name)
    else:
        bad += 1
        fails.append(name)
        print('  ❌ %s %s' % (name, extra))


def md5(path):
    h = hashlib.md5()
    with open(path, 'rb') as f:
        for b in iter(lambda: f.read(1 << 20), b''):
            h.update(b)
    return h.hexdigest()


def big_part(pt):
    return sorted(pt['parts'], key=lambda p: p['sectors'])[-1]


def make_disk(fmt, path, with_files=True):
    d = run('img.create', out=path, **{'size-gb': 2, 'format': fmt, 'table': 'gpt',
                                       'sys-mb': 256, 'rec-mb': 0, 'limits': 'off'})
    assert d.get('ok'), d
    lba = big_part(data(run('part.show', img=path))['pt'])['first']
    d = run('fs.format', img=path, lba=lba, fs='fat32')
    assert d.get('ok'), d
    names = []
    if with_files:
        for i in range(3):
            src = os.path.join(W, 'f%d.bin' % i)
            with open(src, 'wb') as f:
                f.write((b'compact-fmt-%d.' % i) * 40000)   # ~ 600 KB
            name = '/f%d.bin' % i
            d = run('fat.put', img=path, lba=lba, src=src, dst=name)
            assert d.get('ok'), d
            names.append(name)
    return path, lba, names


def main():
    print('════ 1. 四种源格式 compact（都有文件）════')
    for fmt in ('raw', 'qcow2', 'vhd', 'vmdk'):
        src = os.path.join(W, 'src.' + fmt)
        make_disk(fmt, src, with_files=True)
        before = md5(src)
        out = os.path.join(W, 'out-%s.qcow2' % fmt)
        d = run('img.compact', src=src, out=out)
        if not d.get('ok'):
            check('%s：compact 成功' % fmt, False, json.dumps(d, ensure_ascii=False)[:300])
            continue
        dd = data(d)
        check('%s：compact 成功' % fmt, True)
        check('%s：bytes_written > 0（%s）' % (fmt, dd.get('bytes_written')),
              (dd.get('bytes_written') or 0) > 0, json.dumps(dd)[:240])
        check('%s：--verify 通过' % fmt, dd.get('verified') is True, json.dumps(dd)[:240])
        check('%s：源文件 md5 不变' % fmt, md5(src) == before)
        check('%s：产物比源小' % fmt,
              os.path.getsize(out) < os.path.getsize(src),
              '%d vs %d' % (os.path.getsize(out), os.path.getsize(src)))
        lba = big_part(data(run('part.show', img=out))['pt'])['first']
        ls = data(run('fat.ls', img=out, lba=lba, path='/'))
        names = sorted(e['name'] for e in ls.get('entries', []))
        check('%s：产物里文件都在（%s）' % (fmt, names),
              names == ['f0.bin', 'f1.bin', 'f2.bin'], names)
        exported = os.path.join(W, 'exp-%s.bin' % fmt)
        run('fat.export', img=out, lba=lba, path='/f1.bin', dst=exported)
        good = os.path.exists(exported) and open(exported, 'rb').read() == open(
            os.path.join(W, 'f1.bin'), 'rb').read()
        check('%s：产物里文件内容一致' % fmt, good)

    print('════ 2. 空盘（只有分区表）三种格式都要成功且 0 字节 ════')
    for fmt in ('raw', 'vhd', 'vmdk'):
        src = os.path.join(W, 'empty.' + fmt)
        run('img.create', out=src, **{'size-gb': 1, 'format': fmt, 'table': 'gpt',
                                      'sys-mb': 64, 'rec-mb': 0, 'limits': 'off'})
        out = os.path.join(W, 'empty-%s.qcow2' % fmt)
        d = run('img.compact', src=src, out=out)
        check('%s 空盘：compact 成功' % fmt, d.get('ok'), json.dumps(d, ensure_ascii=False)[:240])
        if d.get('ok'):
            dd = data(d)
            check('%s 空盘：bytes_written=0（%s）' % (fmt, dd.get('bytes_written')),
                  dd.get('bytes_written') == 0, json.dumps(dd)[:240])
            check('%s 空盘：verify 通过' % fmt, dd.get('verified') is True)

    print()
    print('════ 通过 %d · 失败 %d ════' % (ok, bad))
    if bad:
        print('临时目录（保留排查）：' + W)
        return 1
    shutil.rmtree(W, ignore_errors=True)
    return 0


if __name__ == '__main__':
    sys.exit(main())