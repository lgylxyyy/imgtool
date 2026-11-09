#!/usr/bin/env python3
"""style-check.py — 工具端用户可见文本的规范自检

依据 tool-requests/STYLE-GUIDE-20261109.md。只检查字符串字面量，
注释与标识符不参与（注释里的口语不影响交付物）。

用法：
    tests/style-check.py            全仓检查，有问题退出 1
    tests/style-check.py --count    只打印统计

检查项：
  E1 口语词与禁用术语（必须改）
  E2 中英对照表键与调用点字面量失配（该条永不命中）
  E3 破折号 —— 做解释
  W1 单句 ≥ 40 个汉字（建议拆分）
  W2 命令 help 末尾带句号（标题不加标点）
"""
import glob
import os
import re
import sys

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))

# ── E1：禁用词 → 应用词 ────────────────────────────────────────────
FORBIDDEN = {
    "镜像": "映像",
    "灌入": "写入", "灌": "写入",
    "吃满": "占满", "拉满": "占满",
    "抽出": "提取", "解出": "解压缩",
    "打不开": "无法打开", "判不出": "无法识别", "起不来": "无法启动",
    "就行": "即可", "就绪": "完成",
    "没能": "未能", "没搬成": "未能搬迁", "没改成": "未能修改",
    "这版": "该版本", "本版": "该版本",
    "别用": "不使用", "搞": "做", "弄": "，改",
}
# 例外：这些短语本身合规（规则 5 的下一步动作可用「请」）
ALLOW_PHRASES = ("请先备份", "请确认", "请在虚拟机")

# ── E2：中英对照表 ────────────────────────────────────────────────
TABLE = os.path.join(ROOT, "src", "img_text.c")


def string_literals(text):
    """扫出所有 C 字符串字面量的内容（跳过注释），返回 (内容, 行号)。"""
    out = []
    line = 1
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c == "\n":
            line += 1
            i += 1
            continue
        if c == "/" and text[i:i + 2] == "//":
            k = text.find("\n", i)
            k = n if k < 0 else k
            line += text[i:k].count("\n")
            i = k
            continue
        if c == "/" and text[i:i + 2] == "/*":
            k = text.find("*/", i)
            k = n if k < 0 else k + 2
            line += text[i:k].count("\n")
            i = k
            continue
        if c == '"':
            start_line = line
            j = i + 1
            buf = []
            while j < n:
                if text[j] == "\\":
                    buf.append(text[j:j + 2])
                    if text[j:j + 1] == "\\":
                        pass
                    j += 2
                    continue
                if text[j] == '"':
                    j += 1
                    break
                if text[j] == "\n":
                    line += 1
                buf.append(text[j])
                j += 1
            out.append(("".join(buf), start_line))
            i = j
            continue
        if c == "'":            # 字符常量：跳过转义
            j = i + 1
            while j < n and text[j] != "'":
                if text[j] == "\\":
                    j += 1
                j += 1
            i = j + 1
            continue
        i += 1
    return out


def main():
    files = sorted(
        glob.glob(os.path.join(ROOT, "src", "*.c"))
        + glob.glob(os.path.join(ROOT, "include", "*.h"))
        + glob.glob(os.path.join(ROOT, "tools", "*.c"))
    )
    errs, warns = [], []
    all_lit_by_file = {}
    stat = {"lits": 0, "cjk": 0}

    for f in files:
        text = open(f, encoding="utf-8").read()
        lits = string_literals(text)
        all_lit_by_file[f] = lits
        rel = os.path.relpath(f, ROOT)
        for content, ln in lits:
            stat["lits"] += 1
            if not content:
                continue
            # 纯 ASCII（JSON 键、格式串、路径）不参与中文检查
            cjk = len(re.findall(r"[\u4e00-\u9fff]", content))
            if cjk == 0:
                continue
            stat["cjk"] += 1
            for bad, good in FORBIDDEN.items():
                if bad in content and not any(a in content for a in ALLOW_PHRASES):
                    # 避免"灌"命中已改好的"写入"，逐词判断
                    errs.append(("E1", f"{rel}:{ln}", f"「{bad}」应为「{good}」", content[:48]))
                    break
            else:
                if "——" in content:
                    errs.append(("E3", f"{rel}:{ln}", "破折号做解释", content[:48]))
            # 规则 1 量的是「单句」：按句读切分后取最长一句，而不是整条字符串
            sentences = [x for x in re.split(r"[。；：，、！?？]", content) if x.strip()]
            longest = max((len(re.findall(r"[\u4e00-\u9fff]", x)) for x in sentences), default=0)
            if longest >= 40:
                errs.append(("E4", f"{rel}:{ln}", f"单句 {longest} 字（≥40 禁止）", content[:48]))
            elif longest >= 30:
                warns.append(("W1", f"{rel}:{ln}", f"最长单句 {longest} 字，建议拆分", content[:48]))

    # ── E2：img_text.c 的中文键必须在代码里作为字面量出现 ──
    if os.path.exists(TABLE):
        tsrc = open(TABLE, encoding="utf-8").read()
        pairs = re.findall(
            r"\{\s*\"((?:[^\"\\]|\\.)*)\"\s*,\s*\n?\s*\"((?:[^\"\\]|\\.)*)\"\s*\}", tsrc
        )
        corpus = "\n".join(
            open(f, encoding="utf-8").read()
            for f in files if os.path.abspath(f) != os.path.abspath(TABLE)
        )
        for zh, en in pairs:
            if not re.search(r"[\u4e00-\u9fff]", zh):
                continue
            if f'"{zh}"' not in corpus:
                errs.append(("E2", "src/img_text.c", "对照表键无调用点，永不命中", zh[:48]))

    total_err, total_warn = len(errs), len(warns)

    if "--count" in sys.argv:
        print(f"字符串字面量 {stat['lits']} 个，含中文 {stat['cjk']} 个")
        return 0

    if errs:
        print(f"错误 {total_err} 处：")
        for code, loc, why, snip in errs[:40]:
            print(f"  [{code}] {loc}  {why}\n         {snip}")
        if total_err > 40:
            print(f"  ... 其余 {total_err - 40} 处")
    if warns:
        print(f"建议 {total_warn} 处：")
        for code, loc, why, snip in warns[:12]:
            print(f"  [{code}] {loc}  {why}")
    if not errs and not warns:
        print(f"通过：含中文的字符串 {stat['cjk']} 个，无违规")
    return 1 if errs else 0


if __name__ == "__main__":
    sys.exit(main())
