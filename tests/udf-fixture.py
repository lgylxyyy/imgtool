#!/usr/bin/env python3
"""造一张"够我们用"的最小 UDF 镜像（REQ-45 回归夹具）。

只满足 img_iso.c 的读取路径：AVDP → VDS(PD/LVD/TERM) → FSD → 根 FE → 目录数据。
故意做两个坑（真机上踩过）：
  1. 根目录拆成 **两个 100 字节的 extent**（都不是块对齐）——
     旧 read_fe_data 按 ceil(total) 分配、按整块写，会写飞 2 KB → SIGSEGV 的一种形态；
  2. （--bogus）最后一条 FID 的 Implementation-Use 长度写成离谱值 ——
     旧 iter_dir 用它算指针，直接读到缓冲外。

用法：udf-fixture.py <out.iso> [--bogus]
"""
import struct
import sys

LB = 2048
TAG_AVDP, TAG_PD, TAG_LVD, TAG_TERM = 2, 5, 6, 8
TAG_FSD, TAG_FID, TAG_FE = 256, 257, 261


def blk(n):
    return bytearray(LB * n)


def put16(b, off, v):
    struct.pack_into('<H', b, off, v & 0xFFFF)


def put32(b, off, v):
    struct.pack_into('<I', b, off, v & 0xFFFFFFFF)


def put64(b, off, v):
    struct.pack_into('<Q', b, off, v & 0xFFFFFFFFFFFFFFFF)


def fid(ch, name, icb_lb, iu=0, extra_iu=b''):
    ent = bytearray(38 + iu + len(name))
    put16(ent, 0, TAG_FID)
    put16(ent, 16, 1)                    # file version
    ent[18] = ch
    ent[19] = len(name)
    put32(ent, 20, 0)                    # ICB long_ad: length
    put32(ent, 24, icb_lb)               # ICB long_ad: lb
    put32(ent, 28, 0)                    # part ref
    put16(ent, 36, iu)
    if iu:
        ent[38:38 + len(extra_iu)] = extra_iu[:iu]
    ent[38 + iu:38 + iu + len(name)] = name
    pad = (4 - (len(ent) % 4)) % 4
    return bytes(ent) + b'\0' * pad


def file_fe(size, mtime=1700000000):
    fe = bytearray(2048)
    put16(fe, 0, TAG_FE)
    put16(fe, 16, 1)
    fe[27] = 5                            # file type: regular
    put16(fe, 34, 0)
    put64(fe, 56, size)
    put32(fe, 168, 0)
    put32(fe, 172, 0)
    put32(fe, 176, 0)
    # 修改时间：type=1/tz=0/year=2023(LE16)/month…（list_cb 会读这里）
    t = 84
    fe[t] = 1
    put16(fe, t + 2, 2023)
    fe[t + 4:t + 10] = bytes([11, 14, 22, 13, 14, 15])
    return bytes(fe)


def dir_fe(extents):
    fe = bytearray(2048)
    put16(fe, 0, TAG_FE)
    put16(fe, 16, 1)
    fe[27] = 4                            # directory
    put16(fe, 34, 0)                      # short_ad，非内嵌
    put64(fe, 56, sum(n for n, _ in extents))
    put64(fe, 64, 0)                      # blocks_recorded=0 → 长度按字节
    put32(fe, 168, 0)
    put32(fe, 172, 8 * len(extents))
    ad = 176
    for n, lb in extents:
        put32(fe, ad, n)
        put32(fe, ad + 4, lb)
        ad += 8
    return bytes(fe)


def main():
    out = sys.argv[1]
    bogus = '--bogus' in sys.argv

    PD_LB, LVD_LB, TERM_LB = 100, 101, 102
    FSD_LB = 205              # 绝对块号；分区从 200 开始 → 分区内 lb=5
    PART_START = 200
    ROOT_FE_LB = 10
    DATA_LB1, DATA_LB2 = 20, 21
    FILE_FE_LB = 31

    img = blk(600)

    # AVDP @256
    put16(img, 256 * LB, TAG_AVDP)
    put32(img, 256 * LB + 16, 3 * LB)
    put32(img, 256 * LB + 20, PD_LB)

    # PD
    put16(img, PD_LB * LB, TAG_PD)
    put32(img, PD_LB * LB + 188, PART_START)
    # LVD
    put16(img, LVD_LB * LB, TAG_LVD)
    put32(img, LVD_LB * LB + 212, LB)
    put32(img, LVD_LB * LB + 252, FSD_LB - PART_START)
    vol = b'UDFTEST'
    img[LVD_LB * LB + 84] = 8
    img[LVD_LB * LB + 85:LVD_LB * LB + 85 + len(vol)] = vol
    # TERM
    put16(img, TERM_LB * LB, TAG_TERM)

    # FSD
    put16(img, FSD_LB * LB, TAG_FSD)
    put32(img, FSD_LB * LB + 404, ROOT_FE_LB)

    # 根 FE：两个【非块对齐】extent（88 + 44 = 132）—— 旧写法会写飞 2 KB
    root = dir_fe([(88, DATA_LB1), (44, DATA_LB2)])
    img[PART_START * LB + ROOT_FE_LB * LB:PART_START * LB + (ROOT_FE_LB + 1) * LB] = root

    # 目录数据：extent1 = parent + Sub；extent2 = a.txt（+ 可选坏 FID）
    d1 = bytearray(88)
    f_parent = fid(0x08, b'', ROOT_FE_LB, iu=4, extra_iu=b'ABCD')
    f_sub = fid(0x02, b'Sub', 40)
    d1[0:len(f_parent)] = f_parent
    d1[len(f_parent):len(f_parent) + len(f_sub)] = f_sub

    d2 = bytearray(44)
    iu = 60000 if bogus else 0
    f_txt = fid(0x00, b'a.txt', FILE_FE_LB, iu=iu, extra_iu=b'Z' * 8)
    d2[0:min(len(f_txt), len(d2))] = f_txt[:len(d2)]

    img[PART_START * LB + DATA_LB1 * LB:PART_START * LB + DATA_LB1 * LB + len(d1)] = d1
    img[PART_START * LB + DATA_LB2 * LB:PART_START * LB + DATA_LB2 * LB + len(d2)] = d2

    # a.txt 的 FE（大小 12345，mtime）
    img[PART_START * LB + FILE_FE_LB * LB:PART_START * LB + (FILE_FE_LB + 1) * LB] = file_fe(12345)

    with open(out, 'wb') as f:
        f.write(img)
    print(out, len(img))


if __name__ == '__main__':
    main()