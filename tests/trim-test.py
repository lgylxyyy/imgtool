#!/usr/bin/env python3
"""trim-test.py —— img.trim（REQ-66）回归

一期契约（见 docs/工作计划-2026-11-10-img-trim.md）：
  ① 只做 NTFS / FAT32；按卷自己的空闲位图释放 qcow2 簇
  ② 有快照 / 有 backing 的映像 → 拒绝，且【一个字节都不改】
  ③ 只释放 refcount==1 的簇；只释放完全落在分区内的簇
  ④ 默认打洞（宿主文件真的变小），不支持打洞只计数、不写零
  ⑤ 顺序必须是 L2=0 → fsync → refcount=0 → fsync → punch
     ⇒ 中断最多留"泄漏簇"，绝不能丢数据（本用例用 qemu-img check + 读回校验兜）
"""
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
IMGCMD = os.path.join(ROOT, "build", "tools", "imgcmd")
MiB = 1024 * 1024

passed = failed = 0


def check(cond, name, detail=""):
    global passed, failed
    if cond:
        passed += 1
        print("  ✅ " + name)
    else:
        failed += 1
        print("  ❌ " + name + ("  " + str(detail)[:240] if detail else ""))


def run(*args):
    p = subprocess.run([IMGCMD] + list(args) + ["--progress=none"],
                       capture_output=True, text=True, errors="replace")
    try:
        return json.loads(p.stdout)
    except Exception:
        return {"ok": False, "raw": (p.stdout or "")[:200] + (p.stderr or "")[:200]}


def md5_file(path):
    h = hashlib.md5()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def du_bytes(path):
    st = os.stat(path)
    return st.st_blocks * 512


def make_disk(path, size_gb, win_mb, fs):
    r = run("img.create", "--out=" + path, "--size-gb=%d" % size_gb, "--table=mbr",
            "--sys-mb=128", "--win-mb=%d" % win_mb, "--limits=off")
    parts = (r.get("data") or {}).get("pt", {}).get("parts", [])
    lba = sorted(parts, key=lambda p: p.get("sectors", 0), reverse=True)[0]["first"]
    r = run("fs.format", "--img=" + path, "--lba=%d" % lba, "--fs=" + fs)
    return (r.get("ok"), lba)


def qemu_img_check(path):
    if not shutil.which("qemu-img"):
        return None
    p = subprocess.run(["qemu-img", "check", path], capture_output=True, text=True)
    return p.stdout + p.stderr


