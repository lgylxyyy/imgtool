#!/usr/bin/env python3
# fat-alias-test.py — FAT32：8.3 短名 ↔ 长名（REQ-47）
#
# 真机症状：fat.ls 印 wgl4_boot.ttf，安装脚本/用户写 WGL4_BOO.TTF（8.3 短名）
#   → rm 报 PATH-SRC-NOTFOUND。根因：dir_find 认为“有 LFN 就只比 LFN”。
# 这里用独立的小 FAT32 盘逐项验证：
#   ① 8.3 短名能 stat/rm（与 Windows 一致）
#   ② 用 8.3 名 put 是【覆盖】而不是又建一条
#   ③ 删完别的条目还在（不误伤目录簇）
#   ④ 找不到时 hint 里有“同目录里最接近的名字”
import json
import os
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
T = os.path.join(ROOT, "build", "tools")
IMGCMD = os.path.join(T, "imgcmd")
LBA = 2048

passed = failed = 0


def ok(n, extra=""):
    global passed
    passed += 1
    print("  ✅ %s%s" % (n, ("  — " + extra) if extra else ""))


def ng(n, why):
    global failed
    failed += 1
    print("  ❌ %s\n       %s" % (n, why))


def check(cond, n, why=""):
    ok(n) if cond else ng(n, why)
    return cond


def run(*args):
    p = subprocess.run([IMGCMD] + list(args), capture_output=True, text=True)
    try:
        return json.loads(p.stdout.strip().splitlines()[-1])
    except Exception:
        return {"ok": False, "raw": p.stdout + p.stderr}


def names(img, path="/"):
    r = run("fat.ls", "--img=" + img, "--lba=%d" % LBA, "--path=" + path)
    return [e["name"] for e in r.get("data", {}).get("entries", [])] if r.get("ok") else []


def main():
    W = tempfile.mkdtemp(prefix="fatalias-")
    img = os.path.join(W, "t.qcow2")
    try:
        subprocess.run([os.path.join(T, "imglayout"), "mk", img, str(96 * 1024 * 1024),
                        "gpt", "--esp", "48"], check=True, capture_output=True)
        subprocess.run([os.path.join(T, "imgmkfs"), img, "--mkfs", str(LBA), "T"],
                       check=True, capture_output=True)
        src = os.path.join(W, "p.txt")
        with open(src, "w") as f:
            f.write("hello")
        src2 = os.path.join(W, "p2.txt")
        with open(src2, "w") as f:
            f.write("hello hello")

        print("════ ① 建长名文件（8.3 短名应为 HELLO_WO.TXT）════")
        r = run("fat.put", "--img=" + img, "--lba=%d" % LBA, "--src=" + src, "--dst=/hello_world.txt")
        check(r.get("ok"), "put hello_world.txt", json.dumps(r, ensure_ascii=False)[:200])
        check("hello_world.txt" in names(img), "ls 里是长名（小写原样）")

        print("════ ② 用 8.3 短名访问（真机上就是这条失败）════")
        r = run("fat.exists", "--img=" + img, "--lba=%d" % LBA, "--path=/HELLO_WO.TXT")
        check(r.get("ok") and r.get("data", {}).get("exists"), "fat.exists 认 8.3 短名",
              json.dumps(r, ensure_ascii=False)[:200])
        r = run("fat.stat", "--img=" + img, "--lba=%d" % LBA, "--path=/HELLO_WO.TXT")
        check(r.get("ok") and r.get("data", {}).get("size") == 5, "fat.stat 走 8.3 拿到大小",
              json.dumps(r, ensure_ascii=False)[:200])

        print("════ ③ 8.3 名 put = 覆盖，不准再建一条 ════")
        n_before = len(names(img))
        r = run("fat.put", "--img=" + img, "--lba=%d" % LBA, "--src=" + src2, "--dst=/HELLO_WO.TXT")
        check(r.get("ok"), "put（8.3 名）成功", json.dumps(r, ensure_ascii=False)[:200])
        n_after = len(names(img))
        check(n_after == n_before, "条目数不变（没有生成重复文件）",
              "%d → %d %s" % (n_before, n_after, names(img)))
        r = run("fat.stat", "--img=" + img, "--lba=%d" % LBA, "--path=/hello_world.txt")
        check(r.get("ok") and r.get("data", {}).get("size") == 11, "内容按覆盖写进原文件",
              json.dumps(r, ensure_ascii=False)[:200])

        print("════ ④ 找不到时的 hint（REQ-47 §4.3）════")
        r = run("rm", "--path=%s!lba%d:/hello_xx.txt" % (img, LBA))
        hint = r.get("err", {}).get("hint", "")
        check(r.get("ok") is False and r.get("err", {}).get("error_code") == "PATH-SRC-NOTFOUND",
              "错误码仍是 PATH-SRC-NOTFOUND")
        check("hello_world" in hint or "HELLO_WO" in hint, "hint 里有同目录最接近的名字", hint or "(空)")

        print("════ ⑤ 删 8.3 名：别人不能受牵连 ════")
        for nm in ("beta.txt", "gamma.txt"):
            run("fat.put", "--img=" + img, "--lba=%d" % LBA, "--src=" + src, "--dst=/" + nm)
        r = run("rm", "--path=%s!lba%d:/HELLO_WO.TXT" % (img, LBA))
        check(r.get("ok") and r.get("data", {}).get("removed") == 1, "rm 8.3 短名成功",
              json.dumps(r, ensure_ascii=False)[:200])
        left = names(img)
        check("hello_world.txt" not in left, "目标确实没了", str(left))
        check("beta.txt" in left and "gamma.txt" in left, "同目录其它文件还在", str(left))

        print("════ ⑥ 结构自检 ════")
        r = run("qc.verify", "--img=" + img)
        check(r.get("ok"), "qcow2 结构自检通过", json.dumps(r, ensure_ascii=False)[:200])
        r = run("part.scan", "--img=" + img, "--lba=%d" % LBA)
        check(r.get("ok") and r.get("data", {}).get("fs") == "FAT32", "part.scan 仍认 FAT32",
              json.dumps(r, ensure_ascii=False)[:200])
    finally:
        if failed:
            print("\n（调试目录保留：%s）" % W)
        else:
            shutil.rmtree(W, ignore_errors=True)
    print("\n════════ fat-alias-test：通过 %d · 失败 %d ════════" % (passed, failed))
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())