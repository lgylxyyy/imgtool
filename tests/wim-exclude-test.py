#!/usr/bin/env python3
"""wim-exclude-test.py —— wim.capture 的 --exclude 与三段进度（REQ-67 回归）

抓四件事：
  ① estimate 与实捕【同一套】排除规则（以前一个走 img_du、一个走 img_xcopy，
     于是"算 11.7 GB、实捕 7.8 GB"）；
  ② 默认名单（pagefile.sys / hiberfil.sys / swapfile.sys）默认生效，能关；
  ③ 被排掉的条目要计数（data.excluded），App 结果卡写"跳过 N 项"；
  ④ 进度要覆盖 extract / pack / write 三段，pct 在同一段内不倒退。
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


def check(cond, name, detail=""):
    global passed, failed
    if cond:
        passed += 1
        print("  ✅ " + name)
    else:
        failed += 1
        print("  ❌ " + name + ("  " + str(detail)[:260] if detail else ""))


def run(*args):
    p = subprocess.run([IMGCMD] + list(args), capture_output=True, text=True,
                       errors="replace")
    try:
        return json.loads(p.stdout)
    except Exception:
        return {"ok": False, "raw": (p.stdout or "")[:200] + (p.stderr or "")[:200]}


def run_progress(*args):
    """跑一条命令，同时收集 JSON 进度事件。"""
    p = subprocess.run([IMGCMD] + list(args) + ["--progress=json"],
                       capture_output=True, text=True, errors="replace")
    events, result = [], None
    for line in (p.stdout or "").splitlines():
        line = line.strip()
        if not line.startswith("{"):
            continue
        try:
            d = json.loads(line)
        except Exception:
            continue
        if d.get("ev") == "progress":
            events.append(d)
        elif d.get("cmd"):
            result = d
    return events, result


def main():
    if not os.path.exists(IMGCMD):
        print("⏭ 没有 build/tools/imgcmd")
        return 77
    feats = json.loads(subprocess.run([IMGCMD, "--list"], capture_output=True,
                                      text=True).stdout)["features"]
    if not (feats.get("ntfs3g") and feats.get("wimlib")):
        print("⏭ 缺 ntfs3g 或 wimlib")
        return 77

    W = tempfile.mkdtemp(prefix="wimexcl")
    try:
        img = os.path.join(W, "s.qcow2")
        tree = os.path.join(W, "tree")
        os.makedirs(os.path.join(tree, "Windows", "System32"))
        MiB = 1024 * 1024
        with open(os.path.join(tree, "pagefile.sys"), "wb") as f:
            f.write(b"\0" * (8 * MiB))
        with open(os.path.join(tree, "hiberfil.sys"), "wb") as f:
            f.write(b"\0" * (4 * MiB))
        with open(os.path.join(tree, "swapfile.sys"), "wb") as f:
            f.write(b"\0" * (1 * MiB))
        with open(os.path.join(tree, "Windows", "System32", "k.bin"), "wb") as f:
            f.write(os.urandom(2 * MiB))
        with open(os.path.join(tree, "Windows", "System32", "note.txt"), "wb") as f:
            f.write(b"hello\n")

        r = run("img.create", "--out=" + img, "--size-gb=1", "--table=mbr",
                "--sys-mb=700", "--esp-mb=0", "--rec-mb=0", "--limits=off")
        parts = (r.get("data") or {}).get("pt", {}).get("parts", [])
        lba = sorted(parts, key=lambda p: p.get("sectors", 0), reverse=True)[0]["first"]
        r = run("fs.format", "--img=" + img, "--lba=%d" % lba, "--fs=ntfs")
        check(r.get("ok"), "建盘 + 格式化 NTFS", r)

        r = run("copy", "host:" + tree, img + "!lba%d:/cap" % lba,
                "--conflict=overwrite")
        check(r.get("ok"), "写入 5 个文件（3 个默认可省 + 2 个真文件）", r)

        # ① 默认名单生效
        r = run("wim.capture", "--img=" + img, "--lba=%d" % lba, "--path=/cap",
                "--estimate=true")
        d = r.get("data") or {}
        check(r.get("ok") and d.get("files") == 2,
              "① 默认排除后 estimate files=2（旧行为 5）", (r.get("ok"), d.get("files")))
        check(d.get("excluded") == 3, "③ estimate 报 excluded=3（三个默认可省文件）",
              d.get("excluded"))
        check(d.get("bytes_needed") == 2 * MiB + 6,
              "① bytes_needed 只算两个真文件（%d）" % (2 * MiB + 6), d.get("bytes_needed"))
        est_bytes = d.get("bytes_needed")

        # ② 能关
        r = run("wim.capture", "--img=" + img, "--lba=%d" % lba, "--path=/cap",
                "--estimate=true", "--exclude-default=false")
        d = r.get("data") or {}
        check(r.get("ok") and d.get("files") == 5 and d.get("excluded") == 0,
              "② --exclude-default=false 时 5 个文件全算、excluded=0",
              (d.get("files"), d.get("excluded")))
        check((d.get("bytes_needed") or 0) > 13 * MiB,
              "② 关掉后 bytes_needed 回到 13 MB 量级", d.get("bytes_needed"))

        # ②b 自定义通配与默认名单叠加
        r = run("wim.capture", "--img=" + img, "--lba=%d" % lba, "--path=/cap",
                "--estimate=true", "--exclude=*.bin")
        d = r.get("data") or {}
        check(r.get("ok") and d.get("files") == 1 and d.get("excluded") == 4,
              "② --exclude=*.bin 与默认名单叠加：files=1 excluded=4",
              (d.get("files"), d.get("excluded")))

        # ④ 实捕：三段进度 + 口径一致
        out = os.path.join(W, "o.wim")
        events, res = run_progress("wim.capture", "--img=" + img, "--lba=%d" % lba,
                                   "--path=/cap", "--out=" + out, "--comp=xpress",
                                   "--capture-acl=false")
        d = (res or {}).get("data") or {}
        check((res or {}).get("ok"), "④ 捕获成功", res)
        check(d.get("files") == 2 and d.get("excluded") == 3,
              "④ 实捕 files=2 excluded=3（与 estimate 同口径）",
              (d.get("files"), d.get("excluded")))
        check(d.get("bytes_in") == est_bytes,
              "① 实捕 bytes_in 与 estimate bytes_needed 完全相等（%s）" % est_bytes,
              (d.get("bytes_in"), est_bytes))

        phases = [e.get("phase") for e in events]
        for ph in ("extract", "pack", "write"):
            check(ph in phases, "④ 进度里有 phase=%s（旧代码后两段是黑的）" % ph,
                  sorted(set(phases)))

        # REQ-70 §2：pack 的 item 必须是 WIM 内路径，不能带宿主临时目录前缀
        pack_items = [e.get("item") for e in events
                      if e.get("phase") == "pack" and e.get("item")]
        check(all(i.startswith("/") for i in pack_items) and
              not any("imgtool-tmp" in i for i in pack_items),
              "pack.item 是 WIM 内路径（无宿主临时目录前缀）", pack_items[:2])

        # REQ-70 Q3：write 段不发 item（临时目录根没信息量）
        write_items = [e.get("item") for e in events
                       if e.get("phase") == "write" and e.get("item")]
        check(not write_items, "write 段 item 为空", write_items[:2])

        # pct 在同一段内不倒退
        bad = []
        last, lastp = None, -1
        for e in events:
            if e.get("phase") != last:
                last, lastp = e.get("phase"), -1
            p = e.get("pct", -1)
            if p >= 0 and p < lastp:
                bad.append((e.get("phase"), lastp, p))
            lastp = max(lastp, p)
        check(not bad, "④ 同一段内 pct 单调不减", bad[:3])

        # 产物里确实没有 pagefile
        r = run("wim.ls", "--wim=" + out, "--index=1", "--path=/")
        names = [x.get("name") for x in (r.get("data") or {}).get("entries", [])]
        check("pagefile.sys" not in names and "hiberfil.sys" not in names,
              "① 产物 WIM 里没有 pagefile.sys / hiberfil.sys", names)
    finally:
        shutil.rmtree(W, ignore_errors=True)

    print("\n════════ wim-exclude-test：通过 %d · 失败 %d ════════" % (passed, failed))
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())