def case_fs(W, fs, total_files, del_files, cluster_mb):
    """一个卷：写 N 个文件，删掉 del 个，trim 后剩下的必须完好。"""
    img = os.path.join(W, "t-%s.qcow2" % fs)
    ok, lba = make_disk(img, 2 if fs == "ntfs" else 1, 1500 if fs == "ntfs" else 600, fs)
    check(ok, "[%s] 建盘 + 格式化" % fs)
    if not ok:
        return

    src = os.path.join(W, "src-" + fs)
    os.makedirs(os.path.join(src, "del"))
    os.makedirs(os.path.join(src, "keep"))
    for i in range(total_files - del_files):
        with open(os.path.join(src, "keep", "k%02d.bin" % i), "wb") as f:
            f.write(os.urandom(8 * MiB))
    for i in range(del_files):
        with open(os.path.join(src, "del", "d%02d.bin" % i), "wb") as f:
            f.write(os.urandom(8 * MiB))

    r = run("copy", "host:" + src, "%s!lba%d:/" % (img, lba), "--conflict=overwrite")
    check(r.get("ok"), "[%s] 写入 %d MB" % (fs, total_files * 8), r)

    # 记住一个要保留的文件的 md5（从映像里读回来对比）
    keep_rel = "/src-%s/keep/k00.bin" % fs
    back = os.path.join(W, "back-%s.bin" % fs)
    run("copy", "%s!lba%d:%s" % (img, lba, keep_rel), "host:" + back)
    keep_md5 = md5_file(back) if os.path.exists(back) else None
    check(keep_md5 is not None, "[%s] 抽一个保留文件做基线" % fs)

    r = run("rm", "%s!lba%d:/src-%s/del" % (img, lba, fs), "--recursive=true")
    check(r.get("ok"), "[%s] 删掉一半文件" % fs, r)

    du0 = du_bytes(img)

    # ① dry-run：数字要有，文件一个字节不能变
    md5_before = md5_file(img)
    r = run("img.trim", "--img=" + img, "--lba=%d" % lba, "--dry-run=true")
    d = r.get("data") or {}
    check(r.get("ok") and d.get("clusters_freed", 0) >= del_files * 8 / cluster_mb,
          "[%s] dry-run 报 clusters_freed=%s（删了 %d MB）" % (fs, d.get("clusters_freed"), del_files * 8),
          d)
    check(md5_file(img) == md5_before and du_bytes(img) == du0,
          "[%s] dry-run 没改任何一个字节" % fs)

    # ② 真跑
    r = run("img.trim", "--img=" + img, "--lba=%d" % lba)
    d = r.get("data") or {}
    check(r.get("ok"), "[%s] trim 成功" % fs, r)
    check(d.get("punch_failed") == 0 and d.get("host_bytes_punched", 0) > 0,
          "[%s] 打洞成功 %s 字节" % (fs, d.get("host_bytes_punched")), d)
    check(du_bytes(img) < du0, "[%s] 宿主占用变小（%d → %d）" % (fs, du0, du_bytes(img)))

    # ③ 数据完好：保留文件 md5 不变、qemu 认、镜像自检过
    back2 = os.path.join(W, "back2-%s.bin" % fs)
    run("copy", "%s!lba%d:%s" % (img, lba, keep_rel), "host:" + back2)
    check(os.path.exists(back2) and md5_file(back2) == keep_md5,
          "[%s] 保留文件 md5 不变" % fs)
    out = qemu_img_check(img)
    if out is not None:
        check("No errors were found" in out, "[%s] qemu-img check 无错" % fs, out[-200:])
    r = run("qc.verify", "--img=" + img)
    check(r.get("ok"), "[%s] qc.verify 通过" % fs, r)

    # ④ 幂等
    r = run("img.trim", "--img=" + img, "--lba=%d" % lba, "--dry-run=true")
    check((r.get("data") or {}).get("clusters_freed") == 0, "[%s] 再跑一次 0 可释放（幂等）" % fs, r)


def main():
    if not os.path.exists(IMGCMD):
        print("⏭ 没有 build/tools/imgcmd")
        return 77
    feats = json.loads(subprocess.run([IMGCMD, "--list"], capture_output=True,
                                      text=True).stdout)["features"]
    if not feats.get("ntfs3g"):
        print("⏭ 缺 libntfs-3g")
        return 77

    W = tempfile.mkdtemp(prefix="trim")
    try:
        case_fs(W, "ntfs", 12, 8, 64)
        case_fs(W, "fat32", 8, 5, 64)

        # ⑤ 门禁：有快照 / 有 backing → 拒绝，且文件不变
        img = os.path.join(W, "gate.qcow2")
        ok, lba = make_disk(img, 1, 600, "ntfs")
        md5_before = md5_file(img)
        if shutil.which("qemu-img"):
            snap = os.path.join(W, "snap.qcow2")
            shutil.copyfile(img, snap)
            subprocess.run(["qemu-img", "snapshot", "-c", "s1", snap], capture_output=True)
            md5_snap = md5_file(snap)
            r = run("img.trim", "--img=" + snap, "--lba=%d" % lba)
            check(not r.get("ok") and "TRIM-NOTSUP" == (r.get("err") or {}).get("error_code"),
                  "有快照的映像被拒（TRIM-NOTSUP）", r)
            check(md5_file(snap) == md5_snap, "被拒时快照映像一个字节没改")

            ov = os.path.join(W, "ov.qcow2")
            subprocess.run(["qemu-img", "create", "-f", "qcow2", "-b", img, "-F", "qcow2", ov],
                           capture_output=True)
            r = run("img.trim", "--img=" + ov, "--lba=%d" % lba)
            check(not r.get("ok") and "TRIM-NOTSUP" == (r.get("err") or {}).get("error_code"),
                  "有 backing 的映像被拒（TRIM-NOTSUP）", r)
        check(md5_file(img) == md5_before, "门禁用例没碰别的文件")
    finally:
        shutil.rmtree(W, ignore_errors=True)

    print("\n════════ trim-test：通过 %d · 失败 %d ════════" % (passed, failed))
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())