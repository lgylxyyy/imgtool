#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""readonly-dryrun-test —— REQ-82：dry-run 用只读方式打开也必须能做

真机（2026-11-18）：App 释放空间页点「分析」（img.sparsify --dry-run，
App 传 writable=false → 工具端 O_RDONLY）→ 立刻报「映像是只读的，无法操作」。
dry-run 语义是「一个字节都不写」，只读打开完全合法 —— 以前的拦截是 bug。

断言：
  ① 只读打开 + sparsify --dry-run  → ok（修复前 SPARSIFY-READONLY）
  ② 只读打开 + sparsify 实写       → 仍报 SPARSIFY-READONLY（不变）
  ③ 只读打开 + trim --dry-run      → ok（同款修正）
  ④ 只读打开 + trim 实写           → 仍报 TRIM-READONLY（不变）
  ⑤ 可写打开 + 实写                → ok（正常路径不受影响）
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
        print("  ❌ " + name + ("  " + str(detail)[:220] if detail else ""))


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

    W = tempfile.mkdtemp(prefix="rodry")
    try:
        img = os.path.join(W, "q.qcow2")
        r = run("img.create", "--out=" + img, "--size-gb=1", "--table=gpt",
                "--sys-mb=200", "--rec-mb=0", "--limits=off")
        parts = (r.get("data") or {}).get("pt", {}).get("parts", [])
        check("建 GPT 盘", r.get("ok") and len(parts) >= 1, r)
        lba = sorted(parts, key=lambda p: p.get("sectors", 0), reverse=True)[0]["first"]
        r = run("fs.format", "--img=" + img, "--lba=%d" % lba, "--fs=fat32")
        check("格式化 FAT32", r.get("ok"), r)

        # ① 只读 + sparsify dry-run → ok
        r = run("--ro", "img.sparsify", "--img=" + img, "--dry-run=true")
        d = r.get("data") or {}
        check("① 只读 + sparsify --dry-run → ok（修复前 SPARSIFY-READONLY）",
              r.get("ok") and "would_free_bytes" in d,
              (r.get("ok"), (r.get("err") or {}).get("error_code")))

        # ② 只读 + sparsify 实写 → 仍拦
        r = run("--ro", "img.sparsify", "--img=" + img)
        check("② 只读 + sparsify 实写 → 仍 SPARSIFY-READONLY",
              (not r.get("ok")) and (r.get("err") or {}).get("error_code") == "SPARSIFY-READONLY",
              (r.get("ok"), (r.get("err") or {}).get("error_code")))

        # ③ 只读 + trim dry-run → ok
        r = run("--ro", "img.trim", "--img=" + img, "--lba=%d" % lba, "--dry-run=true")
        check("③ 只读 + trim --dry-run → ok（同款修正）",
              r.get("ok"), (r.get("ok"), (r.get("err") or {}).get("error_code")))

        # ④ 只读 + trim 实写 → 仍拦
        r = run("--ro", "img.trim", "--img=" + img, "--lba=%d" % lba)
        check("④ 只读 + trim 实写 → 仍 TRIM-READONLY",
              (not r.get("ok")) and (r.get("err") or {}).get("error_code") == "TRIM-READONLY",
              (r.get("ok"), (r.get("err") or {}).get("error_code")))

        # ⑤ 可写 + 实写 → ok（正常路径不受影响）
        r = run("img.sparsify", "--img=" + img)
        check("⑤ 可写 + sparsify 实写 → ok", r.get("ok"), r)
        r = run("img.trim", "--img=" + img, "--lba=%d" % lba)
        check("⑤ 可写 + trim 实写 → ok", r.get("ok"), r)
    finally:
        shutil.rmtree(W, ignore_errors=True)

    print("\n════════ readonly-dryrun-test：通过 %d · 失败 %d ════════" % (passed, failed))
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())