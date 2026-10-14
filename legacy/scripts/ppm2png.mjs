import fs from 'node:fs'; import zlib from 'node:zlib';
const buf=fs.readFileSync(process.argv[2]);
// 解析 P6 PPM
let pos=0, fields=[];
while(fields.length<4){ // magic, w, h, maxval
  while(buf[pos]===0x23){ while(buf[pos]!==0x0a) pos++; pos++; }   // 注释
  if(/\s/.test(String.fromCharCode(buf[pos]))){ pos++; continue; }
  let s=''; while(pos<buf.length && !/\s/.test(String.fromCharCode(buf[pos]))) s+=String.fromCharCode(buf[pos++]);
  fields.push(s);
}
const [magic,w,h,maxv]=[fields[0],+fields[1],+fields[2],+fields[3]];
while(/\s/.test(String.fromCharCode(buf[pos]))) pos++;
const px=buf.subarray(pos);
console.log('  PPM '+magic+' '+w+'x'+h+' max='+maxv+'  像素数据 '+px.length+' 字节');
const raw=Buffer.alloc(h*(1+w*3));
for(let y=0;y<h;y++){ raw[y*(1+w*3)]=0; px.copy(raw,y*(1+w*3)+1,y*w*3,(y+1)*w*3); }
function chunk(type,data){ const len=Buffer.alloc(4); len.writeUInt32BE(data.length);
  const td=Buffer.concat([Buffer.from(type,'latin1'),data]); const crc=Buffer.alloc(4); crc.writeUInt32BE(zlib.crc32?zlib.crc32(td):crc32(td));
  return Buffer.concat([len,td,crc]); }
function crc32(b){ let c=~0; for(const x of b){ c^=x; for(let k=0;k<8;k++) c=(c>>>1)^(0xEDB88320&-(c&1)); } return (~c)>>>0; }
const ihdr=Buffer.alloc(13); ihdr.writeUInt32BE(w,0); ihdr.writeUInt32BE(h,4); ihdr[8]=8; ihdr[9]=2;
const png=Buffer.concat([Buffer.from([0x89,0x50,0x4e,0x47,0x0d,0x0a,0x1a,0x0a]), chunk('IHDR',ihdr), chunk('IDAT',zlib.deflateSync(raw)), chunk('IEND',Buffer.alloc(0))]);
fs.writeFileSync(process.argv[3], png);
console.log('  → '+process.argv[3]+'  '+png.length+' 字节');
