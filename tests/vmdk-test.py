#!/usr/bin/env python3
"""VMDK sparse 后端回归：建盘、写入、跨进程重开、qemu 互验。

覆盖的三个已知缺陷：
  D1a  img_qcow2_close 不调 img_fmt_close → GD 永不落盘（新进程读回全零）
  D1b  sparse 头字段偏移整体偏 4 字节 → qemu 读到 gdOffset/overHead 全是垃圾
  D1c  描述符/元数据区的结束符字符（single/non/double EndLineChar）未写

断言口径：
  - 头 6 个字段按 VMDK 规范偏移读出（24/28/32/36/44/52），值必须自洽
  - qemu-img info 的 virtual-size 等于容量；qemu 能 convert 出非零数据
  - 写入后**换一个进程**再读：数据与 GPT 都在（验证 GD 落盘）
"""
import ctypes
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..'))
TOOL = os.path.join(ROOT, 'build/tools/imgcmd')
LIB = os.path.join(ROOT, 'build/libimgtool.so')

fails = []


def ok(msg):
    print('  ✅ ' + msg)


def ng(msg):
    print('  ❌ ' + msg)
    fails.append(msg)


def run(*args):
    p = subprocess.run(args, capture_output=True, text=True)
    return p.returncode, p.stdout, p.stderr


def cmd(*args):
    rc, out, err = run(TOOL, *args)
    if rc != 0:
        raise RuntimeError('imgcmd %s → rc=%d %s %s' % (args, rc, out, err))
    return json.loads(out)


_lib = None


def lib():
    """块设备读写走公开 C API（没有 img.write 命令行；这也是 App 用的路径）。"""
    global _lib
    if _lib is None:
        L = ctypes.CDLL(LIB)
        L.img_io_open_existing.argtypes = [ctypes.c_char_p, ctypes.c_bool]
        L.img_io_open_existing.restype = ctypes.c_void_p
        L.img_qcow2_open.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
        L.img_qcow2_open.restype = ctypes.c_void_p
        L.img_qcow2_write.argtypes = [ctypes.c_void_p, ctypes.c_uint64, ctypes.c_void_p, ctypes.c_size_t]
        L.img_qcow2_write.restype = ctypes.c_int
        L.img_qcow2_read.argtypes = [ctypes.c_void_p, ctypes.c_uint64, ctypes.c_void_p, ctypes.c_size_t]
        L.img_qcow2_read.restype = ctypes.c_int
        L.img_qcow2_close.argtypes = [ctypes.c_void_p]
        L.img_qcow2_write_init.argtypes = [ctypes.c_void_p]
        L.img_qcow2_write_init.restype = ctypes.c_int
        L.img_io_close.argtypes = [ctypes.c_void_p]
        _lib = L
    return _lib


def blk_write(path, off, payload):
    L = lib()
    io = L.img_io_open_existing(os.fsencode(path), True)
    assert io, 'open io %s' % path
    q = L.img_qcow2_open(io, None)
    assert q, 'open image %s' % path
    try:
        # qcow2 必须在 open 之后显式进写模式；raw/vhd/vmdk 不需要（返回 0 也不伤）
        rc = L.img_qcow2_write_init(q)
        assert rc == 0, 'write_init rc=%d' % rc
        rc = L.img_qcow2_write(q, off, payload, len(payload))
        assert rc == 0, 'write rc=%d' % rc
    finally:
        L.img_qcow2_close(q)          # ★ 这一句负责把 GD 落盘
        L.img_io_close(io)


def blk_read(path, off, n):
    L = lib()
    io = L.img_io_open_existing(os.fsencode(path), False)
    assert io
    q = L.img_qcow2_open(io, None)
    assert q, 'open image (read) %s' % path
    buf = ctypes.create_string_buffer(n)
    try:
        rc = L.img_qcow2_read(q, off, buf, n)
        assert rc == 0, 'read rc=%d' % rc
        return buf.raw
    finally:
        L.img_qcow2_close(q)
        L.img_io_close(io)


def header_fields(path):
    with open(path, 'rb') as f:
        h = f.read(512)
    assert h[:4] == b'KDMV', path
    version, flags = struct.unpack_from('<II', h, 4)
    capacity = struct.unpack_from('<Q', h, 12)[0]
    grain = struct.unpack_from('<I', h, 20)[0]
    d_off, d_size = struct.unpack_from('<QQ', h, 28)
    gtes = struct.unpack_from('<I', h, 44)[0]
    rgd, gd, over = struct.unpack_from('<QQQ', h, 48)
    return dict(version=version, flags=flags, capacity=capacity, grain=grain,
                d_off=d_off, d_size=d_size, gtes=gtes, rgd=rgd, gd=gd,
                over=over, tail=h[72:77])


