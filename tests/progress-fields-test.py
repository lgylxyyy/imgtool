#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""progress-fields-test —— 进度事件的三个新字段（REQ-77）

  files_cur / files_total  逐文件处理的文件计数（0 = 该任务没有文件概念）
  dst_item                 当前写入的【目标】路径（item 是源路径）

断言：
  · 树复制（宿主目录 → 宿主目录）：files_total=3、files_cur 能到 3
  · 复制过程里 dst_item 非空，且指向目标目录下的文件
  · 非逐文件命令（img.info）：files_cur=files_total=0、dst_item 空
  · 事件 JSON 里这三个字段**始终存在**（缺字段 = 旧 so，App 显示「—」）
"""
import json
import os
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
IMGCMD = os.path.join(ROOT, 'build', 'tools', 'imgcmd')

W = tempfile.mkdtemp(prefix='progfields', dir=os.environ.get('IMG_TEST_TMP') or None)
ok = bad = 0


def check(name, cond, extra=''):
    global ok, bad
    if cond:
        ok += 1
        print('  ✅ %s' % name)
    else:
        bad += 1
        print('  ❌ %s %s' % (name, extra))


def run(*args):
    p = subprocess.run(args, capture_output=True, text=True)
    return p.returncode, p.stdout, p.stderr


def events(out):
    """从 stdout 里挑出进度事件（每行一个 JSON）。"""
    r = []
    for line in out.splitlines():
        line = line.strip()
        if not line.startswith('{'):
            continue
        try:
            d = json.loads(line)
        except Exception:
            continue
        if d.get('ev') == 'progress':
            r.append(d)
    return r


def mk(rel, data=b'x'):
    p = os.path.join(W, rel)
    os.makedirs(os.path.dirname(p), exist_ok=True)
    with open(p, 'wb') as f:
        f.write(data)
    return p


def main():
    if not os.path.exists(IMGCMD):
        print('SKIP: imgcmd 未构建')
        return 77

    # 源树：3 个文件、2 层子目录
    mk('tree/a.txt', b'hello\n')
    mk('tree/sub/b.bin', os.urandom(200000))
    mk('tree/sub/deep/c.txt', b'deep\n')
    dst = os.path.join(W, 'dst')
    os.makedirs(dst, exist_ok=True)

    print('════ 1. 树复制（宿主 → 宿主）：文件计数 ════')
    rc, out, err = run(IMGCMD, 'copy',
                       '--src=host:' + os.path.join(W, 'tree'),
                       '--dst=host:' + dst, '--progress=json')
    ev = events(out)
    check('复制成功（rc=0）', rc == 0, err[-200:])
    check('有进度事件', len(ev) > 0, len(ev))

    if ev:
        # 字段必须每条都在（App 缺字段显示「—」，但我们要的是"始终有"）
        check('每条事件都有 files_cur/files_total/dst_item',
              all('files_cur' in e and 'files_total' in e and 'dst_item' in e for e in ev))
        totals = [e['files_total'] for e in ev]
        curs = [e['files_cur'] for e in ev]
        check('files_total 最终 = 3（%d）' % totals[-1], totals[-1] == 3, totals)
        check('files_total 单调不减', all(b >= a for a, b in zip(totals, totals[1:])), totals)
        check('files_cur 能到 3（%d）' % max(curs), max(curs) == 3, curs)
        check('files_cur 单调不减', all(b >= a for a, b in zip(curs, curs[1:])), curs)
        check('files_cur ≤ files_total',
              all(c <= t for c, t in zip(curs, totals)), list(zip(curs, totals))[:6])

    print('════ 2. dst_item：正在写到哪里 ════')
    dsts = [e['dst_item'] for e in ev if e.get('dst_item')]
    check('有非空 dst_item（%d 条）' % len(dsts), len(dsts) > 0)
    if dsts:
        check('dst_item 指向目标目录下的文件',
              all(d.startswith(dst) for d in dsts), dsts[:3])
        check('dst_item 里有源文件名（a.txt）',
              any('a.txt' in d for d in dsts), dsts[:3])

    print('════ 3. 非逐文件命令：不该有文件计数 ════')
    rc, out, err = run(IMGCMD, 'img.info', '--img=' + os.path.join(W, 'tree', 'a.txt'))
    ev2 = events(out)
    check('img.info 没有进度事件（正常）', len(ev2) == 0, len(ev2))

    # img.sparsify 有进度但没有文件概念 → files 应为 0
    q = os.path.join(W, 'q.qcow2')
    run(IMGCMD, 'img.create', '--out=' + q, '--size-gb=1', '--format=qcow2',
        '--table=none', '--limits=off')
    rc, out, err = run(IMGCMD, 'img.sparsify', '--img=' + q, '--dry-run=true',
                        '--progress=json')
    ev3 = events(out)
    if ev3:
        check('img.sparsify：files_cur=files_total=0（%s / %s）'
              % (ev3[-1]['files_cur'], ev3[-1]['files_total']),
              ev3[-1]['files_cur'] == 0 and ev3[-1]['files_total'] == 0)
        check('img.sparsify：dst_item 为空',
              ev3[-1].get('dst_item', '') == '')

    print()
    print('════ 通过 %d · 失败 %d ════' % (ok, bad))
    if bad:
        print('临时目录：' + W)
        return 1
    shutil.rmtree(W, ignore_errors=True)
    return 0


if __name__ == '__main__':
    sys.exit(main())
