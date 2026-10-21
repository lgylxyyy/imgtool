#!/usr/bin/env python3
"""mkbcdtpl.py — 把 reference/bcd-tpl-*.bin 转成可编译的 C 数组（src/img_bcd_tpl.h）

模板来源：真实、能启动的 BCD，**设备二进制（11000001/21000001/31000003）已清零**，
所以文件里没有任何机器的磁盘签名/分区 GUID/路径偏移；bcd.create 会在运行时按
目标盘重写。字符串（描述/locale/winload 路径）是通用的，保留。

用法: tools/mkbcdtpl.py            # 重新生成 src/img_bcd_tpl.h
"""
import os, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
TPLS = [("legacy_bios",  "bcd-tpl-legacy-bios.bin",  "Vista/7 BIOS（6.x，MBR）"),
        ("modern_bios",  "bcd-tpl-modern-bios.bin",  "8/8.1/10/11 BIOS（10.x，MBR）"),
        ("modern_uefi",  "bcd-tpl-modern-uefi.bin",  "8/8.1/10/11 UEFI（10.x，GPT）")]


def emit(name, data, out):
    out.append("static const unsigned char IMG_BCD_TPL_%s[%d] = {" % (name.upper(), len(data)))
    for i in range(0, len(data), 16):
        out.append("    " + ",".join("0x%02x" % b for b in data[i:i + 16]) + ",")
    out.append("};")
    out.append("")


def main():
    out = ["/* img_bcd_tpl.h — 自动生成，别手改（tools/mkbcdtpl.py）",
           " *",
           " * BCD 内置模板：一张「真实能启动」的 BCD，设备二进制已清零。",
           " * bcd.create 用它做底座，再按目标盘重写设备/路径/默认项。",
           " * 分两代：6.x（Vista/7）与 10.x（8/8.1/10/11）——元素表跨代兼容，",
           " * 但 bootmgr 的加载器路径与个别布尔项不同代有差异，分两包最稳。 */",
           "#ifndef IMG_BCD_TPL_H", "#define IMG_BCD_TPL_H", ""]
    for name, fn, desc in TPLS:
        p = os.path.join(ROOT, "reference", fn)
        data = open(p, "rb").read()
        out.append("/* %s：%s，%d 字节（%s） */" % (name, desc, len(data), fn))
        emit(name, data, out)
    out.append("#endif")
    dst = os.path.join(ROOT, "src", "img_bcd_tpl.h")
    open(dst, "w").write("\n".join(out) + "\n")
    print("wrote %s (%d 字节)" % (dst, os.path.getsize(dst)))


main()