def static_header(path, sectors):
    """偏移照 qemu-img 造的 vmdk 逐字段实测：20/28/36/44/48/56/64/72。"""
    h = header_fields(path)
    exact = (h['d_off'] == 1 and h['d_size'] >= 1 and h['gtes'] in (128, 512)
             and h['rgd'] > h['d_off'] + h['d_size'] - 1
             and h['gd'] > h['rgd'] and h['over'] > h['gd'])
    if exact:
        ok('头字段按实测偏移可读（desc@28=%d descSize@36=%d gtes@44=%d gd@56=%d over@64=%d）'
           % (h['d_off'], h['d_size'], h['gtes'], h['gd'], h['over']))
    else:
        ng('头字段偏移不符：%s' % h)
    if h['tail'] == b'\x00\n \r\n':
        ok('结束符字符（unclean=0 / LF / 空格 / CRLF）就位')
    else:
        ng('结束符字符错误：%r' % h['tail'])
    if h['capacity'] == sectors:
        ok('capacity=%d 扇区（= 请求容量）' % sectors)
    else:
        ng('capacity=%d，应为 %d' % (h['capacity'], sectors))
    if h['grain'] == 128:
        ok('granularity=128 扇区（64 KiB，qemu 接受）')
    else:
        ng('granularity=%d，应为 128' % h['grain'])
    return h


def qemu_info(path):
    rc, out, err = run('qemu-img', 'info', '-f', 'vmdk', '--output=json', path)
    if rc != 0:
        return None, err
    return json.loads(out), None


def qemu_roundtrip(path, expect_size, tag):
    info, err = qemu_info(path)
    if info is None:
        ng('%s：qemu-img info 失败 %s' % (tag, err.strip()[:120]))
        return None
    if info['virtual-size'] == expect_size:
        ok('%s：qemu-img 报 virtual-size=%d' % (tag, expect_size))
    else:
        ng('%s：qemu-img 报 virtual-size=%d，应为 %d' % (tag, info['virtual-size'], expect_size))
    raw = path + '.qemuraw'
    rc, out, err = run('qemu-img', 'convert', '-f', 'vmdk', '-O', 'raw', path, raw)
    if rc != 0 or not os.path.exists(raw):
        ng('%s：qemu-img convert 失败 %s' % (tag, err.strip()[:120]))
        return None
    return {'raw': raw}


