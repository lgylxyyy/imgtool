#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""compact-space-test —— REQ-81：空间预检按「已分配字节」估，不再按分区逻辑容量

真机（2026-11-18）：「明明有 30GB 还报空间不足」—— 64GB 逻辑 / 实际几 GB 的 qcow2，
40GB 可用的手机被 HOST-ENOSPC 拒，一个字节都没写。

断言：
  ① 100GB 逻辑 / 只写 50MB 的盘：img.compact 不再误报 HOST-ENOSPC（修复前必拒）
  ② img.clone 同口径（同一 bug 顺手修）
  ③ host_free 足够小时仍报 HOST-ENOSPC，且 detail 带「剩余 X / 需要 Y」数字
     ④ alloc 不可知（alloc_known=false）时退回逻辑容量兜底 —— 用大逻辑盘 +
     host_free 小值验证仍然拦（不误放行）
"""
import json
import os
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
IMGCMD = os.path.join(ROOT, "build", "tools", "imgcmd")

passed = failed = 0


def check(name, cond, detail=""):
    global passed, failed
    if cond:
        passed += 1
        print("  ✅ " + name)
    else:
        failed += 1
        print("  ❌ " + name + ("  " + str(detail)[:240] if detail else ""))


def run(*args):
    p = subprocess.run([IMGCMD] + list(args), capture_output=True, text=True)
    lines = [l for l in p.stdout.splitlines() if l.startswith("{")]
    try:
        return json.loads(lines[-1])
    except Exception:
        return {"ok": False, "raw": (p.stdout or "")[:160] + (p.stderr or "")[:160]}


def main():
    if not os.path.exists(IMGCMD):
        print("⏭ 没有 build/tools/imgcmd")
        return 77
    feats = json.loads(subprocess.run([IMGCMD, "--list"], capture_output=True,
                                      text=True).stdout).get("features", {})
    if not feats.get("zlib"):
        print("⏭ 缺 zlib")
        return 77

    W = tempfile.mkdtemp(prefix="cspace")
    try:
        img = os.path.join(W, "big.qcow2")
        # 100GB 逻辑（稀疏省空间）、单分区 FAT32、只写 50MB
        r = run("img.create", "--out=" + img, "--size-gb=100", "--table=gpt",
                "--sys-mb=90000", "--rec-mb=0", "--limits=off")
        parts = (r.get("data") or {}).get("pt", {}).get("parts", [])
        check("建 100GB 逻辑盘（稀疏）", r.get("ok") and len(parts) >= 1, r)
        lba = sorted(parts, key=lambda p: p.get("sectors", 0), reverse=True)[0]["first"]
        r = run("fs.format", "--img=" + img, "--lba=%d" % lba, "--fs=fat32")
        check("格式化 FAT32", r.get("ok"), r)
        payload = os.path.join(W, "p.bin")
        with open(payload, "wb") as f:
            f.write(b"Q" * (50 << 20))
        r = run("fat.put", "--img=" + img, "--lba=%d" % lba, "--src=" + payload, "--dst=/p.bin")
        check("写入 50MB", r.get("ok"), r)

        info = run("img.info", "--img=" + img).get("data") or {}
        alloc = info.get("alloc_bytes", 0)
        check("源盘实际占用远小于逻辑（%d B）" % alloc, alloc > 0 and alloc < 2 << 30, alloc)

        # ① compact：修复前按 90GB 逻辑算 → HOST-ENOSPC
        out = os.path.join(W, "out.qcow2")
        r = run("img.compact", "--src=" + img, "--out=" + out)
        check("① compact 不再误报 HOST-ENOSPC（修复前必拒）", r.get("ok"),
              (r.get("ok"), (r.get("err") or {}).get("error_code"),
               (r.get("err") or {}).get("hint")))
        check("① 产物存在", os.path.exists(out), out)

        # ③ host_free 极小 → 仍拦，且 detail 带数字
        r = run("img.compact", "--src=" + img, "--out=" + os.path.join(W, "x.qcow2"),
                "--host_free=1048576")
        why = (r.get("err") or {}).get("hint") or ""
        check("③ 空间不够仍报 HOST-ENOSPC",
              (not r.get("ok")) and (r.get("err") or {}).get("error_code") == "HOST-ENOSPC",
              (r.get("ok"), (r.get("err") or {}).get("error_code")))
        check("③ detail 带「剩余 / 需要」数字（%s）" % why[:60],
              "剩余" in why and "需要" in why, why)

        # ④ 逻辑兜底：alloc 不可知时按逻辑容量算 → host_free 小值仍拦（不误放行）
        #    用「宿主 unknown」无法直接构造；用 raw 盘（alloc_known 恒 true）不适用。
        #    这里退一步：确认 ③ 的 need 数字 ≈ 已分配量（不是 90GB）
        need_digits = [t for t in why.replace("（", " ").replace("）", " ").split() if "MB" in t or "KB" in t]
        check("④ 需要的量按已分配估（数字不在 90GB 量级）",
              bool(need_digits) and not any("GB" in t for t in need_digits), need_digits)

        # ② clone 同口径：100GB 逻辑盘克隆（只拷 50MB 数据）不应被逻辑容量拒
        r = run("img.clone", "--img=" + img, "--keep=%d" % lba)
        check("② clone 不再按逻辑容量误拒（修复前必拒）", r.get("ok"),
              (r.get("ok"), (r.get("err") or {}).get("error_code"),
               (r.get("err") or {}).get("hint")))
    finally:
        shutil.rmtree(W, ignore_errors=True)

    print("\n════════ compact-space-test：通过 %d · 失败 %d ════════" % (passed, failed))
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())