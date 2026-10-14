// 把真 Windows 的 $Boot 引导代码（扇区 0~9）移植到目标卷，保留目标卷的 BPB
import { NodeBlockSource, Qcow2Reader } from '/workspace/qcow2-viewer/src/block/reader.mjs';
import fs from 'node:fs';

const [,, volPath, partLba, srcPath, srcLba] = process.argv;
const LBA=+partLba, VOLSEC=+process.argv[5];

// ① 从源盘取 $Boot 扇区 0~9（5120 字节）
const sq=await new Qcow2Reader(await NodeBlockSource.open(srcPath)).init();
const src=Buffer.alloc(512*10);
for(let s=0;s<10;s++) Buffer.from(await sq.read((+srcLba+s)*512,512)).copy(src,s*512);

// ② 读目标卷扇区 0，取其 BPB（0x0B~0x53）
const fd=fs.openSync(volPath,'r+');
const dst=Buffer.alloc(512); fs.readSync(fd,dst,0,512,0);
const bpb=dst.subarray(0x0b,0x54);      // 0x0B..0x53 共 72 字节 BPB
const hidden=dst.readUInt32LE(0x1c), total=Number(new DataView(dst.buffer,dst.byteOffset,512).getBigUint64(0x28,true));
console.log('  目标卷 BPB: HiddenSectors='+hidden+' totSec64='+total+' $MFT LCN='+Number(new DataView(dst.buffer,dst.byteOffset,512).getBigInt64(0x30,true))+' $MFTMirr LCN='+Number(new DataView(dst.buffer,dst.byteOffset,512).getBigUint64(0x38,true)));

// ③ 组装新 $Boot：源代码 + 目标 BPB + 目标签名
const out=Buffer.from(src);
bpb.copy(out,0x0b);                      // ★ 保留目标卷的 BPB
out[510]=0x55; out[511]=0xaa;
// 卷序列号保持目标卷的（0x48~0x4F 属于 BPB 范围已覆盖）

// ④ 写入物理扇区 0~9
fs.writeSync(fd,out,0,512*10,0);
// ⑤ 备份 VBR（卷末扇区）写扇区 0 的内容
const vbr=out.subarray(0,512);
fs.writeSync(fd,vbr,0,512,(VOLSEC-1)*512);
fs.closeSync(fd);
console.log('  ✓ 已写入扇区 0~9（'+out.length+' 字节）+ 备份 VBR');
console.log('  移植代码量: 扇区1~9 = '+src.subarray(512).filter(x=>x!==0).length+' 字节非零');
