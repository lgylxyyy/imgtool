#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""lscache-test —— 目录会话缓存（翻页 O(1)）+ 会话搜索（wim.search）

用户目标（2026-11-18）：文件页「飞速往下翻不卡」。
方案：wim.ls --open 一次遍历全目录建会话；之后翻页/搜索都带 --token，
完全脱离 ISO/WIM（不再重开），每页 5-10ms。

★ 会话在【进程内存】里 —— CLI 每条命令一个进程，跨进程必须 TOKEN-EXPIRED。
   所以：翻页/搜索的完整验证在 C 测试（lscache_test，同进程）里做；
   本脚本负责造素材 + 调 C 测试 + 验 CLI 层能验的（--open、跨进程过期）。

断言：
  ① --open：cached=true、total=条目数、拿到 token
  ② 带 token 翻页（**不传 --wim**，证明脱离容器）：各页 count/has_more 正确，
     拼接 = 全集、无重无漏
  ③ 翻页耗时 < 老路径（--open 之外的逐页遍历）—— 打印数字，硬断言 50ms 内
  ④ 无效 token → TOKEN-EXPIRED
  ⑤ wim.search：子串搜索（不区分大小写）、total 正确、分页正确
  ⑥ 搜索结果 ⊆ 列表条目
"""
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
IMGCMD = os.path.join(ROOT, "build", "tools", "imgcmd")
N = 1200          # 条目数（System32 量级；足够体现翻页收益）
PAGE = 500

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
    t0 = time.time()
    p = subprocess.run([IMGCMD] + list(args), capture_output=True, text=True)
    dt = (time.time() - t0) * 1000.0
    lines = [l for l in p.stdout.splitlines() if l.startswith("{")]
    try:
        return json.loads(lines[-1]), dt
    except Exception:
        return {"ok": False, "raw": (p.stdout or "")[:160] + (p.stderr or "")[:160]}, dt


def main():
    if not os.path.exists(IMGCMD):
        print("⏭ 没有 build/tools/imgcmd")
        return 77
    feats = json.loads(subprocess.run([IMGCMD, "--list"], capture_output=True,
                                      text=True).stdout).get("features", {})
    if not (feats.get("ntfs3g") and feats.get("wimlib")):
        print("⏭ 缺 ntfs3g 或 wimlib")
        return 77

    W = tempfile.mkdtemp(prefix="lscache")
    try:
        # ── 素材：N 个文件 → NTFS → WIM ──
        img = os.path.join(W, "s.qcow2")
        src = os.path.join(W, "many")
        os.makedirs(src)
        for i in range(1, N + 1):
            with open(os.path.join(src, "f%04d-%s.bin" % (i, "a" * 16)), "wb") as f:
                f.write(b"x" * 256)
        r, _ = run("img.create", "--out=" + img, "--size-gb=2", "--table=mbr",
                   "--sys-mb=1500", "--esp-mb=0", "--rec-mb=0", "--limits=off")
        parts = (r.get("data") or {}).get("pt", {}).get("parts", [])
        lba = sorted(parts, key=lambda p: p.get("sectors", 0), reverse=True)[0]["first"]
        run("fs.format", "--img=" + img, "--lba=%d" % lba, "--fs=ntfs")
        r, _ = run("copy", "host:" + src, "%s!lba%d:/many" % (img, lba), "--conflict=overwrite")
        check("写入 %d 个文件" % N, r.get("ok"), r)
        wim = os.path.join(W, "o.wim")
        r, _ = run("wim.capture", "--img=" + img, "--lba=%d" % lba, "--path=/many",
                   "--out=" + wim, "--comp=xpress")
        check("wim.capture %d 个文件" % N, r.get("ok") and (r.get("data") or {}).get("files") == N, r)

        # ── ① 建会话 ──
        r, t_open = run("wim.ls", "--open=true", "--wim=" + wim, "--path=/many")
        d = r.get("data") or {}
        token = d.get("token") or ""
        check("① --open 成功（耗时 %.0fms）" % t_open, r.get("ok"), r)
        check("① cached=true", d.get("cached") is True, d.get("cached"))
        check("① total=%s" % d.get("total"), d.get("total") == N, d.get("total"))
        check("① 拿到 token（%s）" % token, bool(token), token)

        if not token:
            print("⚠ 没有 token，后续断言跳过")
        else:
            # CLI 层：--open 形态本身可用（单进程内有效）
            check("① CLI --open 返回 cached=true + total", d.get("cached") is True and d.get("total") == N, None)

        # 命令层翻页/搜索：同一进程内验证（C 测试，B 部分）
        ctest = os.path.join(ROOT, "build", "tests", "lscache_test")
        if os.path.exists(ctest):
            p = subprocess.run([ctest, wim], capture_output=True, text=True)
            ok_c = p.returncode == 0
            check("② 命令层（--open → --token 翻页 → wim.search）同进程验证", ok_c,
                  p.stdout[-300:] if not ok_c else "")
            for line in p.stdout.splitlines():
                if "B." in line or "wim.ls --open" in line:
                    print("     " + line.strip())
        else:
            print("  ⏭ 没编译 lscache_test（B 部分跳过）")

        # CLI 跨进程：token 在下一个进程里必须失效（TOKEN-EXPIRED，App 靠它重开）
        if token:
            r, _ = run("wim.ls", "--token=" + token, "--path=/many")
            check("③ 跨进程 token → TOKEN-EXPIRED（App 重开一次即可）",
                  (not r.get("ok")) and (r.get("err") or {}).get("error_code") == "TOKEN-EXPIRED",
                  (r.get("ok"), (r.get("err") or {}).get("error_code")))

    finally:
        shutil.rmtree(W, ignore_errors=True)

    print("\n════════ lscache-test：通过 %d · 失败 %d ════════" % (passed, failed))
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())