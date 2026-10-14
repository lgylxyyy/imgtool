import fs from 'node:fs';
const [,, disk, sizeSectorsStr, winLbaStr, winSizeStr] = process.argv;
const disksize=+sizeSectorsStr, winLba=+winLbaStr, winSize=+winSizeStr;
const m=Buffer.alloc(512,0);
fs.readFileSync('mbr-bootcode.bin').copy(m,0);
const SPT=63,H=255;
const chs=l=>{const C=Math.min(Math.floor(l/(SPT*H)),1023),r=l%(SPT*H);return [Math.floor(r/SPT),((r%SPT)+1)|((C>>8)<<6),C&0xff];};
function ent(st,lba,sz,ty){const e=Buffer.alloc(16,0);e[0]=st;
  const a=chs(lba);e[1]=a[0];e[2]=a[1];e[3]=a[2];e[4]=ty;
  const b=chs(lba+sz-1);e[5]=b[0];e[6]=b[1];e[7]=b[2];
  e.writeUInt32LE(lba,8);e.writeUInt32LE(sz,12);return e;}
ent(0x80,2048,102400,0x07).copy(m,446);
ent(0x00,winLba,winSize,0x07).copy(m,462);
Buffer.from([0x2b,0x93,0xcd,0x4c]).copy(m,440);
m[510]=0x55;m[511]=0xaa;
const fd=fs.openSync(disk,'r+'); fs.writeSync(fd,m,0,512,0); fs.closeSync(fd);
const end=winLba+winSize;
console.log('   ✓ MBR: #1 LBA2048 size102400 | #2 LBA'+winLba+' size'+winSize+' 结束'+end+' 盘'+disksize+' '+(end<=disksize?'✅盘内':'❌超盘'));
