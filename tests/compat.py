# -*- coding: utf-8 -*-
"""compat.py — 旧命令名 → 新 copy/move/rm 的测试兼容层

2026-10-24 起命令表只保留 copy / move / rm（旧的 ntfs.copy / fat.copy /
file.copy / ntfs.move / fat.move / ntfs.rm / fat.rm / ntfs.rename 已删除）。
历史测试仍在用旧名字；这个模块把它们翻译成新路径语法，并把新错误码映射回
旧断言使用的码，避免为了改名重写 100+ 条用例。

★ 这不是"兼容命令"：CLI 已经不认识旧名字了（cmd.list 46 条）。
  它只存在于测试里。
"""

OLD = {
    'ntfs.copy': 'copy', 'fat.copy': 'copy',
    'ntfs.move': 'move', 'fat.move': 'move',
    'ntfs.rm': 'rm', 'fat.rm': 'rm',
}


def _loc(img, lba, path):
    if not isinstance(path, str) or not path.startswith('/'):
        return path
    if img is None or lba is None:
        return path
    return '%s!lba%s:%s' % (img, lba, path)


def translate_kwargs(cmd, kw):
    """返回 (new_cmd, new_kw, orig_cmd)。"""
    orig = cmd
    if cmd not in OLD:
        return cmd, kw, None
    img = kw.pop('img', None)
    lba = kw.pop('lba', None)
    for k in ('src', 'dst', 'path'):
        if k in kw:
            kw[k] = _loc(img, lba, kw[k])
    if 'list_file' in kw:
        kw['list_file'] = rewrite_list(kw['list_file'], img, lba)
    # 旧 ntfs.move 用 overwrite=true；新 move 用 conflict=overwrite
    ov = kw.pop('overwrite', None)
    if ov is not None and str(ov).lower() in ('1', 'true', 'yes', 'on'):
        kw['conflict'] = 'overwrite'
    kw.pop('allow_dir_move', None)
    # 旧 fat.move 要求父目录必须存在；新 move 默认会建
    if orig == 'fat.move' and 'create_parents' not in kw:
        kw['create_parents'] = 'false'
    return OLD[cmd], kw, orig


def translate_argv(argv):
    """argv = ['fat.move', '--img=...', '--lba=...', '--src=/x', ...]"""
    if not argv or argv[0] not in OLD:
        return argv, None
    orig = argv[0]
    kv = {}
    rest = []
    for a in argv[1:]:
        if a.startswith('--') and '=' in a:
            k, v = a[2:].split('=', 1)
            kv[k] = v
        else:
            rest.append(a)
    img = kv.pop('img', None)
    lba = kv.pop('lba', None)
    for k in ('src', 'dst', 'path'):
        if k in kv:
            kv[k] = _loc(img, lba, kv[k])
    ov = kv.pop('overwrite', None)
    if ov is not None and str(ov).lower() in ('1', 'true', 'yes', 'on'):
        kv['conflict'] = 'overwrite'
    kv.pop('allow_dir_move', None)
    if orig == 'fat.move' and 'create_parents' not in kv:
        kv['create_parents'] = 'false'
    out = [OLD[orig]] + ['--%s=%s' % (k, v) for k, v in kv.items()] + rest
    return out, orig


def rewrite_list(path, img, lba):
    """把旧清单（/a>/b）改写成新路径（img!lba:/a>img!lba:/b）。"""
    try:
        lines = open(path, encoding='utf-8').read().splitlines()
    except Exception:
        return path
    out = []
    for ln in lines:
        s = ln.strip()
        if not s or s.startswith('#'):
            out.append(ln)
            continue
        if '>' not in s:
            out.append(ln)
            continue
        a, b = s.split('>', 1)
        a, b = a.strip(), b.strip()
        out.append('%s > %s' % (_loc(img, lba, a), _loc(img, lba, b)))
    newp = path + '.compat'
    with open(newp, 'w', encoding='utf-8') as f:
        f.write('\n'.join(out) + '\n')
    return newp


