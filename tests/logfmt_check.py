#!/usr/bin/env python3
"""校验日志每一行是合法 logfmt（我们约定的格式）：
   time="YYYY-MM-DD HH:MM:SS.mmm" level=XXX logger/tool=... event=...
   规则：必须以 time=" 开头、引号成对、含 level= 与 event=、行尾换行。"""
import sys, re
bad = 0; n = 0
for f in sys.argv[1:]:
    for l in open(f, encoding='utf-8', errors='replace'):
        if not l.endswith('\n'): bad += 1; print(f"  ✗ 半行: {l[:60]!r}"); continue
        l = l.rstrip('\n'); n += 1
        if not re.match(r'^time="\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d{3}" ', l): bad += 1; print(f"  ✗ 时间头不对: {l[:60]!r}"); continue
        if l.count('"') % 2: bad += 1; print(f"  ✗ 引号不成对: {l[:60]!r}"); continue
        if not re.search(r'( |^)level=', l): bad += 1; print(f"  ✗ 缺 level=: {l[:60]!r}"); continue
        if not re.search(r'( |^)event=', l): bad += 1; print(f"  ✗ 缺 event=: {l[:60]!r}"); continue
print(f"  日志 {n} 行，坏行 {bad}")
sys.exit(1 if (bad or n==0) else 0)
