#!/usr/bin/env python3
"""vhd-fault-test.py —— N6-3：VHD 写入在「任意一步失败」之后还剩下什么保证

思路：拿一个只在测试构建里编译的故障注入钩子（-DIMG_FAULT_INJECT=1，
见 src/img_io.c 的 fault_hit），在 VHD 追加块的第 k 次 read/write/sync/truncate
上人为失败（或直接 _exit 模拟被杀），然后重开映像，逐条判定：

  致命（失败即算 bug）
    ① 映像还能被打开
    ② qemu-img 能解析（尾部 footer 在正确位置）
    ③ 之前已经写好的文件，内容一字节都没变
  已知（只记录，不判失败）
    ④ 当次正在写的文件：可能完整、可能不存在、可能半写
       —— FAT 层的写入本来就不是事务性的，这一点要如实写进文档

用法：
  tests/vhd-fault-test.py            # 跑全部采样点
  tests/vhd-fault-test.py quick      # 只跑写入点 1..6（调试用）

缺 qemu-img → 跳过（exit 77）。构建失败 → 失败（不是跳过：那是真问题）。
"""
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = "/tmp/imgtool-fault"
T = os.path.join(BUILD, "tools")
IMGCMD = os.path.join(T, "imgcmd")

passed = failed = warned = 0


def check(cond, name, detail=""):
    global passed, failed
    if cond:
        passed += 1
        print("  ✅ " + name)
    else:
        failed += 1
        print("  ❌ " + name + ("  " + str(detail)[:200] if detail else ""))


def note(msg):
    global warned
    warned += 1
    print("  ⚠ " + msg)


def build():
    if os.path.exists(IMGCMD):
        return True
    print("════ 构建带故障注入的 imgcmd（一次性，约 40 s）════")
    os.makedirs(BUILD, exist_ok=True)
    cfg = subprocess.run(
        ["cmake", "-S", ROOT, "-B", BUILD, "-DCMAKE_BUILD_TYPE=Release",
         "-DCMAKE_C_FLAGS=-DIMG_FAULT_INJECT=1",
         "-DIMG_BUILD_TESTS=OFF", "-DIMG_BUILD_TOOLS=ON",
         "-DIMG_WITH_NTFS=OFF", "-DIMG_WITH_WIM=OFF", "-DIMG_WITH_ARCHIVE=OFF"],
        capture_output=True, text=True)
    if cfg.returncode != 0:
        print(cfg.stdout[-1500:] + cfg.stderr[-1500:])
        return False
    b = subprocess.run(["cmake", "--build", BUILD, "-j", str(os.cpu_count() or 4),
                        "--target", "imgcmd"], capture_output=True, text=True)
    if b.returncode != 0:
        print(b.stdout[-1500:] + b.stderr[-1500:])
        return False
    return os.path.exists(IMGCMD)


def run(*args, fault=None):
    env = dict(os.environ)
    env.pop("IMGTOOL_FAULT", None)
    if fault:
        env["IMGTOOL_FAULT"] = fault
    p = subprocess.run([IMGCMD] + list(args), capture_output=True, text=True, env=env)
    try:
        j = json.loads(p.stdout)
    except Exception:
        j = {"ok": False, "raw": (p.stdout or "")[:200] + (p.stderr or "")[:200]}
    return j, p.returncode


def md5(path):
    if not os.path.exists(path):
        return None
    h = hashlib.md5()
    with open(path, "rb") as f:
        for b in iter(lambda: f.read(65536), b""):
            h.update(b)
    return h.hexdigest()


def setup(W, img):
    """建一张 VHD（MBR + FAT32），写两个文件作为「已提交」基线。"""
    for f in list(os.listdir(W)):
        os.unlink(os.path.join(W, f)) if os.path.isfile(os.path.join(W, f)) else None
    r, _ = run("img.create", "--out=" + img, "--size-gb=1", "--format=vhd",
               "--table=mbr", "--esp-mb=0", "--sys-mb=0", "--rec-mb=0", "--limits=off")
    if not r.get("ok"):
        return None
    parts = r.get("data", {}).get("pt", {}).get("parts") or []
    # 取最大的那个分区当数据区（MBR/GPT 的字段名不同：first/last/sectors）
    cand = sorted(parts, key=lambda p: p.get("sectors", 0), reverse=True)
    lba = cand[0]["first"] if cand else None
    if lba is None:
        return None
    r, _ = run("fs.format", "--img=" + img, "--lba=%d" % lba, "--fs=fat32")
    if not r.get("ok"):
        return None
    base = {}
    for i in (1, 2):
        p = os.path.join(W, "f%d.bin" % i)
        with open(p, "wb") as f:
            f.write(bytes([i]) * (64 * 1024))
        run("fat.put", "--img=" + img, "--lba=%d" % lba, "--src=" + p, "--dst=/f%d.bin" % i)
        base["f%d.bin" % i] = md5(p)
    return lba, base


