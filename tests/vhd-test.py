#!/usr/bin/env python3
"""Dynamic VHD regression: create, write across blocks, reopen and QEMU round-trip."""
import json
import os
import shutil
import struct
import subprocess
import tempfile

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..'))
TOOL = os.path.join(ROOT, 'build/tools/imgcmd')


def run(*args):
    p = subprocess.run(args, capture_output=True, text=True, check=True)
    return p.stdout


def cmd(*args):
    r = json.loads(run(TOOL, *args))
    assert r['ok'], (args, r)
    return r['data']


def footer(path):
    with open(path, 'rb') as f:
        first = f.read(512)
        f.seek(-512, 2)
        last = f.read(512)
    assert first == last and first[:8] == b'conectix', path
    assert struct.unpack_from('>I', first, 60)[0] == 3, path
    checksum = struct.unpack_from('>I', first, 64)[0]
    assert ((sum(first) - sum(first[64:68]) + sum(b'\0' * 4)) & 0xffffffff) ^ 0xffffffff == checksum


def roundtrip(path, expected):
    footer(path)
    info = json.loads(run('qemu-img', 'info', '-f', 'vpc', '--output=json', path))
    assert info['format'] == 'vpc' and info['virtual-size'] >= expected
    raw = path + '.raw'
    run('qemu-img', 'convert', '-f', 'vpc', '-O', 'raw', path, raw)
    with open(raw, 'rb') as f:
        assert f.read(512)[510:] == b'\x55\xaa'
    return raw


def main():
    if not shutil.which('qemu-img'):
        print('SKIP: qemu-img unavailable')
        return
    with tempfile.TemporaryDirectory(prefix='vhd-test-') as d:
        q = os.path.join(d, 'qemu.vhd')
        run('qemu-img', 'create', '-f', 'vpc', q, '2G')
        cmd('part.layout', '--img=' + q, '--sys-gb=1', '--rec-gb=0')
        assert cmd('part.show', '--img=' + q)['pt']['table'] == 'MBR'
        roundtrip(q, 1 << 30)
        print('QEMU-created VHD: layout, footer and QEMU conversion OK')

        m = os.path.join(d, 'mbr.vhd')
        cmd('img.create', '--out=' + m, '--size-gb=1', '--format=vhd', '--table=mbr', '--limits=off')
        assert cmd('part.show', '--img=' + m)['pt']['table'] == 'MBR'
        roundtrip(m, 1 << 30)
        print('Tool-created MBR VHD: layout, footer and QEMU conversion OK')

        g = os.path.join(d, 'gpt.vhd')
        cmd('img.create', '--out=' + g, '--size-gb=2', '--format=vhd', '--table=gpt',
            '--sys-mb=1024', '--rec-mb=0', '--limits=off')
        pt = cmd('part.show', '--img=' + g)['pt']
        assert pt['table'] == 'GPT' and pt['parts']
        raw = roundtrip(g, 2 << 30)
        with open(raw, 'rb') as f:
            f.seek(512)
            assert f.read(8) == b'EFI PART'
            f.seek(-512, 2)
            assert f.read(8) == b'EFI PART'
        print('Tool-created GPT VHD: primary/backup GPT and QEMU conversion OK')

        # Reopen and write via the block API, including an unallocated distant block.
        import ctypes
        lib = ctypes.CDLL(os.path.join(ROOT, 'build/libimgtool.so'))
        lib.img_io_open_existing.argtypes = [ctypes.c_char_p, ctypes.c_bool]
        lib.img_io_open_existing.restype = ctypes.c_void_p
        lib.img_qcow2_open.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
        lib.img_qcow2_open.restype = ctypes.c_void_p
        lib.img_qcow2_write.argtypes = [ctypes.c_void_p, ctypes.c_uint64, ctypes.c_void_p, ctypes.c_size_t]
        lib.img_qcow2_write.restype = ctypes.c_int
        lib.img_qcow2_close.argtypes = [ctypes.c_void_p]
        lib.img_io_close.argtypes = [ctypes.c_void_p]
        io = lib.img_io_open_existing(os.fsencode(m), True)
        assert io
        image = lib.img_qcow2_open(io, None)
        assert image
        payload = b'block-reopen-check' + bytes(512 - len('block-reopen-check'))
        offset = 8 * 1024 * 1024
        try:
            assert lib.img_qcow2_write(image, offset, payload, len(payload)) == 0
        finally:
            lib.img_qcow2_close(image)
            lib.img_io_close(io)
        raw = roundtrip(m, 1 << 30)
        with open(raw, 'rb') as f:
            f.seek(offset)
            assert f.read(len(payload)) == payload
        print('Reopen and allocate a distant block: QEMU reads the exact payload')


if __name__ == '__main__':
    main()