def main():
    if not shutil.which('qemu-img'):
        print('SKIP: qemu-img unavailable')
        return 0
    if not os.path.exists(LIB):
        print('SKIP: libimgtool.so 未构建')
        return 77
    payload = b'VMDK-REOPEN-MARK' + bytes(range(1, 256)) + b'!' * (512 - 16 - 255)

    with tempfile.TemporaryDirectory(prefix='vmdk-test-') as d:
        # ── ① 工具自建 GPT VMDK：头、分区表、qemu 三方一致 ──
        p = os.path.join(d, 'g.vmdk')
        cmd('img.create', '--out=' + p, '--size-gb=2', '--format=vmdk',
            '--table=gpt', '--sys-mb=64', '--rec-mb=0', '--limits=off')
        static_header(p, (2 << 30) // 512)
        pt = cmd('part.show', '--img=' + p)['data']['pt']
        if pt['table'] == 'GPT' and pt['parts']:
            ok('自建 vmdk：part.show 认出 GPT（%d 个分区）' % len(pt['parts']))
        else:
            ng('自建 vmdk：part.show 认不出 GPT（%s）' % pt)

        r = qemu_roundtrip(p, 2 << 30, '自建 GPT vmdk')
        if r:
            with open(r['raw'], 'rb') as f:
                f.seek(512)
                gpt = f.read(8)
            if gpt == b'EFI PART':
                ok('自建 vmdk：qemu 还原后 GPT 头就位')
            else:
                ng('自建 vmdk：qemu 还原后 GPT 头是 %r' % gpt)
        # 写 8 MiB 数据后，qemu 必须仍能打开，且宿主占用真的长起来
        # （以前 GD 不落盘 → 文件长但数据区是洞里出来的 0，qemu 只看到 60 KiB）
        before_st = os.stat(p)
        blk_write(p, 1 << 20, b'\xa5' * (8 << 20))
        after_st = os.stat(p)
        grew = after_st.st_blocks - before_st.st_blocks
        if grew * 512 >= (7 << 20):
            ok('写入 8 MiB 后宿主块增长 %d KiB（数据真的落盘）' % (grew * 512 // 1024))
        else:
            ng('写入 8 MiB 后宿主块只增长 %d KiB' % (grew * 512 // 1024))
        if qemu_info(p)[0] is None:
            ng('写入后 qemu 打不开')
        else:
            ok('写入后 qemu 仍能打开（GD 自洽）')

        # ── ② 写入后换进程重开：GD 必须落盘 ──
        offs = (2048 * 3 * 512, (2 << 30) - 4096 * 512, 1 << 20)
        for o in offs:
            blk_write(p, o, payload)
        allok = True
        for o in offs:
            if blk_read(p, o, 512) != payload:
                allok = False
                ng('新进程读回偏移 %d 不一致' % o)
        if allok:
            ok('三处写入（低区 / 2 GiB 末尾 / 中区）新进程全部读回一致 → GD 已落盘')

        r = qemu_roundtrip(p, 2 << 30, '写入后 vmdk')
        if r:
            with open(r['raw'], 'rb') as f:
                good = True
                for o in offs:
                    f.seek(o)
                    if f.read(512) != payload:
                        good = False
                if good:
                    ok('写入后 vmdk：qemu 读到同样的三处数据')
                else:
                    ng('写入后 vmdk：qemu 读到的数据不一致')

        # ── ③ close 改为调 fmt_close 后，qcow2 / raw 不受影响 ──
        q1 = os.path.join(d, 'x.qcow2')
        cmd('img.create', '--out=' + q1, '--size-gb=1', '--format=qcow2',
            '--table=mbr', '--limits=off')
        blk_write(q1, 512, payload)
        if blk_read(q1, 512, 512) == payload:
            ok('qcow2 回归：close 改动后读写正常')
        else:
            ng('qcow2 回归：close 改动影响了 qcow2 读写')
        rw = os.path.join(d, 'x.raw')
        with open(rw, 'wb') as f:
            f.truncate(1 << 20)
        blk_write(rw, 4096, payload)
        if blk_read(rw, 4096, 512) == payload:
            ok('raw 回归：close 改动后读写正常')
        else:
            ng('raw 回归：close 改动影响了 raw 读写')

        # ── ④ 与 qemu 造的 vmdk 互操作（我们读 qemu 的盘，qemu 读我们的写入）──
        qv = os.path.join(d, 'qemu.vmdk')
        rc, _, err = run('qemu-img', 'create', '-f', 'vmdk', qv, '3G')
        if rc == 0:
            cmd('part.layout', '--img=' + qv, '--sys-gb=1', '--rec-gb=0')
            pt = cmd('part.show', '--img=' + qv)['data']['pt']
            if pt['table'] == 'MBR':
                ok('qemu 造的 vmdk：我们能读出分区表')
            else:
                ng('qemu 造的 vmdk：分区表读不出')
            blk_write(qv, 4096 * 512, payload)
            r = qemu_roundtrip(qv, 3 << 30, 'qemu 造的 vmdk')
            if r:
                with open(r['raw'], 'rb') as f:
                    f.seek(4096 * 512)
                    if f.read(512) == payload:
                        ok('qemu 造的 vmdk：我们写入的数据 qemu 能读到')
                    else:
                        ng('qemu 造的 vmdk：我们写的数据 qemu 读不到')

        # ── ⑤ 头部不能有脏字符：描述符区必须可被 qemu 解析 ──
        rc, out, err = run('qemu-img', 'dd', '-f', 'vmdk', 'if=' + p, 'of=' + os.path.join(d, 'one.raw'),
                           'bs=512', 'count=1')
        if rc == 0:
            ok('qemu-img dd 能打开并读首扇区（描述符/元数据自洽）')
        else:
            ng('qemu-img dd 打不开：%s' % err.strip()[:160])
    print()
    if fails:
        print('════ 失败 %d ════' % len(fails))
        return 1
    print('════ 全通过 ════')
    return 0


if __name__ == '__main__':
    sys.exit(main())