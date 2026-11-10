#!/usr/bin/env python3
"""qcow2-zstd-test.py —— qcow2 的 zstd 压缩簇能不能正确解出来

背景（2026-11-10）：qcow2 头里 offset 104 的 compression_type 是【1 字节】
（0=deflate，1=zstd）。以前按 2 字节大端读，zstd 镜像读到的是 0x0100，
于是恒被当成 zlib 解 —— 解出来是错的，且报的是笼统的「数据损坏」。

用例：
  ① 本构建有没有 zstd 能力（--list 的 features.zstd）
  ② 用 qemu-img 把一张我们建好的盘压成 zstd 簇
  ③ 我们的工具能打开它，且导出的文件和原始字节完全一致
  ④ 对照：同一张盘压成 zlib，行为一致（防止只为 zstd 打补丁）

缺 zstd 能力、缺 qemu-img、或 qemu 不支持 compression_type 时 → 跳过（exit 77）。
"""
import json
import os
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
IMGCMD = os.path.join(ROOT, "build", "tools", "imgcmd")

passed = 0
failed = 0


def check(cond, name, detail=""):
    global passed, failed
    if cond:
        passed += 1
        print("  ✅ " + name)
    else:
        failed += 1
        print("  ❌ " + name + ("  " + str(detail)[:220] if detail else ""))


def run(*args, **kw):
    p = subprocess.run([IMGCMD] + list(args), capture_output=True, text=True, **kw)
    try:
        return json.loads(p.stdout)
    except Exception:
        return {"ok": False, "raw": (p.stdout or "") + (p.stderr or "")}


def main():
    if not os.path.exists(IMGCMD):
        print("⏭ 没有 build/tools/imgcmd")
        return 77

    # ① 能力
    info = subprocess.run([IMGCMD, "--list"], capture_output=True, text=True)
    try:
        feats = json.loads(info.stdout)["features"]
    except Exception:
        print("⏭ 读不到 --list 的 features")
        return 77
    if not feats.get("zstd"):
        print("⏭ 本构建没有 zstd 能力（没找到 libzstd）")
        return 77
    check(True, "features.zstd = true")

    if not shutil.which("qemu-img"):
        print("⏭ 没有 qemu-img，造不出 zstd 夹具")
        return 77

    W = tempfile.mkdtemp(prefix="zstdqcow")
    try:
        img = os.path.join(W, "a.qcow2")
        payload = os.path.join(W, "payload.bin")
        out = os.path.join(W, "out.bin")
        with open(payload, "wb") as f:
            f.write(os.urandom(300 * 1024))

        print("════ ① 建盘 + 写入 ════")
        run("img.create", "--out=" + img, "--size-gb=1", "--table=gpt",
            "--esp-mb=128", "--sys-mb=0", "--rec-mb=0", "--limits=off")
        pts = run("part.show", "--img=" + img).get("data", {}).get("pt", {}).get("parts", [])
        esp = [p["first"] for p in pts if p["typeGuid"].startswith("c12a7328")]
        check(bool(esp), "GPT 里有 ESP 分区", pts)
        if not esp:
            return 1
        lba = esp[0]
        r = run("fs.format", "--img=" + img, "--lba=%d" % lba, "--fs=fat32")
        check(r.get("ok"), "ESP 格式化成 FAT32", r)
        r = run("fat.put", "--img=" + img, "--lba=%d" % lba,
                "--src=" + payload, "--dst=/p.bin")
        check(r.get("ok"), "写入 300 KiB 文件", r)

        src_md5 = subprocess.run(["md5sum", payload], capture_output=True, text=True).stdout.split()[0]

        for ctype in ("zstd", "zlib"):
            print("════ %s：压缩后读回 ════" % ctype)
            comp = os.path.join(W, "c-%s.qcow2" % ctype)
            cv = subprocess.run(
                ["qemu-img", "convert", "-O", "qcow2", "-c",
                 "-o", "compression_type=%s" % ctype, img, comp],
                capture_output=True, text=True)
            if cv.returncode != 0:
                if ctype == "zstd":
                    print("⏭ qemu-img 不支持 compression_type=zstd：" + cv.stderr.strip()[:120])
                    return 77
                check(False, "qemu-img 压成 " + ctype, cv.stderr)
                continue
            check(True, "qemu-img 压成 %s 簇" % ctype)

            r = run("img.info", "--img=" + comp)
            check(r.get("ok"), "%s 镜像能被打开" % ctype, r)

            if os.path.exists(out):
                os.unlink(out)
            r = run("fat.export", "--img=" + comp, "--lba=%d" % lba,
                    "--path=/p.bin", "--dst=" + out)
            check(r.get("ok"), "%s 镜像里能导出文件" % ctype, r)
            if os.path.exists(out):
                got = subprocess.run(["md5sum", out], capture_output=True, text=True).stdout.split()[0]
                check(got == src_md5, "%s 导出内容与原始一致" % ctype, (src_md5, got))
            else:
                check(False, "%s 导出文件不存在" % ctype)
    finally:
        shutil.rmtree(W, ignore_errors=True)

    print("\n════════ qcow2-zstd-test：通过 %d · 失败 %d ════════" % (passed, failed))
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())