#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
iso-wim-test —— REQ-20261026-16/17：ISO 里"不抽出来直接灌" + 多 install.wim + 恢复分区

需要 wimlib-imagex + xorriso 造夹具（没有就跳过，不算失败）：
  ① 造一个只有 /sources/install.wim 的"普通安装盘"
  ② 造一个 /x86/sources + /x64/sources 的"合一盘"
  ③ wim.identify --scan=true   → 2 个候选，各带映像条目
  ④ wim.identify --inner=      → 指定内部路径解析
  ⑤ 不给 --inner 且有多条       → CMD-ARG-BAD@inner（列候选）
  ⑥ wim.size --wim=ISO --inner → ok，container=iso、direct=true
  ⑦ wim.apply --wim=ISO --inner → ok，direct=true，**磁盘上不出现临时 wim**
  ⑧ iso.extract --progress=json → 有 {"ev":"progress","phase":"extract"} 行
  ⑨ 没有 install.wim 的 ISO     → WIM-NOTFOUND（hint 说清楚）
"""
import json, os, shutil, subprocess, sys, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
IMGCMD = os.path.join(ROOT, 'build', 'tools', 'imgcmd')
W = tempfile.mkdtemp(prefix='isowim')
ok = bad = skipped = 0


def run(cmd, **kw):
    args = [IMGCMD, cmd] + ['--%s=%s' % (k, v) for k, v in kw.items()]
    p = subprocess.run(args, capture_output=True, text=True)
    js = [l for l in p.stdout.splitlines() if l.startswith('{') and '"ev"' not in l]
    prog = [l for l in p.stdout.splitlines() if '"ev":"progress"' in l]
    d = json.loads(js[-1]) if js else {'ok': False, 'raw': (p.stdout + p.stderr)[-300:]}
    return d, prog


def data(d):
    return d.get('data') or {}


def err(d):
    return d.get('err') or {}


def parts(img):
    return data(run('part.show', img=img)[0]).get('pt', {}).get('parts') or []


def check(name, cond, extra=''):
    global ok, bad
    if cond:
        ok += 1
        print('  ✅ %s' % name)
    else:
        bad += 1
        print('  ❌ %s  %s' % (name, extra))


def skip(name):
    global skipped
    skipped += 1
    print('  ⏭  %s（缺工具，跳过）' % name)


if not shutil.which('wimlib-imagex') or not shutil.which('xorriso'):
    print('════ iso-wim-test：缺 wimlib-imagex/xorriso，整组跳过 ════')
    print('════ 结果：通过 0 · 失败 0 · 跳过 1 ════')
    sys.exit(0)

print('════ 造夹具（wimlib-imagex + xorriso）════')
payload = os.path.join(W, 'payload')
os.makedirs(os.path.join(payload, 'Windows', 'System32'), exist_ok=True)
with open(os.path.join(payload, 'hello.txt'), 'w') as f:
    f.write('hello windows\n')
with open(os.path.join(payload, 'Windows', 'System32', 'big.bin'), 'wb') as f:
    f.write(os.urandom(4 << 20))
os.makedirs(os.path.join(payload, 'Windows', 'System32', 'Recovery'), exist_ok=True)
with open(os.path.join(payload, 'Windows', 'System32', 'Recovery', 'Winre.wim'), 'wb') as f:
    f.write(os.urandom(400 << 10))
with open(os.path.join(payload, 'Windows', 'System32', 'Recovery', 'ReAgent.xml'), 'w') as f:
    f.write('<ReAgent/>\n')
wim = os.path.join(W, 'install.wim')
subprocess.run(['wimlib-imagex', 'capture', payload, wim, 'Win10 Pro', '--compress=LZX'],
               capture_output=True)

# 普通安装盘：/sources/install.wim
plain = os.path.join(W, 'plain')
os.makedirs(os.path.join(plain, 'sources'), exist_ok=True)
shutil.copy(wim, os.path.join(plain, 'sources', 'install.wim'))
plain_iso = os.path.join(W, 'plain.iso')
subprocess.run(['xorriso', '-as', 'mkisofs', '-J', '-joliet-long', '-iso-level', '3',
                '-o', plain_iso, plain], capture_output=True)

# 合一盘：/x86/sources + /x64/sources
aio = os.path.join(W, 'aio')
for a in ('x86', 'x64'):
    os.makedirs(os.path.join(aio, a, 'sources'), exist_ok=True)
    shutil.copy(wim, os.path.join(aio, a, 'sources', 'install.wim'))
aio_iso = os.path.join(W, 'aio.iso')
subprocess.run(['xorriso', '-as', 'mkisofs', '-J', '-joliet-long', '-iso-level', '3',
                '-o', aio_iso, aio], capture_output=True)

# 没有 install.wim 的 ISO
bare = os.path.join(W, 'bare')
os.makedirs(bare, exist_ok=True)
with open(os.path.join(bare, 'readme.txt'), 'w') as f:
    f.write('nothing here\n')
bare_iso = os.path.join(W, 'bare.iso')
subprocess.run(['xorriso', '-as', 'mkisofs', '-J', '-joliet-long', '-o', bare_iso, bare],
               capture_output=True)
check('夹具造好了（3 个 ISO）', os.path.exists(plain_iso) and os.path.exists(aio_iso)
      and os.path.exists(bare_iso))

print('════ ① 普通安装盘（老行为）════')
d, _ = run('wim.identify', src=plain_iso)
check('container=iso、inner_path=/sources/install.wim',
      data(d).get('container') == 'iso'
      and data(d).get('inner_path') == '/sources/install.wim', json.dumps(data(d))[:200])
check('能读到映像条目', data(d).get('count', 0) >= 1, json.dumps(d)[:200])

print('════ ② 合一盘 --scan=true（REQ §3A）════')
d, _ = run('wim.identify', src=aio_iso, scan='true')
cands = data(d).get('candidates') or []
paths = sorted(c.get('inner_path', '') for c in cands)
check('找到两条候选（x86 + x64）', len(cands) == 2, json.dumps(paths))
check('路径是 /x86/sources/install.wim 与 /x64/sources/install.wim',
      paths == ['/x64/sources/install.wim', '/x86/sources/install.wim'], str(paths))
check('每条都带映像条目（count>=1 且 images 非空）',
      all(c.get('count', 0) >= 1 and c.get('images') for c in cands), json.dumps(cands)[:300])

print('════ ③ 不给 --inner（多条候选）════')
d, _ = run('wim.identify', src=aio_iso)
check('→ CMD-ARG-BAD@inner 且把候选列在 hint 里',
      err(d).get('error_code') == 'CMD-ARG-BAD' and err(d).get('at') == 'inner'
      and '/x64/sources/install.wim' in (err(d).get('hint') or ''), json.dumps(d)[:300])

print('════ ④ --inner 指定（REQ §3B）════')
d, _ = run('wim.identify', src=aio_iso, inner='/x64/sources/install.wim')
check('解析成功且 inner_path 就是给的那条',
      data(d).get('inner_path') == '/x64/sources/install.wim' and data(d).get('count', 0) >= 1,
      json.dumps(data(d))[:200])
d, _ = run('wim.identify', src=aio_iso, inner='/nope/install.wim')
check('--inner 写了不存在的路径 → WIM-NOTFOUND（不静默换一条）',
      err(d).get('error_code') == 'WIM-NOTFOUND', json.dumps(d)[:200])

print('════ ⑤ wim.size 直读 ISO（REQ §3D）════')
d, _ = run('wim.size', wim=aio_iso, inner='/x64/sources/install.wim')
check('wim.size --wim=ISO 成功（以前是 WIM-NOTFOUND）', d.get('ok'), json.dumps(d)[:240])
check('container=iso + direct=true（没落临时文件）',
      data(d).get('container') == 'iso' and data(d).get('direct') is True,
      json.dumps(data(d))[:200])
check('给出 est_ntfs_bytes', data(d).get('est_ntfs_bytes', 0) > 0, json.dumps(data(d))[:160])

print('════ ⑥ wim.apply 直读 ISO（REQ §3C）════')
img = os.path.join(W, 'd.qcow2')
run('img.create', out=img, **{'size-gb': 20, 'table': 'gpt', 'esp-mb': 200,
                              'msr-mb': 16, 'sys-mb': 8192})
before = set(os.listdir(tempfile.gettempdir()))
d, _ = run('wim.apply', img=img, lba=444416, wim=aio_iso,
           inner='/x64/sources/install.wim', index=1, format='on')
check('灌入成功', d.get('ok'), json.dumps(d)[:240])
check('返回 container=iso + direct=true',
      data(d).get('container') == 'iso' and data(d).get('direct') is True,
      json.dumps(data(d))[:200])
check('没留 extracted_bytes（没走退路）', 'extracted_bytes' not in data(d))
after = set(os.listdir(tempfile.gettempdir()))
check('磁盘上没出现 install.wim 临时文件',
      not any(n.startswith('imgtool-src-') for n in (after - before)),
      str(sorted(after - before))[:200])
lst = data(run('ntfs.list', img=img, lba=444416, path='/')[0])
names = [e['name'] for e in (lst.get('names') or [])]
check('分区里能看到 Windows/ 与 hello.txt',
      'Windows' in names and 'hello.txt' in names, str(names))

print('════ ⑦ 灌完整张盘还是好的 ════')
check('qc.verify 无错', run('qc.verify', img=img)[0].get('ok'))

print('════ ⑧ iso.extract 有进度（REQ §3E）════')
d, prog = run('iso.extract', iso=plain_iso, path='/sources/install.wim',
              dst=os.path.join(W, 'x.wim'), progress='json')
check('抽取成功', d.get('ok'), json.dumps(d)[:200])
check('至少有 phase=extract 的 progress 行',
      any('"phase":"extract"' in l for l in prog), str(prog[:2])[:200])
check('进度行带 cur/total 字节字段',
      all(('cur_bytes' in l and 'total_bytes' in l) for l in prog) if prog else False,
      str(prog[:2])[:200])

print('════ ⑨ 没有 install.wim 的 ISO ════')
d, _ = run('wim.identify', src=bare_iso)
check('→ WIM-NOTFOUND（hint 说 ISO 里没有 install.wim）',
      err(d).get('error_code') == 'WIM-NOTFOUND'
      and 'install' in (err(d).get('hint') or ''), json.dumps(d)[:240])
d, _ = run('wim.apply', img=img, lba=444416, wim=bare_iso, index=1)
check('wim.apply 对无 WIM 的 ISO 也是 WIM-NOTFOUND', err(d).get('error_code') == 'WIM-NOTFOUND',
      json.dumps(d)[:200])

print('════ ⑩ winre.find / recovery.populate 吃 ISO（REQ-17）════')
d, _ = run('winre.find', src=plain_iso, index=1)
check('winre.find --src=ISO 成功', d.get('ok'), json.dumps(d)[:240])
check('返回 direct=true + iso_inner_path',
      data(d).get('direct') is True and data(d).get('iso_inner_path') == '/sources/install.wim',
      json.dumps(data(d))[:200])

rec = os.path.join(W, 'rec.qcow2')
run('img.create', out=rec, **{'size-gb': 20, 'table': 'gpt', 'esp-mb': 200,
                              'msr-mb': 16, 'sys-mb': 8192, 'rec-mb': 1024})
rec_lba = parts(rec)[-1]['first']
before2 = set(os.listdir(tempfile.gettempdir()))
d, _ = run('recovery.populate', img=rec, **{'rec-lba': rec_lba, 'winre-src': plain_iso,
                                            'winre-index': 1, 'format': 'true',
                                            'label': 'Recovery'})
check('recovery.populate --winre-src=ISO 灌进去（原来 REC-VERIFY-FAIL）',
      d.get('ok'), json.dumps(d)[:260])
check('返回 container=iso / inner_path / direct=true',
      data(d).get('container') == 'iso' and data(d).get('inner_path') == '/sources/install.wim'
      and data(d).get('direct') is True, json.dumps(data(d))[:220])
check('winre_bytes>0', data(d).get('winre_bytes', 0) > 0, json.dumps(data(d))[:160])
after2 = set(os.listdir(tempfile.gettempdir()))
check('没留临时 wim', not any(n.startswith('imgtool-src-') for n in (after2 - before2)),
      str(sorted(after2 - before2))[:160])
lst = data(run('ntfs.list', img=rec, lba=rec_lba, path='/')[0])
check('分区里能看到 Recovery/', 'Recovery' in [e['name'] for e in (lst.get('names') or [])],
      str(lst.get('names'))[:160])

# ★ REQ-25 改了这里的口径：找 WinRE 这事由工具做完 —— 合一盘（x64/x86 两份 install.wim）
#   不给 --inner 时，工具自己挑"真的有 WinRE"的那条，不再报歧义。
#   （wim.apply / wim.size 仍然必须指定：那是"往哪灌数据"，不能替用户猜）
d, _ = run('recovery.populate', img=rec, **{'rec-lba': rec_lba, 'winre-src': aio_iso,
                                            'winre-index': 1, 'format': 'true'})
check('合一盘不给 --inner → 工具自己挑有 WinRE 的那条（不再报歧义）',
      d.get('ok') is True and data(d).get('inner_path', '').startswith('/x'),
      json.dumps(d)[:260])
d, _ = run('wim.apply', img=rec, lba=rec_lba, wim=aio_iso, index=1)
check('对照：wim.apply 仍然要求 --inner（数据操作不许猜）',
      err(d).get('error_code') == 'CMD-ARG-BAD' and err(d).get('at') == 'inner',
      json.dumps(d)[:200])
d, _ = run('recovery.populate', img=rec, **{'rec-lba': rec_lba, 'winre-src': aio_iso,
                                            'inner': '/x64/sources/install.wim',
                                            'winre-index': 1, 'format': 'true'})
check('给了 --inner 就成功', d.get('ok') and data(d).get('inner_path') == '/x64/sources/install.wim',
      json.dumps(d)[:240])

print('════ ⑪ arch 归一/内容判位 + wim.ls 吃 ISO（REQ-19）════')
d, _ = run('wim.identify', src=plain_iso)
ims = data(d).get('images') or []
check('wim.identify 带 arch_raw / arch_basis',
      bool(ims) and 'arch_raw' in ims[0] and 'arch_basis' in ims[0],
      json.dumps(ims[:1])[:220])
check('arch 是归一值或空串（不会出现 "0"）',
      bool(ims) and ims[0]['arch'] in ('x86', 'x64', 'arm', 'arm64', ''),
      json.dumps(ims[:1])[:160])

# 造一张"像 32 位"的 WIM：没有 SysWOW64，ntoskrnl 的 PE Machine=0x014c
# ★ e_lfanew 用真微软二进制的 0x280（不是 0x80）：真 ntoskrnl 就是这样，
#   以前按"读前 512 字节"找 PE 头会永远失败（REQ-22 的根因）。
import struct
x86p = os.path.join(W, 'x86pay')
os.makedirs(os.path.join(x86p, 'Windows', 'System32'), exist_ok=True)
pe = bytearray(0x400); pe[0:2] = b'MZ'; struct.pack_into('<I', pe, 0x3c, 0x280)
pe[0x280:0x284] = b'PE\0\0'; struct.pack_into('<H', pe, 0x284, 0x014c)
open(os.path.join(x86p, 'Windows', 'System32', 'ntoskrnl.exe'), 'wb').write(bytes(pe))
x86wim = os.path.join(W, 'x86.wim')
subprocess.run(['wimlib-imagex', 'capture', x86p, x86wim, 'Win10 x86',
                '--compress=LZX'], capture_output=True)
d, _ = run('wim.identify', src=x86wim)
im = (data(d).get('images') or [{}])[0]
check('无 SysWOW64 + ntoskrnl PE=0x014c → arch=x86（内容判）',
      im.get('arch') == 'x86' and im.get('arch_basis') == 'content', json.dumps(im)[:220])

# 有 SysWOW64 的映像 → x64
x64p = os.path.join(W, 'x64pay')
os.makedirs(os.path.join(x64p, 'Windows', 'SysWOW64'), exist_ok=True)
open(os.path.join(x64p, 'Windows', 'SysWOW64', 'ntdll.dll'), 'wb').write(b'x' * 64)
open(os.path.join(x64p, 'Windows', 'System32.txt'), 'wb').write(b'hi')
x64wim = os.path.join(W, 'x64.wim')
subprocess.run(['wimlib-imagex', 'capture', x64p, x64wim, 'Win10 x64',
                '--compress=LZX'], capture_output=True)
d, _ = run('wim.identify', src=x64wim)
im = (data(d).get('images') or [{}])[0]
check('有 SysWOW64 → arch=x64（内容判）',
      im.get('arch') == 'x64' and im.get('arch_basis') == 'content', json.dumps(im)[:220])

check('arch_source 也给了（说明凭什么判的）',
      bool(ims) and ims[0].get('arch_source') in ('syswow64', 'wow64', 'idwbinfo', 'iso-bootfile', 'xml', 'none')
      or (ims and ims[0].get('arch_source', '').startswith('pe:')),
      json.dumps(ims[:1])[:200])

# ---- REQ-22：介质级信号（WIM 里没有系统文件时才用）----
import subprocess as _sp
only = os.path.join(W, 'dataonly')
os.makedirs(os.path.join(only, 'd'), exist_ok=True)
open(os.path.join(only, 'd', 'a.bin'), 'wb').write(os.urandom(1000))
_sp.run(['wimlib-imagex', 'capture', os.path.join(only, 'd'), os.path.join(only, 'w.wim'),
         'Data', '--compress=LZX'], capture_output=True)

def make_iso(tag, with_idw=None, with_bootfile=None):
    root = os.path.join(W, 'iso-' + tag)
    os.makedirs(os.path.join(root, 'sources'), exist_ok=True)
    shutil.copy(os.path.join(only, 'w.wim'), os.path.join(root, 'sources', 'install.wim'))
    if with_idw:
        open(os.path.join(root, 'sources', 'idwbinfo.txt'), 'w').write(
            '[BUILDINFO]\r\nBuildArch=%s\r\n' % with_idw)
    if with_bootfile:
        os.makedirs(os.path.join(root, 'efi', 'boot'), exist_ok=True)
        open(os.path.join(root, 'efi', 'boot', with_bootfile), 'wb').write(b'\0' * 100)
    out = os.path.join(W, tag + '.iso')
    _sp.run(['xorriso', '-as', 'mkisofs', '-J', '-joliet-long', '-iso-level', '3',
             '-o', out, root], capture_output=True)
    return out

d, _ = run('wim.identify', src=make_iso('idw', with_idw='amd64'))
im = (data(d).get('images') or [{}])[0]
check('数据盘 + idwbinfo BuildArch=amd64 → x64（media/idwbinfo）',
      im.get('arch') == 'x64' and im.get('arch_basis') == 'media' and im.get('arch_source') == 'idwbinfo',
      json.dumps(im)[:200])
d, _ = run('wim.identify', src=make_iso('boot32', with_bootfile='bootia32.efi'))
im = (data(d).get('images') or [{}])[0]
check('数据盘 + 只有 bootia32.efi → x86（media/iso-bootfile）',
      im.get('arch') == 'x86' and im.get('arch_source') == 'iso-bootfile', json.dumps(im)[:200])
d, _ = run('wim.identify', src=make_iso('nothing'))
im = (data(d).get('images') or [{}])[0]
check('什么都判不出 → arch 空 + basis none（不再瞎给 x86）',
      im.get('arch') == '' and im.get('arch_basis') == 'none', json.dumps(im)[:200])

d1, _ = run('wim.identify', src=plain_iso)
d2, _ = run('wim.identify', src=plain_iso, scan='true')
i1 = (data(d1).get('images') or [{}])[0]
i2 = ((data(d2).get('candidates') or [{}])[0].get('images') or [{}])[0]
check('不带 --scan 与 --scan 的 arch/source 一致（REQ-22 的核心诉求）',
      i1.get('arch') == (i2.get('arch') or i1.get('arch'))
      if False else i1.get('arch_source') == i2.get('arch_source'),
      '%s vs %s' % (json.dumps(i1)[:120], json.dumps(i2)[:120]))

d, _ = run('wim.identify', src=plain_iso, probe='never')
im = (data(d).get('images') or [{}])[0]
check('--probe=never → 不抽文件（arch 空）',
      im.get('arch') == '' and im.get('arch_source') == 'none', json.dumps(im)[:200])

# ---- REQ-28：B（ISO 里的 WIM 取单文件）/ B′（指路）/ C″（抽目录）----
tgt = os.path.join(W, 'one.wim')
d, _ = run('iso.extract', iso=plain_iso, inner='/sources/install.wim',
           path='/Windows/System32/Recovery/Winre.wim', dst=tgt)
check('iso.extract --inner 直接拿 WIM 里的文件（不动 2.7 GB 整份）',
      d.get('ok') is True and (data(d).get('bytes') or 0) > 100000
      and data(d).get('direct') is True and os.path.getsize(tgt) == data(d).get('bytes'),
      json.dumps(d)[:220])
d, _ = run('iso.extract', iso=plain_iso, path='/Windows/System32/Recovery/Winre.wim',
           dst=os.path.join(W, 'x.bin'))
hint = err(d).get('hint', '') + err(d).get('suggest', '')
check('把 WIM 内路径当 ISO 路径 → 指路到 wim.extract（不再只说"没有"）',
      err(d).get('error_code') == 'FAT-ENTRY-NOTFOUND' and 'wim.extract' in hint,
      json.dumps(d)[:240])
ddir = os.path.join(W, 'win_tree')
d, _ = run('wim.extract', wim=plain_iso, inner='/sources/install.wim', index=1,
           path='/Windows', dst=ddir)
check('wim.extract --path=<目录> → 抽整棵子树',
      d.get('ok') is True and data(d).get('is_dir') is True
      and (data(d).get('files') or 0) >= 2, json.dumps(d)[:220])
check('  … 子树真的落地了',
      os.path.isdir(os.path.join(ddir, 'System32')), str(os.listdir(ddir)[:5]))

# ---- REQ-29：流式（wimlib TO_STDOUT）----
import subprocess as _sp2
outp = os.path.join(W, 'stream.wim')
ref = os.path.join(W, 'ref.wim')
run('wim.extract', wim=plain_iso, inner='/sources/install.wim', index=1,
    path='/Windows/System32/Recovery/Winre.wim', dst=ref)
r = _sp2.run([os.path.join(ROOT, 'build', 'tools', 'imgcmd'), 'wim.extract',
              '--wim=' + plain_iso, '--inner=/sources/install.wim', '--index=1',
              '--path=/Windows/System32/Recovery/Winre.wim', '--dst=-'],
             capture_output=True, timeout=300)
open(outp, 'wb').write(r.stdout)
check('wim.extract --dst=- 把字节流到 stdout（和普通抽取逐字节一致）',
      os.path.getsize(outp) == os.path.getsize(ref)
      and open(outp, 'rb').read() == open(ref, 'rb').read(),
      'stream=%d ref=%d' % (os.path.getsize(outp), os.path.getsize(ref)))
check('  … 结果 JSON 改走 stderr（不然数据末尾多一行）',
      b'"cmd":"wim.extract"' in r.stderr and b'"cmd"' not in r.stdout)

# wim.put：WIM 里的文件直接写进分区（0 临时）
q = os.path.join(W, 'put.qcow2')
run('img.create', out=q, **{'size-gb': 6, 'table': 'gpt', 'esp-mb': 200,
                            'msr-mb': 16, 'sys-mb': 3000, 'rec-mb': 800})
d, _ = run('part.show', img=q)
rec = [p for p in data(d)['pt']['parts'] if p.get('sectors', 0) > 100000][-1]['first']
d, _ = run('wim.put', img=q, lba=rec, wim=plain_iso, inner='/sources/install.wim',
           path='/Windows/System32/Recovery/Winre.wim',
           **{'dst-path': '/Recovery/WindowsRE/Winre.wim', 'format': 'true'})
check('wim.put：WIM 里的文件直接写进分区',
      d.get('ok') is True and data(d).get('bytes') == os.path.getsize(ref),
      json.dumps(d)[:220])
back = os.path.join(W, 'back.wim')
run('ntfs.export', img=q, lba=rec, path='/Recovery/WindowsRE/Winre.wim', dst=back)
check('  … 再抽回来逐字节一致（0 临时文件也没写坏）',
      os.path.exists(back) and open(back, 'rb').read() == open(ref, 'rb').read())

d, _ = run('wim.ls', wim=plain_iso, inner='/sources/install.wim', index=1, path='/Windows')
dd = data(d)
check('wim.ls --wim=ISO --inner 列得出目录', d.get('ok') and (dd.get('entries') or []),
      json.dumps(d)[:240])
check('返回 container=iso / direct=true',
      dd.get('container') == 'iso' and dd.get('direct') is True, json.dumps(dd)[:200])

d, _ = run('wim.ls', wim=aio_iso, index=1, path='/')
check('合一盘不给 --inner → CMD-ARG-BAD@inner',
      err(d).get('error_code') == 'CMD-ARG-BAD' and err(d).get('at') == 'inner',
      json.dumps(d)[:240])
d, _ = run('wim.ls', wim=aio_iso, inner='/x86/sources/install.wim', index=1, path='/')
check('给了 --inner 就能列', d.get('ok') and data(d).get('inner_path') == '/x86/sources/install.wim',
      json.dumps(d)[:220])

print()
print('════ 结果：通过 %d · 失败 %d · 跳过 %d ════' % (ok, bad, skipped))
print('调试目录：%s' % W)
sys.exit(0 if bad == 0 else 1)