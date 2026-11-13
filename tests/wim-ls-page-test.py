#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""wim-ls-page-test —— wim.ls 分页（offset/total/has_more）+ 大结果不再 RESULT-TOO-BIG

背景（用户真机 2026-11-13）：文件页浏览
  <ISO>!:/sources/install.wim!1:/Windows/System32
报「未知错误，错误码 RESULT-TOO-BIG」，大量文件无法访问。

根因链（三环）：
  ① wim.ls 没有 offset 分页（ntfs.list/fat.ls 都有）—— App 一次要 2000 条；
  ② 2000 条 System32 条目的 entries JSON ≈ 300 KB，超过渲染缓冲
     （JNI 首渲 256 KB / CLI 128 KB）；
  ③ img_result_json 顶满时返回的是【短错误信封】（正数），JNI 的
     「n>=cap-1 → 换 4MB 重渲」永远不触发 → 用户拿到 RESULT-TOO-BIG。

修法：
  · img_result_json 顶满 → 返回 -(cap)（负数=「缓冲不够，请换更大的重渲」，
    buf 里仍保留合法错误信封兜底）；调用方（JNI/CLI）拿到负数换 4 MB 重渲。
  · wim.ls 加 offset（与 ntfs.list/fat.ls 同口径）：先 contains 过滤，
    再 offset/limit 分页；结果加 total / offset / has_more。

本用例：2500 个长名文件 → NTFS → wim.capture → wim.ls：
  ① limit=2000 一页：rc=ok（修复前 CLI 128KB 必 RESULT-TOO-BIG）、
     count=2000、total=2500、has_more=true
  ② offset=2000 续页：count=500、has_more=false
  ③ 两页拼接 = 全集 2500 条，无重无漏
  ④ contains+offset 组合分页正确
"""
import json
import os
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
IMGCMD = os.path.join(ROOT, "build", "tools", "imgcmd")
N = 2500          # System32 量级（几千条）
PAGE = 2000

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
    try:
        return json.loads(p.stdout)
    except Exception:
        return {"ok": False, "raw": (p.stdout or "")[:200] + (p.stderr or "")[:200]}


def main():
    if not os.path.exists(IMGCMD):
        print("⏭ 没有 build/tools/imgcmd")
        return 77
    feats = json.loads(subprocess.run([IMGCMD, "--list"], capture_output=True,
                                      text=True).stdout).get("features", {})
    if not (feats.get("ntfs3g") and feats.get("wimlib")):
        print("⏭ 缺 ntfs3g 或 wimlib")
        return 77

    W = tempfile.mkdtemp(prefix="wimls")
    try:
        # ── 素材：2500 个长名文件 → NTFS → WIM ──
        img = os.path.join(W, "s.qcow2")
        src = os.path.join(W, "many")
        os.makedirs(src)
        for i in range(1, N + 1):
            # 长文件名：把单条 entries JSON 顶到 ~150B，2000 条 > 渲染缓冲
            with open(os.path.join(src, "f%04d-%s.bin" % (i, "a" * 24)), "wb") as f:
                f.write(b"x" * 512)

        r = run("img.create", "--out=" + img, "--size-gb=2", "--table=mbr",
                "--sys-mb=1500", "--esp-mb=0", "--rec-mb=0", "--limits=off")
        parts = (r.get("data") or {}).get("pt", {}).get("parts", [])
        lba = sorted(parts, key=lambda p: p.get("sectors", 0), reverse=True)[0]["first"]
        r = run("fs.format", "--img=" + img, "--lba=%d" % lba, "--fs=ntfs")
        check("建盘 + NTFS 格式化", r.get("ok"), r)
        r = run("copy", "host:" + src, "%s!lba%d:/many" % (img, lba), "--conflict=overwrite")
        check("写入 %d 个长名文件" % N, r.get("ok"), r)

        wim = os.path.join(W, "o.wim")
        r = run("wim.capture", "--img=" + img, "--lba=%d" % lba, "--path=/many",
                "--out=" + wim, "--comp=xpress")
        d = r.get("data") or {}
        check("wim.capture %d 个文件" % N, r.get("ok") and d.get("files") == N, (r.get("ok"), d.get("files")))

        # ── ① 第一页：limit=2000 ──
        r = run("wim.ls", "--wim=" + wim, "--path=/many", "--limit=%d" % PAGE)
        d = r.get("data") or {}
        check("① wim.ls limit=2000 成功（修复前这里 RESULT-TOO-BIG）", r.get("ok"),
              (r.get("ok"), (r.get("err") or {}).get("error_code")))
        check("① count=%s" % d.get("count"), d.get("count") == PAGE, d.get("count"))
        check("① total=%s" % d.get("total"), d.get("total") == N, d.get("total"))
        check("① has_more=true", d.get("has_more") is True, d.get("has_more"))
        p1 = [e.get("path") for e in (d.get("entries") or [])]

        # ── ② 第二页：offset=2000 ──
        r = run("wim.ls", "--wim=" + wim, "--path=/many", "--limit=%d" % PAGE,
                "--offset=%d" % PAGE)
        d = r.get("data") or {}
        check("② offset=2000 续页成功", r.get("ok"), r.get("ok"))
        check("② count=%s" % d.get("count"), d.get("count") == N - PAGE, d.get("count"))
        check("② has_more=false", d.get("has_more") is False, d.get("has_more"))
        p2 = [e.get("path") for e in (d.get("entries") or [])]

        # ── ③ 两页拼接 = 全集 ──
        allp = p1 + p2
        check("③ 两页拼接共 %d 条（期望 %d）" % (len(allp), N), len(allp) == N, len(allp))
        check("③ 无重复", len(set(allp)) == len(allp), len(set(allp)))
        expect = set(os.path.join("/many", fn) for fn in os.listdir(src))
        check("③ 与源目录集合一致", set(allp) == expect,
              "差 %d 条" % len(expect.symmetric_difference(set(allp))))

        # ── ④ contains + offset 组合（先过滤再分页）──
        hit = [fn for fn in os.listdir(src) if "0042" in fn]
        r = run("wim.ls", "--wim=" + wim, "--path=/many", "--contains=0042",
                "--limit=10", "--offset=10")
        d = r.get("data") or {}
        check("④ contains 分页成功", r.get("ok"), r.get("ok"))
        check("④ contains total=%s（过滤后计数，期望 %d）" % (d.get("total"), len(hit)),
              d.get("total") == len(hit), (d.get("total"), len(hit)))
        check("④ 第二页无重无漏",
              len(set(e.get("path") for e in (d.get("entries") or []))) == len(d.get("entries") or []),
              d.get("count"))
    finally:
        shutil.rmtree(W, ignore_errors=True)

    print("\n════════ wim-ls-page-test：通过 %d · 失败 %d ════════" % (passed, failed))
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
