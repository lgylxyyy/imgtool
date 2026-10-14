/* 用真实 Windows NTFS 引导码替换 mkntfs 的错误桩
 * mkntfs 的 VBR 入口只打印 "This is not a bootable disk"，不加载 bootmgr。
 * 从已知能启动的参考盘拷贝 0x4A..0x1FD 引导码，并修正 HiddenSectors。
 * 用法: patch-vbr.mjs <image> <partLba> <volSectors> <refImage> <refLba>
 */
import fs from 'node:fs';

const [, , img, partLbaS, volSectorsS, refVbrFile] = process.argv;
const partLba = Number(partLbaS), volSectors = Number(volSectorsS);
const fd = fs.openSync(img, 'r+');
const size = fs.fstatSync(fd).size;

const rd = (off, n) => { const b = Buffer.alloc(n); let d = 0; while (d < n) { const k = fs.readSync(fd, b, d, n - d, off + d); if (k <= 0) break; d += k; } return b.subarray(0, d); };
const wr = (off, b) => fs.writeSync(fd, b, 0, b.length, off);

// 参考盘 VBR（真实 Windows 引导码）——必须是原始 512 字节扇区文件
const refVbr = fs.readFileSync(refVbrFile);
if (refVbr.length < 512) { console.error('参考 VBR 文件长度不足: ' + refVbr.length); process.exit(1); }
if (refVbr[510] !== 0x55 || refVbr[511] !== 0xaa) { console.error('参考 VBR 缺 55AA'); process.exit(1); }
if (refVbr.indexOf('This is not a bootable') >= 0) { console.error('参考 VBR 本身也是错误桩！'); process.exit(1); }

const volSec = volSectors;
const offsets = [partLba * 512, partLba * 512 + (volSec - 1) * 512];  // 主 + 备份(卷末扇区)
let patched = 0;
for (const off of offsets) {
  if (off + 512 > size) { console.log(`  跳过越界偏移 0x${off.toString(16)}`); continue; }
  const vbr = Buffer.from(rd(off, 512));
  if (vbr[510] !== 0x55 || vbr[511] !== 0xaa) { console.log(`  @LBA${off / 512} 无 55AA，跳过`); continue; }
  const before = vbr.indexOf('This is not a bootable');
  // 1) 拷贝真实引导码 0x4A..0x1FD（保留 0x00..0x49 的 BPB/卷参数/序列号）
  refVbr.copy(vbr, 0x4a, 0x4a, 0x1fe);
  // 2) HiddenSectors = 本分区起始 LBA
  vbr.writeUInt32LE(partLba, 0x1c);
  // 3) 校验和字段（0x46，若参考盘有值也一并对齐）—— 保留自己的序列号 0x42
  const after = vbr.indexOf('This is not a bootable');
  wr(off, vbr);
  patched++;
  console.log(`  @LBA${off / 512} 已打补丁  引导码源=参考盘  HiddenSectors=${partLba}  错误串: ${before >= 0 ? '存在→' : '本无'}${after >= 0 ? '仍存在❌' : '已移除✅'}`);
}
fs.closeSync(fd);
console.log(`共修补 ${patched} 个引导扇区（主 + 备份）`);
