// 从 qcow2 内的 NTFS 分区提取文件（只读，走我们自己的读取栈）
import { NodeBlockSource, Qcow2Reader } from '/workspace/qcow2-viewer/src/block/reader.mjs';
import { readPartitions } from '/workspace/qcow2-viewer/src/fs/partition.mjs';
import { NtfsVolume } from '/workspace/qcow2-viewer/src/fs/ntfs.mjs';
import fs from 'node:fs';

const [,, qcow, partIdx, path, out] = process.argv;
const q = await new Qcow2Reader(await NodeBlockSource.open(qcow)).init();
const total = Math.floor(q.size / 512);
const t = await readPartitions((o, l) => q.read(o, l), total);
const p = t.partitions.find(x => x.index === Number(partIdx));
if (!p) { console.error('找不到分区 ' + partIdx); process.exit(1); }
const nv = await new NtfsVolume((o, l) => q.read(o, l), p.lba * 512, p.sectors * 512).init();

// 逐段解析路径
let dirClus = 5, ent = null;
const parts = path.split('/').filter(Boolean);
for (let i = 0; i < parts.length; i++) {
  const list = await nv.listDir(dirClus);
  ent = list.find(e => (e.longName || e.name).toLowerCase() === parts[i].toLowerCase());
  if (!ent) { console.error('找不到: ' + parts[i]); process.exit(2); }
  if (i < parts.length - 1) dirClus = ent.mftIndex;
}
if (ent.isDir) { console.error('是目录'); process.exit(3); }
const data = await nv.readFileData(ent.mftIndex, 0, ent.size);
fs.writeFileSync(out, Buffer.from(data));
console.log(`${path} -> ${out} (${data.length} 字节)`);
