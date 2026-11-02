import { ApiError, describeFailure, RECONCILE } from '../api/errors.ts';
import { currentUser } from '../auth/protocol.ts';
import { validateName } from '../drive/protocol.ts';
import { bindUpload, BLOCK, bound, FILE_LIMIT, matchHead, now, offsetNumber, terminal } from './protocol.ts';
import type { RecordValue, Upload } from './protocol.ts';
import type { TransferApi } from './api.ts';
import type { RecordStore } from './records.ts';
import type { HashFile } from './hash.ts';
import type { LockRunner } from './locks.ts';
export interface Job { file: File; space_id: string; parent_id: string; name: string }
const wait = (ms: number, signal: AbortSignal) => new Promise<void>((resolve,reject) => {
  signal.throwIfAborted();
  const abort = () => { clearTimeout(timer); reject(Error('已停止等待。')); };
  const timer = setTimeout(() => { signal.removeEventListener('abort',abort); resolve(); },ms);
  signal.addEventListener('abort',abort,{once:true});
});
export class TransferStore {
  queue: Job[] = [];
  busy = false;
  notice = '';
  phase = '等待操作';
  hashed = 0;
  confirmed = '0';
  current: Upload | null = null;
  #controller = new AbortController();
  #dead = false;
  #listeners = new Set<() => void>();
  readonly api: TransferApi;
  readonly records: RecordStore;
  readonly hash: HashFile;
  readonly lock: LockRunner;
  constructor(api: TransferApi, records: RecordStore, hash: HashFile, lock: LockRunner) { this.api=api;this.records=records;this.hash=hash;this.lock=lock; }
  subscribe(fn: () => void): () => void { this.#listeners.add(fn); return () => this.#listeners.delete(fn); }
  #emit(): void { if (!this.#dead) for (const fn of this.#listeners) fn(); }
  enqueue(files: File[], target: {space_id:string;id:string}): void {
    if (this.busy || files.length + this.queue.length > 16 || !files.length) throw Error('队列最多 16 个文件，请停止当前操作后再添加。');
    const jobs = files.map(file => {
      if (!Number.isSafeInteger(file.size) || BigInt(file.size) > FILE_LIMIT) throw Error('单文件上限为 1 TiB。');
      return { file, space_id: target.space_id, parent_id: target.id, name: validateName(file.name) };
    });
    this.queue.push(...jobs); this.#emit();
  }
  clearQueue(): void { if (this.busy) return; this.queue = []; this.#emit(); }
  pause(): void {
    this.#controller.abort(); this.notice = '已停止本地调度；请求取消不证明服务器回滚。请核对记录并重新选择文件后明确恢复。'; this.#emit();
  }
  destroy(): void { this.#dead = true; this.#controller.abort(); this.queue = []; this.#listeners.clear(); }
  async #identity(signal: AbortSignal): Promise<void> {
    const u = await this.api.client.request('/api/v1/users/me', { status:200, signal, decode:currentUser });
    if (!u.enabled || u.id !== this.api.binding.user_id || u.instance_id !== this.api.binding.instance_id) throw Error('身份已改变，已停止传输。');
  }
  async #save(r: RecordValue, u?: Upload): Promise<RecordValue> {
    // 本地时间不与服务器事件时间比较；重复核对沿用首次终态观察时间。
    const saved = u && terminal(u.state)
      ? (await this.records.list(this.api.binding)).find(value => value.client_upload_id === r.client_upload_id)
      : undefined;
    const localTime = now();
    const updated = saved && terminal(saved.state) ? saved.updated_at
      : terminal(r.state) ? r.updated_at : localTime < r.updated_at ? r.updated_at : localTime;
    const next: RecordValue = { ...r, ...(u ? {upload_id:u.id, parent_id:u.parent_id, name:u.name, confirmed_offset:u.offset, state:u.state} : {}), updated_at: updated };
    await this.records.put(next); return next;
  }
  async #read(r: RecordValue, signal: AbortSignal): Promise<Upload> {
    if (!bound(r,this.api.binding)) throw Error('恢复记录不属于当前身份。');
    let u: Upload;
    if (r.upload_id) u = await this.api.get(r.upload_id,signal);
    else {
      const p = await this.api.list(undefined,signal,r.client_upload_id);
      if (p.items.length !== 1) throw Error('未找到创建结果；不会自动重发或生成替代上传。请核对服务器列表。');
      u = p.items[0]!;
    }
    bindUpload(u,r); matchHead(u,await this.api.head(u.id,signal));
    this.current = u; this.confirmed = u.offset; this.#emit(); return u;
  }
  async #run(action: (signal: AbortSignal) => Promise<void>): Promise<void> {
    if (this.busy || this.#dead) return;
    this.busy = true; this.notice = ''; this.#controller = new AbortController(); const signal = this.#controller.signal; this.#emit();
    try { await this.#identity(signal); signal.throwIfAborted(); await action(signal); }
    catch (error) {
      if (!signal.aborted) this.notice = error instanceof ApiError ? describeFailure(error) : error instanceof Error ? error.message : '传输失败，请核对。';
    } finally { this.busy = false; this.#emit(); }
  }
  async start(): Promise<void> {
    await this.#run(async signal => {
      while (this.queue.length) {
        signal.throwIfAborted(); const job = this.queue[0]!;
        const key = { ...this.api.binding, client_upload_id: crypto.randomUUID() };
        await this.lock(key, async () => {
          this.phase = '正在计算完整 SHA-256'; this.hashed = 0; this.current = null; this.confirmed = '0'; this.#emit();
          const sha256 = await this.hash(job.file,signal,offset => { this.hashed = offset; this.#emit(); });
          signal.throwIfAborted(); const time = now();
          let r: RecordValue = { ...key, record_version:1, upload_id:null, space_id:job.space_id,parent_id:job.parent_id,name:job.name,size:String(job.file.size),sha256,confirmed_offset:'0',state:'INITIAL',created_at:time,updated_at:time };
          await this.#save(r); signal.throwIfAborted();
          // 从此刻起队列不再自动创建此项；未知责任保留在持久记录中。
          this.queue.shift(); r = await this.#save({...r,state:'UNCERTAIN'}); signal.throwIfAborted();
          try {
            const u = await this.api.create(r,signal); r = await this.#save(r,u);
            await this.#continue(r,u,job.file,signal);
          } catch (error) { await this.#reconcileFailure(r,signal); throw error; }
        });
        // 待处理/目标错误仍有服务器责任；暂停剩余队列，交还用户决定。
        if (this.current?.state !== 'COMPLETED') break;
      }
    });
  }
  async resume(r: RecordValue, file: File): Promise<void> {
    await this.#run(signal => this.lock(r,async () => {
      if (!bound(r,this.api.binding)) throw Error('恢复记录不属于当前身份。');
      if (String(file.size) !== r.size) throw Error('重新选择的文件长度不同，拒绝恢复。');
      this.phase = '正在重新计算完整 SHA-256'; this.hashed = 0; this.#emit();
      const sha = await this.hash(file,signal,n => { this.hashed=n; this.#emit(); });
      if (sha !== r.sha256) throw Error('重新选择的文件摘要不同，拒绝恢复。');
      signal.throwIfAborted();
      try { const u = await this.#read(r,signal); const saved = await this.#save(r,u); await this.#continue(saved,u,file,signal); }
      catch(error) { await this.#reconcileFailure(r,signal); throw error; }
    }));
  }
  async #continue(r: RecordValue, initial: Upload, file: File, signal: AbortSignal): Promise<void> {
    let u = initial;
    this.current=u; this.confirmed=u.offset; this.#emit();
    if (u.state === 'OPEN') {
      while (BigInt(u.offset) < BigInt(u.length)) {
        signal.throwIfAborted();
        const start = offsetNumber(u.offset); const end = Math.min(start+BLOCK,start+u.max_patch_bytes,file.size);
        r = await this.#save({...r,state:'UNCERTAIN'}); signal.throwIfAborted();
        this.phase='正在发送分块（以服务器确认偏移为准）'; this.#emit();
        const offset = await this.api.patch(u,file.slice(start,end),signal);
        u = {...u,offset}; this.current=u; this.confirmed=offset; this.#emit();
        r = await this.#save(r,u);
      }
      signal.throwIfAborted(); r = await this.#save({...r,state:'UNCERTAIN'}); signal.throwIfAborted();
      u = await this.api.complete(u,signal); r = await this.#save(r,u);
    }
    await this.#poll(r,u,signal);
  }
  async #poll(r: RecordValue, initial: Upload, signal: AbortSignal): Promise<void> {
    let u = initial; const deadline = performance.now()+60_000;
    for (let attempt=0; (['CREATING','VERIFYING','COMMITTING'].includes(u.state) || (u.state === 'READY' && u.last_error === null)) && attempt<20 && performance.now()<deadline; attempt++) {
      this.phase = '服务器已接纳，尚未确认发布'; this.current=u; this.#emit();
      await wait(500,signal);
      const remaining = Math.max(1,Math.floor(deadline-performance.now()));
      u = await this.api.get(u.id,AbortSignal.any([signal,AbortSignal.timeout(remaining)])); bindUpload(u,r); r = await this.#save(r,u);
    }
    this.current=u; this.confirmed=u.offset;
    this.phase = u.state === 'COMPLETED' ? (u.result_deleted_at ? '历史结果已删除，不会重新发布' : '发布已确认') : u.state === 'READY' && u.last_error !== null ? '等待用户选择目标并明确再次完成' : terminal(u.state) ? `上传已终止：${u.state}` : '仍待服务器处理，请稍后明确检查';
    this.#emit();
  }
  async #reconcileFailure(r: RecordValue, signal: AbortSignal): Promise<void> {
    if (signal.aborted || this.#dead) return;
    try { const u = await this.#read(r,signal); await this.#save(r,u); } catch { /* 核对失败不改变原始未知结果，也不重新写服务器。 */ }
    this.notice = RECONCILE; this.#emit();
  }
  inspect(r: RecordValue): Promise<void> {
    return this.#run(signal => this.lock(r,async () => { const u=await this.#read(r,signal); await this.#save(r,u); this.phase=u.result_deleted_at ? '历史结果已删除，不会重新发布' : `服务器状态：${u.state}`; }));
  }
  cancel(r: RecordValue): Promise<void> {
    return this.#run(signal => this.lock(r,async () => {
      const u=await this.#read(r,signal);
      if (terminal(u.state)) { await this.#save(r,u); this.notice='上传已终止，无需再次取消。'; return; }
      await this.#save({...r,upload_id:u.id,state:'UNCERTAIN'}); signal.throwIfAborted();
      try { await this.api.cancel(u.id,signal); this.notice='服务器已确认逻辑取消；物理清理可能尚未完成。'; const next=await this.#read({...r,upload_id:u.id},signal); await this.#save(r,next); }
      catch(error) { await this.#reconcileFailure(r,signal); throw error; }
    }));
  }
  retarget(r: RecordValue, target: {space_id:string;id:string}, name: string): Promise<void> {
    return this.#run(signal => this.lock(r,async () => {
      if (target.space_id !== r.space_id) throw Error('完成目标必须位于原空间。');
      const u=await this.#read(r,signal);
      if (!['OPEN','READY'].includes(u.state) || u.offset!==u.length) throw Error('当前状态不可更改完成目标。');
      validateName(name); await this.#save({...r,state:'UNCERTAIN'}); signal.throwIfAborted();
      try { const next=await this.api.complete(u,signal,{parent_id:target.id,name}); const saved=await this.#save(r,next); await this.#poll(saved,next,signal); }
      catch(error) { await this.#reconcileFailure(r,signal); throw error; }
    }));
  }
  inspectServer(uploadId: string): Promise<void> {
    return this.#run(async signal => { const u = await this.api.get(uploadId,signal); matchHead(u,await this.api.head(uploadId,signal)); this.current=u; this.confirmed=u.offset; this.phase=`服务器状态：${u.state}`; });
  }
  cancelServer(uploadId: string): Promise<void> {
    return this.#run(async signal => {
      const u=await this.api.get(uploadId,signal);
      await this.lock({...this.api.binding,client_upload_id:u.client_upload_id},async()=>{
        try { await this.api.cancel(uploadId,signal); this.notice='服务器已确认逻辑取消；物理清理可能尚未完成。'; }
        catch(error) { if (!signal.aborted) { try { this.current=await this.api.get(uploadId,signal); } catch {} } throw error; }
        this.current=await this.api.get(uploadId,signal); this.phase=`服务器状态：${this.current.state}`;
      });
    });
  }
  forget(r: RecordValue): Promise<void> { return this.#run(() => this.lock(r,()=>this.records.remove(r))); }
}
