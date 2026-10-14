import fs from 'node:fs';
const t=fs.readFileSync(process.argv[2],'utf8');
const bytes=[];
for(const line of t.split('\n')){
  const m=line.match(/^([0-9a-f]{8}):\s+((?:[0-9a-f]{2}\s+){1,16})/);
  if(m){ for(const b of m[2].trim().split(/\s+/)) bytes.push(parseInt(b,16)); }
}
if(!bytes.length){ console.log('  没抓到显存数据（monitor 输出格式可能不同）'); console.log(t.slice(0,400)); process.exit(0); }
let text='';
for(let i=0;i<bytes.length;i+=2){ const c=bytes[i]; text += (c>=32&&c<127)?String.fromCharCode(c):(c===0?' ':'·'); }
console.log('══ 显存文本内容（80列×25行）══');
for(let i=0;i<text.length;i+=80){ const l=text.slice(i,i+80).replace(/\s+$/,''); console.log('  '+String(i/80).padStart(2)+'| '+l); }
