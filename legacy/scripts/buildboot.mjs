// 移植真 Windows 的 $Boot 引导代码（扇区0~9）到目标卷，保留目标卷 BPB
import fs from 'node:fs';
const [,, vol, hiddenStr, volsecStr] = process.argv;
const hidden=+hiddenStr, volsec=+volsecStr;
const ref=fs.readFileSync('bootcode-5120.bin');
const fd=fs.openSync(vol,'r+');
const dst=Buffer.alloc(512); fs.readSync(fd,dst,0,512,0);
const bpb=Buffer.from(dst.subarray(0x0b,0x54));
const out=Buffer.from(ref);
bpb.copy(out,0x0b);
out.writeUInt32LE(hidden,0x1c);
out.writeUInt16LE(63,0x18);
out.writeUInt16LE(255,0x1a);
out[510]=0x55; out[511]=0xaa;
fs.writeSync(fd,out,0,5120,0);
fs.writeSync(fd,out.subarray(0,512),0,512,(volsec-1)*512);
fs.closeSync(fd);
let nz=0; for(const x of out.subarray(512)) if(x!==0) nz++;
console.log('   ✓ 移植 '+nz+' 字节引导代码 | HiddenSectors='+out.readUInt32LE(0x1c)+' spt='+out.readUInt16LE(0x18)+' heads='+out.readUInt16LE(0x1a)+' totSec='+out.readBigUInt64LE(0x28));
