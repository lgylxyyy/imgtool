#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""search-all-test —— REQ-83 `img.search`：统一路径搜索（五类容器 + 参数语义）

用户 2026-11-19：「任何地方搜任何关键字都能搜全」。
原来 NTFS/FAT/ISO 只能逐目录 BFS（400 目录 / 500 条上限），压缩包只能整包拉出来筛。

本用例覆盖：
  A. NTFS 深树（4 层 × 4 分支，1108 条、命中 512）：
     ① q 过滤 total 正确、条目都含关键字
     ② 大小写不敏感
     ③ 分页拼接（offset 步进）= 全集，无重无漏
     ④ 不带 q = 全量递归列出（total = 目录 + 文件）
     ⑤ --recursive=false 只当前层
     ⑥ 空结果 / 坏 spec / 路径不存在
     ⑦ 跨进程 token → TOKEN-EXPIRED（CLI 每条命令一个进程，设计行为）
  B. FAT32 / ISO / ZIP / HOST / WIM 各一组（命中数对得上）
  C. 会话隔离（同进程，交给 search_sess_test：交叉翻页不串目录、失败不污染）

为什么分页拼接用「每次带 spec + offset」而不是 token：CLI 跨进程 token 必过期；
token 翻页与隔离在 search_sess_test（同进程）里测。
"""
import json
import os
import shutil
import subprocess
import sys
import tempfile
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
IMGCMD = os.path.join(ROOT, "build", "tools", "imgcmd")
SESS = os.path.join(ROOT, "build", "tests", "search_sess_test")

passed = failed = 0


def check(name, cond, detail=""):
    global passed, failed
    if cond:
        passed += 1
        print("  ✅ " + name)
    else:
        failed += 1
        print("  ❌ " + name + (("  " + str(detail)[:240]) if detail else ""))


def run(*args):
    p = subprocess.run([IMGCMD] + list(args), capture_output=True, text=True)
    try:
        return json.loads(p.stdout)
    except Exception:
        return {"ok": False, "raw": (p.stdout or "")[:200] + (p.stderr or "")[:200]}


def search(spec, **kw):
    args = ["img.search", "--spec=" + spec]
    for k, v in kw.items():
        args.append("--%s=%s" % (k, v))
    return run(*args)


def make_tree(root, depth, branch, files):
    """depth 层、每层 branch 个目录、每个叶子 files 个文件（前 2/3 名字含 TargetA）。"""
    n_dirs = n_files = n_hit = 0
    frontier = [root]
    for lv in range(1, depth + 1):
        nxt = []
        for d in frontier:
            for b in range(branch):
                sub = os.path.join(d, "l%d_%d" % (lv, b))
                os.makedirs(sub, exist_ok=True)
                nxt.append(sub)
                n_dirs += 1
        frontier = nxt
    for d in frontier:
        for f in range(files):
            hit = f < files * 2 // 3
            nm = ("TargetA_%s_%d.txt" if hit else "plain_%s_%d.txt") % (
                os.path.basename(d).replace("l", "x"), f)
            with open(os.path.join(d, nm), "w") as fh:
                fh.write("x")
            n_files += 1
            if hit:
                n_hit += 1
    return n_dirs, n_files, n_hit


def biggest_lba(qcow2):
    r = run("part.show", "--img=" + qcow2)
    parts = ((r.get("data") or {}).get("pt") or {}).get("parts") or []
    return sorted(parts, key=lambda p: -p.get("sectors", 0))[0]["first"]


def walk_all_paths(spec, page=300, q=None):
    """按 offset 翻页（每次新会话；顺序稳定），返回全部 path。"""
    out, off = [], 0
    for _ in range(200):
        kw = {"limit": page, "offset": off}
        if q:
            kw["q"] = q
        r = search(spec, **kw)
        if not r.get("ok"):
            return None, r
        d = r.get("data") or {}
        es = d.get("entries") or []
        out += [e.get("path") for e in es]
        if not d.get("has_more"):
            return out, r
        off += len(es)
        if not es:
            return None, r
    return None, {"raw": "页数过多"}


def main():
    if not os.path.exists(IMGCMD):
        print("⏭ 没有 build/tools/imgcmd")
        return 77
    feats = json.loads(subprocess.run([IMGCMD, "--list"], capture_output=True,
                                      text=True).stdout).get("features", {})
    if not feats.get("ntfs3g"):
        print("⏭ 缺 ntfs3g 或 libarchive")
        return 77

    W = tempfile.mkdtemp(prefix="srchall")
    try:
        # ══════ A. NTFS 深树 ══════
        print("════ A. NTFS 深树（4 层 × 4 分支）════")
        src = os.path.join(W, "src", "tree")
        os.makedirs(src)
        nd, nf, nhit = make_tree(src, 4, 4, 3)
        print("     素材：%d 目录 + %d 文件（命中 %d）" % (nd, nf, nhit))

        img = os.path.join(W, "d.qcow2")
        r = run("img.create", "--out=" + img, "--size-gb=2", "--table=mbr",
                "--sys-mb=1500", "--esp-mb=0", "--rec-mb=0", "--limits=off")
        check("A0 建盘", r.get("ok"), r)
        lba = biggest_lba(img)
        r = run("fs.format", "--img=" + img, "--lba=%d" % lba, "--fs=ntfs")
        check("A0 NTFS 格式化", r.get("ok"), r)
        r = run("copy", "host:" + os.path.join(W, "src", "tree"),
                "%s!lba%d:/tree" % (img, lba), "--conflict=overwrite")
        check("A0 写入 %d 个文件" % nf, r.get("ok"), r)

        spec = "%s!lba%d:/" % (img, lba)
        # ① 过滤 + total
        r = search(spec, q="targeta", limit=100)
        d = r.get("data") or {}
        check("A① q 过滤成功", r.get("ok"), r)
        check("A① total=%s（期望 %d，过滤后计数）" % (d.get("total"), nhit), d.get("total") == nhit,
              d.get("total"))
        check("A① count=%s / has_more=true" % d.get("count"),
              d.get("count") == 100 and d.get("has_more") is True, (d.get("count"), d.get("has_more")))
        check("A① kind=ntfs / 带 token",
              d.get("kind") == "ntfs" and bool(d.get("token")), d.get("kind"))
        bad = [e.get("path") for e in (d.get("entries") or []) if "targeta" not in e.get("path", "").lower()]
        check("A① 本页每条 path 都含关键字", not bad, bad[:3])

        # ② 大小写
        r2 = search(spec, q="TARGETA", limit=1)
        check("A② q=TARGETA 结果相同（大小写不敏感）",
              (r2.get("data") or {}).get("total") == nhit, (r2.get("data") or {}).get("total"))

        # ③ 分页拼接（先过滤后分页：每次带 spec + offset，CLI 下 token 必过期）
        out, off, pages = [], 0, 0
        while True:
            r = search(spec, q="targeta", limit=200, offset=off)
            dd = r.get("data") or {}
            es = dd.get("entries") or []
            out += [e.get("path") for e in es]
            pages += 1
            if not dd.get("has_more"):
                break
            off += len(es)
            if not es or pages > 20:
                break
        check("A③ 分页拼接 %d 条（期望 %d，%d 页）" % (len(out), nhit, pages),
              len(out) == nhit and pages == 3, (len(out), pages))
        check("A③ 无重复", len(set(out)) == len(out), len(out) - len(set(out)))

        # ④ 全量递归（不带 q）
        r = search("%s!lba%d:/" % (img, lba), limit=1)
        d = r.get("data") or {}
        total_all = nd + nf + 1        # + /tree 自己（spec 给的是根 /）
        check("A④ 不带 q = 全量递归 total=%s（期望 %d）" % (d.get("total"), total_all),
              d.get("total") == total_all, d.get("total"))
        check("A④ depth 字段存在（0=直接子项）", (d.get("entries") or [{}])[0].get("depth") == 0,
              (d.get("entries") or [{}])[0])

        # ⑤ 不递归
        r = search("%s!lba%d:/tree" % (img, lba), recursive="false", limit=10)
        d = r.get("data") or {}
        check("A⑤ --recursive=false 只当前层 total=%s（期望 4）" % d.get("total"),
              d.get("total") == 4, [e.get("path") for e in (d.get("entries") or [])])

        # ⑥ 空结果 / 坏 spec / 路径不存在
        r = search(spec, q="zzz_nothing_zzz", limit=10)
        d = r.get("data") or {}
        check("A⑥ 空结果 ok=true / total=0 / entries=[]",
              r.get("ok") and d.get("total") == 0 and d.get("entries") == [], r)
        r = search("!!!", q="x")
        check("A⑥ 坏 spec → CMD-ARG-BAD",
              not r.get("ok") and (r.get("err") or {}).get("error_code") == "CMD-ARG-BAD", r)
        r = search("%s!lba%d:/no_such_dir" % (img, lba), q="x")
        check("A⑥ 路径不存在 → SEARCH-NOTFOUND",
              not r.get("ok") and (r.get("err") or {}).get("error_code") == "SEARCH-NOTFOUND", r)

        # ⑦ 跨进程 token
        tok = (search(spec, q="targeta", limit=1).get("data") or {}).get("token")
        r = run("img.search", "--token=" + (tok or "x"), "--limit=10")
        check("A⑦ 跨进程 token → TOKEN-EXPIRED",
              not r.get("ok") and (r.get("err") or {}).get("error_code") == "TOKEN-EXPIRED", r)

        # ══════ B1. FAT32 ══════
        print("════ B1. FAT32 ════")
        fsrc = os.path.join(W, "fat", "t2")
        os.makedirs(os.path.join(fsrc, "sub"))
        for nm in ("TargetA_fat.txt", "targetA_two.txt", "plain1.txt", "plain2.txt"):
            open(os.path.join(fsrc, nm), "w").write("x")
        open(os.path.join(fsrc, "sub", "TargetA_three.txt"), "w").write("x")
        fimg = os.path.join(W, "f.qcow2")
        run("img.create", "--out=" + fimg, "--size-gb=1", "--table=mbr",
            "--sys-mb=300", "--esp-mb=0", "--rec-mb=0", "--limits=off")
        flba = biggest_lba(fimg)
        run("fs.format", "--img=" + fimg, "--lba=%d" % flba, "--fs=fat32")
        r = run("copy", "host:" + os.path.join(W, "fat", "t2"),
                "%s!lba%d:/t2" % (fimg, flba), "--conflict=overwrite")
        check("B1 写入 FAT", r.get("ok"), r)
        r = search("%s!lba%d:/" % (fimg, flba), q="TARGETA", limit=10)
        d = r.get("data") or {}
        check("B1 FAT 搜索 total=%s（期望 3）" % d.get("total"), d.get("total") == 3, r)

        # ══════ B2. ISO ══════
        iso = None
        print("════ B2. ISO ════")
        if subprocess.run(["which", "xorrisofs"], capture_output=True).returncode == 0:
            isrc = os.path.join(W, "isrc")
            os.makedirs(os.path.join(isrc, "d1"))
            open(os.path.join(isrc, "TargetA_iso.txt"), "w").write("x")
            open(os.path.join(isrc, "d1", "targeta_iso2.txt"), "w").write("x")
            open(os.path.join(isrc, "d1", "nope.txt"), "w").write("x")
            iso = os.path.join(W, "t.iso")
            subprocess.run(["xorrisofs", "-o", iso, isrc], capture_output=True)
            r = search("%s!:/" % iso, q="targeta", limit=10)
            d = r.get("data") or {}
            check("B2 ISO 搜索 total=%s（期望 2）" % d.get("total"), r.get("ok") and d.get("total") == 2, r)
            r = search("%s!:/" % iso, limit=100)
            check("B2 ISO 全量列出 total=%s（期望 4：1 目录 + 3 文件）" % (r.get("data") or {}).get("total"),
                  (r.get("data") or {}).get("total") == 4, r)
        else:
            print("  ⏭ 无 xorrisofs，跳过 ISO")

        # ══════ B3. ZIP ══════
        print("════ B3. ZIP ════")
        zp = os.path.join(W, "t.zip")
        with zipfile.ZipFile(zp, "w") as z:
            for i in range(1, 301):
                nm = "/d%d/TargetA_%03d.txt" % (i % 7, i) if i % 10 == 0 else "/d%d/n%03d.txt" % (i % 7, i)
                z.writestr(nm, "x")
        r = search("%s!:/" % zp, q="targeta", limit=5)
        d = r.get("data") or {}
        check("B3 zip 搜索 total=%s（期望 30）" % d.get("total"),
              r.get("ok") and d.get("total") == 30, r)
        check("B3 kind=archive", d.get("kind") == "archive", d.get("kind"))
        hitzip, _ = walk_all_paths("%s!:/" % zp, page=10, q="targeta")
        expect = set()
        with zipfile.ZipFile(zp) as z:
            for nm in z.namelist():
                if "targeta" in nm.lower():
                    expect.add(nm)
        check("B3 带 q 分页拼接 %d 条，与本地筛选对账" % len(hitzip or []),
              hitzip is not None and set(hitzip) == expect,
              "差 %d" % (len(expect.symmetric_difference(set(hitzip or [])))))

        # ══════ B4. HOST ══════
        print("════ B4. HOST ════")
        r = search("host:" + src, q="targeta", limit=3)
        d = r.get("data") or {}
        check("B4 宿主目录搜索 total=%s（期望 %d）" % (d.get("total"), nhit),
              r.get("ok") and d.get("total") == nhit and d.get("kind") == "host", r)
        check("B4 path 是绝对宿主路径", (d.get("entries") or [{}])[0].get("path", "").startswith(src),
              (d.get("entries") or [{}])[0].get("path"))

        # ══════ B5. WIM ══════
        print("════ B5. WIM ════")
        if feats.get("wimlib"):
            wim = os.path.join(W, "o.wim")
            r = run("wim.capture", "--img=" + img, "--lba=%d" % lba,
                    "--path=/tree/l1_0", "--out=" + wim, "--comp=xpress")
            check("B5 wim.capture 一个子树", r.get("ok"), r)
            r = search("%s!1:/" % wim, q="targeta", limit=5)
            d = r.get("data") or {}
            check("B5 WIM 搜索 total=%s（期望 128）" % d.get("total"),
                  r.get("ok") and d.get("total") == 128, r)
            check("B5 kind=wim", d.get("kind") == "wim", d.get("kind"))
        else:
            print("  ⏭ 无 wimlib，跳过 WIM")

        # ══════ C. 会话隔离（同进程）══════
        print("════ C. 会话隔离（search_sess_test，同进程）════")
        if os.path.exists(SESS):
            p = subprocess.run([SESS, img, str(lba), zp], capture_output=True, text=True)
            print(p.stdout.rstrip())
            check("C 会话隔离测试全绿", p.returncode == 0, "见上方输出")
        else:
            print("  ⏭ 没有 build/tests/search_sess_test")
    finally:
        shutil.rmtree(W, ignore_errors=True)

    print("\n════════ search-all-test：通过 %d · 失败 %d ════════" % (passed, failed))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())