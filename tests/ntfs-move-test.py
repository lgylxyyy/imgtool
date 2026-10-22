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
d = run('ntfs.move', img=IMG, lba=LBA, src='/dirA', dst='/dirB')
check('目录移动目前明确不放行 → NTFS-SRC-IS-DIR', code(d) == 'NTFS-SRC-IS-DIR', code(d))
check('  被拒后目录还在（不是半途失败）', ex('/dirA/x.txt'))
d = run('ntfs.move', img=IMG, lba=LBA, src='/dirA/x.txt', dst='/dirA', overwrite='true')
check('目标是目录 → 不许用文件顶掉 NTFS-DEST-IS-DIR', code(d) == 'NTFS-DEST-IS-DIR', code(d))

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
d = run('ntfs.rm', img=IMG, lba=LBA, path='/dirA/x.txt')
check('删文件 /dirA/x.txt', d.get('ok'), json.dumps(d)[:200])
check('  删完确实不在', not ex('/dirA/x.txt'))
d = run('ntfs.rm', img=IMG, lba=LBA, path='/dirA')
check('删目录 → NTFS-IS-DIR（绝不递归删）', code(d) == 'NTFS-IS-DIR', code(d))
d = run('ntfs.rm', img=IMG, lba=LBA, path='/ghost')
check('删不存在 → NTFS-PATH-NOTFOUND', code(d) == 'NTFS-PATH-NOTFOUND', code(d))
d = run('ntfs.rm', img=IMG, lba=LBA, path='/')
check('删根 → CMD-ARG-BAD', code(d) == 'CMD-ARG-BAD', code(d))

# —— 卷完整性复查 ——
d = run('qc.verify', img=IMG)
check('qc.verify 卷完好', d.get('ok'), json.dumps(d)[:200])
d = run('ntfs.list', img=IMG, lba=LBA, path='/sub/deep')
names = [e['name'] for e in (d.get('data') or {}).get('names', [])]   # ★ 字段是 names[]
check('列目录只剩搬来的 c.txt', names == ['c.txt'], str(names))

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
p6 = run('ntfs.list', img=IMG, lba=LBA, path='/pg', limit='99999')
d6 = p6.get('data') or {}
check('limit 超上限被夹到 2000', d6.get('limit') == 2000, json.dumps(d6)[:200])

# cmd.list 的 JSON 是分块多行，直接解析不好办；用 --list（单行 JSON）取权威命令表
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
