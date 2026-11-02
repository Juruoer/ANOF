import { test } from 'node:test';
import assert from 'node:assert/strict';
import { ApiClient } from '../../src/api/client.ts';
import { TransferApi, PATCH_TIMEOUT } from '../../src/transfer/api.ts';
import { TransferStore } from '../../src/transfer/store.ts';
import { upload, record, head, matchHead, bindUpload, BLOCK, offsetNumber, now } from '../../src/transfer/protocol.ts';
import { retained } from '../../src/transfer/records.ts';
import { hashFile } from '../../src/transfer/hash.ts';
import { withRecordLock } from '../../src/transfer/locks.ts';
import { profile, INSTANCE, ID, json, me } from './support.mjs';
const U='30000000-0000-4000-8000-000000000001', C='40000000-0000-4000-8000-000000000001';
const SHA='a'.repeat(64), time=now();
const binding={origin:'https://localhost',instance_id:INSTANCE,user_id:ID};
const base={id:U,instance_id:INSTANCE,client_upload_id:C,space_id:ID,parent_id:ID,name:'test.txt',length:'3',offset:'0',expected_sha256:SHA,actual_sha256:null,state:'OPEN',result_node_id:null,result_deleted_at:null,created_at:time,last_progress_at:time,expires_at:'2099-01-01T00:00:00.000000Z',terminal_at:null,last_error:null,max_patch_bytes:8*BLOCK};
const rec={...binding,record_version:1,client_upload_id:C,upload_id:U,space_id:ID,parent_id:ID,name:'test.txt',size:'3',sha256:SHA,confirmed_offset:'0',state:'OPEN',created_at:time,updated_at:time};
const h=u=>({offset:u.offset,length:u.length,state:u.state,expires_at:u.expires_at});
function fixture(overrides={}) {
 const calls=[], rows=[];let current={...base};
 const api={binding,client:{request:async()=>({...profile,instance_id:INSTANCE})},create:async r=>{calls.push('create');current={...current,client_upload_id:r.client_upload_id,length:r.size,name:r.name};return current;},get:async()=>{calls.push('get');return current;},head:async()=>{calls.push('head');return h(current);},list:async()=>({items:[current],next_cursor:null}),patch:async(u,body)=>{calls.push(`patch:${u.offset}:${body.size}`);current={...current,offset:String(Number(u.offset)+body.size)};return current.offset;},complete:async()=>{calls.push('complete');current={...current,state:'COMPLETED',actual_sha256:SHA,result_node_id:U,terminal_at:time};return current;},cancel:async()=>{calls.push('cancel');current={...current,state:'CANCELLED',terminal_at:time};},...overrides};
 const records={list:async()=>rows,put:async r=>{rows.push(structuredClone(r));},remove:async()=>{calls.push('remove');}};
 const store=new TransferStore(api,records,async()=>SHA,async(_r,action)=>action());
 return {store,api,records,calls,rows,set:u=>{current=u;}};
}
test('upload decoder binds state, terminal evidence, offsets, hashes and 1 TiB synthetic limit',()=>{
 assert.equal(upload(base).state,'OPEN');assert.equal(offsetNumber('1099511627776'),1099511627776);
 for(const patch of [{length:'1099511627777'},{offset:'4'},{state:'OTHER'},{state:'COMPLETED'},{state:'READY'},{max_patch_bytes:8*BLOCK+1},{result_deleted_at:time},{terminal_at:time},{last_error:'<script>'}])assert.throws(()=>upload({...base,...patch}));
 assert.throws(()=>bindUpload({...base,length:'2'},rec));assert.throws(()=>bindUpload({...base,instance_id:ID},rec));assert.throws(()=>bindUpload({...base,name:'changed.txt'},{...rec,upload_id:null}));
});
test('HEAD has no JSON body and is not a mutation; header disagreement stops recovery',async()=>{
 const client=new ApiClient(binding.origin,async(_p,init)=>{assert.equal(init.method,'HEAD');assert.equal(init.headers.has('X-ANOF-Request'),false);return new Response(null,{headers:{'Upload-Offset':'0','Upload-Length':'3','Upload-State':'OPEN','Upload-Expires':base.expires_at}});});
 const api=new TransferApi(client,binding);matchHead(base,await api.head(U,new AbortController().signal));
 assert.throws(()=>matchHead(base,{...h(base),offset:'1'}));assert.throws(()=>head(new Headers()));
 const denied=new TransferApi(new ApiClient(binding.origin,async()=>new Response(null,{status:404})),binding);
 await assert.rejects(denied.head(U,new AbortController().signal),e=>e.status===404&&e.code==='');
});
test('raw PATCH preserves shared write admission, binary type, exact confirmation and finite policy',async()=>{
 let finish;const client=new ApiClient(binding.origin,async(_p,init)=>{assert.equal(init.headers.get('Content-Type'),'application/offset+octet-stream');assert.equal(init.headers.has('Content-Length'),false);assert.equal(init.body.size,3);return new Promise(r=>finish=r);});
 const api=new TransferApi(client,binding);const pending=api.patch(base,new Blob(['abc']),new AbortController().signal);
 await assert.rejects(client.request('/api/v1/nodes',{method:'POST',status:201,body:{},decode:v=>v}),e=>e.kind==='busy');
 finish(new Response(null,{status:204,headers:{'Upload-Offset':'3'}}));assert.equal(await pending,'3');assert.equal(PATCH_TIMEOUT,375000);
 const bad=new TransferApi(new ApiClient(binding.origin,async()=>new Response(null,{status:204,headers:{'Upload-Offset':'2'}})),binding);
 await assert.rejects(bad.patch(base,new Blob(['abc']),new AbortController().signal),e=>e.kind==='unknown');
});
test('records reject extra credential fields, corrupt quantities and unsupported versions; retention preserves uncertainty',()=>{
 assert.equal(record(rec).state,'OPEN');for(const patch of [{record_version:2},{token:'secret'},{sha256:'wrong'},{confirmed_offset:'4'},{size:'01'},{origin:'http://localhost'}])assert.throws(()=>record({...rec,...patch}));
 assert.throws(()=>retained(Array(65).fill(rec)));
 const old={...rec,created_at:'2020-01-01T00:00:00.000000Z',updated_at:'2020-01-01T00:00:00.000000Z'};
 assert.equal(retained([{...old,state:'COMPLETED'},{...old,state:'UNCERTAIN'}]).length,1);
});
test('queue bound is atomic, files are not hashed eagerly, and slices stay at one MiB',async()=>{
 const f=fixture();let hashes=0;const store=new TransferStore(f.api,f.records,async()=>{hashes++;return SHA;},async(_r,a)=>a());
 const files=Array.from({length:16},()=>new File(['abc'],'test.txt'));store.enqueue(files,{space_id:ID,id:ID});assert.equal(hashes,0);
 assert.throws(()=>store.enqueue([files[0]],{space_id:ID,id:ID}));assert.equal(store.queue.length,16);store.clearQueue();
 store.enqueue([new File([new Uint8Array(BLOCK+7)],'test.txt')],{space_id:ID,id:ID});await store.start();
 assert.deepEqual(f.calls.filter(c=>c.startsWith('patch')),['patch:0:1048576','patch:1048576:7']);assert.equal(hashes,1);
});
test('zero-byte upload is persisted before create and completes without PATCH',async()=>{
 const f=fixture();f.store.enqueue([new File([],'empty.txt')],{space_id:ID,id:ID});await f.store.start();
 assert.deepEqual(f.calls,['create','complete']);assert.equal(f.rows[0].state,'INITIAL');assert.equal(f.rows[0].upload_id,null);assert.equal(f.store.phase,'发布已确认');
});
test('initial persistence failure prevents create and later failure prevents more writes',async()=>{
 const f=fixture();f.records.put=async()=>{throw Error('disk failed');};f.store.enqueue([new File(['abc'],'test.txt')],{space_id:ID,id:ID});await f.store.start();assert.deepEqual(f.calls,[]);
 const g=fixture();let saves=0;g.records.put=async()=>{if(++saves>=4)throw Error('disk failed');};g.store.enqueue([new File(['abc'],'test.txt')],{space_id:ID,id:ID});await g.store.start();assert.equal(g.calls.includes('complete'),false);assert.equal(g.calls.some(c=>c.startsWith('patch')),false);
});
test('unknown creation reconciles by key without replay or replacement; absent match stops resume',async()=>{
 const f=fixture({create:async()=>{f.calls.push('create');throw Error('lost');}});f.store.enqueue([new File(['abc'],'test.txt')],{space_id:ID,id:ID});await f.store.start();assert.equal(f.calls.filter(c=>c==='create').length,1);assert.equal(f.store.queue.length,0);
 const g=fixture({list:async()=>({items:[],next_cursor:null})});await g.store.resume({...rec,upload_id:null},new File(['abc'],'test.txt'));assert.match(g.store.notice,/未找到/);assert.equal(g.calls.length,0);
});
test('resume rejects wrong source before server writes and uses authoritative offset, not saved hint',async()=>{
 const f=fixture();await f.store.resume(rec,new File(['longer'],'test.txt'));assert.equal(f.calls.length,0);
 f.set({...base,offset:'2'});await f.store.resume(rec,new File(['abc'],'test.txt'));assert.deepEqual(f.calls.filter(c=>c.startsWith('patch')),['patch:2:1']);
 const g=fixture();const bad=new TransferStore(g.api,g.records,async()=> 'b'.repeat(64),async(_r,a)=>a());await bad.resume(rec,new File(['abc'],'test.txt'));assert.equal(g.calls.length,0);assert.match(bad.notice,/摘要不同/);
});
test('uncertain PATCH and complete query state without replay; busy rejection cannot advance progress',async()=>{
 for(const operation of ['patch','complete']){
  const f=fixture();f.api[operation]=async()=>{f.calls.push(operation);throw Error('lost');};await f.store.resume(rec,new File(['abc'],'test.txt'));
  assert.equal(f.calls.filter(c=>c===operation).length,1);assert.ok(f.calls.includes('head'));assert.equal(f.store.busy,false);
 }
});
test('READY requires a decision; terminal/deleted results never restart publication',async()=>{
 for(const state of ['READY','FAILED','EXPIRED','COMPLETED']){
  const f=fixture();f.set({...base,offset:'3',state,last_error:state==='READY'?'NAME_CONFLICT':null,actual_sha256:SHA,result_node_id:state==='COMPLETED'?U:null,result_deleted_at:state==='COMPLETED'?time:null});
  await f.store.resume(rec,new File(['abc'],'test.txt'));assert.equal(f.calls.includes('complete'),false);assert.equal(f.calls.some(c=>c.startsWith('patch')),false);
  if(state==='COMPLETED')assert.match(f.store.phase,/历史结果已删除/);
 }
});
test('local deletion never cancels server; uncertain cancel reconciles once',async()=>{
 const f=fixture();await f.store.forget(rec);assert.deepEqual(f.calls,['remove']);
 const g=fixture({cancel:async()=>{g.calls.push('cancel');throw Error('lost');}});await g.store.cancel(rec);assert.equal(g.calls.filter(c=>c==='cancel').length,1);assert.ok(g.calls.includes('head'));
});
test('pause retires hashing and prevents create, destroy clears queued files without server cancel',async()=>{
 const f=fixture();let entered;const waiting=new Promise(r=>entered=r);
 const store=new TransferStore(f.api,f.records,async(_file,signal)=>{entered();return new Promise((_r,reject)=>signal.addEventListener('abort',()=>reject(Error('stopped')),{once:true}));},async(_r,a)=>a());
 store.enqueue([new File(['abc'],'test.txt')],{space_id:ID,id:ID});const work=store.start();await waiting;store.pause();await work;assert.equal(store.busy,false);assert.equal(f.calls.length,0);store.destroy();assert.equal(store.queue.length,0);
});
test('Web Locks unavailable rejects writes, occupied lock never waits or invokes action',async()=>{
 const original=Object.getOwnPropertyDescriptor(globalThis,'navigator');let called=false;
 try{
  Object.defineProperty(globalThis,'navigator',{configurable:true,value:{}});await assert.rejects(withRecordLock(rec,async()=>{called=true;}));
  Object.defineProperty(globalThis,'navigator',{configurable:true,value:{locks:{request:async(_key,options,action)=>{assert.equal(options.ifAvailable,true);return action(null);}}}});await assert.rejects(withRecordLock(rec,async()=>{called=true;}));assert.equal(called,false);
 }finally{if(original)Object.defineProperty(globalThis,'navigator',original);}
});
test('Worker acknowledgements bound progress and cancellation terminates worker',async()=>{
 const original=globalThis.Worker;let worker;
 class FakeWorker {sent=[];terminated=false;constructor(){worker=this;}postMessage(m){this.sent.push(m);}terminate(){this.terminated=true;}}
 globalThis.Worker=FakeWorker;
 try{
  const signal=new AbortController();const work=hashFile(new Blob([new Uint8Array(BLOCK+1)]),signal.signal,()=>{});
  assert.equal(worker.sent.length,1);worker.onmessage({data:{type:'progress',offset:BLOCK}});assert.deepEqual(worker.sent[1],{type:'ack'});
  signal.abort();await assert.rejects(work);assert.equal(worker.terminated,true);
 }finally{globalThis.Worker=original;}
});

