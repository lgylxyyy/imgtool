#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""info-alloc-test —— img.info 的「容量 / 占用」口径（T5）

用户看到的「容量 8 GiB · 占用 200 KB」和「这个盘是不是稀疏」全靠这几个字段：

  logical_bytes  虚拟容量（guest 看到的磁盘大小）
  alloc_bytes    宿主【实际占的块】×512（st_blocks），不是 st_size
  alloc_known    上面这个数是否可信（Android /sdcard 走 FUSE，有已知不可靠案例）
  granule_bytes  分配粒度：qcow2 簇 / VHD 2 MiB 块 / VMDK grain / raw 宿主块
  sparse_ratio   省下来的百分比整数（0..100）
  hole_bytes     容量 − 占用

断言：
  · 四种格式空盘的 logical/granule/ratio
  · 写 100 MiB 后 alloc_bytes ≈ 100 MiB（四格式同量级，说明与格式无关）
  · alloc_bytes 与宿主 du 的口径一致（差 < 5%）
  · 64 GiB 稀疏 raw：写 1 MiB → alloc ≈ 1 MiB、ratio=100、alloc_known=true
"""
import ctypes
import json
import os
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
IMGCMD = os.path.join(ROOT, 'build', 'tools', 'imgcmd')
LIB = os.path.join(ROOT, 'build', 'libimgtool.so')

W = tempfile.mkdtemp(prefix='infoalloc', dir=os.environ.get('IMG_TEST_TMP') or None)
ok = bad = 0


def run(*args):
    p = subprocess.run(args, capture_output=True, text=True)
    return p.returncode, p.stdout, p.stderr


def cmd(*args):
    rc, out, err = run(IMGCMD, *args)
    if rc != 0:
        raise RuntimeError('%s → rc=%d %s %s' % (args, rc, out[-200:], err[-200:]))
    return json.loads(out.splitlines()[-1])['data']


def check(name, cond, extra=''):
    global ok, bad
    if cond:
        ok += 1
        print('  ✅ %s' % name)
    else:
        bad += 1
        print('  ❌ %s %s' % (name, extra))


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
    assert io
    q = L.img_qcow2_open(io, None)
    assert q
    try:
        assert L.img_qcow2_write_init(q) == 0
        rc = L.img_qcow2_write(q, off, payload, len(payload))
        assert rc == 0, rc
    finally:
        L.img_qcow2_close(q)
        L.img_io_close(io)


def du_bytes(path):
    rc, out, _ = run('du', '-B1', path)
    if rc != 0:
        return None
    return int(out.split()[0])


def main():
    if not os.path.exists(LIB):
        print('SKIP: libimgtool.so 未构建')
        return 77
    payload = os.urandom(100 << 20)

    print('════ 1. 四种格式空盘 8 GiB ════')
    gran = {'raw': 4096, 'vhd': 2 << 20, 'vmdk': 64 << 10, 'qcow2': 64 << 10}
    for f in ('raw', 'vhd', 'vmdk', 'qcow2'):
        p = os.path.join(W, 'e.' + f)
        cmd('img.create', '--out=' + p, '--size-gb=8', '--format=' + f,
            '--table=none', '--limits=off')
        d = cmd('img.info', '--img=' + p)
        check('%s：logical_bytes ≈ 8 GiB（%d）' % (f, d['logical_bytes']),
              8 << 30 <= d['logical_bytes'] <= (8 << 30) + (2 << 20))
        check('%s：alloc_known=true' % f, d['alloc_known'] is True)
        check('%s：granule_bytes=%d' % (f, gran[f]), d['granule_bytes'] == gran[f],
              d['granule_bytes'])
        check('%s：空盘 sparse_ratio≥99（%d）' % (f, d['sparse_ratio']), d['sparse_ratio'] >= 99)
        check('%s：hole_bytes = 容量 − 占用' % f,
              d['hole_bytes'] == max(0, d['logical_bytes'] - d['alloc_bytes']))
        check('%s：alloc_bytes 与 du 一致（%d vs %d）' % (f, d['alloc_bytes'], du_bytes(p)),
              abs(d['alloc_bytes'] - du_bytes(p)) <= 4096)

    print('════ 2. 写 100 MiB 后：四格式占用同量级 ════')
    for f in ('raw', 'vhd', 'vmdk', 'qcow2'):
        p = os.path.join(W, 'e.' + f)
        blk_write(p, 1 << 20, payload)
        d = cmd('img.info', '--img=' + p)
        lo, hi = 100 << 20, 135 << 20
        check('%s：alloc_bytes 在 [100,135] MiB（%d）' % (f, d['alloc_bytes'] // (1 << 20)),
              lo <= d['alloc_bytes'] <= hi, d['alloc_bytes'])
        check('%s：alloc 与 du 一致' % f, abs(d['alloc_bytes'] - du_bytes(p)) <= 65536,
              '%d vs %d' % (d['alloc_bytes'], du_bytes(p)))
        check('%s：sparse_ratio 仍是 98 上下（%d）' % (f, d['sparse_ratio']),
              95 <= d['sparse_ratio'] <= 99)

    print('════ 3. 64 GiB 稀疏 raw（真稀疏，不是格式模拟）════')
    r = os.path.join(W, 'big.raw')
    with open(r, 'wb') as f:
        f.truncate(64 << 30)
    blk_write(r, 1000 << 20, os.urandom(1 << 20))
    d = cmd('img.info', '--img=' + r)
    check('64 GiB：logical_bytes=68719476736', d['logical_bytes'] == 68719476736)
    check('64 GiB：alloc ≈ 1 MiB（%d KiB）' % (d['alloc_bytes'] // 1024),
          1 << 20 <= d['alloc_bytes'] <= (3 << 20))
    check('64 GiB：alloc_known=true', d['alloc_known'] is True)
    check('64 GiB：sparse_ratio=100（%d）' % d['sparse_ratio'], d['sparse_ratio'] == 100)
    check('64 GiB：allloc 与 du 一致', abs(d['alloc_bytes'] - du_bytes(r)) <= 65536,
          '%d vs %d' % (d['alloc_bytes'], du_bytes(r)))

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