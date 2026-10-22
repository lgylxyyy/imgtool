#!/usr/bin/env python3
# fat-fs-test.py — FAT32 盘内文件操作端到端（fat.put / fat.move / fat.rm）
#
# 为什么要有它：fat.move 是新能力（改 FAT 目录项 + LFN + 目录的 ".."），
#   只靠"命令返回 ok"是自证；这里用【独立的 qcow2 + FAT32 解析器】从盘上
#   把目录项读回来，验证名字/簇/大小/时间戳，并用 mtools（若装了）做第三方交叉验证。
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
T = os.path.join(ROOT, "build", "tools")
IMGCMD = os.path.join(T, "imgcmd")
ESP_LBA = 2048

passed = failed = 0


def ok(name, extra=""):
    global passed
    passed += 1
    print("  ✅ %s%s" % (name, ("  — " + extra) if extra else ""))


def ng(name, why):
    global failed
    failed += 1
    print("  ❌ %s\n       %s" % (name, why))


def check(cond, name, why=""):
    ok(name) if cond else ng(name, why)
    return cond


def run(args, want_ok=True):
    p = subprocess.run([IMGCMD] + args, capture_output=True, text=True)
    out = p.stdout.strip().splitlines()
    try:
        j = json.loads(out[-1]) if out else {}
    except Exception:
        j = {"raw": p.stdout + p.stderr}
    if want_ok and not j.get("ok"):
        raise RuntimeError("命令失败: %s\n%s" % (" ".join(args), json.dumps(j, ensure_ascii=False)))
    return j