test('pending verification polling has a finite attempt budget and never repeats complete',async()=>{
 const f=fixture();f.set({...base,offset:'3',state:'VERIFYING'});await f.store.resume(rec,new File(['abc'],'test.txt'));
 assert.equal(f.calls.filter(c=>c==='get').length,21);assert.equal(f.calls.includes('complete'),false);assert.match(f.store.phase,/仍待服务器处理/);
});
test('missing server record and changed identity stop recovery without new writes',async()=>{
 const f=fixture({get:async()=>{throw new Error('missing');}});await f.store.resume(rec,new File(['abc'],'test.txt'));assert.equal(f.calls.some(c=>c.startsWith('patch')||c==='create'||c==='complete'),false);
 const g=fixture();g.api.client.request=async()=>({...profile,id:U,instance_id:INSTANCE});await g.store.resume(rec,new File(['abc'],'test.txt'));assert.equal(g.calls.length,0);assert.match(g.store.notice,/身份已改变/);
});
test('confirmed complete survives a later persistence failure without another publication',async()=>{
 const f=fixture();const save=f.records.put;f.records.put=async r=>{if(r.state==='COMPLETED')throw Error('controlled final persistence loss');return save(r);};
 await f.store.resume(rec,new File(['abc'],'test.txt'));assert.equal(f.calls.filter(c=>c==='complete').length,1);assert.equal(f.store.current.state,'COMPLETED');assert.equal(f.store.current.result_node_id,U);
});

