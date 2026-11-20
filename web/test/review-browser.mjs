// Optional full review check: a running Chromium CDP endpoint and a page URL.
// Local files require --allow-file-access-from-files; this starts no server.
import assert from 'node:assert/strict';
const endpoint=process.argv[2] || 'http://127.0.0.1:9226';
const base=process.argv[3] || new URL('../review.html',import.meta.url).href;
const pages=await(await fetch(endpoint+'/json')).json();
const ws=new WebSocket(pages.find(p=>p.type==='page').webSocketDebuggerUrl);await new Promise(r=>ws.onopen=r);
let id=0;const pending=new Map(),errors=[];
ws.onmessage=e=>{const m=JSON.parse(e.data);if(m.id){const p=pending.get(m.id);pending.delete(m.id);m.error?p.reject(m.error):p.resolve(m.result);}else if(m.method==='Runtime.exceptionThrown')errors.push(m.params.exceptionDetails.exception?.description);};
function send(method,params={}){return new Promise((resolve,reject)=>{const n=++id;pending.set(n,{resolve,reject});ws.send(JSON.stringify({id:n,method,params}));});}
await send('Page.enable');await send('Runtime.enable');
const run=async expression=>{const r=await send('Runtime.evaluate',{expression,awaitPromise:true,returnByValue:true});if(r.exceptionDetails)throw Error(r.exceptionDetails.exception.description);return r.result.value;};
await send('Page.navigate',{url:base});
await new Promise(r=>setTimeout(r,1500));
console.log('Initial',await run('document.getElementById("status")?.textContent'));
const cases=await run(`(async()=>{
 const {WORKFLOWS,applicable}=await import('./review-flows.js');
 globalThis.entries=WORKFLOWS.flatMap(flow=>flow.nodes.map(node=>({flow,node,id:flow.id+'.'+node.id})));
 const cases=[];
 entries.forEach((entry,i)=>{for(const platform of ['browser','android'])for(const operation of entry.node.sharedOperation?['install','backup','upgrade','restore']:['install']){
  if(!applicable(entry.node,{platform,operation}))continue;
  for(const variant of entry.node.variants||[{id:''}])cases.push({i,platform,operation,variant:variant.id});
 }});return cases;
})()`);
const failed=[];
for(let i=0;i<cases.length;i++){
 const c=cases[i];
 const result=await run(`(async()=>{
  document.getElementById('platform').value=${JSON.stringify(c.platform)};
  document.getElementById('operation').value=${JSON.stringify(c.operation)};
  const {select}=await import('./review.js');await select(entries[${c.i}]);
  if(${JSON.stringify(c.variant)}){document.getElementById('variant').value=${JSON.stringify(c.variant)};document.getElementById('variant').dispatchEvent(new Event('change'));}
  return {id:entries[${c.i}].id};
 })()`);
 // A detail change starts another replay; wait for that replay as well.
 let status='';for(let n=0;n<500;n++){status=await run('document.getElementById("status").textContent');if(!status.startsWith('Replaying'))break;await new Promise(r=>setTimeout(r,25));}
 if(!status.startsWith('Checkpoint ready')){failed.push({...c,...result,status});console.log('FAIL',failed.at(-1));}
 if(i%20===0)console.log('Reached',i+1,'/',cases.length,result.id,c.platform,c.operation);
 if(result.id==='settings.ready')assert.equal(await run('document.getElementById("preview").contentDocument.getElementById("config").hidden'),false);
 if(result.id.startsWith('firmware.')){
  assert.equal(await run('!!document.querySelector(`#workflows [data-flow="update"]`)'),false);
  if(c.operation==='upgrade')assert.equal(await run('document.getElementById("map").textContent.includes("Manual boot loader")'),false);
 }
}
assert.deepEqual(failed,[]);assert.deepEqual(errors,[]);
console.log('All',cases.length,'platform/action/error-detail previews passed.');
await send('Page.navigate',{url:base+'?state=upgrade.success'});await new Promise(r=>setTimeout(r,2000));
assert.equal(await run('document.getElementById("identity").textContent'),'firmware.success');
assert.equal(await run('document.getElementById("operation").value'),'upgrade');
assert.equal(await run('document.getElementById("platform").value'),'android');
assert.equal(await run('document.getElementById("map").textContent.includes("Manual boot loader")'),false);
await send('Page.navigate',{url:base+'?state=settings.refused&platform=browser'});await new Promise(r=>setTimeout(r,1500));
assert.equal(await run('!!document.querySelector(`#workflows [data-flow="firmware"]`)'),false);
const count=await run(`(async()=>{const {WORKFLOWS,applicable}=await import('./review-flows.js');return WORKFLOWS.flatMap(f=>f.nodes).filter(n=>applicable(n,{platform:'browser',operation:'install'})).length})()`);
await run('document.getElementById("scan").click()');let complete=false;
for(let i=0;i<6000;i++){
 if(await run(`document.getElementById("coverage").textContent.startsWith("${count} / ${count}") && document.getElementById("scan").textContent!=="Stop collection"`)){complete=true;break;}
 await new Promise(r=>setTimeout(r,25));
}
assert.ok(complete);assert.ok(!(await run('document.getElementById("coverage").textContent')).includes('preview failures'));
await run('document.getElementById("filter").value="error";document.getElementById("filter").dispatchEvent(new Event("change"));document.getElementById("search").value="Nothing was saved";document.getElementById("search").dispatchEvent(new Event("input"));');
assert.ok((await run('document.getElementById("index").textContent')).includes('settings.refused'));
assert.deepEqual(errors,[]);console.log('Platform-scoped collection, legacy links, and actual-message search passed.');ws.close();