_NTFS = {
    'COPY-DST-EXISTS': 'NTFS-DEST-EXISTS',
    'COPY-DST-CONFLICT': 'NTFS-DEST-IS-DIR',
    'COPY-UNSUPPORTED': 'NTFS-SRC-IS-DIR',
    'COPY-INTO-SELF': 'NTFS-MOVE-INTO-SELF',
    'RM-DIR-NOT-EMPTY': 'NTFS-DIR-NOT-EMPTY',
    'PATH-SRC-NOTFOUND': 'NTFS-PATH-NOTFOUND',
    'PATH-DST-NOTFOUND': 'NTFS-PATH-NOTFOUND',
    'COPY-SAME-FILE': 'NTFS-DEST-EXISTS',
    'PATH-READONLY': 'NTFS-SRC-IS-DIR',
}
_FAT = {
    'COPY-DST-EXISTS': 'FAT-ENTRY-EXISTS',
    'COPY-DST-CONFLICT': 'FAT-ENTRY-EXISTS',
    'COPY-UNSUPPORTED': 'FAT-MOVE-BAD',
    'COPY-INTO-SELF': 'FAT-MOVE-BAD',
    'RM-DIR-NOT-EMPTY': 'FAT-DIR-NOT-EMPTY',
    'PATH-SRC-NOTFOUND': 'FAT-ENTRY-NOTFOUND',
    'PATH-DST-NOTFOUND': 'FAT-DEST-DIR-NOTFOUND',
    'COPY-SAME-FILE': 'FAT-ENTRY-EXISTS',
    'PATH-READONLY': 'FAT-MOVE-BAD',
}


def map_error(j, orig):
    if not orig or not isinstance(j, dict) or j.get('ok'):
        return j
    e = j.get('err') or j.get('error')
    if not isinstance(e, dict):
        return j
    ec = e.get('error_code', '')
    fat = orig.startswith('fat.')
    m = _FAT if fat else _NTFS
    if ec in m:
        e['error_code'] = m[ec]
        return j
    if ec == 'COPY-FAIL':
        e['error_code'] = ('FAT-COPY-FAIL' if fat else 'NTFS-COPY-FAIL') if '.copy' in orig else \
                          (('FAT-MOVE-FAIL' if fat else 'NTFS-MOVE-FAIL') if '.move' in orig else
                           ('FAT-RM-FAIL' if fat else 'NTFS-RM-FAIL'))
    return j

def _spec_to_new(s, img, wim_img=None, wim_index='1', lba_default=None):
    """旧位置串 → 新路径串。img 用于 ntfs@/fat@/iso；wim 用 wim_img。"""
    if not isinstance(s, str):
        return s
    if s.startswith('host:'):
        return s
    if s.startswith('ntfs@') or s.startswith('fat@'):
        head, path = s.split(':', 1)
        lba = head.split('@', 1)[1]
        return '%s!lba%s:%s' % (img, lba, path)
    if s.startswith('ntfs:') or s.startswith('fat:'):
        return '%s!lba%s:%s' % (img, lba_default, s.split(':', 1)[1])
    if s.startswith('iso:'):
        return '%s!:%s' % (img, s[4:])
    if s.startswith('wim:'):
        rest = s[4:]
        if rest.startswith('/'):
            return '%s!%s:%s' % (wim_img or img, wim_index, rest)
        idx, path = rest.split(':', 1)
        return '%s!%s:%s' % (wim_img or img, idx, path)
    if s.startswith('/'):
        return '%s!:%s' % (img, s)     # 裸盘内路径（老 file.copy 不允许，但容错）
    return s


def translate_filecopy(kw):
    """file.copy 的旧参数（src/dst 位置串 + src_img/dst_img/lba）→ 新 copy。"""
    img = kw.pop('img', None)
    src_img = kw.pop('src_img', None)
    dst_img = kw.pop('dst_img', None)
    src_lba = kw.pop('src_lba', None)
    dst_lba = kw.pop('dst_lba', None)
    widx = str(kw.pop('src_index', '1'))
    if 'src' in kw:
        kw['src'] = _spec_to_new(kw['src'], src_img or img, src_img or img, widx, src_lba)
    if 'dst' in kw:
        kw['dst'] = _spec_to_new(kw['dst'], dst_img or img, dst_img or img, widx, dst_lba)
    if 'list_file' in kw:
        path = kw['list_file']
        try:
            lines = open(path, encoding='utf-8').read().splitlines()
        except Exception:
            lines = []
        out = []
        for ln in lines:
            st = ln.strip()
            if not st or st.startswith('#') or '>' not in st:
                out.append(ln); continue
            a, b = st.split('>', 1)
            a, b = a.strip(), b.strip()
            out.append('%s > %s' % (_spec_to_new(a, img, src_img or img, widx, src_lba),
                                   _spec_to_new(b, img, dst_img or img, widx, dst_lba)))
        np = path + '.compat'
        with open(np, 'w', encoding='utf-8') as f:
            f.write('\n'.join(out) + '\n')
        kw['list_file'] = np
    return 'copy', kw
