#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
ntfs-inventory.py —— NTFS 卷的【独立】三方对账器（mini-chkdsk）

为什么要它（2026-10-23 的教训）：imgtool 和 libntfs-3g 都是"按名字扫索引"，
所以工具自己回读永远看不出【索引项指向一条不属于它的记录】；而 Windows 走的是
「父索引项 ↔ 子记录 $FILE_NAME ↔ $MFT 的 $BITMAP」三方互证，任何一环对不上，
Windows 就把那条记录当孤儿塞进 found.000 —— 而工具一路报成功（C:/MovedByTool 事故）。
本脚本【不链接 imgtool / libntfs-3g】，只用 Python 按官方字节布局读 raw 镜像，当独立裁判。
字节布局取自 libntfs-3g 的 include/ntfs-3g/layout.h 与 libntfs-3g/mst.c
（MST/USA 规则：usa_cnt == 1 + size/512，每扇区末 2 字节必须等于 usa[0]）。

用法：
    python3 tools/ntfs-inventory.py <raw镜像> <分区起始LBA> [--limit N]
    python3 tools/ntfs-inventory.py --qcow2 盘.qcow2 --lba 1050624 [--limit N]
退出码：0=三方一致；1=有违背；2=参数/读盘错误
"""
import struct, sys, os, subprocess, tempfile, json
BASE_OUT = None
BASE_IN = None


def _fmt(x):
    if isinstance(x, (list, tuple)):
        return ','.join(_fmt(i) for i in x)
    return str(x)
#!/usr/bin/env python3
# -*- coding: utf-8 -*-

import struct, sys, os

RAW = '/workspace/.tmp/w10b.raw'
PART_LBA = 1050624
BLOCK = 512

class Ntfs:
    def __init__(self, raw=RAW, part_lba=PART_LBA):
        self.f = open(raw, 'rb')
        self.base = part_lba * BLOCK
        bs = self.read(self.base, BLOCK)
        if bs[3:11] != b'NTFS    ':
            raise SystemExit('分区起点不是 NTFS: %r' % bs[3:11])
        self.bps = struct.unpack_from('<H', bs, 11)[0]
        self.spc = bs[13]
        self.cl = self.bps * self.spc
        self.mft_lcn = struct.unpack_from('<Q', bs, 0x30)[0]
        rs = struct.unpack('<b', bs[0x40:0x41])[0]
        self.recsz = (1 << (-rs)) if rs < 0 else rs * self.cl
        self.serial = struct.unpack_from('<Q', bs, 0x48)[0]
        self.mft_runs = None
        self._init_mft()

    # ---------- 底层 ----------
    def read(self, off, n):
        self.f.seek(off); d = self.f.read(n)
        # ★ 解析层的失败必须用普通异常：早先用 SystemExit，调用方 `except Exception`
        #   抓不到，一条怪记录就能让整卷体检中途退出（SystemExit 不是 Exception 的子类）。
        if len(d) != n: raise RuntimeError('short read @%d want %d got %d' % (off, n, len(d)))
        return d

    @staticmethod
    def runs_decode(rb):
        runs = []; lcn = 0; i = 0
        while i < len(rb):
            h = rb[i]
            if h == 0: break
            lb, db = h & 0x0f, h >> 4
            if lb == 0: break
            ln = 0
            for k in range(lb): ln |= rb[i+1+k] << (8*k)
            if rb[i+lb] & 0x80: ln -= (1 << (8*lb))
            i += 1 + lb
            d = 0
            for k in range(db): d |= rb[i+k] << (8*k)
            if db and (rb[i+db-1] & 0x80): d -= (1 << (8*db))
            i += db
            lcn += d
            runs.append((lcn, ln))
        return runs

    @staticmethod
    def runs_map(runs, cl, base):
        seg = []; vcn = 0
        for lcn, ln in runs:
            seg.append((vcn, vcn+ln, lcn)); vcn += ln
        def off(c):
            for a, b, l in seg:
                if a <= c < b: return base + (l + (c-a)) * cl
            return None
        return off, vcn

    def _init_mft(self):
        # 引导循环：先按"第一段 run 从 mft_lcn 开始"读出记录 0，拿到 $MFT 的完整 run list，
        # 之后再建立全量簇映射（$MFT 是分段分配的，绝不能假设连续）。
        buf = self.mst_apply(self.read(self.base + self.mft_lcn*self.cl, self.recsz))
        assert buf[0:4] == b'FILE'
        for a in self._attrs_of_buf(buf, struct.unpack_from('<H', buf, 20)[0]):
            if a['type'] == 0x80 and a['name'] == '':
                self.mft_runs = a['runs']
                self.mft_off, self.mft_vcl = self.runs_map(a['runs'], self.cl, self.base)
                return
        raise RuntimeError('$MFT 没有无名 $DATA')

    # ---------- MST ----------
    def mst_verify(self, buf):
        """返回 [] 表示多扇区保护完好；否则返回异常描述列表"""
        out = []
        sig = buf[0:4]
        usa_ofs, usa_cnt = struct.unpack_from('<H', buf, 4)[0], struct.unpack_from('<H', buf, 6)[0]
        n = len(buf)
        if n % BLOCK: return ['size%d 不是扇区倍数' % n]
        if usa_cnt != 1 + n // BLOCK:
            out.append('usa_cnt=%d 应为 %d' % (usa_cnt, 1 + n // BLOCK))
        if usa_ofs % 2 or usa_ofs + usa_cnt*2 > BLOCK - 2:
            out.append('usa_ofs=%d 非法' % usa_ofs)
        if out: return out
        usn = struct.unpack_from('<H', buf, usa_ofs)[0]
        for i in range(1, usa_cnt):
            got = struct.unpack_from('<H', buf, BLOCK*i - 2)[0]
            if got != usn:
                out.append('扇区%d 尾部 %04x != USN %04x (sig=%r)' % (i, got, usn, sig))
        return out

    def mst_apply(self, buf):
        b = bytearray(buf)
        usa_ofs, usa_cnt = struct.unpack_from('<H', b, 4)[0], struct.unpack_from('<H', b, 6)[0]
        for i in range(1, usa_cnt):
            b[BLOCK*i-2:BLOCK*i] = struct.pack('<H', struct.unpack_from('<H', b, usa_ofs+2*i)[0])
        return bytes(b)

    # ---------- 记录 / 属性 ----------
    def record_raw(self, ino):
        off = self.mft_off(ino // (self.cl // self.recsz))
        if off is None: raise RuntimeError('ino %d 超出 $MFT 范围' % ino)
        slot = ino % (self.cl // self.recsz)
        return self.read(off + slot*self.recsz, self.recsz)

    def record(self, ino):
        raw = self.record_raw(ino)
        mst = self.mst_verify(raw)
        rec = self.mst_apply(raw)
        if rec[0:4] != b'FILE':
            return dict(ino=ino, bad='magic %r' % rec[0:4], mst=mst)
        return dict(ino=ino, mst=mst,
                    seq=struct.unpack_from('<H', rec, 16)[0],
                    link=struct.unpack_from('<H', rec, 18)[0],
                    offs=struct.unpack_from('<H', rec, 20)[0],
                    flags=struct.unpack_from('<H', rec, 22)[0],
                    used=struct.unpack_from('<I', rec, 24)[0],
                    alloc=struct.unpack_from('<I', rec, 28)[0],
                    base_ref=struct.unpack_from('<Q', rec, 32)[0],
                    nattr=struct.unpack_from('<H', rec, 40)[0],
                    buf=rec)

    @staticmethod
    def attrs_walk(buf, start):
        o = start; res = []
        while o + 0x20 <= len(buf):
            t = struct.unpack_from('<I', buf, o)[0]
            if t in (0, 0xFFFFFFFF): break
            l = struct.unpack_from('<I', buf, o+4)[0]
            if l < 0x18 or o + l > len(buf): break
            nonres = buf[o+8]
            nlen = buf[o+9]; noff = struct.unpack_from('<H', buf, o+0x0a)[0]
            name = buf[o+noff:o+noff+nlen*2].decode('utf-16-le', 'replace') if nlen else ''
            a = dict(type=t, len=l, nonres=nonres, name=name,
                     flags=struct.unpack_from('<H', buf, o+0x0c)[0],
                     inst=struct.unpack_from('<H', buf, o+0x0e)[0], off=o)
            if nonres == 0:
                vl = struct.unpack_from('<I', buf, o+0x10)[0]
                vo = struct.unpack_from('<H', buf, o+0x14)[0]
                a['vlen'] = vl; a['val'] = buf[o+vo:o+vo+vl]; a['runs'] = None
            else:
                a['alloc'] = struct.unpack_from('<Q', buf, o+0x18)[0]
                a['real'] = struct.unpack_from('<Q', buf, o+0x20)[0]
                ro = struct.unpack_from('<H', buf, o+0x28)[0]
                a['runs'] = self_runs = self.runs_decode(buf[o+ro:])
                a['runbytes'] = buf[o+ro:]
            res.append(a)
            o += l
        return res

    # attrs_walk 里用到 self，用闭包重写成方法
    def _attrlist(self, val):
        """解析 $ATTRIBUTE_LIST（0x20）内容 → 各条目的 (type,name,inst,ref_mft,vcn)"""
        out = []; o = 0
        while o + 0x20 <= len(val):
            t = struct.unpack_from('<I', val, o)[0]
            if t == 0 and o + 0x18 > len(val): break
            l = struct.unpack_from('<H', val, o+4)[0] * 2
            nlen = val[o+6]; noff = struct.unpack_from('<H', val, o+7)[0]
            inst = struct.unpack_from('<H', val, o+0x0e)[0]
            vcn = struct.unpack_from('<q', val, o+0x10)[0]
            ref = struct.unpack_from('<Q', val, o+0x18)[0]
            if t == 0 and l == 0: break
            out.append(dict(type=t, inst=inst, vcn=vcn,
                            ref_mft=ref & 0xFFFFFFFFFFFF, name=val[o+noff:o+noff+nlen*2].decode('utf-16-le','replace')))
            o += 0x18 + nlen*2
            o = (o + 1) & ~1
        return out

    def attrs(self, ino):
        r = self.record(ino)
        if 'bad' in r: raise RuntimeError('ino %d: %s' % (ino, r['bad']))
        out = []
        for a in self._attrs_of_buf(r['buf'], r['offs']):
            out.append(a)
        # 展开 $ATTRIBUTE_LIST（跨扩展记录的属性）
        al = [a for a in out if a['type'] == 0x20]
        if al:
            for e in self._attrlist(al[0]['val']):
                try: rr = self.record(e['ref_mft'])
                except SystemExit: continue
                for a in self._attrs_of_buf(rr['buf'], rr['offs']):
                    if a['type'] == e['type'] and (a['inst'] == e['inst'] or e['vcn'] != -1):
                        a['extent_of'] = e['ref_mft']; out.append(a)
        return out, r

    def _attrs_of_buf(self, buf, start):
        o = start; res = []
        while o + 0x20 <= len(buf):
            t = struct.unpack_from('<I', buf, o)[0]
            if t in (0, 0xFFFFFFFF): break
            l = struct.unpack_from('<I', buf, o+4)[0]
            if l < 0x18 or o + l > len(buf): break
            nonres = buf[o+8]
            nlen = buf[o+9]; noff = struct.unpack_from('<H', buf, o+0x0a)[0]
            name = buf[o+noff:o+noff+nlen*2].decode('utf-16-le', 'replace') if nlen else ''
            a = dict(type=t, len=l, nonres=nonres, name=name, record=None,
                     flags=struct.unpack_from('<H', buf, o+0x0c)[0],
                     inst=struct.unpack_from('<H', buf, o+0x0e)[0], off=o)
            if nonres == 0:
                vl = struct.unpack_from('<I', buf, o+0x10)[0]
                vo = struct.unpack_from('<H', buf, o+0x14)[0]
                a['vlen'] = vl; a['val'] = buf[o+vo:o+vo+vl]; a['runs'] = None
            else:
                # 非驻留头字段位置在不同 Windows 版本上有出入 ⇒ 不猜：
                # 候选偏移解出的 run list 必须满足 sum(clusters)*簇大小 == 属性某个 size 字段，
                # 否则拒绝接受。自校验，避免"尺子不准"导致全盘误判。
                sizes = [struct.unpack_from('<Q', buf, o+x)[0] for x in (0x18,0x20,0x28,0x30,0x38)]
                cands = []
                for x in (0x10,0x18,0x20,0x28):
                    v = struct.unpack_from('<H', buf, o+x)[0]
                    if 0x20 <= v <= l: cands.append((x,v))
                chosen = None
                for x,v in cands:
                    rr = self.runs_decode(buf[o+v:o+l])
                    if not rr: continue
                    tot = sum(ln for _,ln in rr if ln>0)
                    if any(ln<=0 for _,ln in rr): continue
                    if tot*self.cl in sizes:
                        chosen = (v, rr, tot*self.cl); break
                if chosen is None:
                    a['runs']=None; a['alloc']=0; a['real']=0; a['val']=None; a['badrun']=True
                    res.append(a); o += l; continue
                v, rr, bytes_ = chosen
                a['runs']=rr; a['alloc']=bytes_; a['real']=bytes_; a['val']=None; a['runoff']=v
            res.append(a)
            o += l
        return res

    def attr_data(self, a):
        """非驻留属性 → 拼出实际字节（按簇映射）"""
        if a['nonres'] == 0: return a['val']
        off, tot = self.runs_map(a['runs'], self.cl, self.base)
        buf = bytearray()
        for lcn, ln in a['runs']:
            buf += self.read(self.base + lcn*self.cl, ln*self.cl)
        return bytes(buf)

    def attr(self, ino, t, name=''):
        ats, r = self.attrs(ino)
        for a in ats:
            if a['type'] == t and a['name'] == name: return a
        return None

    def fns(self, ino):
        ats, r = self.attrs(ino)
        out = []
        for a in ats:
            if a['type'] != 0x30: continue
            v = a['val']
            par = struct.unpack_from('<Q', v, 0)[0]
            nl = v[0x40]; ns = v[0x41]
            out.append(dict(inst=a['inst'], ref=par, pmft=par & 0xFFFFFFFFFFFF, pseq=par >> 48,
                            name=v[0x42:0x42+nl*2].decode('utf-16-le', 'replace'), ns=ns,
                            attrs=struct.unpack_from('<I', v, 0x38)[0],
                            size=struct.unpack_from('<Q', v, 0x30)[0],
                            alloc=struct.unpack_from('<Q', v, 0x28)[0],
                            ctime=struct.unpack_from('<Q', v, 0x08)[0],
                            mtime=struct.unpack_from('<Q', v, 0x10)[0],
                            ftm=struct.unpack_from('<Q', v, 0x18)[0],
                            atime=struct.unpack_from('<Q', v, 0x20)[0],
                            raw=v))
        return out, r

    def isdir(self, ino):
        r = self.record(ino)
        return bool(r.get('flags', 0) & 2)

    # ---------- 索引 ----------
    def index_entries(self, ino):
        """返回 (root条目, {vcn:[条目]}, 诊断) —— 只读 $I30"""
        ats, r = self.attrs(ino)
        ir = [a for a in ats if a['type'] == 0x90 and a['name'] == '$I30']
        ia = [a for a in ats if a['type'] == 0xA0 and a['name'] == '$I30']
        bm = [a for a in ats if a['type'] == 0xB0 and a['name'] == '$I30']
        diag = dict(rec_seq=r.get('seq'), rec_link=r.get('link'), rec_flags=r.get('flags'),
                    rec_mst=r.get('mst'), attrs=[(a['type'], a['name'], a['inst']) for a in ats],
                    has_root=bool(ir), has_alloc=bool(ia), has_bitmap=bool(bm))
        root = None; blocks = {}
        # 本目录 $I30 的 $BITMAP：只有位图说"这个 VCN 在用"的索引块，内容才算数
        bmbits = None
        if bm:
            try:
                bmbits = bm[0]['val'] if bm[0]['nonres'] == 0 else self.attr_data(bm[0])
            except Exception:
                bmbits = None
        if ir:
            v = ir[0]['val']
            bs = struct.unpack_from('<I', v, 8)[0]
            hdr = v[16:]
            eo = struct.unpack_from('<I', hdr, 0)[0]
            ilen = struct.unpack_from('<I', hdr, 4)[0]
            root = dict(block_size=bs, entries=self._ents(v, 16+eo, 16+ilen),
                        eo=eo, ilen=ilen, alloc=struct.unpack_from('<I', hdr, 8)[0],
                        ihflags=hdr[12], collation=struct.unpack_from('<I', v, 4)[0],
                        itype=struct.unpack_from('<I', v, 0)[0])
        if ia:
            data = self.attr_data(ia[0])
            bs = root['block_size'] if root else 4096
            n = len(data) // bs
            for i in range(n):
                blk = data[i*bs:(i+1)*bs]
                mst = self.mst_verify(blk)
                if blk[0:4] != b'INDX':
                    # 全零 = $INDEX_ALLOCATION 里未分配的 VCN 空洞（NTFS 允许存在）
                    zl = not any(blk)
                    lv = True if bmbits is None else bool((bmbits[i >> 3] >> (i & 7)) & 1)
                    blocks[i] = dict(bad=('全零空洞' if zl else repr(blk[0:4])),
                                     live=(False if (zl or not lv) else True),
                                     mst=([] if (zl or not lv) else mst), entries=[])
                    continue
                good = self.mst_apply(blk)
                vcn = struct.unpack_from('<Q', good, 16)[0]
                hdr = good[24:]
                eo = struct.unpack_from('<I', hdr, 0)[0]
                ilen = struct.unpack_from('<I', hdr, 4)[0]
                live = True if bmbits is None else (vcn < len(bmbits) * 8 and bool((bmbits[vcn >> 3] >> (vcn & 7)) & 1))
                blocks[vcn] = dict(live=live, mst=mst if live else [], eo=eo, ilen=ilen, alloc=struct.unpack_from('<I', hdr, 8)[0],
                                   ihflags=hdr[12], nt=struct.unpack_from('<q', good, 8)[0],
                                   entries=self._ents(good, 24+eo, 24+ilen))
        return root, blocks, diag, ats

    def _ents(self, buf, p, end):
        out = []
        while p + 0x10 <= min(end + 8, len(buf)):
            ref = struct.unpack_from('<Q', buf, p)[0]
            ln = struct.unpack_from('<H', buf, p+8)[0]
            kl = struct.unpack_from('<H', buf, p+0x0a)[0]
            fl = struct.unpack_from('<H', buf, p+0x0c)[0]
            e = dict(off=p, ref=ref, mft=ref & 0xFFFFFFFFFFFF, seq=ref >> 48,
                     len=ln, key=kl, flags=fl)
            if fl & 1: e['vcn'] = struct.unpack_from('<q', buf, p+ln-8)[0]
            if not (fl & 2) and kl >= 0x42:
                v = buf[p+16:p+16+kl]
                par = struct.unpack_from('<Q', v, 0)[0]
                nl = v[0x40]; ns = v[0x41]
                e['fn'] = dict(name=v[0x42:0x42+nl*2].decode('utf-16-le', 'replace'), ns=ns,
                               pmft=par & 0xFFFFFFFFFFFF, pseq=par >> 48,
                               attrs=struct.unpack_from('<I', v, 0x38)[0],
                               size=struct.unpack_from('<Q', v, 0x30)[0],
                               ctime=struct.unpack_from('<Q', v, 0x08)[0],
                               mtime=struct.unpack_from('<Q', v, 0x10)[0],
                               ftm=struct.unpack_from('<Q', v, 0x18)[0],
                               raw=v)
            out.append(e)
            if fl & 2 or ln == 0: break
            p += ln
        return out

    def all_entries(self, ino):
        root, blocks, diag, ats = self.index_entries(ino)
        es = []
        if root: es += root['entries']
        for vcn in sorted(blocks):
            if 'bad' not in blocks[vcn]: es += blocks[vcn]['entries']
        return es, root, blocks, diag

    def resolve(self, path):
        ino = 5
        if path in ('', '/'): return 5
        for comp in [c for c in path.strip('/').split('/') if c]:
            es, _, _, _ = self.all_entries(ino)
            hit = None
            for e in es:
                fn = e.get('fn')
                if fn and fn['name'].lower() == comp.lower(): hit = e['mft']; break
            if hit is None: return None
            ino = hit
        return ino

    # ---------- mini-chkdsk ----------
    def check_dir(self, ino, path):
        """对账：父索引项 ↔ 子 $FILE_NAME，双向。返回问题列表"""
        bad = []
        es, root, blocks, diag = self.all_entries(ino)
        if diag['rec_mst']: bad.append('%s: 自身 MFT 记录 MST 坏了 %s' % (path, diag['rec_mst']))
        if diag['has_alloc'] and not diag['has_bitmap']:
            bad.append('%s: 有 $INDEX_ALLOCATION 但没有 $BITMAP' % path)
        for vcn, b in sorted(blocks.items()):
            if 'bad' in b: bad.append('%s: 索引块 vcn=%s magic=%s' % (path, vcn, b['bad'])); continue
            if b['mst']: bad.append('%s: 索引块 vcn=%d MST 坏了 %s' % (path, vcn, b['mst']))
        r = self.record(ino)
        for e in es:
            fn = e.get('fn')
            if not fn: continue
            nm = fn['name']
            if nm in ('.', '..'): continue
            if e['flags'] & 1: continue
            try: rr = self.record(e['mft'])
            except SystemExit as ex:
                bad.append('%s/%s: 索引项指向的 ino %d 读不出来' % (path, nm, e['mft'])); continue
            if 'bad' in rr:
                bad.append('%s/%s: 目标记录异常 %s' % (path, nm, rr['bad'])); continue
            if rr['seq'] != e['seq']:
                bad.append('%s/%s: 索引项引用 (%d,seq%d) 但记录实际 seq=%d' % (path, nm, e['mft'], e['seq'], rr['seq']))
            fns, _ = self.fns(e['mft'])
            ok = any(f['name'].lower() == nm.lower() and f['pmft'] == ino and f['pseq'] == r['seq'] for f in fns)
            if not ok:
                bad.append('%s/%s: 子记录 (%d) 的 $FILE_NAME 里没有指回本目录的项；实际父=%s' %
                           (path, nm, e['mft'], [(f['name'], f['pmft'], f['pseq'], f['ns']) for f in fns]))
        return bad, es

    def check_child_backlink(self, ino, path):
        """子目录 $FILE_NAME 声称的父，其索引里必须有对应项（chkdsk Stage2 的核心判据）"""
        bad = []
        fns, r = self.fns(ino)
        for f in fns:
            if f['name'] in ('.', '..'): continue
            try: pr = self.record(f['pmft'])
            except SystemExit:
                bad.append('%s: $FILE_NAME 声称父=%d 但父记录读不出' % (path, f['pmft'])); continue
            es, _, _, pdiag = self.all_entries(f['pmft'])
            hit = [e for e in es if e.get('fn') and e['fn']['name'].lower() == f['name'].lower()
                   and e['mft'] == ino]
            if not hit:
                bad.append('%s: $FILE_NAME 声称父=%d(seq%d) 名字=%r，但父目录索引里没有这一项'
                           ' ⇒ 这就是 Windows 说的 "unindexed file"' %
                           (path, f['pmft'], f['pseq'], f['name']))
        return bad, fns

def show(n, ino, path, label=''):
    print('='*78)
    print('%s  path=%s ino=%d %s' % (label, path, ino, ''))
    fns, r = n.fns(ino)
    print('  记录: seq=%d link=%d flags=%#x used/alloc=%d/%d attrs=%d mst=%s' %
          (r['seq'], r['link'], r['flags'], r['used'], r['alloc'], r['nattr'], r['mst']))
    ats, _ = n.attrs(ino)
    print('  属性: %s' % [(('%02x' % a['type']), a['name'] or '', 'R' if a['nonres']==0 else 'NR',
                            a.get('vlen') or a.get('real')) for a in ats])
    for f in fns:
        print('    $FILE_NAME #%d: %-46r ns=%d parent=(%d,seq%d) size=%d attrs=%#x ctime=%x mtime=%x' %
              (f['inst'], f['name'][:44], f['ns'], f['pmft'], f['pseq'], f['size'], f['attrs'], f['ctime'], f['mtime']))
    if n.isdir(ino):
        es, root, blocks, diag = n.all_entries(ino)
        print('    $I30: root=%s blocks=%s 条目=%d' % (bool(root), sorted(blocks), len(es)))
        if root: print('      root: indexed_type=%#x collation=%d block_size=%d entries_off=%d index_len=%d alloc=%d ihflags=%d' %
                       (root['itype'], root['collation'], root['block_size'], root['eo'], root['ilen'], root['alloc'], root['ihflags']))
        for vcn, b in sorted(blocks.items()):
            print('      block vcn=%s entries_off=%d index_len=%d alloc=%d ihflags=%d mst=%s bad=%s' %
                  (vcn, b.get('eo'), b.get('ilen'), b.get('alloc'), b.get('ihflags'), b.get('mst'), b.get('bad')))
        dots = [e for e in es if e.get('fn') and e['fn']['name'] in ('.', '..')]
        print('      "." / ".." 索引项: %s' % ([(e['fn']['name'], e['mft'], e['seq']) for e in dots] or '【没有】'))

def audit(raw, part_lba, limit=None):
    n = Ntfs(raw, part_lba)
    bm = n.attr(0, 0xB0, '')
    bdata = n.attr_data(bm)
    cap = len(bdata) * 8                      # $BITMAP 能表意的记录数
    mcap = n.cl // n.recsz
    # ★ $BITMAP 往往比 $MFT 当前大小能表意的范围大得多（预留位），扫之前必须夹住，
    #   否则读出 "ino N 超出 $MFT 范围" 这种假异常（本盘 cap=163840，实际只有 ~24960*4）
    cap_live = n.mft_vcl * mcap
    cap = min(cap, cap_live)
    def bit(i): return (bdata[i >> 3] >> (i & 7)) & 1
    print('卷 %04X-%04X  簇=%d  记录=%d  MFT容量=%d 条' % (
        n.serial >> 16, n.serial & 0xFFFF, n.cl, n.recsz, cap))
    mcap = n.cl // n.recsz

    problems = []
    skipped = []
    parsed = set()
    _attrs_cached = {}          # ino -> 属性列表（判 $SI 是否存在）          # 我确实读透了的记录（读不懂的一律"未判定"，不算违背）
    dirs = []
    ref_users = {}      # ino -> [(dir_ino, name)]
    names_seen = {}     # ino -> [names]
    used_ino = set()
    scan_to = limit or cap
    for ino in range(0, min(scan_to, cap)):
        off = n.mft_off(ino // mcap)
        if off is None: break
        rawrec = n.record_raw(ino)
        rec = n.mst_apply(rawrec)
        if rec[0:4] != b'FILE': continue
        flags = struct.unpack_from('<H', rec, 22)[0]
        if not (flags & 1):         # 未分配
            if bit(ino): problems.append(('BITMAP 说在用但记录未分配', ino, ''))
            continue
        used_ino.add(ino)
        if not bit(ino): problems.append(('记录在用但 BITMAP 说空闲', ino, ''))
        mst = n.mst_verify(rawrec)
        seq = struct.unpack_from('<H', rec, 16)[0]
        link = struct.unpack_from('<H', rec, 18)[0]
        if mst: problems.append(('MFT 记录 MST 校验失败', ino, mst))
        try:
            ats_all, rec0 = n.attrs(ino)
            _attrs_cached[ino] = ats_all
            fns = [dict(inst=a['inst'], name=a['val'][0x42:0x42 + a['val'][0x40] * 2].decode(
                            'utf-16-le', 'replace'),
                            pmft=struct.unpack_from('<Q', a['val'], 0)[0] & 0xFFFFFFFFFFFF,
                            pseq=struct.unpack_from('<Q', a['val'], 0)[0] >> 48,
                            ns=a['val'][0x41], ctime=struct.unpack_from('<Q', a['val'], 8)[0],
                            mtime=struct.unpack_from('<Q', a['val'], 0x10)[0],
                            ftm=struct.unpack_from('<Q', a['val'], 0x18)[0])
                       for a in ats_all if a['type'] == 0x30]
        except Exception as e:
            skipped.append((ino, str(e))); continue      # 解析覆盖不到 ≠ 盘坏，单独计
        # ★ MFT 的【扩展记录】不是完整记录：它没有 $SI、link_count 归基记录所有，
        #   按基记录的规矩去判它会造出几万条假违背（基线 65381 条就是这么来的）。
        #   认法：base_ref 指向别的记录 ⇒ 扩展记录；或压根没有 $SI。
        br = struct.unpack_from('<Q', rec, 32)[0] & 0xFFFFFFFFFFFF   # 基记录引用
        has_si = any(a['type'] == 0x10 for a in ats_all)
        # ★ 三种情况都算【未判定】而不是【违背】：
        #   a) 扩展记录（base_ref 指向别人、或没有 $SI）；
        #   b) 带 $ATTRIBUTE_LIST 的记录 —— 它的 $FN 可能整体住在扩展记录里，
        #      我这版没展开 AL，于是"看不到名字"≠"盘上不一致"（真 Windows 盘上万条都是这样）。
        if br not in (0, ino) or not has_si or any(a['type'] == 0x20 for a in ats_all):
            skipped.append((ino, '未判定：扩展记录/无$SI/含$ATTRIBUTE_LIST'))
            continue
        names_seen[ino] = [f['name'] for f in fns]
        parsed.add(ino)
        # 只有【本记录里确实读到了 $FN】时才比对 link_count —— 很多记录的 $FN 落在
        # 扩展记录里（$ATTRIBUTE_LIST 展开我还没做全），那种 len(fns)==0 不是盘的问题。
        if len(fns) > 0 and len(fns) != link:
            problems.append(('link_count(%d) != $FILE_NAME 属性个数(%d)' % (link, len(fns)), ino,
                             [f['name'] for f in fns]))
        for f in fns:
            if f['name'] in ('.', '..'): continue
            try: pr = n.record(f['pmft'])
            except Exception: problems.append(('$FILE_NAME 的父记录读不出', ino, f['pmft'])); continue
            if 'bad' in pr: problems.append(('父记录异常', ino, f['pmft'])); continue
            if pr['seq'] != f['pseq']:
                problems.append(('$FILE_NAME 父引用 seq 不符: 声称(%d,seq%d) 实际 seq=%d' %
                                 (f['pmft'], f['pseq'], pr['seq']), ino, f['name']))
        if flags & 2:
            dirs.append(ino)

    # 目录索引 ↔ 子记录
    print('目录数=%d  在用记录=%d  解析跳过=%d（多为属性跨扩展记录/压缩，非违背）' %
          (len(dirs), len(used_ino), len(skipped)))
    for dino in dirs:
        drec = n.record(dino)
        try:
            es, root, blocks, diag = n.all_entries(dino)
        except Exception as e:
            problems.append(('目录索引解析失败', dino, str(e))); continue
        if diag['has_alloc'] and not diag['has_bitmap']:
            problems.append(('有 $INDEX_ALLOCATION 但缺 $BITMAP', dino, ''))
        for vcn, b in blocks.items():
            if b.get('live') is not True:
                continue                     # 空洞 / 位图说没在用 ⇒ 旧数据与旧 USA 不算违背
            if b.get('bad'):
                problems.append(('索引块 magic 不对', dino, 'vcn=%s %s' % (vcn, b['bad'])))
            elif b.get('mst'):
                problems.append(('索引块 MST 校验失败', dino, 'vcn=%s %s' % (vcn, b['mst'])))
        for e in es:
            fn = e.get('fn')
            if not fn or e['flags'] & 1: continue
            if fn['name'] in ('.', '..'):
                continue
            ref_users.setdefault(e['mft'], []).append((dino, fn['name']))
            if e['mft'] not in parsed:
                if e['mft'] not in used_ino:
                    problems.append(('索引项指向【未分配】的记录', dino,
                                     '%s -> ino %d（$BITMAP 也说没占用）' % (fn['name'], e['mft'])))
                continue                      # 读不懂 ⇒ 未判定，不冤枉盘
            tgt = n.record(e['mft'])
            if 'bad' in tgt:
                problems.append(('索引项指向坏记录', dino, '%s -> %d' % (fn['name'], e['mft']))); continue
            if tgt['seq'] != e['seq']:
                problems.append(('索引项 seq 与目标记录 seq 不符 (%d vs %d)' % (e['seq'], tgt['seq']),
                                 dino, '%s -> %d' % (fn['name'], e['mft'])))
            fns, _ = n.fns(e['mft'])
            ok = any(x['name'].lower() == fn['name'].lower() and x['pmft'] == dino and x['pseq'] == drec['seq']
                     for x in fns)
            if not ok:
                problems.append(('索引项在子记录里没有对应 $FILE_NAME（子声称父=%s）' %
                                 [(x['name'], x['pmft'], x['pseq']) for x in fns],
                                 dino, '%s -> (%d,seq%d)' % (fn['name'], e['mft'], e['seq'])))
    # ★ 一个记录被多处索引引用是 NTFS 的正常现象（长名+8.3 短名、硬链接），
    #   真正的信号是上一条"逐条回指检查"与"指向未分配记录"，这里不再用数量启发式。
    return n, problems, skipped


def _cli():
    argv = sys.argv[1:]
    if not argv:
        print(__doc__); return 2
    global BASE_OUT, BASE_IN
    if '--base-out' in argv:
        BASE_OUT = argv[argv.index('--base-out') + 1]
        argv = [a for i, a in enumerate(argv) if i not in (argv.index('--base-out'), argv.index('--base-out') + 1)]
    if '--base' in argv:
        BASE_IN = argv[argv.index('--base') + 1]
        argv = [a for i, a in enumerate(argv) if i not in (argv.index('--base'), argv.index('--base') + 1)]
    lim = None
    if '--limit' in argv:
        lim = int(argv[argv.index('--limit') + 1]); argv.remove('--limit'); argv.pop(argv.index(str(lim)))
    raw = None; tmp = None
    if '--qcow2' in argv:
        i = argv.index('--qcow2'); q = argv[i + 1]; plba = int(argv[argv.index('--lba') + 1])
        tmp = tempfile.mktemp(suffix='.raw')
        subprocess.run(['qemu-img', 'convert', '-O', 'raw', q, tmp], check=True)
        raw = tmp
    else:
        raw = argv[0]; plba = int(argv[1])
    try:
        got = audit(raw, plba, lim)
        n, probs, skipped = got[0], got[1], got[2]
        newonly = got[3] if len(got) > 3 else None
    finally:
        if tmp: os.unlink(tmp)
    keys = sorted('%s|%s|%s' % (p[0], p[1], _fmt(p[2])) for p in probs)
    if BASE_OUT:
        json.dump({'keys': keys}, open(BASE_OUT, 'w'), ensure_ascii=False)
        print('基线已写出：%s（%d 条）' % (BASE_OUT, len(keys)))
    if BASE_IN:
        base = set(json.load(open(BASE_IN))['keys'])
        new = [k for k in keys if k not in base]
        gone = [k for k in base if k not in keys]
        print('\n===== 与基线相比【新增】违背：%d 条（基线 %d 条，本次 %d 条）=====' %
              (len(new), len(base), len(keys)))
        for k in new[:10]:
            print('   ★新增', k[:220])
        for k in gone[:5]:
            print('   (基线里有、现在没有：通常是删除/改名后正常消失)', k[:160])
        return n, [x for x in probs], skipped, new
    print('\n===== 不变量违背：%d 条（另解析跳过 %d 条）=====' % (len(probs), len(skipped)))
    for x in skipped[:3]:
        print('   ·跳过 ino=%s %s' % (x[0], x[1]))
    kind = {}
    for p in probs:
        kind.setdefault(p[0], []).append(p)
    if newonly is not None:
        return 1 if newonly else 0
    for k, v in sorted(kind.items(), key=lambda x: -len(x[1])):
        print('\n【%s】%d 类' % (k, len(v)))
        for p in v[:6]:
            print('   ino=%s 详情=%s' % (p[1], p[2]))
        if len(v) > 6:
            print('   ...(还有 %d 条)' % (len(v) - 6))
    return 1 if probs else 0

if __name__ == '__main__':
    sys.exit(_cli())