def verify(img, lba, base, W, tag, fault):
    """故障后重开：致命三条 + 记录第四条。"""
    r, _ = run("img.info", "--img=" + img)
    check(r.get("ok"), tag + "① 映像仍能打开", r)
    if not r.get("ok"):
        return
    qi = subprocess.run(["qemu-img", "info", img], capture_output=True, text=True)
    check(qi.returncode == 0, tag + "② qemu-img 能解析（尾部 footer 正确）", qi.stderr.strip()[:160])
    ok_all = True
    for name, want in base.items():
        out = os.path.join(W, "re-" + name)
        if os.path.exists(out):
            os.unlink(out)
        rr, _ = run("fat.export", "--img=" + img, "--lba=%d" % lba,
                    "--path=/" + name, "--dst=" + out)
        got = md5(out)
        if not (rr.get("ok") and got == want):
            ok_all = False
            check(False, tag + "③ 已提交的 %s 未受影响" % name, (rr.get("ok"), want, got))
    if ok_all:
        check(True, tag + "③ 已提交的两个文件内容未变")

    # ④ 当次写入的那个文件：只记录
    out = os.path.join(W, "re-f3.bin")
    if os.path.exists(out):
        os.unlink(out)
    rr, _ = run("fat.export", "--img=" + img, "--lba=%d" % lba, "--path=/f3.bin", "--dst=" + out)
    want3 = md5(os.path.join(W, "f3.bin"))
    got3 = md5(out)
    if not rr.get("ok"):
        note("%s 故障=%s → f3.bin 不存在（干净的失败）" % (tag, fault))
    elif got3 == want3:
        note("%s 故障=%s → f3.bin 完整" % (tag, fault))
    else:
        note("%s 故障=%s → f3.bin 半写（已知：FAT 写入非事务性）" % (tag, fault))


def main():
    quick = "quick" in sys.argv[1:]
    if not shutil.which("qemu-img"):
        print("⏭ 没有 qemu-img，无法交叉校验 footer")
        return 77
    if not build():
        print("❌ 带故障注入的构建失败")
        return 1

    W = tempfile.mkdtemp(prefix="vhdfault")
    img = os.path.join(W, "d.vhd")
    try:
        # 先量一下「一次追加写」大致有多少个写入点：取一个上限，够覆盖即可
        print("════ 采样点 ════")
        writes = list(range(1, 7 if quick else 13))
        syncs = [1, 2, 3] if quick else [1, 2, 3, 4]
        truncs = [1, 2]
        kills = [1, 3, 5]

        for kind, points in (("write", writes), ("sync", syncs), ("truncate", truncs)):
            for k in points:
                fault = "%s:%d" % (kind, k)
                print("════ %s ════" % fault)
                got = setup(W, img)
                if not got:
                    check(False, "前置建盘失败（%s）" % fault)
                    continue
                lba, base = got
                p3 = os.path.join(W, "f3.bin")
                with open(p3, "wb") as f:
                    f.write(os.urandom(300 * 1024))
                r, _ = run("fat.put", "--img=" + img, "--lba=%d" % lba,
                           "--src=" + p3, "--dst=/f3.bin", fault=fault)
                verify(img, lba, base, W, "%s " % fault, fault)

        for k in kills:
            fault = "write:%d:kill" % k
            print("════ %s ════" % fault)
            got = setup(W, img)
            if not got:
                check(False, "前置建盘失败（%s）" % fault)
                continue
            lba, base = got
            p3 = os.path.join(W, "f3.bin")
            with open(p3, "wb") as f:
                f.write(os.urandom(300 * 1024))
            run("fat.put", "--img=" + img, "--lba=%d" % lba,
                "--src=" + p3, "--dst=/f3.bin", fault=fault)
            verify(img, lba, base, W, "%s " % fault, fault)
    finally:
        shutil.rmtree(W, ignore_errors=True)

    print("\n════════ vhd-fault-test：通过 %d · 失败 %d · 记录 %d ════════"
          % (passed, failed, warned))
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())