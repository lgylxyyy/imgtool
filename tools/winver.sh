#!/bin/bash
# winver.sh — 一条命令看「这个文件/这张盘到底是哪个 Windows」
#
#   用法: tools/winver.sh <xxx.iso|xxx.esd|xxx.wim|xxx.qcow2>
#
#   · ISO/ESD/WIM：读里面的映像清单（不解包）→ 选 index 用
#   · qcow2：自己找 Windows 分区（不信用户报的 LBA），读【6 个来源】交叉验证
#
# 设计要点（都是踩过的坑）：
#   1) 不挑「最大的分区」：实测 Windows 11 UEFI 盘的数据分区比系统分区大，
#      挑最大会把数据分区当系统盘 → 报 PART-FS-BAD，看着像"盘坏了"。
#   2) 候选分区逐个试：能读出 ntoskrnl/注册表的那个才是系统分区。
#   3) 分区表读不出来 ≠ 找不到 Windows：分开报，别让用户以为系统没了。
set -u
T=$(cd "$(dirname "$0")/.." && pwd)/build/tools
export IMGCMD="$T/imgcmd"
exec python3 - "$@" <<'PY'
import json, os, subprocess, sys

IMG = os.environ["IMGCMD"]
ARCH = {"0": "x86", "5": "arm", "9": "x64", "12": "arm64"}
# GPT 类型里【不是】Windows 系统分区的：EFI / MSR / WinRE / BIOS boot
NOT_OS_GPT = ("c12a7328", "e3c9e316", "de94bba4", "21686148")
# MBR 类型里不是 NTFS 系统分区的：0x27 恢复、0x0c/0x0b FAT32、0xef ESP、0x82 Linux swap
NOT_OS_MBR = (0x27, 0x0C, 0x0B, 0xEF, 0x82)


def run(*args):
    r = subprocess.run([IMG] + list(args) + ["--progress=none"],
                       capture_output=True, text=True)
    try:
        return json.loads(r.stdout)
    except Exception:
        return {"ok": False, "err": {"error_code": "?", "why": (r.stdout or r.stderr)[:200]}}


def fail(msg, why=""):
    print("结论 : ❌ " + msg)
    if why:
        print("       " + why)
    sys.exit(1)


def print_version(path, lba, d):
    x = d["data"]
    ev = x.get("evidence", [])
    g = lambda k, dv="-": x.get(k, dv)
    print("文件 : %s" % path)
    print("分区 : LBA %d（自动探测：能读出内核/注册表的那一个）" % lba)
    print("结论 : build %s  |  %s  |  edition %s"
          % (g("consensus_build"), g("registry.product_name"), g("registry.edition_id")))
    if g("registry.display_version") != "-":
        print("版本 : %s（注册表 CurrentBuild %s）"
              % (g("registry.display_version"), g("registry.current_build")))
    print("证据 : %d/6 个来源" % len(ev))
    for e in ev:
        print("   %-12s %-6s %s" % (e["source"], e["build"], e["version"]))
    if g("servicing_version") != "-" and not any(e["source"] == "servicing" for e in ev):
        print("   servicing     -      %s（目录名解析失败，仅展示）" % g("servicing_version"))
    if x.get("unreadable"):
        print("读不出 : %s（文件在，但被 WOF/CompactOS 压缩，PE 版本取不到）" % x["unreadable"])
    if x.get("skus"):
        print("SKU  : %s" % x["skus"][:110])
    if x.get("mismatch"):
        print("判定 : ⚠ 证据跨代矛盾 —— 这个镜像被换过版本/精简过，别按名字判断")
    elif len(ev) <= 1:
        print("判定 : ⚠ 只有 1 个来源，可信度低")
    else:
        print("判定 : ✅ 证据一致")
    if x.get("note"):
        print("备注 : %s" % x["note"])


def do_qcow2(path):
    d = run("part.show", "--img=" + path)
    if not d.get("ok"):
        e = d.get("err", {})
        print("文件 : %s" % path)
        fail("读不出分区表（%s）" % e.get("error_code", "?"),
             "这张盘可能还没建分区表（img.create 之后没 imglayout），或者分区表坏了——"
             "这不是「找不到 Windows」。")

    parts = d["data"]["pt"]["parts"]
    cands = []
    for p in parts:
        tg = (p.get("typeGuid") or "").lower()
        mb = p.get("mbri") or 0
        if tg:
            if tg[:8] in NOT_OS_GPT or tg[:8] != "ebd0a0a2":
                continue
        else:
            if mb in NOT_OS_MBR or mb != 7:
                continue
        cands.append(p)
    # 主分区优先（逻辑分区基本是数据盘），其次按大小
    cands.sort(key=lambda p: (1 if p.get("logical") else 0, -p["sectors"]))
    if not cands:
        print("文件 : %s" % path)
        fail("分区表里没有 NTFS/Windows 分区",
             "分区表是 %s，%d 个分区，但没有可当系统盘的类型。" % (d["data"]["pt"]["table"], len(parts)))

    tried = []
    for p in cands[:4]:
        lba = p["first"]
        v = run("win.version", "--img=" + path, "--lba=%d" % lba)
        if v.get("ok"):
            print_version(path, lba, v)
            return
        tried.append((lba, p["sectors"] * 512 // (1024 ** 3), v.get("err", {}).get("error_code", "?")))

    print("文件 : %s" % path)
    print("试过的分区：")
    for lba, gb, code in tried:
        print("   LBA %-12d %4d GiB  → %s" % (lba, gb, code))
    fail("这些分区都读不出版本",
         "要么是空的/没格式化，要么不是 Windows 系统分区。"
         "可以先用 `imgcmd ntfs.list --img=<盘> --lba=<LBA> --path=/Windows` 手工确认。")


def do_image(path):
    d = run("wim.identify", "--src=" + path)
    if not d.get("ok"):
        e = d.get("err", {})
        print("文件 : %s" % path)
        fail("不是 WIM/ESD/ISO（%s）" % e.get("error_code", "?"), e.get("why", ""))
    x = d["data"]
    print("文件 : %s" % path)
    if x.get("volume"):
        print("卷标 : %s" % x["volume"])
    line = "容器 : %s" % x["container"]
    if x.get("inner_path"):
        line += "  内路径 %s（%s）" % (x["inner_path"], x.get("inner_kind", "?"))
    print(line)
    print("映像 : %d 个" % x["count"])
    print("  idx  %-34s %-12s %-6s %s" % ("名称", "版本", "架构", "edition"))
    for i in x["images"]:
        print("  %-4s %-34s %-12s %-6s %s"
              % (i["index"], i["name"][:34], i["version"],
                 ARCH.get(i["arch"], i["arch"]), i["edition_id"]))
    if x["count"]:
        print("提示 : 装第 N 个 → wim.apply --index=N（别按名字猜）")


def main():
    if len(sys.argv) < 2 or not os.path.isfile(sys.argv[1]):
        print("用法: %s <iso|esd|wim|qcow2>" % os.path.basename(sys.argv[0]))
        sys.exit(2)
    f = sys.argv[1]
    if f.lower().endswith(".qcow2"):
        do_qcow2(f)
    else:
        do_image(f)


main()
PY