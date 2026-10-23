#!/usr/bin/env python3
"""ntfs-move-test — ntfs.move / ntfs.rm 端到端（真建盘 + 真格式化 + 真改目录项）

覆盖（每条都是对【真实 qcow2 里的 NTFS 卷】操作，不是 mock）：
  改名 / 跨目录移动（自动建父目录）/ 目标冲突拒绝 / overwrite / 源不存在 /
  源是目录被拒（政策未放行）/ 目标是目录不被文件顶掉 / 删文件 / 删目录被拒 /
  删不存在 / 搬完【内容必须逐字节一致】/ 参数反例 / qc.verify 复查卷完好。
★ 关键设计：move 之后除了"新位置在、旧位置不在"，还必须导出比对内容 ——
  link+delete 这套如果顺序错了，症状正是"名字还在但数据没了"。
"""
import json, os, subprocess, sys, tempfile, shutil

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
IMGCMD = os.path.join(ROOT, 'build', 'tools', 'imgcmd')
W = tempfile.mkdtemp(prefix='ntfsmv')
ok = 0; bad = 0

def run(cmd, **kw):
    args = [IMGCMD, cmd] + ['--%s=%s' % (k, v) for k, v in kw.items()]
    p = subprocess.run(args, capture_output=True, text=True)
    js = [l for l in p.stdout.splitlines() if l.startswith('{')]
    if not js:
        return {'ok': False, 'error': {'error_code': 'NO-JSON'},
                'error_msg': (p.stdout + p.stderr)[-260:]}
    try:
        return json.loads(js[-1])
    except Exception as e:
        return {'ok': False, 'error': {'error_code': 'BAD-JSON'}, 'error_msg': str(e)}

def code(d):
    # ★ 失败信封的字段名是 err（不是 error）：{"ok":false,"err":{"rc":-11,"error_code":"..."}}
    return (d.get('err') or d.get('error') or {}).get('error_code', '')

def ex(path):
    """ntfs.exists 对【不存在】也返回 ok:true —— 真相在 data.exists，
    拿 ok 当存在性会把每条反例都判成通过（第一版就这么错过 13 条）。"""
    d = run('ntfs.exists', img=IMG, lba=LBA, path=path)
    return bool(d.get('ok')) and bool((d.get('data') or {}).get('exists'))

def check(desc, cond, extra=''):
    global ok, bad
    if cond:
        ok += 1; print('  ✅ %s' % desc)
    else:
        bad += 1; print('  ❌ %s %s' % (desc, extra))

IMG = os.path.join(W, 'd.qcow2')
BIG = b'HELLO-MOVE-' + b'x' * 4096          # 跨簇的内容，验证搬移不动数据

def put(name, data):
    lp = os.path.join(W, 'loc_' + os.path.basename(name))
    with open(lp, 'wb') as f:
        f.write(data)
    return run('ntfs.put', img=IMG, lba=LBA, src=lp, dst=name, mkdir='true')

def exported(path):
    out = os.path.join(W, 'exp_' + path.replace('/', '_'))
    d = run('ntfs.export', img=IMG, lba=LBA, path=path, dst=out)
    if not d.get('ok') or not os.path.exists(out):
        return None
    with open(out, 'rb') as f:
        return f.read()

print('════ ntfs.move / ntfs.rm 端到端 ════')
d = run('img.create', **{'out': IMG, 'size-gb': 2, 'table': 'gpt', 'esp-mb': 64, 'msr-mb': 16})
check('img.create 建 2GiB GPT 盘', d.get('ok'), json.dumps(d)[:200])
ps = (d.get('data') or {}).get('pt', {}).get('parts') or []
if not ps:
    print('  ❌ 拿不到分区表，测试无法继续'); sys.exit(1)
LBA = ps[-1]['first']
d = run('fs.format', img=IMG, lba=LBA, fs='ntfs', label='DATA', quick='true')
check('fs.format NTFS（LBA=%d）' % LBA, d.get('ok'), json.dumps(d)[:200])

