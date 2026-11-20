// Optional guided-tour browser check. See web/README.md.
import assert from 'node:assert/strict';
const endpoint=process.argv[2] || 'http://127.0.0.1:9226';
const base=process.argv[3] || new URL('../index.html', import.meta.url).href;
const pages=await(await fetch(endpoint+'/json')).json();
const ws=new WebSocket(pages.find(p=>p.type==='page').webSocketDebuggerUrl);await new Promise(r=>ws.onopen=r);
let id=0;const pending=new Map(),errors=[];
ws.onmessage=e=>{const m=JSON.parse(e.data);if(m.id){const p=pending.get(m.id);pending.delete(m.id);m.error?p.reject(m.error):p.resolve(m.result);}else if(m.method==='Runtime.exceptionThrown')errors.push(m.params.exceptionDetails.exception?.description);};
function send(method,params={}){return new Promise((resolve,reject)=>{const n=++id;pending.set(n,{resolve,reject});ws.send(JSON.stringify({id:n,method,params}));});}
await send('Page.enable');await send('Runtime.enable');
const run=async expression=>{const r=await send('Runtime.evaluate',{expression,awaitPromise:true,returnByValue:true});if(r.exceptionDetails)throw Error(r.exceptionDetails.exception.description);return r.result.value;};
const wait=async expr=>{for(let i=0;i<1800;i++){if(await run(expr))return;await new Promise(r=>setTimeout(r,25));}throw Error('Timed out: '+expr+' '+await run('document.getElementById("mock-tour-description")?.textContent'));};
const go=async query=>{await send('Page.navigate',{url:base+'?'+query});await wait('document.getElementById("mock-tour")?.dataset.ready==="true"');};
const step=async n=>wait(`document.getElementById("mock-tour")?.dataset.step==="${n}"&&document.getElementById("mock-tour").dataset.ready==="true"`);
const next=async n=>{await run('document.getElementById("mock-next").click()');await step(n);};
await go('mock=normal');
await next(1);await new Promise(r=>setTimeout(r,3400));assert.ok(await run('document.querySelector("#settings .pending")'));
await next(2);assert.ok(await run('document.querySelectorAll("#settings tr").length>100'));
await next(3);await next(4);assert.equal(await run('document.getElementById("save").disabled'),true);
await next(5);assert.equal(await run('document.getElementById("save").disabled'),false);
await next(6);await new Promise(r=>setTimeout(r,3400));assert.equal(await run('document.getElementById("save").textContent'),'Saving…');
await next(7);assert.ok(await run('document.getElementById("toast").textContent.includes("Saved")'));
await run('document.getElementById("mock-previous").click()');await step(6);assert.equal(await run('document.getElementById("save").textContent'),'Saving…');
await next(7);await next(8);assert.equal(await run('document.getElementById("home").hidden'),false);
await go('mock=normal&outcome=failure&step=7');await step(7);assert.ok(await run('document.getElementById("mock-notice").textContent.includes("refused")'));
await go('mock=normal&view=app&step=4');await step(4);assert.equal(await run('document.getElementById("padjob").hidden'),false);await next(5);assert.ok(await run('document.getElementById("actionresult").textContent.includes("Backed up")'));
await go('mock=manual');assert.equal(await run('document.getElementById("homelist").hidden'),false);await next(1);await next(2);assert.ok(await run('document.querySelector(".device-package-status").textContent.includes("Need to manually enter bootloader")'));
await go('mock=normal');await run('document.getElementById("mock-play").click()');await step(8);assert.equal(await run('document.getElementById("mock-play").textContent'),'Play');
assert.deepEqual(errors,[]);console.log('Tour smoke passed: held loading/saving, validation, save, refusal, rewind/replay, disconnect, app progress and manual loader.');ws.close();
