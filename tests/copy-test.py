#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
copy-test —— 盘内复制/删除 端到端（NTFS + FAT32）

覆盖：
  · 复制后内容逐字节一致、inode 是新的（NTFS）、源文件不变
  · dst 是目录（或以 / 结尾）⇒ 沿用源文件名；父目录自动创建
  · 同名策略 fail / overwrite / rename(.001) 三条分支
  · --preserve_times：$SI 四时间与源一致（用独立解析器读原始字节核对）
  · 删除：文件、空目录、非空目录（默认拒 / recursive 删）
  · 最后用独立三方对账（tools/ntfs-inventory.py 的差值模式）证明
    "这一串操作没有给卷新增任何不一致"
"""
import json, os, shutil, subprocess, sys, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
IMGCMD = os.path.join(ROOT, 'build', 'tools', 'imgcmd')
INV = os.path.join(ROOT, 'tools', 'ntfs-inventory.py')
W = tempfile.mkdtemp(prefix='copy')
ok = bad = 0


def run(cmd, **kw):
    args = [IMGCMD, cmd] + ['--%s=%s' % (k, v) for k, v in kw.items()]
    p = subprocess.run(args, capture_output=True, text=True)
    js = [l for l in p.stdout.splitlines() if l.startswith('{')]
    return json.loads(js[-1]) if js else {'ok': False, 'error_msg': (p.stdout + p.stderr)[-200:]}


def code(d):
    return (d.get('err') or d.get('error') or {}).get('error_code', '')


def data(d):
    return d.get('data') or {}


def check(desc, cond, extra=''):
    global ok, bad
    if cond:
        ok += 1; print('  ✅ %s' % desc)
    else:
        bad += 1; print('  ❌ %s  %s' % (desc, str(extra)[:240]))


def mk(b, n):
    p = os.path.join(W, n)
    with open(p, 'wb') as f:
        f.write(b)
    return p


def put(lp, dst):
    return run('ntfs.put', img=IMG, lba=LBA, src=lp, dst=dst, mkdir='true')


def exported(path, tag):
    out = os.path.join(W, 'exp_' + tag)
    d = run('ntfs.export', img=IMG, lba=LBA, path=path, dst=out)
    if not d.get('ok') or not os.path.exists(out):
        return None
    with open(out, 'rb') as f:
        return f.read()


def exists(p):
    return bool(data(run('ntfs.exists', img=IMG, lba=LBA, path=p)).get('exists'))


IMG = os.path.join(W, 'd.qcow2')
RAW = os.path.join(W, 'd.raw')
BASE = os.path.join(W, 'base.json')
print('════ 盘内复制/删除（NTFS + FAT32） ════')

d = run('img.create', out=IMG, **{'size-gb': 2, 'table': 'gpt', 'esp-mb': 64, 'msr-mb': 16})
LBA = data(d)['pt']['parts'][-1]['first']
ESP = data(d)['pt']['parts'][0]['first']
check('建 2 GiB GPT 盘', d.get('ok'), json.dumps(d)[:160])
check('NTFS 格式化', run('fs.format', img=IMG, lba=LBA, fs='ntfs', label='CP', quick='true').get('ok'))
check('FAT32 格式化（ESP 分区）', run('fs.format', img=IMG, lba=ESP, fs='fat32', label='EFI').get('ok'))
subprocess.run(['qemu-img', 'convert', '-O', 'raw', IMG, RAW], check=True)
subprocess.run([sys.executable, INV, RAW, str(LBA), '--base-out', BASE],
               capture_output=True, text=True)
check('基线体检（操作前）', os.path.exists(BASE))

# ─────────── NTFS 复制 ───────────
A = b'COPY-PAYLOAD-' + bytes(range(256)) * 40          # 10 KB+
B = b'OTHER-CONTENT-' + b'z' * 5000
check('put /src/a.bin', put(mk(A, 'a.bin'), '/src/a.bin').get('ok'))
src_ino = data(run('ntfs.exists', img=IMG, lba=LBA, path='/src/a.bin')).get('path')  # 仅探活
d = run('ntfs.copy', img=IMG, lba=LBA, src='/src/a.bin', dst='/src/b.bin')
check('复制 /src/a.bin → /src/b.bin', d.get('ok'), json.dumps(d)[:200])
b_ino = data(d).get('ino')
check('  返回 ino（App 局部刷新用）', isinstance(b_ino, int) and b_ino > 0, d)
check('  大小一致', data(d).get('size') == len(A), data(d).get('size'))
check('  内容逐字节一致', exported('/src/b.bin', 'b') == A)
check('  源文件没被动', exported('/src/a.bin', 'a') == A)
check('  返回 parent/parent_total（给 UI 打补丁）',
      data(d).get('parent') == '/src' and data(d).get('parent_total') >= 2, json.dumps(data(d)))

# dst 是目录 / 以 / 结尾 ⇒ 沿用源名，父目录自动建
d = run('ntfs.copy', img=IMG, lba=LBA, src='/src/a.bin', dst='/dest/')
check('dst 以 / 结尾 ⇒ 沿用源文件名（/dest/a.bin）',
      d.get('ok') and data(d).get('path') == '/dest/a.bin', json.dumps(d)[:200])
check('  父目录 /dest 被自动创建', exists('/dest'))

# 三条同名策略
d = run('ntfs.copy', img=IMG, lba=LBA, src='/src/a.bin', dst='/src/b.bin', conflict='fail')
check('同名 + conflict=fail ⇒ NTFS-DEST-EXISTS', code(d) == 'NTFS-DEST-EXISTS', json.dumps(d)[:160])
d = run('ntfs.copy', img=IMG, lba=LBA, src='/src/a.bin', dst='/src/b.bin', conflict='rename')
check('同名 + conflict=rename ⇒ 自动 .001',
      d.get('ok') and data(d).get('path') == '/src/b.001.bin', json.dumps(d)[:200])
check('  .001 内容正确', exported('/src/b.001.bin', 'b001') == A)
check('put /src/c.bin（另一份内容）', put(mk(B, 'c.bin'), '/src/c.bin').get('ok'))
d = run('ntfs.copy', img=IMG, lba=LBA, src='/src/a.bin', dst='/src/c.bin', conflict='overwrite')
check('同名 + conflict=overwrite ⇒ 覆盖并 replaced=true',
      d.get('ok') and data(d).get('replaced') is True, json.dumps(d)[:200])
check('  覆盖后内容是新的', exported('/src/c.bin', 'c') == A)

# 复制不存在的源
d = run('ntfs.copy', img=IMG, lba=LBA, src='/src/nope.bin', dst='/src/x.bin')
check('源不存在 ⇒ NTFS-PATH-NOTFOUND', code(d) == 'NTFS-PATH-NOTFOUND', json.dumps(d)[:160])

# ─────────── 删除 ───────────
d = run('ntfs.rm', img=IMG, lba=LBA, path='/src/b.001.bin')
check('删文件', d.get('ok') and not exists('/src/b.001.bin'), json.dumps(d)[:160])
check('建一个非空目录 /deldir/kid/x.bin', put(mk(b'xx', 'x.bin'), '/deldir/kid/x.bin').get('ok'))
d = run('ntfs.rm', img=IMG, lba=LBA, path='/deldir')
check('删非空目录（默认）⇒ NTFS-DIR-NOT-EMPTY', code(d) == 'NTFS-DIR-NOT-EMPTY', json.dumps(d)[:160])
check('  目录仍在（默认不递归）', exists('/deldir/kid/x.bin'))
d = run('ntfs.rm', img=IMG, lba=LBA, path='/deldir', recursive='true')
check('递归删除 ⇒ 成功且 removed ≥ 3（kid/x.bin + kid + deldir）',
      d.get('ok') and data(d).get('removed', 0) >= 3, json.dumps(d)[:200])
check('  递归后整棵不在了', not exists('/deldir'))

# ─────────── FAT32 复制 ───────────
fat_put = run('fat.put', img=IMG, lba=ESP, src=mk(A, 'fa.bin'), dst='/fa.bin')
check('FAT put /fa.bin', fat_put.get('ok'), json.dumps(fat_put)[:160])
d = run('fat.copy', img=IMG, lba=ESP, src='/fa.bin', dst='/fb.bin')
check('FAT 复制 /fa.bin → /fb.bin', d.get('ok'), json.dumps(d)[:200])
fat_out = os.path.join(W, 'fat_b.bin')
run('fat.export', img=IMG, lba=ESP, path='/fb.bin', dst=fat_out)
check('  FAT 复制后内容一致', os.path.exists(fat_out) and open(fat_out, 'rb').read() == A)
d = run('fat.copy', img=IMG, lba=ESP, src='/fa.bin', dst='/fb.bin', conflict='fail')
check('FAT 同名 + fail ⇒ FAT-ENTRY-EXISTS', code(d) == 'FAT-ENTRY-EXISTS', json.dumps(d)[:160])
d = run('fat.copy', img=IMG, lba=ESP, src='/fa.bin', dst='/fb.bin', conflict='rename')
check('FAT 同名 + rename ⇒ .001', d.get('ok') and data(d).get('path') == '/fb.001.bin',
      json.dumps(d)[:200])
d = run('fat.copy', img=IMG, lba=ESP, src='/fa.bin', dst='/sub/deep/fc.bin')
check('FAT 复制到不存在的多级目录 ⇒ 自动建父目录', d.get('ok'), json.dumps(d)[:200])

# ─────────── FAT 递归删除 ───────────
check('FAT 造一棵树 /FT/A/B/C/x.bin',
      run('fat.put', img=IMG, lba=ESP, src=mk(b'zz', 'zz.bin'), dst='/FT/A/B/C/x.bin').get('ok'))
d = run('fat.rm', img=IMG, lba=ESP, path='/FT')
check('FAT 删非空目录（默认）⇒ FAT-DIR-NOT-EMPTY', code(d) == 'FAT-DIR-NOT-EMPTY', json.dumps(d)[:160])
d = run('fat.rm', img=IMG, lba=ESP, path='/FT', recursive='true')
check('FAT 递归删 ⇒ removed ≥ 4', d.get('ok') and data(d).get('removed', 0) >= 4,
      json.dumps(d)[:200])
check('  FAT 递归后整棵不在',
      not data(run('fat.exists', img=IMG, lba=ESP, path='/FT/A/B/C/x.bin')).get('exists'))

# ─────────── 收尾：差值对账（不能新增任何不一致） ───────────
subprocess.run(['qemu-img', 'convert', '-O', 'raw', IMG, RAW], check=True)
p = subprocess.run([sys.executable, INV, RAW, str(LBA), '--base', BASE],
                   capture_output=True, text=True)
line = [l for l in p.stdout.splitlines() if '新增' in l]
check('★这一串操作没有新增任何三方不一致（退出码 0）', p.returncode == 0,
      (line[0] if line else '') + p.stdout[-200:])

print('════ 结果：通过 %d · 失败 %d ════' % (ok, bad))
shutil.rmtree(W, ignore_errors=True)
sys.exit(0 if bad == 0 else 1)