# —— 改名 ——
check('ntfs.put /a.txt', put('/a.txt', BIG).get('ok'))
d = run('ntfs.move', img=IMG, lba=LBA, src='/a.txt', dst='/b.txt')
check('改名 /a.txt → /b.txt', d.get('ok'), json.dumps(d)[:200])
check('  新位置在', ex('/b.txt'))
check('  旧位置不在', not ex('/a.txt'))
check('  搬完内容逐字节一致', exported('/b.txt') == BIG)

# —— 跨目录移动（父目录不存在要自动建）——
check('ntfs.put /c.txt', put('/c.txt', b'C-CONTENT').get('ok'))
d = run('ntfs.move', img=IMG, lba=LBA, src='/c.txt', dst='/sub/deep/c.txt')
check('移动 /c.txt → /sub/deep/c.txt（自动建父目录）', d.get('ok'), json.dumps(d)[:200])
check('  新路径内容一致', exported('/sub/deep/c.txt') == b'C-CONTENT')

# —— 目标冲突：默认绝不覆盖 ——
d = run('ntfs.move', img=IMG, lba=LBA, src='/b.txt', dst='/sub/deep/c.txt')
check('目标已存在 → 拒绝（不悄悄吃掉）', (not d.get('ok')) and code(d) == 'NTFS-DEST-EXISTS', code(d))
d = run('ntfs.move', img=IMG, lba=LBA, src='/b.txt', dst='/sub/deep/c.txt', overwrite='true')
check('显式 overwrite=true → 成功', d.get('ok'), json.dumps(d)[:200])
check('  覆盖后内容是新名的那份', exported('/sub/deep/c.txt') == BIG)
check('  源已消失', not ex('/b.txt'))

# —— 反例 ——
d = run('ntfs.move', img=IMG, lba=LBA, src='/ghost.txt', dst='/x.txt')
check('源不存在 → NTFS-PATH-NOTFOUND', code(d) == 'NTFS-PATH-NOTFOUND', code(d))
d = run('ntfs.move', img=IMG, lba=LBA, src='a.txt', dst='/x.txt')
check('相对路径 → CMD-ARG-BAD', code(d) == 'CMD-ARG-BAD', code(d))
d = run('ntfs.move', img=IMG, lba=LBA, src='/x.txt', dst='/x.txt')
check('自己移到自己 → noop 成功', d.get('ok') and (d.get('data') or {}).get('noop'))
check('ntfs.put /dirA/x.txt（造出目录）', put('/dirA/x.txt', b'IN-DIR').get('ok'))
# —— 目录：【同父改名】可以；【跨父搬移】收回 ——
# 真 Windows 实测（Vectras / Win10 X-Lite，卷号 2B00-726F）：
#   跨父搬走的 C:\MovedByTool\LogFiles → Windows "找不到路径"，
#   chkdsk /scan 判它 "unindexed file"（0x114a=4426）并塞进 found.000；
#   同父改名的 C:\Windows\appcompat → AppCompat-Renamed 完全正常。
# 所以契约改成：只放开同父改名。证据与原因写在 img_ntfs_fs.c 的 move_path 注释里。
def parent(path):
    d = run('ntfs.parent', img=IMG, lba=LBA, path=path)
    return d.get('data') or {}
pd0 = parent('/dirA')
check('ntfs.parent 基线：/dirA 自洽（parent_ino==parent_by_path 且 backlink_ok）',
      pd0.get('consistent') is True, json.dumps(pd0)[:200])
root_ino = parent('/').get('ino')
check('ntfs.parent 认得根（ino==parent_ino）', pd0.get('parent_ino') == root_ino,
      json.dumps(pd0)[:160])
d = run('ntfs.move', img=IMG, lba=LBA, src='/dirA', dst='/sub/dirA')
check('目录【跨父搬移】被拒（NTFS-SRC-IS-DIR，真 Windows 判负后收回）',
      (not d.get('ok')) and code(d) == 'NTFS-SRC-IS-DIR', code(d))
