#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
ntfs-inventory-test —— ①独立裁判进回归 ②幽灵目录必须被当场抓住

背景（2026-10-23 真盘事故，完整取证见 docs/NTFS移动改名-规范与实现对照.md）：
imgtool 建过一个【幽灵目录】C:\\MovedByTool —— 父索引项落盘了，被建目录自己的 MFT
记录却没落盘，那条索引项于是指向别人的记录（一个文件）；后续写入全挂在这个不存在
的父上。工具自己回读"名字在索引里"就一路报成功，Windows 看不见，
chkdsk 把挂在下面的东西当孤儿塞进 found.000。

所以这个测试要钉住两层：
  ① 正常流程跑完后，用【不链接 imgtool/libntfs-3g】的 tools/ntfs-inventory.py
     做「父索引项 ↔ 子记录 $FILE_NAME ↔ $MFT $BITMAP」三方对账，必须 0 违背；
  ② 手工把一条目录项改指向一个文件的记录（精确复刻事故形状）之后：
     a) 独立裁判必须报违背；
     b) imgtool 的写后自证必须当场拒绝，错误码 NTFS-DIR-PHANTOM —— 绝不再"报成功"。

注入选 /vdir/sub 这种【小目录的子项】：它的索引一定还在自己那条 MFT 记录里，
不跨扇区、也不在 $INDEX_ALLOCATION 块里，偏移算得准。
（第一版想在 raw 上按明文特征串搜，空转了 —— 根索引跨扇区那 2 字节早被
  USA 多扇区保护替换掉，明文串在盘上并不存在。）
