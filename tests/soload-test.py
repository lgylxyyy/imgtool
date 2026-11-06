#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
soload-test —— 共享库必须"进得来"（REQ-20261026-18 的 CI 门禁）

教训：`-Wl,--wrap=X` 只在**可执行文件**里由链接器补上 `__real_X`；
到了共享库，`__real_X` 就成了**动态未定义符号**。App 的 libimgapp.so 带 `-z now`，
dlopen 阶段要把符号全解析完 → 一个 `U __real_close` 就让**整个 App 的 JNI 全挂**
（imgcmd 是可执行文件，所以本地一直好好的）。

本测试两道闸：
  ① 库里不许有 U __real_* / U __wrap_*（必须自己用 dlsym 定义）
  ② 真的 dlopen 一次（宿主用 ctypes；有交叉编译产物就一起查）
"""
import ctypes
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ok = bad = 0


def check(name, cond, extra=''):
    global ok, bad
    if cond:
        ok += 1
        print('  ✅ %s' % name)
    else:
        bad += 1
        print('  ❌ %s  %s' % (name, extra))


def undef(path):
    p = subprocess.run(['nm', '-D', '--undefined-only', path], capture_output=True, text=True)
    if p.returncode != 0:
        p = subprocess.run(['readelf', '-sW', '--dyn-syms', path], capture_output=True, text=True)
        syms = []
        for line in p.stdout.splitlines():
            f = line.split()
            if len(f) >= 8 and f[6] == 'UND':
                syms.append(f[7].split('@')[0])
        return syms
    return [l.split()[-1].split('@')[0] for l in p.stdout.splitlines() if l.strip()]


def check_lib(path, label):
    if not os.path.exists(path):
        print('  ⏭  %s 不存在，跳过' % label)
        return
    syms = undef(path)
    # 只盯"我们自己该定义"的 __real_/ __wrap_（注意 __realpath_chk 是 libc 的正经符号，
    # 前缀是 __realp 不是 __real_，别误伤）
    bad_real = [s for s in syms if s.startswith('__real_') or s.startswith('__wrap_')]
    check('%s：没有 U __real_*/__wrap_*' % label, not bad_real, ' '.join(bad_real[:6]))
    if path.endswith('.so') and 'android' not in path:
        try:
            ctypes.CDLL(path)
            check('%s：dlopen 成功' % label, True)
        except OSError as e:
            check('%s：dlopen 成功' % label, False, str(e)[:160])
    # 粗筛：未定义符号不该是"空的"（那样等于什么都没链上，上面的检查会假通过）
    check('%s：确实有外部依赖（%d 个）' % (label, len(syms)), len(syms) > 10)


print('════ 共享库载入门禁（REQ-18）════')
check_lib(os.path.join(ROOT, 'build', 'libimgtool.so'), '宿主 libimgtool.so')
# 交叉编译产物（有就查：Android 没有 -z now 的保护，更容易被忽略）
for abi in ('arm64-v8a', 'armeabi-v7a', 'x86_64'):
    check_lib(os.path.join(ROOT, '..', '..', 'build', 'android', abi, 'libimgtool.so'),
              'Android %s libimgtool.so' % abi)

print('════ 静态 TLS 不许长回来（REQ-21）════')
import re
for path, label in ((os.path.join(ROOT, 'build', 'libimgtool.so'), '宿主 libimgtool.so'),
                    (os.path.join(ROOT, '..', '..', 'build', 'android', 'arm64-v8a',
                                  'libimgtool.so'), 'Android arm64 libimgtool.so')):
    if not os.path.exists(path):
        continue
    out = subprocess.run(['readelf', '-SW', path], capture_output=True, text=True).stdout
    sz = 0
    for line in out.splitlines():
        # 列： [n] 名字 类型 地址 偏移 大小 …
        m = re.search(r'\]\s+\.(?:tbss|tdata)\s+\S+\s+[0-9a-fA-F]+\s+[0-9a-fA-F]+\s+([0-9a-fA-F]+)', line)
        if m:
            sz += int(m.group(1), 16)
    # 静态 TLS 会计入线程栈预算：1.5 MB 的 TLS 让 1 MB 线程直接爆（REQ-21 的根因之一）
    check('%s：静态 TLS %d B < 4096' % (label, sz), sz < 4096, '现在 %d B（>=4KB 会吃掉宿主线程栈）' % sz)

print('════ .so 里 ISO 成员真能读（REQ-27：--wrap= 必须落在 .so 自己的链接上）════')
# ★ 为什么光"能 dlopen"不够：--wrap= 挂在 INTERFACE 上时库照样载入成功、符号也齐，
#   但 __wrap_* 没人调用 → img_vfd 造的 /imgtool-vfd/N 没人认 → wimlib 一 open 就失败。
#   App 里的表现就是"任何 ISO 1~16 ms 就报打不开/没有 WinRE"，而 CLI 一切正常。
#   所以这里必须【真的从 .so 里读一次 ISO 成员】。
import tempfile, shutil, json
SO = os.path.join(ROOT, 'build', 'libimgtool.so')
DL = os.path.join(ROOT, 'build', 'tests', 'dl_cmd')
if os.path.exists(SO) and os.path.exists(DL) and shutil.which('wimlib-imagex') and shutil.which('xorriso'):
    tmp = tempfile.mkdtemp(prefix='sovfd')
    try:
        pay = os.path.join(tmp, 'pay')
        os.makedirs(os.path.join(pay, 'Windows', 'System32', 'Recovery'))
        open(os.path.join(pay, 'Windows', 'System32', 'Recovery', 'Winre.wim'), 'wb').write(b'R' * 4096)
        open(os.path.join(pay, 'Windows', 'System32', 'x.dll'), 'wb').write(b'x' * 1024)
        pe = os.path.join(tmp, 'payboot')
        os.makedirs(os.path.join(pe, 'Windows', 'System32'))
        open(os.path.join(pe, 'Windows', 'System32', 'y.txt'), 'wb').write(b'y' * 100)
        isod = os.path.join(tmp, 'iso')
        os.makedirs(os.path.join(isod, 'sources'))
        subprocess.run(['wimlib-imagex', 'capture', pe, os.path.join(isod, 'sources', 'boot.wim'),
                        'WinPE', '--compress=LZX'], capture_output=True)
        subprocess.run(['wimlib-imagex', 'capture', pay, os.path.join(isod, 'sources', 'install.wim'),
                        'Win10', '--compress=LZX'], capture_output=True)
        iso = os.path.join(tmp, 't.iso')
        subprocess.run(['xorriso', '-as', 'mkisofs', '-J', '-joliet-long', '-iso-level', '3',
                        '-o', iso, isod], capture_output=True)

        def run_dl(*args):
            r = subprocess.run([DL, SO] + list(args), capture_output=True, text=True, timeout=300)
            try:
                return json.loads(r.stdout.strip().splitlines()[-1])
            except Exception:
                return {'ok': False, '_raw': (r.stdout or r.stderr)[-300:]}

        d = run_dl('winre.find', '--src=' + iso)
        check('从 .so 里 winre.find 读 ISO → found',
              d.get('ok') is True and (d.get('data') or {}).get('found') is True,
              json.dumps(d)[:260])
        check('  … 而且真的走了 VFD 直读（direct=true）',
              (d.get('data') or {}).get('direct') is True, json.dumps(d)[:200])
        d = run_dl('wim.size', '--wim=' + iso, '--inner=/sources/install.wim', '--index=1')
        check('从 .so 里 wim.size --inner 读 ISO → ok',
              d.get('ok') is True and (d.get('data') or {}).get('direct') is True,
              json.dumps(d)[:260])

        # ---- REQ-27 验收第 2/3 条：带 --img 的命令（需要 img_ctx）也要能从 .so 跑 ----
        # dl_cmd 以前不建 ctx，等于"只测了 --src/--wim 那半边"，这次补上并锁进用例。
        q = os.path.join(tmp, 't.qcow2')
        d = run_dl('img.create', '--out=' + q, '--size-gb=6', '--table=gpt',
                   '--esp-mb=200', '--msr-mb=16', '--sys-mb=3000', '--rec-mb=800')
        check('从 .so 里 img.create → ok', d.get('ok') is True, json.dumps(d)[:200])
        d = run_dl('part.show', '--img=' + q)
        parts = (((d.get('data') or {}).get('pt') or {}).get('parts')) or []
        check('从 .so 里 part.show --img（要 img_ctx）→ ok',
              d.get('ok') is True and len(parts) >= 3, json.dumps(d)[:200])
        big = max(parts, key=lambda p: p.get('sectors', 0)) if parts else {}
        d = run_dl('wim.apply', '--img=' + q, '--lba=%d' % big.get('first', 0),
                   '--wim=' + iso, '--inner=/sources/install.wim', '--index=1')
        check('从 .so 里 wim.apply --img --wim=ISO --inner → ok（VFD 直读）',
              d.get('ok') is True and (d.get('data') or {}).get('direct') is True,
              json.dumps(d)[:260])
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
else:
    print('  ⏭  跳过（缺 dl_cmd / wimlib-imagex / xorriso）')

# 静态证据：.so 内部确实有调用被改道（0 就是这次 REQ-27 的 bug 又回来了）
if os.path.exists(SO):
    out = subprocess.run(['objdump', '-d', SO], capture_output=True, text=True).stdout
    n = out.count('__wrap_open')
    check('libimgtool.so 内部有 __wrap_open 调用点（%d 处）' % n, n >= 10,
          '0 处 = --wrap= 没落到 .so 自己的链接上（REQ-27 复发）')
    dyn = subprocess.run(['nm', '-D', SO], capture_output=True, text=True).stdout
    check('libimgtool.so 导出 __wrap_*（%d 个）' % dyn.count(' T __wrap_'),
          dyn.count(' T __wrap_') >= 10)

print()
print('════ 结果：通过 %d · 失败 %d ════' % (ok, bad))
sys.exit(0 if bad == 0 else 1)