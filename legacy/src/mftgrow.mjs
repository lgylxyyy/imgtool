/* 预扩展 $MFT 到指定记录数（用已验证的 extendMft），避免灌入中途记录耗尽 */
import { NtfsVolume } from '/workspace/qcow2-viewer/src/fs/ntfs.mjs';
import { NtfsWriter } from '/workspace/qcow2-viewer/src/fs/ntfs-write.mjs';
import { parseAttrs } from '/workspace/qcow2-viewer/src/fs/ntfs-index.mjs';
import fs from 'node:fs';

const volPath = process.argv[2];
const want = Number(process.argv[3] || 500000);
const fd = fs.openSync(volPath, 'r+');
const rd = async (o, l) => { const b = Buffer.alloc(l); let d = 0; while (d < l) { const n = fs.readSync(fd, b, d, l - d, o + d); if (n <= 0) break; d += n; } return new Uint8Array(b.buffer, b.byteOffset, d); };
const wr = async (o, u8) => { fs.writeSync(fd, Buffer.from(u8.buffer, u8.byteOffset, u8.length), 0, u8.length, o); };
const size = fs.fstatSync(fd).size;
const vol = await new NtfsVolume(rd, 0, size, wr).init();
const w = new NtfsWriter(vol, wr);

const before = await w.mftCapacity();
console.log('扩展前 $MFT 容量 = ' + before + ' 条');
if (want > before) {
  const got = await w.extendMft(want);
  console.log('extendMft -> ' + got + ' 条');
}
await vol.refreshMftRuns();
const after = await w.mftCapacity();
console.log('扩展后 $MFT 容量 = ' + after + ' 条');
// 核对
const r0 = await vol.readMftRecord(0);
const a = parseAttrs(r0);
const d = a.find(x => x.type === 0x80);
console.log('$DATA real=' + d.real + ' alloc=' + d.alloc + '  -> ' + Math.floor(d.real / 1024) + ' 条');
const bm = a.find(x => x.type === 0xb0);
console.log('$BITMAP real=' + bm.real + ' (' + bm.real * 8 + ' 位)');
fs.closeSync(fd);
