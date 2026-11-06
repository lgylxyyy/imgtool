#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""boot-populate-test —— REQ-20261026-20：一条命令铺完引导

需要 wimlib-imagex 造夹具（没有就跳过，不算失败）：
  ⑦ 内置 MBR 模板与 reference/mbr-code-bios.bin 逐字节一致（sha256）
  ① UEFI：ESP 没文件系统 → 自动格 FAT32；bootmgfw 写两份（含 \\EFI\\Boot\\bootx64.efi）
  ② UEFI：Fonts / boot.sdi / BCD 都落位；boot.check required_fail=0
  ③ 幂等：第二遍不格已有 ESP（formatted=false），结果仍然 required_fail=0
  ④ BIOS：SR 自动格 NTFS + 活动标志 + bootmgr + 内置 MBR 码 + 内置 VBR 码 + BCD
  ⑤ BIOS：boot.check required_fail=0（且 SR 不再被误当恢复分区报警）
  ⑥ 系统分区里没有 \\Windows\\Boot 时 → BOOT-SRC-MISSING + files[] 里 ok=false
"""
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
IMGCMD = os.path.join(ROOT, 'build', 'tools', 'imgcmd')
W = tempfile.mkdtemp(prefix='bootpop')
ok = bad = skipped = 0


def run(cmd, **kw):
    args = [IMGCMD, cmd] + ['--%s=%s' % (k, v) for k, v in kw.items()]
    p = subprocess.run(args, capture_output=True, text=True)
    js = [l for l in p.stdout.splitlines() if l.startswith('{') and '"ev"' not in l]
    d = json.loads(js[-1]) if js else {'ok': False, 'raw': (p.stdout + p.stderr)[-300:]}
    return d, p


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


have_wimlib = shutil.which('wimlib-imagex') is not None


def make_wim(dst, with_boot=True):
    pay = os.path.join(W, 'pay-' + os.path.basename(dst))
    shutil.rmtree(pay, ignore_errors=True)
    os.makedirs(os.path.join(pay, 'Windows', 'System32'), exist_ok=True)
    for fn in ('winload.exe', 'winload.efi'):
        with open(os.path.join(pay, 'Windows', 'System32', fn), 'wb') as f:
            f.write(b'MZ' + b'\0' * 62)
    if with_boot:
        for d in ('Boot/EFI', 'Boot/PCAT', 'Boot/Fonts', 'Boot/DVD/EFI'):
            os.makedirs(os.path.join(pay, 'Windows', d), exist_ok=True)
        with open(os.path.join(pay, 'Windows', 'Boot', 'EFI', 'bootmgfw.efi'), 'wb') as f:
            f.write(os.urandom(300 << 10))
        with open(os.path.join(pay, 'Windows', 'Boot', 'PCAT', 'bootmgr'), 'wb') as f:
            f.write(os.urandom(400 << 10))
        for fn in ('msyh_boot.ttf', 'segoe_slboot.ttf'):
            with open(os.path.join(pay, 'Windows', 'Boot', 'Fonts', fn), 'wb') as f:
                f.write(os.urandom(8 << 10))
        with open(os.path.join(pay, 'Windows', 'Boot', 'DVD', 'EFI', 'boot.sdi'), 'wb') as f:
            f.write(os.urandom(50 << 10))
    subprocess.run(['wimlib-imagex', 'capture', pay, dst, 'Test Image',
                    '--compress=LZX'], capture_output=True)
    return dst


print('════ ⑦ 内置 MBR 模板完整性 ════')
ref = os.path.join(ROOT, 'reference', 'mbr-code-bios.bin')
hdr = os.path.join(ROOT, 'src', 'img_mbr_tpl.h')
if os.path.exists(ref) and os.path.exists(hdr):
    sha = hashlib.sha256(open(ref, 'rb').read()).hexdigest()
    txt = open(hdr, encoding='utf-8').read()
    check('img_mbr_tpl.h 的 sha256 == reference/mbr-code-bios.bin', sha in txt, sha)
    check('模板就是 440 字节', os.path.getsize(ref) == 440, str(os.path.getsize(ref)))
else:
    skip('MBR 模板完整性')

if not have_wimlib:
    skip('boot.populate（没有 wimlib-imagex）')
else:
    win = make_wim(os.path.join(W, 'win.wim'))

    print('════ ① UEFI：自动格 ESP + 铺文件 ════')
    u = os.path.join(W, 'u.qcow2')
    run('img.create', out=u, **{'size-gb': 20, 'table': 'gpt', 'esp-mb': 200,
                                'msr-mb': 16, 'sys-mb': 8192})
    p_u = parts(u)
    sys_lba = p_u[-1]['first']
    esp_lba = p_u[0]['first']
    d, _ = run('wim.apply', img=u, lba=sys_lba, wim=win, index=1, format='on')
    check('前置：wim.apply 成功', d.get('ok'), json.dumps(d)[:200])

    d, _ = run('boot.populate', img=u, **{'sys-lba': sys_lba, 'esp-lba': esp_lba})
    check('boot.populate ok', d.get('ok'), json.dumps(d)[:300])
    dd = data(d)
    check('自动格式化了 ESP（formatted=true）', dd.get('formatted') is True, str(dd.get('formatted')))
    files = dd.get('files') or []
    check('files[] 全部 ok', bool(files) and all(f['ok'] for f in files),
          str([f for f in files if not f['ok']])[:200])
    names = [f['path'] for f in files]
    check('写了两份 bootmgfw', '/EFI/Microsoft/Boot/bootmgfw.efi' in names
          and '/EFI/Boot/bootx64.efi' in names, str(names)[:220])
    check('拷了 Fonts', any('/Fonts/' in n for n in names), str(names)[:220])
    check('拷了 boot.sdi', '/EFI/Microsoft/Boot/boot.sdi' in names, str(names)[:220])
    check('bcd_bytes > 0', dd.get('bcd_bytes', 0) > 0, str(dd.get('bcd_bytes')))

    print('════ ② UEFI 自检 ════')
    bc = dd.get('boot_check') or {}
    check('boot_check.required_fail == 0', bc.get('required_fail') == 0, json.dumps(bc))
    d2, _ = run('boot.check', img=u, mode='uefi')
    check('独立 boot.check 也是 required_fail=0',
          data(d2).get('required_fail') == 0, json.dumps(data(d2))[:200])

    print('════ ③ 幂等：第二遍不格已有 ESP ════')
    d, _ = run('boot.populate', img=u, **{'sys-lba': sys_lba, 'esp-lba': esp_lba})
    dd2 = data(d)
    check('第二遍 still ok', d.get('ok'), json.dumps(d)[:200])
    check('formatted=false（没动已有文件系统）', dd2.get('formatted') is False, str(dd2.get('formatted')))
    check('第二遍 required_fail 仍是 0',
          (dd2.get('boot_check') or {}).get('required_fail') == 0, json.dumps(dd2.get('boot_check')))
    lst = data(run('fat.ls', img=u, lba=esp_lba, path='/EFI/Boot')[0])
    check('ESP 上 \\EFI\\Boot 里确实有 .efi',
          any((e.get('name') or '').lower().endswith('.efi') for e in (lst.get('entries') or [])),
          json.dumps(lst.get('entries'))[:200])

    print('════ ④⑤ BIOS：格 SR + 活动 + bootmgr + MBR/VBR 码 + BCD + 自检 ════')
    m = os.path.join(W, 'm.qcow2')
    run('img.create', out=m, **{'size-gb': 20, 'table': 'mbr', 'sys-mb': 512, 'win-mb': 12000})
    ps = parts(m)
    win_lba = ps[-1]['first']
    run('wim.apply', img=m, lba=win_lba, wim=win, index=1, format='on')
    d, _ = run('boot.populate', img=m)
    dd = data(d)
    check('BIOS boot.populate ok', d.get('ok'), json.dumps(d)[:300])
    check('自动找到系统分区（带 winload 的那个）', dd.get('sys_lba') == win_lba,
          'sys_lba=%s expect=%s' % (dd.get('sys_lba'), win_lba))
    check('格式化了系统保留分区', dd.get('formatted') is True, str(dd.get('formatted')))
    names = [f['path'] for f in (dd.get('files') or [])]
    check('写了 /bootmgr', '/bootmgr' in names, str(names))
    check('写了 MBR 引导码', '(LBA0)' in names, str(names))
    check('写了 VBR 引导码', '(VBR)' in names, str(names))
    pt = data(run('part.show', img=m)[0]).get('pt') or {}
    check('LBA0 有引导码（hasBootCode）', pt.get('hasBootCode') is True, str(pt.get('hasBootCode')))
    bcd_lst = data(run('ntfs.list', img=m, lba=ps[0]['first'], path='/Boot')[0])
    check('SR 上有 BCD', any((e.get('name') or '').upper() == 'BCD'
                            for e in (bcd_lst.get('names') or [])),
          json.dumps(bcd_lst.get('names'))[:200])
    bc = dd.get('boot_check') or {}
    check('BIOS boot_check.required_fail == 0', bc.get('required_fail') == 0, json.dumps(bc))
    check('BIOS 不再把 SR 误当恢复分区（warn_fail=0）', bc.get('warn_fail') == 0, json.dumps(bc))

    print('════ ⑥ 系统分区里没有 Boot 目录 → BOOT-SRC-MISSING ════')
    emptywin = make_wim(os.path.join(W, 'noboot.wim'), with_boot=False)
    e = os.path.join(W, 'e.qcow2')
    run('img.create', out=e, **{'size-gb': 10, 'table': 'gpt', 'esp-mb': 200,
                                'msr-mb': 16, 'sys-mb': 6144})
    sys_lba2 = parts(e)[-1]['first']
    run('wim.apply', img=e, lba=sys_lba2, wim=emptywin, index=1, format='on')
    d, _ = run('boot.populate', img=e, **{'sys-lba': sys_lba2})
    check('→ BOOT-SRC-MISSING', err(d).get('error_code') == 'BOOT-SRC-MISSING',
          json.dumps(d)[:300])
    files = data(d).get('files') or []
    check('files[] 里标出哪个缺（ok=false）',
          bool(files) and any(f['ok'] is False for f in files), json.dumps(files)[:250])
    check('缺文件时 boot_check 也带回来（App 不用再调）',
          (data(d).get('boot_check') or {}).get('required_fail', 0) >= 1,
          json.dumps(data(d).get('boot_check')))

print()
print('════ 结果：通过 %d · 失败 %d · 跳过 %d ════' % (ok, bad, skipped))
print('调试目录：%s' % W)
sys.exit(0 if bad == 0 else 1)