"""
import importlib.machinery
import importlib.util
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import compat

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
IMGCMD = os.path.join(ROOT, 'build', 'tools', 'imgcmd')
INV = os.path.join(ROOT, 'tools', 'ntfs-inventory.py')
W = tempfile.mkdtemp(prefix='ntfsinv')
ok = bad = 0

_spec = importlib.util.spec_from_file_location('invlib', INV)
invlib = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(invlib)


def run(cmd, **kw):
    cmd, kw, orig = compat.translate_kwargs(cmd, kw)
    args = [IMGCMD, cmd] + ['--%s=%s' % (k, v) for k, v in kw.items()]
    p = subprocess.run(args, capture_output=True, text=True)
    js = [l for l in p.stdout.splitlines() if l.startswith('{')]
    d = json.loads(js[-1]) if js else {'ok': False, 'error_msg': (p.stdout + p.stderr)[-200:]}
    return compat.map_error(d, orig)


def code(d):
    return (d.get('err') or d.get('error') or {}).get('error_code', '')


def check(desc, cond, extra=''):
    global ok, bad
    if cond:
        ok += 1
        print('  ✅ %s' % desc)
    else:
        bad += 1
        print('  ❌ %s  %s' % (desc, str(extra)[:240]))


def put(name, data):
    lp = os.path.join(W, 'loc' + name.replace('/', '_'))
    with open(lp, 'wb') as f:
        f.write(data)
    return run('ntfs.put', img=IMG, lba=LBA, src=lp, dst=name, mkdir='true')


def inventory(raw, lba):
    p = subprocess.run([sys.executable, INV, raw, str(lba)], capture_output=True, text=True)
    return p.returncode, p.stdout + p.stderr


def mst_unseal(buf):
    """解多扇区保护：把 usa[i] 还原回每扇区末 2 字节（mst.c 的规则）"""
    b = bytearray(buf)
    ua, uc = struct.unpack_from('<H', b, 4)[0], struct.unpack_from('<H', b, 6)[0]
    for i in range(1, uc):
        struct.pack_into('<H', b, 512 * i - 2, struct.unpack_from('<H', b, ua + 2 * i)[0])
    return b


def mst_seal(b):
    """重新封保护：原尾部存进 usa[i]，尾部填 usa[0]"""
    ua, uc = struct.unpack_from('<H', b, 4)[0], struct.unpack_from('<H', b, 6)[0]
    usn = struct.unpack_from('<H', b, ua)[0]
    for i in range(1, uc):
        struct.pack_into('<H', b, ua + 2 * i, struct.unpack_from('<H', b, 512 * i - 2)[0])
        struct.pack_into('<H', b, 512 * i - 2, usn)
    return bytes(b)


def index_root_loc(buf):
    """在 MFT 记录字节里找 $INDEX_ROOT($I30)：返回 (属性偏移, 值偏移)"""
    o = struct.unpack_from('<H', buf, 20)[0]
    while o + 0x18 <= len(buf):
        t = struct.unpack_from('<I', buf, o)[0]
        if t in (0, 0xFFFFFFFF):
            break
        ln = struct.unpack_from('<I', buf, o + 4)[0]
        if ln == 0:
            break
        if t == 0x90 and buf[o + 8] == 0:                       # 驻留
            nl = buf[o + 9]
            no = struct.unpack_from('<H', buf, o + 0x0a)[0]
            nm = buf[o + no:o + no + nl * 2].decode('utf-16-le', 'replace') if nl else ''
            if nm == '$I30':
                return o, struct.unpack_from('<H', buf, o + 0x14)[0]
        o += ln
    return None, None


IMG = os.path.join(W, 'd.qcow2')
RAW = os.path.join(W, 'd.raw')
LBA = 0
print('════ ntfs-inventory（独立对账 + 幽灵目录必抓）════')

# ─────────────── 建盘 + 正常流程 ───────────────
d = run('img.create', out=IMG, **{'size-gb': 2, 'table': 'gpt', 'esp-mb': 64, 'msr-mb': 16, 'limits': 'off'})
check('img.create 建 2GiB GPT 盘', d.get('ok'), json.dumps(d)[:200])
LBA = (d.get('data') or {}).get('pt', {}).get('parts')[-1]['first']
check('fs.format NTFS（LBA=%d）' % LBA,
      run('fs.format', img=IMG, lba=LBA, fs='ntfs', label='INV', quick='true').get('ok'))

check('put /vdir/a.txt（自动建 /vdir）', put('/vdir/a.txt', b'A' * 3000).get('ok'))
check('put /file.txt（根上一个普通文件，用来当"假父"）', put('/file.txt', b'F' * 500).get('ok'))
check('put /vdir/sub/f.txt（小目录里的子目录）', put('/vdir/sub/f.txt', b'S' * 200).get('ok'))
check('put /deep/x/y.txt（多级自动建）', put('/deep/x/y.txt', b'Y' * 100).get('ok'))
check('同父改名 /vdir/a.txt → /vdir/b.txt',
      run('ntfs.move', img=IMG, lba=LBA, src='/vdir/a.txt', dst='/vdir/b.txt').get('ok'))
check('往【文件】下面建目录 → NTFS-DIR-PHANTOM',
      code(put('/file.txt/sub/z.txt', b'Z' * 10)) == 'NTFS-DIR-PHANTOM')

subprocess.run(['qemu-img', 'convert', '-O', 'raw', IMG, RAW], check=True)

# ── 权限模型（2026-10-23 的 §7 真问题）──────────────────────────────
# 规则不是"永远不许有内联 $SD"，而是【跟着卷自己的惯例走】：
#   · 父目录 $SI 带 SecurityID（NTFS 3.0 卷，真 Windows 盘都是这种）
#       ⇒ 新建对象必须继承同一个 sid，且【不再新增内联 $SD】（= Windows/wimlib 的做法）
#   · 父目录 $SI 只有 48 字节（NTFS 1.2，我们 mkntfs 快速格式化的卷就是这样，
#     连根目录自己都带内联 $SD）⇒ 无从继承，保持 ntfs-3g 原行为，不算违背。
# 真 Windows 盘上的效果已实测（docs/NTFS移动改名-规范与实现对照.md §7 与
# HANDOFF §1.20）：新建 /AclNew 与其子目录、文件全部 sid=258（=根目录）、无内联 $SD。
_nt = invlib.Ntfs(RAW, LBA)


def _si(ino):
    ats, _r = _nt.attrs(ino)
    return [x for x in ats if x['type'] == 0x10][0]['val']


def _sid(ino):
    v = _si(ino)
    return struct.unpack_from('<I', v, 52)[0] if len(v) >= 56 else None


def _inline_sd(ino):
    return any(a['type'] == 0x50 for a in _nt.attrs(ino)[0])


check('根目录 $SI 有 SecurityID（NTFS 3.0 卷）⇒ 才需要验继承' if _sid(5) is not None
      else '本卷 $SI 是 48 字节（NTFS 1.2）⇒ 继承无从谈起，只验不崩',
      True)
if _sid(5) is not None:
    for pth, par in (('/vdir', '/'), ('/vdir/sub', '/vdir'), ('/vdir/sub/f.txt', '/vdir/sub')):
        i = _nt.resolve(pth)
        check('  %s：继承父 sid=%s 且不新增内联 $SD' % (pth, _sid(_nt.resolve(par))),
              i is not None and _sid(i) == _sid(_nt.resolve(par)) and not _inline_sd(i),
              'sid=%s inline_sd=%s' % (_sid(i) if i else None, _inline_sd(i) if i else None))
else:
    for pth in ('/vdir', '/vdir/sub', '/vdir/sub/f.txt'):
        i = _nt.resolve(pth)
        check('  %s：NTFS 1.2 卷保持原行为（可建可读写）' % pth, i is not None and _inline_sd(i))

rc, out = inventory(RAW, LBA)
check('★正常流程后独立裁判必须 0 违背（退出码 0）', rc == 0, out[-500:])
check('  裁判确实扫到了（报告里有目录数）', '目录数=' in out, out[:200])

# ─────────────── 注入：把 /vdir 里指向 sub 的那条项改成指向 /file.txt ───────────────
nt = invlib.Ntfs(RAW, LBA)
ino_vdir = nt.resolve('/vdir')
ino_sub = nt.resolve('/vdir/sub')
ino_file = nt.resolve('/file.txt')
check('解析出 ino：/vdir=%s /vdir/sub=%s /file.txt=%s' % (ino_vdir, ino_sub, ino_file),
      None not in (ino_vdir, ino_sub, ino_file))

entries = nt.all_entries(ino_vdir)[0]
e = next((x for x in entries if x.get('fn') and x['fn']['name'] == 'sub'), None)
check('在 /vdir 索引里找到 sub 那条项', e is not None and e['mft'] == ino_sub, str(e))

mcap = nt.cl // nt.recsz
rec_base = nt.mft_off(ino_vdir // mcap) + (ino_vdir % mcap) * nt.recsz
with open(RAW, 'rb') as f:
    f.seek(rec_base)
    raw_rec = bytearray(f.read(nt.recsz))
buf = mst_unseal(raw_rec)
check('读回 /vdir 的 MFT 记录（%r）' % bytes(buf[0:4]), buf[0:4] == b'FILE')
ir_off, ir_voff = index_root_loc(buf)
check('  找到它的 $INDEX_ROOT($I30)', ir_off is not None)

loc = None
if e is not None and ir_off is not None:
    eo = struct.unpack_from('<I', buf, ir_off + ir_voff + 16)[0]
    # 实测钉死的偏移关系：条目起点 = 属性起点 + 值起点 + 条目在值内的偏移
    # （$INDEX_ROOT 值里第 0x10 起才是 INDEX_HEADER，entries_offset 也已经含在
    #   invlib 返回的 e['off'] 里，所以这里【不能再 +16】—— 第一版多加了 16，
    #   读到的 u64 是 446676598832 这种鬼数，注入自然空转。）
    cand = ir_off + ir_voff + e['off']
    if cand + 8 <= nt.recsz:
        loc = cand
check('  定位到 sub 条目的记录内偏移（%s）' % loc, loc is not None)
if loc is not None:
    _nm = bytes(buf[loc + 0x10 + 0x42:loc + 0x10 + 0x42 + 6]).decode('utf-16-le', 'replace')
    check('    该偏移读出来的名字确实是 %r（对齐验证）' % _nm, _nm == 'sub')

patched = False
if loc is not None:
    old_ref = struct.unpack_from('<Q', buf, loc)[0]
    check('    注入前它指向 ino %s（应等于 sub=%s）' % (old_ref & 0xFFFFFFFFFFFF, ino_sub),
          (old_ref & 0xFFFFFFFFFFFF) == ino_sub)
    struct.pack_into('<Q', buf, loc, (ino_file & 0xFFFFFFFFFFFF) | (1 << 48))
    sealed = mst_seal(buf)
    with open(RAW, 'r+b') as f:
        f.seek(rec_base)
        f.write(sealed)
    patched = True

# ─────────────── 注入后：裁判必须报违背，imgtool 必须拒绝 ───────────────
rc2, out2 = inventory(RAW, LBA)
check('★注入后独立裁判必须报违背（退出码 1）', patched and rc2 == 1, out2[-400:])
check('  并且点名"索引项在子记录里没有对应 $FILE_NAME"', '索引项在子记录里没有对应' in out2, out2[-400:])

IMG2 = os.path.join(W, 'evil.qcow2')
subprocess.run(['qemu-img', 'convert', '-O', 'qcow2', RAW, IMG2], check=True)
lp = os.path.join(W, 'evil_in.txt')
with open(lp, 'wb') as f:
    f.write(b'Q' * 8)
r3 = run('ntfs.put', img=IMG2, lba=LBA, src=lp, dst='/vdir/sub/zz.txt', mkdir='true')
check('★★imgtool 往幽灵父里写必须当场拒绝（不再"报成功"）', not r3.get('ok'), json.dumps(r3)[:240])
check('  错误码就是 NTFS-DIR-PHANTOM', code(r3) == 'NTFS-DIR-PHANTOM', json.dumps(r3)[:240])

rp = run('ntfs.parent', img=IMG2, lba=LBA, path='/vdir/sub')
check('只读的 ntfs.parent 也不给幽灵目录发"一致"合格证',
      (not rp.get('ok')) or not (rp.get('data') or {}).get('consistent', False), json.dumps(rp)[:240])

print('════ 结果：通过 %d · 失败 %d ════' % (ok, bad))
shutil.rmtree(W, ignore_errors=True)
sys.exit(0 if bad == 0 else 1)