test('202 cannot claim a published result and complete cannot change immutable binding',async()=>{
 for(const [status,body] of [[202,{...base,offset:'3',state:'COMPLETED',actual_sha256:SHA,result_node_id:U,terminal_at:time}],[200,{...base,offset:'3',state:'VERIFYING'}],[202,{...base,offset:'3',state:'VERIFYING',expected_sha256:'b'.repeat(64)}]]){
  const api=new TransferApi(new ApiClient(binding.origin,async()=>json(body,status)),binding);
  await assert.rejects(api.complete({...base,offset:'3'},new AbortController().signal),e=>e.kind==='unknown');
 }
});
test('READY without an error is a publication transition, READY error stops the remaining queue',async()=>{
 const f=fixture();f.set({...base,offset:'3',state:'READY',actual_sha256:SHA});let reads=0;
 const get=f.api.get;f.api.get=async()=>{if(++reads===2)f.set({...base,offset:'3',state:'COMPLETED',actual_sha256:SHA,result_node_id:U,terminal_at:time});return get();};
 await f.store.resume(rec,new File(['abc'],'test.txt'));assert.equal(reads,2);assert.equal(f.store.phase,'发布已确认');assert.equal(f.calls.includes('complete'),false);
 const g=fixture();g.api.complete=async()=>({...base,offset:'3',state:'READY',actual_sha256:SHA,last_error:'NAME_CONFLICT'});
 g.store.enqueue([new File(['abc'],'test.txt'),new File(['abc'],'next.txt')],{space_id:ID,id:ID});await g.store.start();assert.equal(g.calls.filter(c=>c==='create').length,1);assert.equal(g.store.queue.length,1);
});
test('control waits for an aborted binary mutation to retire before acquiring shared write slot',async()=>{
 const controller=new AbortController();const calls=[];
 const client=new ApiClient(binding.origin,async(_path,init)=>{
  calls.push(init.method);
  if(init.method==='PATCH')return new Promise((_r,reject)=>init.signal.addEventListener('abort',()=>reject(Error('aborted')),{once:true}));
  return new Response(null,{status:204});
 });
 const patch=new TransferApi(client,binding).patch(base,new Blob(['abc']),controller.signal);const rejected=assert.rejects(patch,e=>e.kind==='unknown');
 controller.abort();await client.waitForMutation();await client.request('/api/v1/auth/session',{method:'DELETE',status:204,decode:()=>undefined});await rejected;assert.deepEqual(calls,['PATCH','DELETE']);
});