# ---------------- 独立 qcow2 读取器（只支持标准簇，无压缩；够读我们自己写的盘） ----------------
class QCow2:
    def __init__(self, path):
        self.f = open(path, "rb")
        h = self.f.read(112)
        magic, ver = struct.unpack(">II", h[:8])
        assert magic == 0x514649FB, "不是 qcow2"
        self.cluster_bits = struct.unpack(">I", h[20:24])[0]
        self.size = struct.unpack(">Q", h[24:32])[0]
        self.l1_size = struct.unpack(">I", h[36:40])[0]
        self.l1_off = struct.unpack(">Q", h[40:48])[0]
        self.cs = 1 << self.cluster_bits
        self.l1 = self._at(self.l1_off, self.l1_size * 8)

    def _at(self, off, n):
        self.f.seek(off)
        return self.f.read(n)

    def read(self, off, n):
        out = bytearray()
        while n > 0:
            cidx = off >> self.cluster_bits
            coff = off & (self.cs - 1)
            take = min(n, self.cs - coff)
            l1i = cidx >> (self.cluster_bits - 3)     # 一个 L2 表覆盖 2^(bits-3) 个簇
            l2e = struct.unpack(">Q", self.l1[l1i * 8:l1i * 8 + 8])[0]
            l2off = l2e & 0x00FFFFFFFFFFFE00
            if l2off == 0:
                out += b"\x00" * take
            else:
                l2 = self._at(l2off, self.cs)
                i = cidx & (self.cs // 8 - 1)
                e = struct.unpack(">Q", l2[i * 8:i * 8 + 8])[0]
                doff = e & 0x00FFFFFFFFFFFE00
                out += b"\x00" * take if doff == 0 else self._at(doff + coff, take)
            off += take
            n -= take
        return bytes(out)


# ---------------- 独立 FAT32 读取器 ----------------
class Fat32:
    def __init__(self, q, lba):
        self.q = q
        self.base = lba * 512
        bs = q.read(self.base, 512)
        assert bs[0x52:0x5A] == b"FAT32   ", "不是 FAT32"
        self.bps = struct.unpack("<H", bs[11:13])[0]
        self.spc = bs[13]
        self.reserved = struct.unpack("<H", bs[14:16])[0]
        self.nfats = bs[16]
        self.fatsz = struct.unpack("<I", bs[36:40])[0]
        self.root = struct.unpack("<I", bs[44:48])[0]
        self.csize = self.spc * self.bps
        self.fat_off = self.base + self.reserved * self.bps
        self.data_off = self.base + (self.reserved + self.nfats * self.fatsz) * self.bps

    def fat(self, c):
        return struct.unpack("<I", self.q.read(self.fat_off + c * 4, 4))[0] & 0x0FFFFFFF

    def clus(self, c):
        return self.data_off + (c - 2) * self.csize

    def entries(self, clu):
        out, c, guard = [], clu, 0
        while 2 <= c < 0x0FFFFFF8 and guard < 20000:
            guard += 1
            data = self.q.read(self.clus(c), self.csize)
            lfn = {}
            for i in range(0, self.csize, 32):
                e = data[i:i + 32]
                if e[0] == 0:
                    return out
                if e[0] == 0xE5:
                    lfn = {}
                    continue
                if e[11] & 0x0F == 0x0F:
                    o = e[0] & 0x1F
                    if 1 <= o <= 20:
                        raw = e[1:11] + e[14:26] + e[28:32]
                        s = ""
                        for k in range(0, 26, 2):
                            ch = struct.unpack("<H", raw[k:k + 2])[0]
                            if ch == 0:
                                break
                            if ch != 0xFFFF:
                                s += chr(ch)
                        lfn[o] = s
                    continue
                if e[11] & 0x08:      # 卷标
                    lfn = {}
                    continue
                short = e[0:8].decode("ascii", "replace").rstrip()
                if e[8:11].strip():
                    short += "." + e[8:11].decode("ascii", "replace").rstrip()
                name = "".join(lfn[k] for k in sorted(lfn)) if lfn else short
                cl = (struct.unpack("<H", e[20:22])[0] << 16) | struct.unpack("<H", e[26:28])[0]
                out.append(dict(name=name, short=short, dir=bool(e[11] & 0x10),
                                cluster=cl, size=struct.unpack("<I", e[28:32])[0],
                                attr=e[11], wdate=struct.unpack("<H", e[24:26])[0],
                                wtime=struct.unpack("<H", e[22:24])[0], lfn=bool(lfn)))
                lfn = {}
            c = self.fat(c)
        return out

    def find(self, path):
        if path in ("/", ""):
            return dict(name="/", dir=True, cluster=self.root, size=0)
        cur = self.root
        hit = None
        for part in [p for p in path.split("/") if p]:
            hit = None
            for e in self.entries(cur):
                if e["name"].upper() == part.upper():
                    hit = e
                    break
            if not hit:
                return None
            cur = hit["cluster"] if hit["dir"] else 0
        return hit

    def dotdot(self, clu):
        """读子目录首簇第二项（".."）→ 父簇号（根记 0）"""
        e = self.q.read(self.clus(clu) + 32, 32)
        assert e[0:2] == b"..", "首簇第二项不是 '..'"
        return (struct.unpack("<H", e[20:22])[0] << 16) | struct.unpack("<H", e[26:28])[0]


def main():
    if not os.access(IMGCMD, os.X_OK):
        print("❌ 找不到 %s（先 cmake --build build）" % IMGCMD)
        return 1
    W = tempfile.mkdtemp(prefix="fattest")
    img = os.path.join(W, "t.qcow2")

    def cmd(*a, **kw):
        return run(list(a), **kw)

    try:
        subprocess.run([os.path.join(T, "imglayout"), "mk", img, str(96 * 1024 * 1024),
                        "gpt", "--esp", "48"], check=True, capture_output=True)
        subprocess.run([os.path.join(T, "imgmkfs"), img, "--mkfs", str(ESP_LBA), "T"],
                       check=True, capture_output=True)
        src = os.path.join(W, "src.bin")
        with open(src, "wb") as f:
            f.write(b"hello-fat-move" * 10)
        src_md5 = subprocess.run(["md5sum", src], capture_output=True, text=True).stdout.split()[0]

        echo = lambda: Fat32(QCow2(img), ESP_LBA)

        print("\n════ 1. put / 盘内改名 ════")
        cmd("fat.put", "--img=" + img, "--lba=%d" % ESP_LBA, "--src=" + src, "--dst=/EFI/a.txt")
        r = cmd("fat.ls", "--img=" + img, "--lba=%d" % ESP_LBA, "--path=/EFI")
        check(r["data"]["count"] == 1 and r["data"]["entries"][0]["name"] == "a.txt",
              "put 后 fat.ls 看到 a.txt")
        f = echo()
        e0 = f.find("/EFI/a.txt")
        wdate0, wtime0 = e0["wdate"], e0["wtime"]

        r = cmd("fat.move", "--img=" + img, "--lba=%d" % ESP_LBA,
                "--src=/EFI/a.txt", "--dst=/EFI/Renamed.txt")
        check(r.get("ok"), "fat.move 改名成功")
        f = echo()
        check(f.find("/EFI/a.txt") is None, "旧名已消失")
        ne = f.find("/EFI/Renamed.txt")
        check(ne is not None, "新名存在")
        check(ne and ne["size"] == os.path.getsize(src), "新条目大小正确",
              "size=%r" % (ne and ne["size"]))
        check(ne and (ne["wdate"], ne["wtime"]) == (wdate0, wtime0),
              "改名保住原时间戳（Windows 语义：改名不是新建）",
              "before=%04x/%04x after=%04x/%04x" % (wdate0, wtime0, ne["wdate"], ne["wtime"]))
        check(ne and ne["lfn"] and ne["name"] == "Renamed.txt",
              "新条目带 LFN（大小写保住）", ne and ne["name"])
        check(ne and ne["short"].upper().startswith("RENAMED"),
              "8.3 短名同时存在", ne and ne["short"])
        out = os.path.join(W, "a.out")
        cmd("fat.export", "--img=" + img, "--lba=%d" % ESP_LBA, "--path=/EFI/Renamed.txt",
            "--dst=" + out, "--progress=none")
        md5 = subprocess.run(["md5sum", out], capture_output=True, text=True).stdout.split()[0]
        check(md5 == src_md5, "改名后内容逐字节一致")

        print("\n════ 2. 跨目录 / 目录移动（含 \"..\" 同步）════")
        # 目标父目录必须先存在（工具不自动建目录 —— 与 Windows 的 move 语义一致）
        cmd("fat.put", "--img=" + img, "--lba=%d" % ESP_LBA,
            "--src=" + src, "--dst=/EFI/Microsoft/Boot/placeholder")
        cmd("fat.rm", "--img=" + img, "--lba=%d" % ESP_LBA, "--path=/EFI/Microsoft/Boot/placeholder")
        r = cmd("fat.move", "--img=" + img, "--lba=%d" % ESP_LBA,
                "--src=/EFI/Renamed.txt", "--dst=/NoSuchDir/b.txt", want_ok=False)
        check(r.get("err", {}).get("error_code") == "FAT-DEST-DIR-NOTFOUND",
              "目标父目录不存在 → FAT-DEST-DIR-NOTFOUND（不误报源丢了）",
              json.dumps(r, ensure_ascii=False))
        cmd("fat.move", "--img=" + img, "--lba=%d" % ESP_LBA,
            "--src=/EFI/Renamed.txt", "--dst=/EFI/Microsoft/Boot/b.txt")
        f = echo()
        check(f.find("/EFI/Microsoft/Boot/b.txt") is not None, "跨目录移动成功")
        check(f.find("/EFI/Renamed.txt") is None, "原位置已空")
        mclu = f.find("/EFI/Microsoft")["cluster"]
        bclu = f.find("/EFI/Microsoft/Boot")["cluster"]
        check(f.dotdot(bclu) == mclu, "新建子目录的 \"..\" 指向父目录", "%#x" % f.dotdot(bclu))

        cmd("fat.move", "--img=" + img, "--lba=%d" % ESP_LBA,
            "--src=/EFI/Microsoft", "--dst=/EFI/Moved")
        f = echo()
        mclu2 = f.find("/EFI/Moved")["cluster"]
        check(f.dotdot(bclu) == mclu2, "目录移动后子目录 \"..\" 跟着改到新父",
              "want=%#x got=%#x" % (mclu2, f.dotdot(bclu)))
        check(f.dotdot(mclu2) == f.find("/EFI")["cluster"],
              "被移动目录自己的 \"..\" 仍指向 /EFI")

        cmd("fat.move", "--img=" + img, "--lba=%d" % ESP_LBA, "--src=/EFI/Moved", "--dst=/Top")
        f = echo()
        tclu = f.find("/Top")["cluster"]
        check(f.dotdot(tclu) == 0, "移到根下后 \"..\" 记作 0（根没有簇号）", "%#x" % f.dotdot(tclu))
        check(f.find("/Top/Boot/b.txt") is not None, "移动后内部路径仍可解析")

        print("\n════ 3. 拒绝路径 ════")
        r = cmd("fat.move", "--img=" + img, "--lba=%d" % ESP_LBA,
                "--src=/Top/Boot/b.txt", "--dst=/Top", want_ok=False)
        check(r.get("err", {}).get("error_code") == "FAT-ENTRY-EXISTS",
              "目标已存在 → FAT-ENTRY-EXISTS", json.dumps(r, ensure_ascii=False))
        r = cmd("fat.move", "--img=" + img, "--lba=%d" % ESP_LBA,
                "--src=/Top", "--dst=/Top/Boot/inner", want_ok=False)
        check(r.get("err", {}).get("error_code") == "FAT-MOVE-BAD",
              "移进自己的子树 → FAT-MOVE-BAD", json.dumps(r, ensure_ascii=False))
        r = cmd("fat.move", "--img=" + img, "--lba=%d" % ESP_LBA,
                "--src=/nope.txt", "--dst=/x.txt", want_ok=False)
        check(r.get("err", {}).get("error_code") == "FAT-ENTRY-NOTFOUND",
              "源不存在 → FAT-ENTRY-NOTFOUND")

        print("\n════ 4. 删除 ════")
        r = cmd("fat.rm", "--img=" + img, "--lba=%d" % ESP_LBA, "--path=/Top", want_ok=False)
        check(r.get("err", {}).get("error_code") == "FAT-DIR-NOT-EMPTY",
              "删非空目录 → FAT-DIR-NOT-EMPTY（不递归）")
        r = cmd("fat.rm", "--img=" + img, "--lba=%d" % ESP_LBA, "--path=/Top/Boot/b.txt")
        check(r.get("ok"), "删文件成功")
        f = echo()
        check(f.find("/Top/Boot/b.txt") is None, "盘上确实没有这个文件了")
        r = cmd("fat.rm", "--img=" + img, "--lba=%d" % ESP_LBA, "--path=/Top/Boot")
        check(r.get("ok"), "删空目录成功")
        r = cmd("fat.rm", "--img=" + img, "--lba=%d" % ESP_LBA, "--path=/Top/Boot",
                want_ok=False)
        check(r.get("err", {}).get("error_code") == "FAT-ENTRY-NOTFOUND",
              "删不存在的路径 → FAT-ENTRY-NOTFOUND")

        print("\n════ 5. 规模 / 别名 / 深路径 ════")
        many = os.path.join(W, "many.bin")
        open(many, "wb").write(b"x" * 33)
        for i in range(60):
            cmd("fat.put", "--img=" + img, "--lba=%d" % ESP_LBA,
                "--src=" + many, "--dst=/Many/file%03d.txt" % i)
        r = cmd("fat.ls", "--img=" + img, "--lba=%d" % ESP_LBA, "--path=/Many")
        names = [e["name"] for e in r["data"]["entries"]]
        check(len(names) == 60 and len(set(names)) == 60, "同目录 60 个文件不丢不串",
              "count=%d uniq=%d" % (len(names), len(set(names))))
        f = echo()
        real = [e for e in f.entries(f.find("/Many")["cluster"])
                if e["name"] not in (".", "..")]
        check(len(real) == 60, "独立解析器也数到 60 项（不含 . / ..）", "got=%d" % len(real))
        cmd("fat.put", "--img=" + img, "--lba=%d" % ESP_LBA, "--src=" + many, "--dst=/A/a.txt")
        cmd("fat.put", "--img=" + img, "--lba=%d" % ESP_LBA, "--src=" + many, "--dst=/B/a.txt")
        f = echo()
        check(f.find("/A/a.txt") and f.find("/B/a.txt") and
              f.find("/A/a.txt")["cluster"] != f.find("/B/a.txt")["cluster"],
              "同名文件在两个目录里各自独立（不共用簇）")
        deep = "/" + "/".join("D%d" % i for i in range(8)) + "/x.txt"
        cmd("fat.put", "--img=" + img, "--lba=%d" % ESP_LBA, "--src=" + many, "--dst=" + deep)
        f = echo()
        check(f.find(deep) is not None, "8 层深路径写入/解析正常")
        cmd("fat.move", "--img=" + img, "--lba=%d" % ESP_LBA, "--src=" + deep,
            "--dst=/D0/D1/moved.txt")
        f = echo()
        check(f.find("/D0/D1/moved.txt") is not None, "深路径文件也能跨目录移动")

        print("\n════ 6. 第三方交叉验证（mtools）════")
        have = shutil.which("qemu-img") and shutil.which("mdir")
        if not have:
            print("  ⏭ 跳过（没有 qemu-img / mtools）")
        else:
            raw = os.path.join(W, "t.raw")
            subprocess.run(["qemu-img", "convert", "-O", "raw", img, raw], check=True,
                           capture_output=True)
            p = subprocess.run(["mdir", "-i", "%s@@%d" % (raw, ESP_LBA * 512), "::/Many"],
                               capture_output=True, text=True)
            check(p.returncode == 0 and "FILE059" in p.stdout.upper(),
                  "mtools 独立读得懂我们写的目录（含 8.3 与 LFN）", p.stdout[:200])
            p = subprocess.run(["mdir", "-i", "%s@@%d" % (raw, ESP_LBA * 512), "::/A"],
                               capture_output=True, text=True)
            check(p.returncode == 0 and "a.txt" in p.stdout,
                  "mtools 看到小写 LFN 原样保留")

        print("\n════ 7. 结构自检 ════")
        r = cmd("qc.verify", "--img=" + img)
        check(r.get("ok"), "qcow2 结构自检通过", json.dumps(r, ensure_ascii=False)[:300])
        r = cmd("part.scan", "--img=" + img, "--lba=%d" % ESP_LBA)
        check(r.get("ok") and r["data"].get("fs") == "FAT32", "part.scan 仍认得出 FAT32")
    finally:
        if failed:
            print("\n（调试目录保留：%s）" % W)
        else:
            shutil.rmtree(W, ignore_errors=True)

    print("\n════════ fat-fs-test：通过 %d · 失败 %d ════════" % (passed, failed))
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())