check('  被拒后目录与内容完好（不是半途失败）',
      ex('/dirA/x.txt') and not ex('/sub/dirA/x.txt'))
d = run('ntfs.move', img=IMG, lba=LBA, src='/dirA', dst='/dirA2')
check('目录【同父改名】允许（实测这一种在 Windows 下是好的）', d.get('ok'),
      code(d) or json.dumps(d)[:200])
check('  改名后里面的文件跟着走', ex('/dirA2/x.txt') and not ex('/dirA/x.txt'))
pd1 = parent('/dirA2')
check('  改名后 ntfs.parent 仍自洽、inode 号没变（是改名不是重建）',
      pd1.get('consistent') is True and pd1.get('ino') == pd0.get('ino'),
      json.dumps(pd1)[:200])
check('  内容逐字节没变', exported('/dirA2/x.txt') == b'IN-DIR')
d = run('ntfs.move', img=IMG, lba=LBA, src='/dirA2', dst='/dirA2/inner')
check('把目录搬进自己的子树也拒（那也是跨父搬移）',
      (not d.get('ok')) and code(d) == 'NTFS-SRC-IS-DIR', code(d))
check('造 /pf.txt（给 ntfs.parent 的反例用）', put('/pf.txt', b'F').get('ok'))
d = run('ntfs.parent', img=IMG, lba=LBA, path='/pf.txt')
check('ntfs.parent 对文件明确拒绝（NTFS-NOT-A-DIR）', code(d) == 'NTFS-NOT-A-DIR', code(d))

d = run('ntfs.move', img=IMG, lba=LBA, src='/mvdir-f.txt', dst='/dirA2', overwrite='true')
check('源不存在时先报源不存在（不误报成目标问题）', code(d) == 'NTFS-PATH-NOTFOUND', code(d))

# —— 子目录内的改名（2026-10-23 真机抓到的真 bug）——
# 同一目录里 spar/dpar 被 ntfs_inode_open 开了【两份独立副本】，link 写 A 份、
# delete 拿 B 份的旧索引去删 → EIO，且把该子目录的索引写空（文件消失）。
# 根目录因为索引项在磁盘块里，反而看不出问题 —— 所以必须专门测子目录。
print('\n──── 子目录内改名（同名目录两份 inode 的坑）────')
check('造 /mvdir/a.txt 与 /mvdir/keep.txt',
      put('/mvdir/a.txt', b'AAA-IN-DIR').get('ok') and put('/mvdir/keep.txt', b'KEEP').get('ok'))
d = run('ntfs.move', img=IMG, lba=LBA, src='/mvdir/a.txt', dst='/mvdir/b.txt')
check('子目录内改名 /mvdir/a.txt → /mvdir/b.txt', d.get('ok'), json.dumps(d)[:200])
check('  新名字在', ex('/mvdir/b.txt'))
check('  内容逐字节一致', exported('/mvdir/b.txt') == b'AAA-IN-DIR')
check('  同目录的其它文件还在（索引没被写空）', ex('/mvdir/keep.txt'))

# 只改大小写：NTFS 大小写不敏感，"源还在"不能当失败依据
d = run('ntfs.move', img=IMG, lba=LBA, src='/mvdir/b.txt', dst='/mvdir/B.TXT')
check('只改大小写 /mvdir/b.txt → /mvdir/B.TXT', d.get('ok'), json.dumps(d)[:200])
check('  带 case_only=true', (d.get('data') or {}).get('case_only') is True, json.dumps(d)[:200])
check('  内容没变', exported('/mvdir/B.TXT') == b'AAA-IN-DIR')
check('  同目录其它文件仍在', ex('/mvdir/keep.txt'))

