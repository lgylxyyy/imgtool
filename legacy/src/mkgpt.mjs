/* 从零创建 GPT 分区表（含保护 MBR、备份表、CRC32）
 * 用法: mkgpt.mjs <diskImg> <totalSectors> '<partitions.json>'
 * partition: {start, sectors, typeGuid, name, attrs?}  (uniqueGuid 可选，缺省全0)
 */
import fs from 'node:fs';
import { crc32, guidToBytes, guidToString } from '/workspace/qcow2-viewer/src/fs/partition.mjs';

const [, , img, totalStr, json] = process.argv;
const total = Number(totalStr);
const parts = JSON.parse(json);
const SECT = 512;

const diskGuid = process.argv[5];   // 第5参数: 磁盘 GUID

const fd = fs.openSync(img, 'r+');

// ---- 保护 MBR ----
const mbr = Buffer.alloc(SECT, 0);
// 保护性 MBR：分区项必须写在 446 偏移（不是 0）
const o = 446;
mbr[o] = 0x00;            // 状态（保护分区通常不激活）
mbr[o + 1] = 0x00; mbr[o + 2] = 0x02; mbr[o + 3] = 0x00;   // CHS: head=0, sector=2, cyl=0 —— 与能启动的原盘一致（旧值 sector=0 违规）
mbr[o + 4] = 0xee;        // GPT 保护类型
mbr[o + 5] = 0xff; mbr[o + 6] = 0xff; mbr[o + 7] = 0xff;   // CHS 结束
mbr.writeUInt32LE(1, o + 8);                               // 起始 LBA
mbr.writeUInt32LE(Math.min(total - 1, 0xffffffff), o + 12); // 扇区数
mbr[510] = 0x55; mbr[511] = 0xaa;
fs.writeSync(fd, mbr, 0, SECT, 0);

// ---- 分区表 ----
const NUM = 128, ESZ = 128;
const entryLBA = 2;
const backupLBA = total - 1;
const backupEntryLBA = backupLBA - Math.ceil((NUM * ESZ) / SECT); // 32 扇区
const firstUsable = entryLBA + Math.ceil((NUM * ESZ) / SECT);     // 34
const lastUsable = backupEntryLBA - 1;

const entries = Buffer.alloc(NUM * ESZ, 0);
parts.forEach((p, i) => {
  const o = i * ESZ;
  entries.set(guidToBytes(p.typeGuid), o);
  entries.set(guidToBytes(p.uniqueGuid || "00000000-0000-0000-0000-000000000000"), o + 16);
  entries.writeBigUInt64LE(BigInt(p.start), o + 32);
  entries.writeBigUInt64LE(BigInt(p.start + p.sectors - 1), o + 40);
  entries.writeBigUInt64LE(BigInt(p.attrs || 0), o + 48);
  const nm = p.name || '';
  for (let k = 0; k < Math.min(nm.length, 36); k++) entries.writeUInt16LE(nm.charCodeAt(k), o + 56 + k * 2);
  console.log(`  分区#${i + 1} ${nm} LBA ${p.start}..${p.start + p.sectors - 1} (${p.sectors} 扇区) GUID=${p.uniqueGuid}`);
});
const partsCrc = crc32(entries);
fs.writeSync(fd, entries, 0, entries.length, entryLBA * SECT);
fs.writeSync(fd, entries, 0, entries.length, backupEntryLBA * SECT);

// ---- 头 ----
function mkHeader(current, backup, partEntryLBA) {
  const h = Buffer.alloc(SECT, 0);
  h.write('EFI PART', 0, 'latin1');   // 必须 8 字节 "EFI PART"，只写 4 字节会让 OVMF 拒绝整个 GPT
  h.writeUInt32LE(0x00010000, 8);   // revision
  h.writeUInt32LE(92, 12);          // headerSize
  h.writeUInt32LE(0, 16);           // crc 占位
  h.writeUInt32LE(0, 20);
  h.writeBigUInt64LE(BigInt(current), 24);
  h.writeBigUInt64LE(BigInt(backup), 32);
  h.writeBigUInt64LE(BigInt(firstUsable), 40);
  h.writeBigUInt64LE(BigInt(lastUsable), 48);
  h.set(guidToBytes(diskGuid), 56);
  h.writeBigUInt64LE(BigInt(partEntryLBA), 72);
  h.writeUInt32LE(NUM, 80);
  h.writeUInt32LE(ESZ, 84);
  h.writeUInt32LE(partsCrc, 88);
  const crc = crc32(h.subarray(0, 92));
  h.writeUInt32LE(crc, 16);
  return h;
}
fs.writeSync(fd, mkHeader(1, backupLBA, entryLBA), 0, SECT, 1 * SECT);
fs.writeSync(fd, mkHeader(backupLBA, 1, backupEntryLBA), 0, SECT, backupLBA * SECT);
fs.closeSync(fd);
console.log(`GPT 写入完成: 主头@LBA1 备份头@LBA${backupLBA} 备份表@LBA${backupEntryLBA} usable=${firstUsable}..${lastUsable}`);
