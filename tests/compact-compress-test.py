#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""compact-compress-test —— img.compact --compress=zlib（T6）

"压缩映像"卡片背后的能力。三条硬指标，缺一条这个功能就不能发：

  ① 压得动：可压缩数据（重复文本）产物要显著变小；
  ② 打得开：`qemu-img check` 零错误、`qemu-img convert` 能还原、内容 md5 一致；
  ③ 不胡来：压不动的随机数据不硬压（clusters_compressed 接近 0、体积不明显变大），
     未知算法（gzip 等）明确拒绝。
  ④ zstd（P2）：产物是 qcow2 v3 + incompatible「compression type」位 ——
     qemu-img 能打开能解、我们自己也能读、内容逐字节一致；qemu 亲自解压
     （convert 成 raw）再由我们导出比对，两边解出的数据必须一致。

兼容性口径：压缩簇用裸 deflate（无 zlib 头、无长度前缀），L2 的 bit62 置位、
**bit63(COPIED) 绝不能置**（qemu check 原话："copied flag must never be set for
compressed clusters"）。
"""
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
IMGCMD = os.path.join(ROOT, 'build', 'tools', 'imgcmd')
W = tempfile.mkdtemp(prefix='compactzip', dir=os.environ.get('IMG_TEST_TMP') or None)
ok = bad = 0


def run(*args):
    p = subprocess.run(args, capture_output=True, text=True)
    return p.returncode, p.stdout, p.stderr


def cmd(*args):
    rc, out, err = run(IMGCMD, *args)
    lines = [l for l in out.splitlines() if l.startswith('{')]
    if not lines:
        raise RuntimeError('%s → rc=%d %s %s' % (args, rc, out[-300:], err[-300:]))
    return json.loads(lines[-1])


def check(name, cond, extra=''):
    global ok, bad
    if cond:
        ok += 1
        print('  ✅ %s' % name)
    else:
        bad += 1
        print('  ❌ %s %s' % (name, extra))


def md5file(path):
    h = hashlib.md5()
    with open(path, 'rb') as f:
        for b in iter(lambda: f.read(1 << 20), b''):
            h.update(b)
    return h.hexdigest()


def make_disk(name, fmt, payload):
    d = os.path.join(W, name)
    cmd('img.create', '--out=' + d, '--size-gb=2', '--format=' + fmt,
        '--table=gpt', '--sys-mb=256', '--rec-mb=0', '--limits=off')
    pt = cmd('part.show', '--img=' + d)['data']['pt']
    lba = sorted(pt['parts'], key=lambda p: p['sectors'])[-1]['first']
    cmd('fs.format', '--img=' + d, '--lba=%d' % lba, '--fs=fat32')
    src = os.path.join(W, 'payload.bin')
    with open(src, 'wb') as f:
        f.write(payload)
    cmd('fat.put', '--img=' + d, '--lba=%d' % lba, '--src=' + src, '--dst=/p.bin')
    return d, lba


def qemu_check(path):
    rc, out, err = run('qemu-img', 'check', path)
    no_err = 'No errors were found' in out
    return no_err, (out + err).strip().splitlines()[:3]


def main():
    if not shutil.which('qemu-img'):
        print('SKIP: qemu-img unavailable')
        return 77

    repeat = (b'ABCDEFGH' * 3750000)          # 30 MB 可压缩
    print('════ 1. qcow2 源 + 可压缩数据 ════')
    src, lba = make_disk('s.qcow2', 'qcow2', repeat)
    payload_md5 = md5file(os.path.join(W, 'payload.bin'))   # ★ make_disk 会覆写 payload.bin（第2节换随机数据），期望值必须在这里固定
    raw_size = os.path.getsize(src)
    out = os.path.join(W, 'c.qcow2')
    r = cmd('img.compact', '--src=' + src, '--out=' + out, '--compress=zlib')
    check('compact 成功', r['ok'], json.dumps(r, ensure_ascii=False)[:240])
    d = r['data']
    check('clusters_compressed > 0（%d）' % d['clusters_compressed'], d['clusters_compressed'] > 0)
    check('产物显著变小（%d → %d）' % (raw_size, d['bytes_after']),
          d['bytes_after'] < raw_size // 4, '%d vs %d' % (raw_size, d['bytes_after']))
    check('verified=true', d['verified'] is True)
    good, lines = qemu_check(out)
    check('qemu-img check 零错误', good, ' | '.join(lines))

    exp = os.path.join(W, 'exp.bin')
    cmd('fat.export', '--img=' + out, '--lba=%d' % lba, '--path=/p.bin', '--dst=' + exp)
    check('内容逐字节一致', md5file(exp) == payload_md5)

    print('════ 2. 压不动的数据不硬压 ════')
    rnd = os.urandom(8 << 20)
    src2, lba2 = make_disk('r.qcow2', 'qcow2', rnd)
    out2 = os.path.join(W, 'r2.qcow2')
    r = cmd('img.compact', '--src=' + src2, '--out=' + out2, '--compress=zlib')
    d2 = r['data']
    check('随机数据：compact 成功', r['ok'])
    check('随机数据：压缩簇很少（%d）' % d2['clusters_compressed'], d2['clusters_compressed'] < 8)
    check('随机数据：体积没有变大（%d → %d）' % (os.path.getsize(src2), d2['bytes_after']),
          d2['bytes_after'] <= os.path.getsize(src2) + (512 << 10))
    good, lines = qemu_check(out2)
    check('随机数据：qemu-img check 零错误', good, ' | '.join(lines))

    print('════ 3. --compress=off 仍是紧凑化 ════')
    out3 = os.path.join(W, 'c-off.qcow2')
    r = cmd('img.compact', '--src=' + src, '--out=' + out3, '--compress=off')
    check('off：compressed=false', r['data']['compressed'] is False)
    check('off：没有压缩簇', r['data']['clusters_compressed'] == 0)
    check('off：体积大于 zlib 版（%d vs %d）' % (r['data']['bytes_after'], d['bytes_after']),
          r['data']['bytes_after'] > d['bytes_after'])

    print('════ 4. zstd 压缩（qcow2 v3 + compression type 位）════')
    outz = os.path.join(W, 'z.qcow2')
    r = cmd('img.compact', '--src=' + src, '--out=' + outz, '--compress=zstd')
    check('zstd：compact 成功', r['ok'], json.dumps(r.get('err'), ensure_ascii=False)[:200])
    dz = r['data']
    check('zstd：clusters_compressed > 0（%d）' % dz['clusters_compressed'],
          dz['clusters_compressed'] > 0)
    check('zstd：compression_type=zstd', dz.get('compression_type') == 'zstd')
    check('zstd：结果带兼容性提示', 'zstd' in (dz.get('compat_note') or ''))
    check('zstd：产物显著变小（%d → %d）' % (raw_size, dz['bytes_after']),
          dz['bytes_after'] < raw_size // 4)
    good, lines = qemu_check(outz)
    check('zstd：qemu-img check 零错误', good, ' | '.join(lines))
    rz = run('qemu-img', 'convert', '-O', 'raw', outz, os.path.join(W, 'z.raw'))
    check('zstd：qemu 亲自解压（convert）成功', rz[0] == 0, (rz[1] + rz[2])[:160])
    expq = os.path.join(W, 'expq.bin')
    cmd('fat.export', '--img=' + os.path.join(W, 'z.raw'), '--lba=%d' % lba,
        '--path=/p.bin', '--dst=' + expq)
    check('zstd：qemu 解压的内容与原始一致', md5file(expq) == payload_md5)
    expz = os.path.join(W, 'expz.bin')
    cmd('fat.export', '--img=' + outz, '--lba=%d' % lba, '--path=/p.bin', '--dst=' + expz)
    check('zstd：我们读的内容与原始一致', md5file(expz) == payload_md5)
    os.remove(os.path.join(W, 'z.raw'))

    print('════ 4b. 未知压缩算法仍明确拒绝 ════')
    r = cmd('img.compact', '--src=' + src, '--out=' + os.path.join(W, 'zz.qcow2'),
            '--compress=gzip')
    check('未知算法被拒（CMD-ARG-BAD）',
          (not r['ok']) and r['err']['error_code'] == 'CMD-ARG-BAD',
          json.dumps(r.get('err'), ensure_ascii=False)[:160])

    print('════ 5. raw 源也能压缩 ════')
    srcr, lbar = make_disk('s.raw', 'raw', repeat)
    outr = os.path.join(W, 'cr.qcow2')
    r = cmd('img.compact', '--src=' + srcr, '--out=' + outr, '--compress=zlib')
    check('raw 源：compact 成功', r['ok'], json.dumps(r, ensure_ascii=False)[:240])
    check('raw 源：有压缩簇（%d）' % r['data']['clusters_compressed'],
          r['data']['clusters_compressed'] > 0)
    check('raw 源：产物远小于 2 GiB（%d）' % r['data']['bytes_after'],
          r['data']['bytes_after'] < (16 << 20))
    good, lines = qemu_check(outr)
    check('raw 源：qemu-img check 零错误', good, ' | '.join(lines))
    exp2 = os.path.join(W, 'exp2.bin')
    cmd('fat.export', '--img=' + outr, '--lba=%d' % lbar, '--path=/p.bin', '--dst=' + exp2)
    check('raw 源：内容一致', md5file(exp2) == payload_md5)

    print()
    print('════ 通过 %d · 失败 %d ════' % (ok, bad))
    if bad:
        print('临时目录：' + W)
        return 1
    shutil.rmtree(W, ignore_errors=True)
    return 0


if __name__ == '__main__':
    sys.exit(main())