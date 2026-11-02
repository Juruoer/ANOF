import { test, expect, login } from './fixture.mjs';
import { createHash } from 'node:crypto';
import { readFile } from 'node:fs/promises';
test.use({ transferDownloads: true });
test.beforeEach(async () => { test.setTimeout(120_000); });
const digest=b=>createHash('sha256').update(b).digest('hex');
const panel=page=>page.locator('.transfers');
const record=(page,name)=>panel(page).locator('.transfer-record').filter({has:page.getByRole('heading',{name,exact:true})});
async function api(page,path,method='GET',body,etag){
 return page.evaluate(async({path,method,body,etag})=>{
  const r=await fetch(path,{method,credentials:'same-origin',mode:'same-origin',referrerPolicy:'same-origin',headers:{...(method==='GET'?{}:{'X-ANOF-Request':'1'}),...(body===undefined?{}:{'Content-Type':'application/json'}),...(etag?{'If-Match':etag}:{})},...(body===undefined?{}:{body:JSON.stringify(body)})});
  return {status:r.status,etag:r.headers.get('etag'),body:r.status===204?null:await r.json()};
 },{path,method,body,etag});
}
async function open(page){await page.getByRole('link',{name:'传输管理',exact:true}).click();await expect(panel(page).getByRole('heading',{name:'传输管理',exact:true})).toBeVisible();}
async function target(page){await panel(page).getByRole('button',{name:'目标：我的空间',exact:true}).click();await panel(page).getByRole('button',{name:'选用此目录',exact:true}).click();}
async function enqueue(page,name,bytes){await panel(page).getByLabel('选择上传文件（最多 16 个）').setInputFiles({name,mimeType:'application/octet-stream',buffer:bytes});await panel(page).getByRole('button',{name:'加入上传队列',exact:true}).click();}
async function publish(page,name,bytes){await enqueue(page,name,bytes);await panel(page).getByRole('button',{name:'开始上传队列',exact:true}).click();await expect(panel(page).getByText(/^发布已确认；/)).toBeVisible();await expect(record(page,name)).toBeVisible();}
async function allowance(page){
 const spaces=(await api(page,'/api/v1/spaces')).body.items;const space=spaces.find(s=>s.kind==='private');
 const q=await api(page,`/api/v1/admin/spaces/${space.id}/quota`);expect(q.status).toBe(200);
 expect((await api(page,`/api/v1/admin/spaces/${space.id}/quota`,'PUT',{limit:{bytes:'8388608',nodes:'100'}},q.etag)).status).toBe(200);return space;
}
async function downloaded(page,bytes,name){
 const next=page.waitForEvent('download');await panel(page).getByRole('link',{name:'下载已发布文件（浏览器处理）'}).click();const download=await next;
 expect(await download.failure()).toBeNull();expect([name,name.replace(/[<>:"/\\|?*]/gu,'_')].includes(download.suggestedFilename())).toBe(true);const path=await download.path();const data=await readFile(path);expect(data.length).toBe(bytes.length);expect(digest(data)).toBe(digest(bytes));await download.delete();
}

test('real Worker zero and multi-chunk uploads, native Unicode HTML downloads and content protocol probes',async({page,site})=>{
 await login(page,site.origin);await allowance(page);await open(page);await target(page);
 const observed=[];page.on('request',r=>{if(r.method()==='PATCH')observed.push(r);});
 await publish(page,'空文件.txt',Buffer.alloc(0));expect(observed.length).toBe(0);await downloaded(page,Buffer.alloc(0),'空文件.txt');
 const emptyHref=await panel(page).getByRole('link',{name:'下载已发布文件（浏览器处理）'}).getAttribute('href');
 const emptyProbe=await page.evaluate(async href=>{const h=await fetch(href,{method:'HEAD'});const r=await fetch(href,{headers:{Range:'bytes=1-2'}});return [h.status,h.headers.get('content-length'),r.status,(await r.arrayBuffer()).byteLength];},emptyHref);expect(emptyProbe).toEqual([200,'0',200,0]);
 const bytes=Buffer.alloc(2*1024*1024+17,65);Buffer.from('<script>window.downloadExecuted=true</script>').copy(bytes);
 const name='资料 <img onerror=alert(1)>.html';await publish(page,name,bytes);expect(observed.length).toBe(3);
 for(const [i,r] of observed.entries()){const h=await r.allHeaders();expect(h['content-type']).toBe('application/offset+octet-stream');expect(h['content-length']).toBe(String(i===2?17:1048576));expect(h['transfer-encoding']).toBeUndefined();expect(h['upload-offset']).toBe(String(i*1048576));expect(h['x-anof-request']).toBe('1');}
 await downloaded(page,bytes,name);expect(await page.evaluate(()=>window.downloadExecuted===undefined)).toBe(true);expect(await panel(page).locator('img').count()).toBe(0);
 const href=await panel(page).getByRole('link',{name:'下载已发布文件（浏览器处理）'}).getAttribute('href');
 // 以下为通过真实 HTTPS 的内容协议探针，独立于上面的实际浏览器下载。
 const result=await page.evaluate(async href=>{
  const h=await fetch(href,{method:'HEAD'});const etag=h.headers.get('etag');
  const range=await fetch(href,{headers:{Range:'bytes=1-3','If-Range':etag}});
  const conditional=await fetch(href,{headers:{'If-None-Match':etag}});
  const mismatch=await fetch(href,{method:'HEAD',headers:{'If-Match':'"wrong"'}});
  const unsat=await fetch(href,{headers:{Range:'bytes=9999999-'}});
  const fallback=await fetch(href,{headers:{Range:'bytes=1-3','If-Range':'"wrong"'}});await fallback.body.cancel();
  return {head:h.status,length:h.headers.get('content-length'),headSize:(await h.arrayBuffer()).byteLength,etag,range:range.status,rangeLength:(await range.arrayBuffer()).byteLength,contentRange:range.headers.get('content-range'),conditional:conditional.status,mismatch:mismatch.status,unsat:unsat.status,fallback:fallback.status};
 },href);
 expect(result).toEqual({head:200,length:String(bytes.length),headSize:0,etag:`"sha256-${digest(bytes)}"`,range:206,rangeLength:3,contentRange:`bytes 1-3/${bytes.length}`,conditional:304,mismatch:412,unsat:416,fallback:200});
});

test('unknown PATCH persists responsibility; reload wrong-file rejection and explicit server-offset recovery publish once',async({page,site})=>{
 await page.addInitScript(()=>{const original=window.fetch;let lost=false;window.fetch=async(...args)=>{const response=await original(...args);if(!window.transferFaultOff&&!lost&&args[1]?.method==='PATCH'&&String(args[0]).includes('/uploads/')){lost=true;await response.arrayBuffer();throw new TypeError('Controlled PATCH response loss');}return response;};});
 await login(page,site.origin);await allowance(page);await open(page);await target(page);const bytes=Buffer.alloc(1048576+31,90);const name='recover.bin';
 await enqueue(page,name,bytes);await panel(page).getByRole('button',{name:'开始上传队列',exact:true}).click();
 await expect(panel(page).getByRole('status')).toContainText('操作结果未知');await expect(record(page,name)).toContainText('1048576');
 let writes=0;page.on('request',r=>{if(['PATCH','POST','DELETE'].includes(r.method()))writes++;});
 await page.reload();await page.evaluate(()=>{window.transferFaultOff=true;});await expect(record(page,name)).toBeVisible();expect(writes).toBe(0);
 await record(page,name).getByLabel('重新选择原文件').setInputFiles({name,mimeType:'application/octet-stream',buffer:Buffer.alloc(bytes.length,89)});
 await record(page,name).getByRole('button',{name:'验证文件并恢复'}).click();await expect(panel(page).getByRole('status')).toContainText('摘要不同');expect(writes).toBe(0);
 await record(page,name).getByLabel('重新选择原文件').setInputFiles({name,mimeType:'application/octet-stream',buffer:bytes});
 await record(page,name).getByRole('button',{name:'验证文件并恢复'}).click();try { await expect(panel(page).getByText(/^发布已确认；/)).toBeVisible(); }
 catch { const status=await panel(page).getByRole('status').innerText();const known=['请重新选择原文件。','重新选择的文件摘要不同，拒绝恢复。','HEAD 与 GET','操作结果未知'];test.info().annotations.push({type:'safe-failure',description:'Recovery diagnostic: '+(known.find(value=>status.includes(value))??'unclassified')+'; writes='+writes});throw Error('Recovery publication not observed'); }expect(writes).toBe(2);
 await downloaded(page,bytes,name);const uploads=await api(page,'/api/v1/uploads?limit=100');expect(uploads.body.items.length).toBe(1);expect(uploads.body.items[0].state).toBe('COMPLETED');
});

test('controlled lost create and complete responses reconcile without replay or duplicate publication',async({page,site})=>{
 await page.addInitScript(()=>{const original=window.fetch;const lost=new Set();window.fetch=async(...args)=>{const response=await original(...args);const path=String(args[0]),operation=window.lossOperation;if(operation&&!lost.has(operation)&&args[1]?.method==='POST'&&(operation==='create'?path==='/api/v1/uploads':path.endsWith('/complete'))){lost.add(operation);await response.arrayBuffer();throw new TypeError('Controlled transfer response loss');}return response;};});
 await login(page,site.origin);await open(page);await target(page);
 for(const operation of ['create','complete']){
  await page.evaluate(operation=>{window.lossOperation=operation;},operation);
  let count=0;const listener=r=>{if(r.method()==='POST'&&(operation==='create'?r.url().endsWith('/uploads'):r.url().endsWith('/complete')))count++;};page.on('request',listener);
  const name=`unknown-${operation}.txt`,bytes=Buffer.from('bounded-content');await enqueue(page,name,bytes);await panel(page).getByRole('button',{name:'开始上传队列',exact:true}).click();
  await expect(panel(page).getByRole('status')).toContainText('操作结果未知');await expect(record(page,name)).toBeVisible();expect(count).toBe(1);
  await record(page,name).getByLabel('重新选择原文件').setInputFiles({name,mimeType:'text/plain',buffer:bytes});await record(page,name).getByRole('button',{name:'验证文件并恢复'}).click();
  await expect(panel(page).getByText(/^发布已确认；/)).toBeVisible();expect(count).toBe(1);page.off('request',listener);
 }
 const uploads=(await api(page,'/api/v1/uploads?limit=100')).body.items;expect(uploads.length).toBe(2);expect(uploads.every(u=>u.state==='COMPLETED')).toBe(true);
});

test('READY conflict needs explicit target decision; deleted result stays historical; server rejects wrong digest',async({page,site})=>{
 await login(page,site.origin);const space=await allowance(page);await open(page);await target(page);
 await publish(page,'conflict.txt',Buffer.from('first'));
 await enqueue(page,'conflict.txt',Buffer.from('second'));await panel(page).getByRole('button',{name:'开始上传队列',exact:true}).click();
 await expect(panel(page).getByText(/^等待用户选择目标/)).toBeVisible();
 const ready=record(page,'conflict.txt').filter({hasText:'READY'});
 await ready.getByLabel('完成目标名称').fill('resolved.txt');await ready.getByRole('button',{name:'使用所选目录再次完成'}).click();
 await expect(panel(page).getByText(/^发布已确认；/)).toBeVisible();
 const href=await panel(page).getByRole('link',{name:'下载已发布文件（浏览器处理）'}).getAttribute('href');const node=href.slice(0,-8);
 const n=await api(page,node);const trash=await api(page,node,'DELETE',undefined,n.etag);expect(trash.status).toBe(200);
 const detail=await api(page,`/api/v1/trash/${trash.body.id}`);expect((await api(page,`/api/v1/trash/${trash.body.id}`,'DELETE',undefined,detail.etag)).status).toBe(204);
 await record(page,'resolved.txt').getByRole('button',{name:'核对服务器状态'}).click();await expect(panel(page).getByText('历史结果已删除，不会重新发布。',{exact:true})).toBeVisible();
 const upload=await api(page,'/api/v1/uploads','POST',{client_upload_id:crypto.randomUUID(),space_id:space.id,parent_id:space.root_node_id,name:'bad-checksum.txt',length:'3',sha256:'0'.repeat(64)});expect(upload.status).toBe(201);
 expect(await page.evaluate(async id=>(await fetch(`/api/v1/uploads/${id}`,{method:'PATCH',headers:{'Content-Type':'application/offset+octet-stream','Upload-Offset':'0','X-ANOF-Request':'1'},body:new Blob(['abc'])})).status,upload.body.id)).toBe(204);
 expect((await api(page,`/api/v1/uploads/${upload.body.id}/complete`,'POST',{})).status).toBe(202);
 await expect.poll(async()=>(await api(page,`/api/v1/uploads/${upload.body.id}`)).body.state,{timeout:8000}).toBe('FAILED');
 expect((await api(page,`/api/v1/uploads/${upload.body.id}`)).body.last_error).toBe('CHECKSUM_MISMATCH');
});

async function localRows(page){return page.evaluate(()=>new Promise((resolve,reject)=>{const open=indexedDB.open('anof-transfers',1);open.onerror=()=>reject(Error('fixture database'));open.onsuccess=()=>{const db=open.result;const tx=db.transaction('records');const get=tx.objectStore('records').getAll();get.onsuccess=()=>resolve(get.result);tx.oncomplete=()=>db.close();};}));}

test('real cross-tab exclusion and unavailable Web Locks prohibit recovery writes',async({page,context,site})=>{
 await login(page,site.origin);await open(page);await target(page);await publish(page,'lock.txt',Buffer.from('lock'));
 const [saved]=await localRows(page);const key=`anof-upload|${saved.origin}|${saved.instance_id}|${saved.user_id}|${saved.client_upload_id}`;
 await page.evaluate(key=>new Promise(resolve=>{void navigator.locks.request(key,async()=>{resolve();await new Promise(done=>window.releaseTransferLock=done);});}),key);
 const other=await context.newPage();await other.goto(site.origin+'/#/transfers');await expect(record(other,'lock.txt')).toBeVisible();
 let writes=0;other.on('request',r=>{if(['POST','PATCH','DELETE'].includes(r.method()))writes++;});
 await record(other,'lock.txt').getByLabel('重新选择原文件').setInputFiles({name:'lock.txt',mimeType:'text/plain',buffer:Buffer.from('lock')});await record(other,'lock.txt').getByRole('button',{name:'验证文件并恢复'}).click();
 await expect(panel(other).getByRole('status')).toContainText('另一标签页');expect(writes).toBe(0);await page.evaluate(()=>window.releaseTransferLock());
 const unsupported=await context.newPage();await unsupported.addInitScript(()=>Object.defineProperty(navigator,'locks',{configurable:true,value:undefined}));await unsupported.goto(site.origin+'/#/transfers');
 await expect(panel(unsupported).getByText('浏览器不支持 Web Locks，已禁用上传写入和恢复。',{exact:true})).toBeVisible();await expect(panel(unsupported).getByRole('button',{name:'开始上传队列'})).toBeDisabled();
});

test('IndexedDB failed insert and capacity block create; corruption is rejected without silent reset',async({page,site})=>{
 await login(page,site.origin);await open(page);await target(page);
 let creates=0;page.on('request',r=>{if(r.method()==='POST'&&r.url().endsWith('/uploads'))creates++;});
 await page.evaluate(()=>{window.originalPut=IDBObjectStore.prototype.put;IDBObjectStore.prototype.put=function(){throw new DOMException('Controlled persistence failure','QuotaExceededError');};});
 await enqueue(page,'persist.txt',Buffer.from('abc'));await panel(page).getByRole('button',{name:'开始上传队列'}).click();await expect(panel(page).getByRole('status')).toContainText('存储');expect(creates).toBe(0);
 await page.evaluate(()=>{IDBObjectStore.prototype.put=window.originalPut;});await panel(page).getByRole('button',{name:'开始上传队列'}).click();await expect(panel(page).getByText(/^发布已确认；/)).toBeVisible();expect(creates).toBe(1);
 const [saved]=await localRows(page);
 await page.evaluate(saved=>new Promise((resolve,reject)=>{const open=indexedDB.open('anof-transfers',1);open.onsuccess=()=>{const db=open.result;const tx=db.transaction('records','readwrite');for(let i=0;i<63;i++)tx.objectStore('records').put({...saved,client_upload_id:crypto.randomUUID(),upload_id:null,state:'UNCERTAIN'});tx.oncomplete=()=>{db.close();resolve();};tx.onerror=()=>reject(Error('fixture capacity'));};}),saved);
 await enqueue(page,'full.txt',Buffer.from('abc'));await panel(page).getByRole('button',{name:'开始上传队列'}).click();await expect(panel(page).getByRole('status')).toContainText('记录已满');expect(creates).toBe(1);expect((await localRows(page)).length).toBe(64);
 await page.evaluate(saved=>new Promise(resolve=>{const open=indexedDB.open('anof-transfers',1);open.onsuccess=()=>{const db=open.result;const tx=db.transaction('records','readwrite');tx.objectStore('records').put({...saved,record_version:2});tx.oncomplete=()=>{db.close();resolve();};};}),saved);
 await panel(page).getByRole('button',{name:'刷新本地记录'}).click();await expect(panel(page).getByText('本地恢复记录不可用或损坏；不会清空或创建替代上传。',{exact:true})).toBeVisible();expect((await localRows(page)).length).toBe(64);
});

test('controlled Worker backpressure pauses and logout retires work without automatic resume',async({page,site})=>{
 await login(page,site.origin);await allowance(page);
 await page.evaluate(()=>{
  const NativeWorker=window.Worker;window.hashGate=true;window.workerRetirements=0;
  window.Worker=class extends NativeWorker{postMessage(message,...rest){if(window.hashGate&&message?.type==='ack')return;super.postMessage(message,...rest);}terminate(){window.workerRetirements++;super.terminate();}};
 });
 await open(page);await target(page);await enqueue(page,'pause.bin',Buffer.alloc(1048576+1,1));let creates=0;
 page.on('request',r=>{if(r.method()==='POST'&&r.url().endsWith('/uploads'))creates++;});
 await panel(page).getByRole('button',{name:'开始上传队列'}).click();await expect(panel(page).getByText(/已哈希 1048576 字节/)).toBeVisible();
 await panel(page).getByRole('button',{name:'暂停本地上传'}).click();await expect(panel(page).getByRole('button',{name:'开始上传队列'})).toBeEnabled();expect(creates).toBe(0);expect(await page.evaluate(()=>window.workerRetirements)).toBe(1);
 await panel(page).getByRole('button',{name:'开始上传队列'}).click();await expect(panel(page).getByText(/已哈希 1048576 字节/)).toBeVisible();
 await page.getByRole('button',{name:'登出',exact:true}).click();await expect(page.getByRole('status')).toHaveText('已确认登出。');expect(creates).toBe(0);expect(await page.evaluate(()=>window.workerRetirements)).toBe(2);
 await page.getByRole('button',{name:'重新核对身份'}).click();await expect(page.getByRole('heading',{name:'登录',exact:true})).toBeVisible();
 await login(page,site.origin);await open(page);await expect(panel(page).getByText('没有本地恢复记录。',{exact:true})).toBeVisible();expect(creates).toBe(0);
});

test('unknown server cancel reconciles without replay and local deletion cannot cancel again',async({page,site})=>{
 await page.addInitScript(()=>{const original=window.fetch;window.fetch=async(...args)=>{if(args[1]?.method==='PATCH')throw new TypeError('Controlled unsent PATCH');const response=await original(...args);if(args[1]?.method==='DELETE'&&String(args[0]).includes('/uploads/')){await response.arrayBuffer();throw new TypeError('Controlled cancel response loss');}return response;};});
 await login(page,site.origin);await open(page);await target(page);
 await enqueue(page,'cancel.txt',Buffer.from('cancel'));await panel(page).getByRole('button',{name:'开始上传队列'}).click();await expect(panel(page).getByRole('status')).toContainText('操作结果未知');
 let deletes=0;page.on('request',r=>{if(r.method()==='DELETE')deletes++;});
 await record(page,'cancel.txt').getByLabel('我已核对取消或清理操作').check();await record(page,'cancel.txt').getByRole('button',{name:'取消服务器上传',exact:true}).click();
 await expect(panel(page).getByRole('status')).toContainText('操作结果未知');await expect(record(page,'cancel.txt')).toContainText('CANCELLED');expect(deletes).toBe(1);
 await record(page,'cancel.txt').getByLabel('我已核对取消或清理操作').check();await record(page,'cancel.txt').getByRole('button',{name:'仅删除本地记录'}).click();await expect(panel(page).getByText('没有本地恢复记录。',{exact:true})).toBeVisible();expect(deletes).toBe(1);
 await panel(page).getByRole('button',{name:'服务器上传首页'}).click();await expect(panel(page).locator('li').filter({hasText:'cancel.txt：CANCELLED，0/6'})).toBeVisible();
});

test('native download refuses another private identity and revoked Cookie; disable stops upload recovery',async({page,site})=>{
 await login(page,site.origin);await open(page);await target(page);await publish(page,'private.txt',Buffer.from('private'));
 const href=await panel(page).getByRole('link',{name:'下载已发布文件（浏览器处理）'}).getAttribute('href');
 const invite=(await api(page,'/api/v1/admin/invites','POST',{max_uses:1})).body.token;const password='transfer-member-password';site.secrets.push(invite,password);
 const member=(await api(page,'/api/v1/auth/register','POST',{login:'transfer_member',display_name:'Transfer Member',password,invite_token:invite,credential_mode:'cookie'})).body;
 const memberContext=await site.context();const other=await memberContext.newPage();await login(other,site.origin,'transfer_member',password);
 // 原生节点链接激活；拒绝响应本身不构成下载成功。
 let downloads=0;other.on('download',()=>downloads++);
 await other.evaluate(href=>{const a=document.createElement('a');a.href=href;a.textContent='Private content probe';a.id='private-probe';document.body.append(a);},href);
 const refused=other.waitForResponse(r=>r.url()===site.origin+href);await other.locator('#private-probe').click();expect((await refused).status()).toBe(404);expect(downloads).toBe(0);
 await other.addInitScript(()=>{const original=window.fetch;window.fetch=async(...args)=>{if(args[1]?.method==='PATCH')throw new TypeError('Controlled paused upload');return original(...args);};});
 await other.goto(site.origin+'/#/transfers');await target(other);
 await enqueue(other,'disabled.txt',Buffer.from('disable'));await panel(other).getByRole('button',{name:'开始上传队列'}).click();await expect(panel(other).getByRole('status')).toContainText('操作结果未知');
 const detail=await api(page,`/api/v1/admin/users/${member.id}`);expect((await api(page,`/api/v1/admin/users/${member.id}`,'PATCH',{enabled:false},detail.etag)).status).toBe(200);
 await record(other,'disabled.txt').getByRole('button',{name:'核对服务器状态'}).click();await expect(other.getByRole('heading',{name:'登录',exact:true})).toBeVisible();await expect(panel(other)).toHaveCount(0);
 const old=(await page.context().cookies()).find(c=>c.name==='__Host-anof_session');await page.getByRole('button',{name:'登出',exact:true}).click();await expect(page.getByRole('status')).toHaveText('已确认登出。');await page.context().addCookies([old]);
 const revoked=page.waitForResponse(r=>r.url()===site.origin+href);await page.evaluate(href=>{const a=document.createElement('a');a.href=href;a.id='revoked-probe';a.textContent='Revoked probe';document.body.append(a);},href);await page.locator('#revoked-probe').click();expect((await revoked).status()).toBe(401);
});

test('IndexedDB capacity admission is atomic across two real tabs',async({page,context,site})=>{
 await login(page,site.origin);await open(page);await target(page);await publish(page,'seed.txt',Buffer.from('seed'));
 const [saved]=await localRows(page);
 await page.evaluate(saved=>new Promise(resolve=>{const open=indexedDB.open('anof-transfers',1);open.onsuccess=()=>{const db=open.result;const tx=db.transaction('records','readwrite');for(let i=0;i<62;i++)tx.objectStore('records').put({...saved,client_upload_id:crypto.randomUUID(),upload_id:null,state:'UNCERTAIN'});tx.oncomplete=()=>{db.close();resolve();};};}),saved);
 const other=await context.newPage();await other.goto(site.origin+'/#/transfers');await target(other);
 await enqueue(page,'race-a.txt',Buffer.from('a'));await enqueue(other,'race-b.txt',Buffer.from('b'));
 await Promise.all([panel(page).getByRole('button',{name:'开始上传队列'}).click(),panel(other).getByRole('button',{name:'开始上传队列'}).click()]);
 await expect.poll(async()=>[await panel(page).getByRole('status').innerText(),await panel(other).getByRole('status').innerText()].filter(s=>s.includes('记录已满')).length).toBe(1);
 await expect.poll(async()=>(await api(page,'/api/v1/uploads?limit=100')).body.items.filter(u=>u.state==='COMPLETED').length).toBe(2);
 expect((await localRows(page)).length).toBe(64);
});

test('controlled browser clock skew saves terminal records and inspection never renews local retention',async({page,site})=>{
 await page.addInitScript(()=>{
  const NativeDate=Date;window.clockOffset=300_000;
  window.Date=class extends NativeDate {constructor(...args){super(...(args.length?args:[NativeDate.now()+window.clockOffset]));}static now(){return NativeDate.now()+window.clockOffset;}};
 });
 await login(page,site.origin);await open(page);await target(page);
 for(const [offset,name] of [[300_000,'ahead.txt'],[-300_000,'behind.txt']]){
  await page.evaluate(offset=>{window.clockOffset=offset;},offset);
  await publish(page,name,Buffer.alloc(0));
  const saved=(await localRows(page)).find(r=>r.name===name);expect(saved.state).toBe('COMPLETED');expect(saved.updated_at>=saved.created_at).toBe(true);
  const remote=(await api(page,`/api/v1/uploads/${saved.upload_id}`)).body;
  expect(Math.sign(Date.parse(saved.created_at)-Date.parse(remote.created_at))).toBe(Math.sign(offset));
  await page.evaluate(()=>{window.clockOffset+=86400000;});
  await record(page,name).getByRole('button',{name:'核对服务器状态'}).click();
  await expect(panel(page).getByText(/^服务器状态：COMPLETED；/)).toBeVisible();
  expect((await localRows(page)).find(r=>r.name===name).updated_at).toBe(saved.updated_at);
 }
 await page.evaluate(()=>{window.clockOffset=8*86400000;});
 await panel(page).getByRole('button',{name:'刷新本地记录'}).click();
 await expect(panel(page).getByText('没有本地恢复记录。',{exact:true})).toBeVisible();expect(await localRows(page)).toEqual([]);
 expect((await api(page,'/api/v1/uploads?limit=100')).body.items.length).toBe(2);
});

test('real IndexedDB rejects wrong keyPath, autoIncrement and missing stores without changing evidence',async({page:authenticated,site})=>{
 await login(authenticated,site.origin);const cookies=await authenticated.context().cookies();
 for(const kind of ['single','increment','missing','reordered']){
  const context=await site.context();await context.addCookies(cookies);const page=await context.newPage();await page.goto(site.origin+'/#/account');await expect(page.getByRole('heading',{name:'账户资料',exact:true})).toBeVisible();
  const snapshot=()=>page.evaluate(()=>new Promise((resolve,reject)=>{
   const request=indexedDB.open('anof-transfers',1);request.onerror=()=>reject(Error('fixture database'));
   request.onsuccess=()=>{const db=request.result;const name=db.objectStoreNames[0];const tx=db.transaction(name);const store=tx.objectStore(name);const get=store.getAll();const keys=store.getAllKeys();tx.oncomplete=()=>{resolve({names:Array.from(db.objectStoreNames),keyPath:store.keyPath,autoIncrement:store.autoIncrement,rows:get.result,keys:keys.result});db.close();};};
  }));
  await page.evaluate(kind=>new Promise((resolve,reject)=>{
   const request=indexedDB.open('anof-transfers',1);request.onerror=()=>reject(Error('fixture schema'));
   request.onupgradeneeded=()=>{const options=kind==='increment'?{autoIncrement:true}:kind==='reordered'?{keyPath:['user_id','origin','instance_id','client_upload_id']}:{keyPath:'client_upload_id'};
    request.result.createObjectStore(kind==='missing'?'evidence':'records',options).put({client_upload_id:'fixture',origin:'origin',instance_id:'instance',user_id:'user',marker:'preserve'});};
   request.onsuccess=()=>{request.result.close();resolve();};
  }),kind);
  const before=await snapshot();let writes=0;page.on('request',r=>{if(['POST','PATCH','DELETE'].includes(r.method()))writes++;});
  await open(page);await target(page);
  await expect(panel(page).getByText('本地恢复记录不可用或损坏；不会清空或创建替代上传。',{exact:true})).toBeVisible();
  await enqueue(page,'blocked.txt',Buffer.alloc(0));await panel(page).getByRole('button',{name:'开始上传队列'}).click();
  await expect(panel(page).getByRole('status')).toHaveText('本地恢复库结构无效。');expect(writes).toBe(0);
  expect(await snapshot()).toEqual(before);await page.close();
 }
});
