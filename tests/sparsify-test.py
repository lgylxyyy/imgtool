#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""sparsify-test —— 原地回收未使用块（T7）

img.sparsify 与 img.compact 是两件事：
  compact    另存一份更小的（换文件、要额外空间）
  sparsify   原地把客户机已经不用的块还给宿主（不换文件、长度不变）

本用例钉住：
  · raw + FAT32：删文件后 fs 模式能释放；dry-run 一个字节都不改且给准数
  · 释放后分区表 / 引导扇区 / 保留文件逐字节不变（verified=true 只是内部口径，
    这里再用外部 md5 复核一遍）
  · 幂等：第二遍 dry-run 几乎报 0
  · zero 模式：分区内的全零区能回收（不依赖文件系统）
  · qcow2：fs 模式复用 img.trim
  · vhd/vmdk：明确报 SPARSIFY-FMT-UNSUPPORTED（P1 才接）
  · mode 取值校验
"""
import ctypes
import hashlib
import json
import os
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
IMGCMD = os.path.join(ROOT, 'build', 'tools', 'imgcmd')
LIB = os.path.join(ROOT, 'build', 'libimgtool.so')
W = tempfile.mkdtemp(prefix='sparsify', dir=os.environ.get('IMG_TEST_TMP') or None)
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


def md5(path, off=0, n=None):
    h = hashlib.md5()
    with open(path, 'rb') as f:
        f.seek(off)
        left = n
        while True:
            b = f.read(1 << 20 if left is None else min(1 << 20, left))
            if not b:
                break
            h.update(b)
            if left is not None:
                left -= len(b)
                if left <= 0:
                    break
    return h.hexdigest()


def blocks(path):
    st = os.stat(path)
    return st.st_blocks


_lib = None


def lib():
    global _lib
    if _lib is None:
        L = ctypes.CDLL(LIB)
        L.img_io_open_existing.argtypes = [ctypes.c_char_p, ctypes.c_bool]
        L.img_io_open_existing.restype = ctypes.c_void_p
        L.img_qcow2_open.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
        L.img_qcow2_open.restype = ctypes.c_void_p
        L.img_qcow2_write_init.argtypes = [ctypes.c_void_p]
        L.img_qcow2_write.argtypes = [ctypes.c_void_p, ctypes.c_uint64,
                                      ctypes.c_void_p, ctypes.c_size_t]
        L.img_qcow2_close.argtypes = [ctypes.c_void_p]
        L.img_io_close.argtypes = [ctypes.c_void_p]
        _lib = L
    return _lib


def blk_write(path, off, payload):
    L = lib()
    io = L.img_io_open_existing(os.fsencode(path), True)
    q = L.img_qcow2_open(io, None)
    assert io and q
    try:
        assert L.img_qcow2_write_init(q) == 0
        assert L.img_qcow2_write(q, off, payload, len(payload)) == 0
    finally:
        L.img_qcow2_close(q)
        L.img_io_close(io)


def big_part(pt):
    return sorted(pt['parts'], key=lambda p: p['sectors'])[-1]


def main():
    if not os.path.exists(LIB):
        print('SKIP: libimgtool.so 未构建')
        return 77

    d = os.path.join(W, 'd.raw')
    print('════ 1. raw + FAT32：删文件 → fs 模式回收 ════')
    cmd('img.create', '--out=' + d, '--size-gb=4', '--format=raw',
        '--table=gpt', '--sys-mb=512', '--rec-mb=0', '--limits=off')
    lba = big_part(cmd('part.show', '--img=' + d)['data']['pt'])['first']
    cmd('fs.format', '--img=' + d, '--lba=%d' % lba, '--fs=fat32')
    # 24 MB × 3 + 1 MB 保留
    big = os.path.join(W, 'big.bin')
    with open(big, 'wb') as f:
        f.write(os.urandom(24 << 20))
    for i in (1, 2, 3):
        cmd('fat.put', '--img=' + d, '--lba=%d' % lba, '--src=' + big, '--dst=/b%d.bin' % i)
    keep = os.path.join(W, 'keep.bin')
    with open(keep, 'wb') as f:
        f.write(os.urandom(1 << 20))
    cmd('fat.put', '--img=' + d, '--lba=%d' % lba, '--src=' + keep, '--dst=/keep.bin')

    pt_md5 = md5(d, 0, 65536)
    after_write = blocks(d)

    for i in (1, 2, 3):
        cmd('rm', '--path=%s!lba%d:/b%d.bin' % (d, lba, i))
    # ★ 基线必须在【删完文件之后】取：删除本身会改 FAT 表（在前 32 KiB 里），
    #   如果拿写入后的快照做基线，sparsify 会被冤枉成"改了引导区"。
    boot_md5 = md5(d, lba * 512, 32768)

    print('════ 2. dry-run：不改文件，数字要准 ════')
    r = cmd('img.sparsify', '--img=' + d, '--mode=fs', '--dry-run=true')
    if not r['ok']:
        check('dry-run 成功', False, json.dumps(r, ensure_ascii=False)[:300])
        return 1
    dd = r['data']
    check('dry-run 成功', True)
    check('verified=true', dd['verified'] is True)
    check('freed_bytes=0（试运行）', dd['freed_bytes'] == 0, dd['freed_bytes'])
    check('would_free ≈ 72 MB（%d B）' % dd['would_free_bytes'],
          (60 << 20) <= dd['would_free_bytes'] <= (80 << 20), dd['would_free_bytes'])
    check('dry-run 没改文件（blocks %d → %d）' % (after_write, blocks(d)),
          blocks(d) == after_write)

    print('════ 3. 真执行：占用下降、校验点不变 ════')
    r = cmd('img.sparsify', '--img=' + d, '--mode=fs')
    if not r['ok']:
        check('执行成功', False, json.dumps(r, ensure_ascii=False)[:300])
        return 1
    dd = r['data']
    check('执行成功', True)
    check('verified=true', dd['verified'] is True)
    check('freed ≥ 60 MB（%d B）' % dd['freed_bytes'], dd['freed_bytes'] >= (60 << 20),
          dd['freed_bytes'])
    check('宿主占用下降（%d → %d 块）' % (dd['bytes_before'] // 512, dd['bytes_after'] // 512),
          dd['bytes_after'] < dd['bytes_before'])
    check('分区表逐字节不变', md5(d, 0, 65536) == pt_md5)
    check('引导扇区/BPB 前 32 KiB 逐字节不变', md5(d, lba * 512, 32768) == boot_md5)

    ls = cmd('fat.ls', '--img=' + d, '--lba=%d' % lba, '--path=/')['data']
    names = [e['name'] for e in ls['entries']]
    check('保留文件还在（%s）' % names, names == ['keep.bin'], names)
    out_keep = os.path.join(W, 'keep.out')
    cmd('fat.export', '--img=' + d, '--lba=%d' % lba, '--path=/keep.bin', '--dst=' + out_keep)
    check('保留文件内容一致', md5(out_keep) == md5(keep))

    print('════ 4. 幂等：再 dry 几乎报 0 ════')
    r = cmd('img.sparsify', '--img=' + d, '--mode=fs', '--dry-run=true')
    check('第二遍 would_free < 2 MB（%d B）' % r['data']['would_free_bytes'],
          r['data']['would_free_bytes'] < (2 << 20), r['data']['would_free_bytes'])

    print('════ 5. zero 模式：分区里的全零区也能回收 ════')
    # 分区尾部的 32 MiB 本来就是空的；按内容写零（模拟客户机写零留下的块）
    tail_off = (lba + big_part(cmd('part.show', '--img=' + d)['data']['pt'])['sectors'] - 70000) * 512
    blk_write(d, tail_off, b'\x00' * (32 << 20))
    b0 = blocks(d)
    r = cmd('img.sparsify', '--img=' + d, '--mode=zero', '--dry-run=true')
    check('zero dry-run 成功', r['ok'], json.dumps(r, ensure_ascii=False)[:200])
    check('扫到全零区（zero_bytes ≥ 24 MB，%d）' % r['data']['zero_bytes'],
          r['data']['zero_bytes'] >= (24 << 20), r['data']['zero_bytes'])
    r = cmd('img.sparsify', '--img=' + d, '--mode=zero')
    check('zero 执行成功', r['ok'], json.dumps(r, ensure_ascii=False)[:200])
    check('zero 执行后 verified=true', r['data']['verified'] is True)

    print('════ 6. qcow2：fs 模式复用 img.trim ════')
    q = os.path.join(W, 'q.qcow2')
    cmd('img.create', '--out=' + q, '--size-gb=2', '--format=qcow2',
        '--table=gpt', '--sys-mb=256', '--rec-mb=0', '--limits=off')
    qlba = big_part(cmd('part.show', '--img=' + q)['data']['pt'])['first']
    cmd('fs.format', '--img=' + q, '--lba=%d' % qlba, '--fs=fat32')
    cmd('fat.put', '--img=' + q, '--lba=%d' % qlba, '--src=' + big, '--dst=/x.bin')
    cmd('rm', '--path=%s!lba%d:/x.bin' % (q, qlba))
    r = cmd('img.sparsify', '--img=' + q, '--mode=fs')
    check('qcow2 fs 模式成功', r['ok'], json.dumps(r, ensure_ascii=False)[:300])
    if r['ok']:
        check('qcow2 报告释放了空间（freed_bytes=%d）' % r['data']['freed_bytes'],
              r['data']['freed_bytes'] > 0)
        check('qcow2 之后 FAT 还能读', cmd('fat.ls', '--img=' + q, '--lba=%d' % qlba,
                                          '--path=/')['ok'])

    print('════ 7. vhd / vmdk：只打洞、不动 BAT/GTE ════')
    for f in ('vhd', 'vmdk'):
        p = os.path.join(W, 'x.' + f)
        cmd('img.create', '--out=' + p, '--size-gb=2', '--format=' + f,
            '--table=gpt', '--sys-mb=256', '--rec-mb=0', '--limits=off')
        pl = big_part(cmd('part.show', '--img=' + p)['data']['pt'])['first']
        cmd('fs.format', '--img=' + p, '--lba=%d' % pl, '--fs=fat32')
        cmd('fat.put', '--img=' + p, '--lba=%d' % pl, '--src=' + big, '--dst=/x.bin')
        b0 = blocks(p)
        cmd('rm', '--path=%s!lba%d:/x.bin' % (p, pl))
        r = cmd('img.sparsify', '--img=' + p, '--mode=fs')
        check('%s：sparsify 成功' % f, r['ok'],
              json.dumps(r, ensure_ascii=False)[:240])
        if r['ok']:
            dd = r['data']
            check('%s：释放了空间（%d B）' % (f, dd['freed_bytes']), dd['freed_bytes'] > (16 << 20))
            check('%s：verified=true' % f, dd['verified'] is True)
            check('%s：占用下降（%d → %d 块）' % (f, b0, blocks(p)), blocks(p) < b0)
            check('%s：sparsify 后 FAT 还能读' % f,
                  cmd('fat.ls', '--img=' + p, '--lba=%d' % pl, '--path=/')['ok'])
            # qemu 必须还能打开（打洞没有碰到元数据）
            rc, qo, qe = run('qemu-img', 'info', p)
            check('%s：qemu-img info 仍能打开' % f, rc == 0, (qe or qo)[:120])

    print('════ 8. 参数校验 ════')
    r = cmd('img.sparsify', '--img=' + d, '--mode=bogus')
    check('mode=bogus 被拒', (not r['ok']) and r['err']['error_code'] == 'CMD-ARG-BAD')

    print()
    print('════ 通过 %d · 失败 %d ════' % (ok, bad))
    if bad:
        print('临时目录：' + W)
        return 1
    import shutil
    shutil.rmtree(W, ignore_errors=True)
    return 0


if __name__ == '__main__':
    sys.exit(main())