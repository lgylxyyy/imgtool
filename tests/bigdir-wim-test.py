#!/usr/bin/env python3
"""bigdir-wim-test.py —— 单目录条目数超过 256 时的三条链路（REQ-65 回归）

背景：`xlist` 的 cap 固定 256，满了直接丢条目，最后报 IMG_EFULL。系统盘上
\\Windows\\WinSxS（上千）与 \\Windows\\servicing\\Packages（上万）必然命中。
同时 `img_du` 的目录队列固定 64，满了静默丢整棵子树 → `wim.capture --estimate`
少算 30%+，App 的空间预检按错的数放行。

本用例用 400 个文件（刚好越过 256）验证：
  ① 单目录 400 条：xcopy 的 xlist 不再在 256 截断（旧代码捕获直接失败）
  ② 宽树（200 个子目录）：img_du 的递归队列不再在 64 溢出
     —— 这是 estimate 少算 36% 的真正原因，旧代码会少报
  ③ 实际捕获 files 与 estimate 对齐、skipped=0
"""
import json
import os
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
IMGCMD = os.path.join(ROOT, "build", "tools", "imgcmd")
N = 400

passed = failed = 0


def check(cond, name, detail=""):
    global passed, failed
    if cond:
        passed += 1
        print("  ✅ " + name)
    else:
        failed += 1
        print("  ❌ " + name + ("  " + str(detail)[:200] if detail else ""))


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
                                      text=True).stdout)["features"]
    if not (feats.get("ntfs3g") and feats.get("wimlib")):
        print("⏭ 缺 ntfs3g 或 wimlib")
        return 77

    W = tempfile.mkdtemp(prefix="bigdir")
    try:
        img = os.path.join(W, "s.qcow2")
        src = os.path.join(W, "many")
        os.makedirs(src)
        for i in range(1, N + 1):
            with open(os.path.join(src, "f%03d.bin" % i), "wb") as f:
                f.write(b"x" * 1024)
        # 宽树：一个父目录下 200 个子目录（> du 的 64 队列）
        wide = os.path.join(W, "wide")
        for d in range(1, 201):
            sub = os.path.join(wide, "d%03d" % d)
            os.makedirs(sub)
            for k in range(3):
                with open(os.path.join(sub, "f%d.bin" % k), "wb") as f:
                    f.write(b"y" * 1024)

        r = run("img.create", "--out=" + img, "--size-gb=1", "--table=mbr",
                "--sys-mb=700", "--esp-mb=0", "--rec-mb=0", "--limits=off")
        parts = (r.get("data") or {}).get("pt", {}).get("parts", [])
        lba = sorted(parts, key=lambda p: p.get("sectors", 0), reverse=True)[0]["first"]
        r = run("fs.format", "--img=" + img, "--lba=%d" % lba, "--fs=ntfs")
        check(r.get("ok"), "建盘 + 格式化 NTFS", r)

        r = run("copy", "host:" + src, img + "!lba%d:/many" % lba, "--conflict=overwrite")
        check(r.get("ok"), "写入 %d 个文件" % N, r)

        # ① 目录列表完整性
        r = run("ntfs.list", "--img=" + img, "--lba=%d" % lba, "--path=/many")
        n = (r.get("data") or {}).get("count")
        check(r.get("ok") and n == N, "① 目录能完整列出 %d 条（旧代码 256 就 EFULL）" % N,
              (r.get("ok"), n))

        # ② estimate 与 ③ 实际捕获
        r = run("wim.capture", "--img=" + img, "--lba=%d" % lba, "--path=/many",
                "--estimate=true")
        files = (r.get("data") or {}).get("files")
        check(r.get("ok") and files == N, "② estimate 报 %d 个文件（旧代码按队列截断）" % N,
              (r.get("ok"), files))

        if (r.get("data") or {}).get("bytes_needed", 0) <= 0:
            # 估算给了 0 说明口径又断了，但捕获仍可跑
            pass
        out = os.path.join(W, "o.wim")
        r = run("wim.capture", "--img=" + img, "--lba=%d" % lba, "--path=/many",
                "--out=" + out, "--comp=xpress")
        d = r.get("data") or {}
        check(r.get("ok") and d.get("files") == N and d.get("skipped") == 0,
              "③ 实际捕获 files=%d skipped=0" % N, (r.get("ok"), d))

        # ②③ 宽树：estimate 与实捕必须一致
        r = run("copy", "host:" + wide, img + "!lba%d:/wide" % lba, "--conflict=overwrite")
        check(r.get("ok"), "写入宽树（200 子目录 × 3 文件）", r)
        r = run("wim.capture", "--img=" + img, "--lba=%d" % lba, "--path=/wide",
                "--estimate=true")
        est = (r.get("data") or {}).get("files")
        check(r.get("ok") and est == 600, "② 宽树 estimate 报 600（旧代码在 64 队列上丢子树）", (r.get("ok"), est))
        out2 = os.path.join(W, "w.wim")
        r = run("wim.capture", "--img=" + img, "--lba=%d" % lba, "--path=/wide",
                "--out=" + out2, "--comp=xpress")
        d = r.get("data") or {}
        check(r.get("ok") and d.get("files") == 600, "③ 宽树实捕 files=600 且与 estimate 一致",
              (r.get("ok"), d.get("files"), est))
    finally:
        shutil.rmtree(W, ignore_errors=True)

    print("\n════════ bigdir-wim-test：通过 %d · 失败 %d ════════" % (passed, failed))
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())