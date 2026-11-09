#!/usr/bin/env python3
"""style-fix.py — 按 STYLE-GUIDE 修破折号解释（规则 9）

与 style-check.py 共用扫描逻辑：只改字符串字面量内部，
跳过 // 与 /* */ 注释、跳过字符常量（'"' 这类会误导简单解析器）。
"""
import glob, os, re, sys

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
DASH = [(" —— ", "，"), (" ——", "，"), ("—— ", "，"), ("——", "，"),
        (" — ", "，"), (" —", "，"), ("— ", "，")]

def walk(text, fn):
    """把每个字符串字面量的内容交给 fn 处理，返回 (新文本, 改动数)。"""
    out, i, n, ch = [], 0, len(text), 0
    while i < n:
        c = text[i]
        if c == "/" and text[i:i + 2] == "//":
            k = text.find("\n", i); k = n if k < 0 else k
            out.append(text[i:k]); i = k; continue
        if c == "/" and text[i:i + 2] == "/*":
            k = text.find("*/", i); k = n if k < 0 else k + 2
            out.append(text[i:k]); i = k; continue
        if c == "'":                       # 字符常量：整段跳过
            j = i + 1
            while j < n and text[j] != "'":
                j += 2 if text[j] == "\\" else 1
            out.append(text[i:j + 1]); i = j + 1; continue
        if c == '"':
            j = i + 1; buf = []
            while j < n:
                if text[j] == "\\": buf.append(text[j:j + 2]); j += 2; continue
                if text[j] == '"': j += 1; break
                buf.append(text[j]); j += 1
            inner = "".join(buf)
            new = fn(inner)
            if new != inner:
                ch += 1
                out.append('"' + new + '"')
            else:
                out.append('"' + inner + '"')
            i = j; continue
        out.append(c); i += 1
    return "".join(out), ch

def fix(s):
    for a, b in DASH: s = s.replace(a, b)
    s = re.sub(r"，{2,}", "，", s)
    s = re.sub(r"，(。|$)", r"\1", s)
    return s

def walk_scan(text):
    """统计可解析出的字符串字面量数量；末尾未闭合的引号记为异常。"""
    cnt, i, n, open_end = 0, 0, len(text), False
    while i < n:
        c = text[i]
        if c == "/" and text[i:i + 2] == "//":
            k = text.find("\n", i); i = n if k < 0 else k; continue
        if c == "/" and text[i:i + 2] == "/*":
            k = text.find("*/", i); i = n if k < 0 else k + 2; continue
        if c == "'":
            j = i + 1
            while j < n and text[j] != "'":
                j += 2 if text[j] == "\\" else 1
            i = j + 1; continue
        if c == '"':
            j = i + 1; closed = False
            while j < n:
                if text[j] == "\\": j += 2; continue
                if text[j] == '"': closed = True; j += 1; break
                j += 1
            cnt += 1
            if not closed: open_end = True
            i = j; continue
        i += 1
    return cnt, open_end

def main():
    files = sorted(glob.glob(os.path.join(ROOT, "src", "*.c"))
                   + glob.glob(os.path.join(ROOT, "include", "*.h"))
                   + glob.glob(os.path.join(ROOT, "tools", "*.c")))
    tot = 0
    for f in files:
        src = open(f, encoding="utf-8").read()
        ns, c = walk(src, fix)
        if c:
            open(f, "w", encoding="utf-8").write(ns)
            tot += c
            print(f"  {c:3d} 处  {os.path.relpath(f, ROOT)}")
    print(f"合计 {tot} 个字符串字面量")
    # 自检：用解析器重扫一遍，字符串必须都能闭合（裸数引号会被 '"' 字符常量误判）
    bad = 0
    for f in files:
        t = open(f, encoding="utf-8").read()
        _, unclosed = walk_scan(t)
        if unclosed:
            print(f"  !! 字符串未闭合: {os.path.relpath(f, ROOT)}"); bad += 1
    return 1 if bad else 0

if __name__ == "__main__":
    sys.exit(main())
