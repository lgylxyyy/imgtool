#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""boot-plan-repair-test —— 修复计划（boot.plan --what=repair）+ WinRE 解耦

① UEFI 干净盘：每样物件都有来源（disk/template），needs_image=false
② 物件事项齐全（bootmgfw/boot_sdi/fonts/bcd/efi_fallback）
③ 没铺过引导的盘：present=false（App 知道要修什么）
④ EFI 兜底三态：absent → ours（boot.populate 写完）→ foreign（被别人换成垃圾）
⑤ WinRE：恢复分区空 → recovery_partition_empty=true 且 winre_present=false
⑥ WinRE：填了 winre.wim → winre_present=true + 版本号 + location
⑦ boot.check 不再输出 rec.* 项（与 WinRE 彻底解耦）
"""
import json, os, shutil, subprocess, sys, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
IMGCMD = os.path.join(ROOT, 'build', 'tools', 'imgcmd')
W = tempfile.mkdtemp(prefix='bootplan')
ok = bad = skipped = 0

def run(cmd, **kw):
    args = [IMGCMD, cmd] + ['--%s=%s' % (k, v) for k, v in kw.items()]
    p = subprocess.run(args, capture_output=True, text=True)
    js = [l for l in p.stdout.splitlines() if l.startswith('{') and '"ev"' not in l]
    d = json.loads(js[-1]) if js else {'ok': False, 'raw': (p.stdout + p.stderr)[-300:]}
    return d, p

def data(d): return d.get('data') or {}
def parts(img): return data(run('part.show', img=img)[0]).get('pt', {}).get('parts') or []

def check(name, cond, extra=''):
    global ok, bad
    if cond: ok += 1; print('  ✅ %s' % name)
    else: bad += 1; print('  ❌ %s  %s' % (name, extra))

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
    subprocess.run(['wimlib-imagex', 'capture', pay, dst, 'Test Image', '--compress=LZX'],
                   capture_output=True)
    return dst

if not have_wimlib:
    skip('boot.plan --what=repair（没有 wimlib-imagex）')
    print('\n════ 结果：通过 %d · 失败 %d · 跳过 %d ════' % (ok, bad, skipped))
    sys.exit(0)

win = make_wim(os.path.join(W, 'win.wim'))

print('════ ①③ UEFI：空白盘的计划（present 应为 false） ════')
u = os.path.join(W, 'u.qcow2')
run('img.create', out=u, **{'size-gb': 12, 'table': 'gpt', 'esp-mb': 200,
                            'msr-mb': 16, 'sys-mb': 8192})
pu = parts(u); sys_lba = pu[-1]['first']; esp_lba = pu[0]['first']
run('wim.apply', img=u, lba=sys_lba, wim=win, index=1, format='on')
d, _ = run('boot.plan', img=u, what='repair')
dd = data(d)
check('ok', d.get('ok'), json.dumps(d)[:300])
check('mode=gpt/uefi', dd.get('mode') == 'gpt/uefi', str(dd.get('mode')))
check('sys_lba 自动找到', dd.get('sys_lba') == sys_lba, str(dd.get('sys_lba')))
check('boot_partition=esp', dd.get('boot_partition') == 'esp', str(dd.get('boot_partition')))
items = {i['id']: i for i in (dd.get('items') or [])}
check('物件齐全（bootmgfw/boot_sdi/fonts/bcd/efi_fallback）',
      set(items) == {'bootmgfw', 'boot_sdi', 'fonts', 'bcd', 'efi_fallback'}, str(sorted(items)))
check('bootmgfw 来源=disk 且 available', items.get('bootmgfw', {}).get('from') == 'disk'
      and items['bootmgfw']['available'], json.dumps(items.get('bootmgfw')))
check('bcd 来源=template（永远可用）', items.get('bcd', {}).get('from') == 'template'
      and items['bcd']['available'], json.dumps(items.get('bcd')))
check('needs_image=false（盘里 \\Windows\\Boot 齐全）', dd.get('needs_image') is False,
      str(dd.get('needs_image')))
check('verdict=fixable', dd.get('verdict') == 'fixable', str(dd.get('verdict')))
check('① 未铺引导 → bootmgfw.present=false', items['bootmgfw']['present'] is False,
      json.dumps(items['bootmgfw']))
check('④ EFI 兜底 absent（还没写过）', dd.get('efi_fallback_state') == 'absent',
      str(dd.get('efi_fallback_state')))

print('════ ②④ 铺完引导：present 应转 true，兜底 ours ════')
d, _ = run('boot.populate', img=u, **{'sys-lba': sys_lba, 'esp-lba': esp_lba})
check('boot.populate ok', d.get('ok'), json.dumps(d)[:200])
d, _ = run('boot.plan', img=u, what='repair')
dd = data(d); items = {i['id']: i for i in (dd.get('items') or [])}
check('bootmgfw.present=true', items['bootmgfw']['present'] is True, json.dumps(items['bootmgfw']))
check('bcd.present=true', items['bcd']['present'] is True, json.dumps(items['bcd']))
check('fonts.present=true', items['fonts']['present'] is True, json.dumps(items['fonts']))
check('efi_fallback_state=ours', dd.get('efi_fallback_state') == 'ours',
      str(dd.get('efi_fallback_state')))
check('efi_fallback_conflict=false', dd.get('efi_fallback_conflict') is False,
      str(dd.get('efi_fallback_conflict')))

print('════ ④ 换成别人的 BOOTX64.EFI → conflict ════')
junk = os.path.join(W, 'grubx64.efi')
open(junk, 'wb').write(os.urandom(123456))
d, _ = run('fat.put', img=u, lba=esp_lba, src=junk, dst='/EFI/Boot/BOOTX64.EFI')
check('前置：写垃圾 EFI 成功', d.get('ok'), json.dumps(d)[:200])
d, _ = run('boot.plan', img=u, what='repair')
dd = data(d)
check('efi_fallback_state=foreign', dd.get('efi_fallback_state') == 'foreign',
      str(dd.get('efi_fallback_state')))
check('★ efi_fallback_conflict=true（不覆盖，报给 App）',
      dd.get('efi_fallback_conflict') is True, str(dd.get('efi_fallback_conflict')))

print('════ ⑤ WinRE：恢复分区空（有分区无 wim） ════')
r = os.path.join(W, 'r.qcow2')
run('img.create', out=r, **{'size-gb': 12, 'table': 'gpt', 'esp-mb': 200,
                            'msr-mb': 16, 'sys-mb': 8192, 'rec-mb': 512})
pr = parts(r); rsys = pr[-2]['first'] if len(pr) >= 4 else pr[-1]['first']
run('wim.apply', img=r, lba=rsys, wim=win, index=1, format='on')
d, _ = run('recovery.status', img=r)
dd = data(d)
check('recovery_partition_present=true', dd.get('recovery_partition_present') is True,
      json.dumps(dd)[:200])
check('recovery_partition_empty=true（空分区）', dd.get('recovery_partition_empty') is True,
      json.dumps(dd)[:200])
check('winre_present=false（App 隐藏 WinRE 卡片）', dd.get('winre_present') is False,
      json.dumps(dd)[:200])

print('════ ⑥ WinRE：填进去之后 ════')
# 造一个真 WIM 当 winre.wim（版本读不出不算错，只要 present）
fake = os.path.join(W, 'winre.wim')
pay = os.path.join(W, 'winre-pay')
os.makedirs(pay, exist_ok=True)
open(os.path.join(pay, 'x.txt'), 'w').write('x')
subprocess.run(['wimlib-imagex', 'capture', pay, fake, 'RE', '--compress=LZX'],
               capture_output=True)
rec_lba = pr[-1]['first']
d, _ = run('recovery.populate', img=r, **{'rec-lba': rec_lba, 'winre-src': fake, 'format': 'true'})
check('前置：recovery.populate ok', d.get('ok'), json.dumps(d)[:300])
d, _ = run('recovery.status', img=r)
dd = data(d)
check('winre_present=true（App 显示 WinRE 卡片）', dd.get('winre_present') is True,
      json.dumps(dd)[:300])
check('winre_location=recovery', dd.get('winre_location') == 'recovery', str(dd.get('winre_location')))
check('wim_path 指向恢复分区', dd.get('wim_path', '').startswith('/Recovery/WindowsRE/'),
      str(dd.get('wim_path')))
check('wim_bytes>0', dd.get('wim_bytes', 0) > 0, str(dd.get('wim_bytes')))
check('recovery_partition_empty=false', dd.get('recovery_partition_empty') is False,
      str(dd.get('recovery_partition_empty')))
check('recovery_partition_attr_ok=true（0x8000…0001）',
      dd.get('recovery_partition_attr_ok') is True, str(dd.get('recovery_partition_attr')))

print('════ ⑧ 模板导出 / 应用 ════')
d, _ = run('boot.tpl-export', img=u, lba=esp_lba, uefi='true',
           out=os.path.join(W, 't1.bcdtpl'), desc='test')
dd = data(d)
check('boot.tpl-export ok', d.get('ok'), json.dumps(d)[:300])
check('devices_stripped>0（骨架无本机绑定）', dd.get('devices_stripped', 0) > 0,
      str(dd.get('devices_stripped')))
check('objects>0', dd.get('objects', 0) > 0, str(dd.get('objects')))
meta_p = os.path.join(W, 't1.bcdtpl.json')
check('元数据 JSON 生成了', os.path.exists(meta_p))
mj = json.load(open(meta_p))
check('firmware=uefi', mj.get('firmware') == 'uefi', str(mj.get('firmware')))

# 应用：元数据 unknown + 本盘 unknown → 允许（同 unknown 组合），设备照样现生成
d, _ = run('boot.tpl-apply', img=u, tpl=os.path.join(W, 't1.bcdtpl'), **{'sys-lba': sys_lba, 'force': 'true'})
dd = data(d)
check('boot.tpl-apply ok（unknown 组合 + force）', d.get('ok'), json.dumps(d)[:300])
check('mode=gpt/uefi', dd.get('mode') == 'gpt/uefi', str(dd.get('mode')))

# 跨族拒绝：把元数据改成 6.1/x86
mj['family'] = '6.1'; mj['arch'] = 'x86'
bad_meta = os.path.join(W, 't_bad.json')
json.dump(mj, open(bad_meta, 'w'))
shutil.copy(os.path.join(W, 't1.bcdtpl'), os.path.join(W, 't_bad.bcdtpl'))
d, _ = run('boot.tpl-apply', img=u, tpl=os.path.join(W, 't_bad.bcdtpl'), meta=bad_meta,
           **{'sys-lba': sys_lba})
check('跨族 → TPL-NOT-APPLICABLE', not d.get('ok')
      and (d.get('err') or {}).get('error_code') == 'TPL-NOT-APPLICABLE',
      json.dumps(d)[:300])
d, _ = run('boot.tpl-apply', img=u, tpl=os.path.join(W, 't_bad.bcdtpl'), meta=bad_meta,
           **{'sys-lba': sys_lba, 'force': 'true'})
check('--force=true 放行（并写回了 BCD）', d.get('ok'), json.dumps(d)[:300])

# 真模板（真 ISO 抽的）也能应用
real = '/workspace/templates/10.0_x64_uefi.bcdtpl'
if os.path.exists(real):
    d, _ = run('boot.tpl-apply', img=u, tpl=real, **{'sys-lba': sys_lba, 'force': 'true'})
    check('真模板 10.0_x64_uefi 应用 ok', d.get('ok'), json.dumps(d)[:200])
    check('真模板 tpl_family=10.0', data(d).get('tpl_family') == '10.0',
          str(data(d).get('tpl_family')))
else:
    skip('真模板（沙箱无 /workspace/templates）')

# 应用完必须 still 自检全绿
d, _ = run('boot.check', img=u, mode='uefi')
check('模板应用后 boot.check required_fail=0', data(d).get('required_fail') == 0,
      json.dumps(data(d))[:200])

print('════ ⑦ boot.check 不再输出 rec.* ════')
d, _ = run('boot.check', img=r)
ids = [i['id'] for i in (data(d).get('items') or [])]
check('boot.check 零 rec.* 项', not any(i.startswith('rec.') for i in ids), str(ids))

print('\n════ 结果：通过 %d · 失败 %d · 跳过 %d ════' % (ok, bad, skipped))
print('调试目录：%s' % W)
sys.exit(1 if bad else 0)