test('local terminal timestamps tolerate ahead, behind and rollback clocks without renewing retention',async t=>{
 const server=Date.parse(time);
 t.mock.timers.enable({apis:['Date'],now:server});
 for(const offset of [300_000,-300_000]){
  t.mock.timers.setTime(server+offset);
  const f=fixture();let saved;
  f.records.put=async value=>{saved=record(value);};
  f.records.list=async()=>saved?[structuredClone(saved)]:[];
  const complete=f.api.complete;
  f.api.complete=async()=>{t.mock.timers.setTime(server+offset-60_000);return complete();};
  f.store.enqueue([new File(['abc'],'test.txt')],{space_id:ID,id:ID});await f.store.start();
  assert.equal(saved.state,'COMPLETED');assert.equal(f.store.phase,'发布已确认');
  assert.equal(Date.parse(saved.created_at),server+offset);assert.equal(saved.updated_at,saved.created_at);
  const anchor=saved.updated_at;const stale={...saved,state:'OPEN',confirmed_offset:'0'};
  t.mock.timers.setTime(server+offset+86400000);
  await f.store.inspect(stale);assert.equal(saved.updated_at,anchor);
  await f.store.cancel(saved);assert.equal(saved.updated_at,anchor);assert.equal(f.calls.includes('cancel'),false);
  assert.equal(retained([saved],Date.parse(anchor)+7*86400000-1).length,1);
  assert.equal(retained([saved],Date.parse(anchor)+7*86400000).length,0);
  assert.throws(()=>record({...saved,updated_at:'2000-01-01T00:00:00.000000Z'}));
 }
});