# 子目录 → 根（反向也测一遍；这条以前只测过 根 → 子目录）
d = run('ntfs.move', img=IMG, lba=LBA, src='/mvdir/B.TXT', dst='/fromdir.txt')
check('子目录 → 根 /mvdir/B.TXT → /fromdir.txt', d.get('ok'), json.dumps(d)[:200])
check('  内容一致', exported('/fromdir.txt') == b'AAA-IN-DIR')
check('  原目录里已没有它', not ex('/mvdir/B.TXT'))

# —— 删除 ——
d = run('ntfs.rm', img=IMG, lba=LBA, path='/dirA2/x.txt')
check('删文件 /dirA2/x.txt', d.get('ok'), json.dumps(d)[:200])
check('  删完确实不在', not ex('/dirA2/x.txt'))
d = run('ntfs.rm', img=IMG, lba=LBA, path='/dirA2')
check('删目录 → NTFS-IS-DIR（绝不递归删）', code(d) == 'NTFS-IS-DIR', code(d))
d = run('ntfs.rm', img=IMG, lba=LBA, path='/ghost')
check('删不存在 → NTFS-PATH-NOTFOUND', code(d) == 'NTFS-PATH-NOTFOUND', code(d))
d = run('ntfs.rm', img=IMG, lba=LBA, path='/')
check('删根 → CMD-ARG-BAD', code(d) == 'CMD-ARG-BAD', code(d))

# —— 卷完整性复查 ——
d = run('qc.verify', img=IMG)
check('qc.verify 卷完好', d.get('ok'), json.dumps(d)[:200])
d = run('ntfs.list', img=IMG, lba=LBA, path='/sub/deep')
names = sorted(e['name'] for e in (d.get('data') or {}).get('names', []))   # ★ 字段是 names[]
check('列目录只剩搬来的 c.txt（目录没被搬进来过）', names == ['c.txt'], str(names))

# —— ntfs.list 分页（System32 有 4479 项，整页序列化会超输出缓冲 → 以前只回 RESULT-TOO-BIG）——
print('\n──── ntfs.list 分页 ────')
for i in range(25):
    put('/pg/f%02d.txt' % i, b'P%d' % i)
p1 = run('ntfs.list', img=IMG, lba=LBA, path='/pg', limit='10')
d1 = p1.get('data') or {}
check('第 1 页：total=25 / count=10 / has_more',
      d1.get('total') == 25 and d1.get('count') == 10 and d1.get('has_more') is True, json.dumps(d1)[:200])
n1 = [e['name'] for e in d1.get('names', [])]
p2 = run('ntfs.list', img=IMG, lba=LBA, path='/pg', offset='10', limit='10')
d2 = p2.get('data') or {}
n2 = [e['name'] for e in d2.get('names', [])]
check('第 2 页：offset 生效、与第 1 页不重叠不重复',
      d2.get('offset') == 10 and d2.get('count') == 10 and not (set(n1) & set(n2)), str(n2))
p3 = run('ntfs.list', img=IMG, lba=LBA, path='/pg', offset='20', limit='10')
d3 = p3.get('data') or {}
check('第 3 页：只剩 5 项、has_more=false',
      d3.get('count') == 5 and d3.get('has_more') is False, json.dumps(d3)[:200])
check('三页拼起来正好 25 个名字',
      len(set(n1) | set(n2) | {e['name'] for e in d3.get('names', [])}) == 25)
p4 = run('ntfs.list', img=IMG, lba=LBA, path='/pg', offset='999')
d4 = p4.get('data') or {}
check('offset 超出范围：count=0 但 total 仍是 25（翻页要知道边界）',
      d4.get('count') == 0 and d4.get('total') == 25 and d4.get('has_more') is False,
      json.dumps(d4)[:200])
p5 = run('ntfs.list', img=IMG, lba=LBA, path='/pg', limit='0')
d5 = p5.get('data') or {}
check('limit=0 归一成默认值 500（不是"要 0 项"）', d5.get('count') == 25, json.dumps(d5)[:200])
sz = run('ntfs.list', img=IMG, lba=LBA, path='/pg', limit='5', sizes='true')
dsz = sz.get('data') or {}
check('--sizes=true 才给真实大小（f00.txt 内容是 2 字节）',
      dsz.get('sizes') is True and dsz.get('names')
      and any(e['name'] == 'f00.txt' and e['size'] == 2 for e in dsz['names']),
      json.dumps(dsz)[:220])
