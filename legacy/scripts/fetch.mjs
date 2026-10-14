const urls = process.argv.slice(2);
for (const u of urls) {
  try {
    const r = await fetch(u, { headers: { 'User-Agent':'Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 Chrome/120 Safari/537.36' }, redirect:'follow' });
    let h = await r.text();
    const title = (h.match(/<title[^>]*>([\s\S]*?)<\/title>/i)||[])[1]||'';
    h = h.replace(/<script[\s\S]*?<\/script>/gi,' ').replace(/<style[\s\S]*?<\/style>/gi,' ').replace(/<svg[\s\S]*?<\/svg>/gi,' ').replace(/<sup[\s\S]*?<\/sup>/gi,' ');
    let t = h.replace(/<\/(p|div|tr|h[1-6]|li)>/gi,'\n').replace(/<[^>]+>/g,' ').replace(/&nbsp;/g,' ').replace(/&amp;/g,'&').replace(/&lt;/g,'<').replace(/&gt;/g,'>').replace(/&#x27;/g,"'").replace(/&quot;/g,'"');
    t = t.split('\n').map(s=>s.replace(/\s+/g,' ').trim()).filter(s=>s.length>1).join('\n');
    console.log('\n' + '='.repeat(88));
    console.log('URL: ' + u + '   HTTP ' + r.status + '  title=' + title.slice(0,80));
    console.log('-'.repeat(88));
    console.log(t.slice(0, 9000));
  } catch (e) { console.log('\n=== FAIL ' + u + ' -> ' + e.message); }
}