nosz = run('ntfs.list', img=IMG, lba=LBA, path='/pg', limit='5')
check('默认不带 sizes（契约不变：size 恒 0，省几千次 inode 打开）',
      all(e['size'] == 0 for e in (nosz.get('data') or {}).get('names', [])),
      json.dumps(nosz)[:200])
p6 = run('ntfs.list', img=IMG, lba=LBA, path='/pg', limit='99999')
d6 = p6.get('data') or {}
check('limit 超上限被夹到 2000', d6.get('limit') == 2000, json.dumps(d6)[:200])

# cmd.list 的 JSON 是分块多行，直接解析不好办；用 --list（单行 JSON）取权威命令表
# ── ★ 拒绝必须零副作用 + 显式开关（2026-10-23 真机现场修的两件事）──
# 上一版 ntfs.move 是"先建目标父目录，再拒目录跨父"，于是被拒绝的操作也在盘上
# 留下了一个空目录（现场：/MovedByTool 就这么冒出来的）。现在政策判定前移到任何写动作之前。
check('put /SidE/a.bin', put('/SidE/a.bin', b'A' * 900).get('ok'))
check('搬之前 /NoSide 不存在', not ex('/NoSide'))
d = run('ntfs.move', img=IMG, lba=LBA, src='/SidE', dst='/NoSide/SidE')
check('目录跨父搬（默认政策）被拒 = NTFS-SRC-IS-DIR', code(d) == 'NTFS-SRC-IS-DIR', json.dumps(d)[:160])
check('★拒绝之后目标父目录【没被建出来】（拒绝=零副作用）', not ex('/NoSide'),
      'App 因此可以放心原样重试')
check('  源目录仍在原位', ex('/SidE/a.bin'))
_pre = run('ntfs.parent', img=IMG, lba=LBA, path='/SidE')     # 搬前先记下 ino
_ino = (_pre.get('data') or {}).get('ino')
d = run('ntfs.move', img=IMG, lba=LBA, src='/SidE', dst='/NoSide/SidE', allow_dir_move='true')
check('显式 --allow_dir_move=true 才放行跨父搬目录', d.get('ok'), json.dumps(d)[:200])
check('  搬后新位置可读（内容一致）', exported('/NoSide/SidE/a.bin') == b'A' * 900)
check('  旧位置已消失', not ex('/SidE/a.bin'))
_pdd = (run('ntfs.parent', img=IMG, lba=LBA, path='/NoSide/SidE').get('data') or {})
check('  搬完 ino 与搬前一致（同一份记录，不是"新建+删除"）',
      _ino is not None and _pdd.get('ino') == _ino, '%s vs %s' % (_ino, _pdd.get('ino')))
check('  搬完 parent_ino == parent_by_path（父引用指回新父）',
      _pdd.get('backlink_ok') is True and _pdd.get('parent_ino') == _pdd.get('parent_by_path'),
      json.dumps(_pdd))
d = run('ntfs.move', img=IMG, lba=LBA, src='/NoSide/SidE/a.bin', dst='/back.bin')
check('文件跨父移动照旧允许', d.get('ok'), json.dumps(d)[:160])

pl = subprocess.run([IMGCMD, '--list'], capture_output=True, text=True)
try:
    allc = [c['name'] for c in json.loads(pl.stdout)['cmds']]
except Exception:
    allc = []
check('cmd.list 里能看到 ntfs.move/ntfs.rm', 'ntfs.move' in allc and 'ntfs.rm' in allc)
print('  命令总数：%d' % len(allc))

print('════ 结果：通过 %d · 失败 %d ════' % (ok, bad))
shutil.rmtree(W, ignore_errors=True)
sys.exit(0 if bad == 0 else 1)
