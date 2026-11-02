import { button, element, field } from '../ui/dom.ts';
import { DestinationPicker } from '../drive/picker.ts';
import { DriveApi } from '../drive/api.ts';
import type { Node } from '../drive/protocol.ts';
import type { CurrentUser } from '../auth/protocol.ts';
import type { ApiClient } from '../api/client.ts';
import { describeFailure } from '../api/errors.ts';
import { TransferApi } from './api.ts';
import { TransferStore } from './store.ts';
import { Records } from './records.ts';
import { hashFile } from './hash.ts';
import { withRecordLock } from './locks.ts';
import type { RecordValue, Upload } from './protocol.ts';
export class TransferView {
  readonly root = element('section');
  readonly store: TransferStore;
  #dead=false;
  #controller=new AbortController();
  #picker: DestinationPicker | null=null;
  #target: Node | null=null;
  #status=element('p');
  #progress=element('p');
  #queue=element('ul');
  #records=element('section');
  #server=element('section');
  #result=element('div');
  #cursors: (string | undefined)[]=[undefined];
  #epoch=0;
  #recordSnapshot: string | null=null;
  #serverEpoch=0;
  #supported=Boolean(navigator.locks);
  constructor(client: ApiClient,user: CurrentUser) {
    this.root.className='transfers'; this.root.setAttribute('aria-label','传输管理');
    this.store=new TransferStore(new TransferApi(client,{origin:client.origin,instance_id:user.instance_id,user_id:user.id}),new Records(),hashFile,withRecordLock);
    this.#status.setAttribute('role','status'); this.#status.setAttribute('aria-live','polite');
    this.root.append(element('h2','传输管理'),element('p','离开此页会暂停本地调度；不会自动取消服务器任务。恢复需重新选择文件并明确操作。'),this.#status,this.#progress,this.#result);
    if (!this.#supported) this.root.append(element('p','浏览器不支持 Web Locks，已禁用上传写入和恢复。'));
    const pickerHost=element('div'); this.root.append(pickerHost);
    const drive=new DriveApi(client,user);
    void drive.spaces(this.#controller.signal).then(spaces=>{
      if(this.#dead)return;
      this.#picker=new DestinationPicker(drive,spaces,target=>{this.#target=target;}); pickerHost.append(this.#picker.root);
    }).catch(error=>{if(!this.#dead)this.#status.textContent=describeFailure(error);});
    const form=element('form'); const files=field(form,'选择上传文件（最多 16 个）','files','file'); files.multiple=true;
    form.append(this.#action('加入上传队列',()=>{
      if(!this.#target)throw Error('请先明确选用目标目录。');
      if ((files.files?.length ?? 0) > 16) throw Error('队列最多 16 个文件。');
      this.store.enqueue(Array.from(files.files??[]),this.#target); files.value='';
    })); form.addEventListener('submit',e=>e.preventDefault()); this.root.append(form);
    this.root.append(this.#queue,this.#action('开始上传队列',()=>this.store.start()),button('暂停本地上传',()=>this.store.pause()),this.#action('清除未开始队列',()=>this.store.clearQueue()));
    this.root.append(button('刷新本地记录',()=>{void this.#loadRecords();}),this.#records,button('服务器上传首页',()=>{this.#cursors=[undefined];void this.#loadServer();}),this.#server);
    this.store.subscribe(()=>this.#render()); this.#render(); void this.#loadRecords(); void this.#loadServer();
  }
  destroy():void{this.#dead=true;this.#epoch++;this.#serverEpoch++;this.#controller.abort();this.#picker?.destroy();this.store.destroy();}
  #action(label:string,action:()=>void|Promise<void>):HTMLButtonElement {
    const b=button(label,()=>{if(this.store.busy||!this.#supported)return;void(async()=>{try{await action();}catch(error){this.store.notice=error instanceof Error?error.message:describeFailure(error);}if(!this.#dead){this.#render();await this.#loadRecords();}})();});
    b.dataset.transferWrite='true';b.disabled=!this.#supported||this.store.busy;return b;
  }
  #render():void {
    this.#status.textContent=this.store.notice;
    this.#progress.textContent=`${this.store.phase}；已哈希 ${this.store.hashed} 字节；服务器确认偏移 ${this.store.confirmed}`;
    this.#queue.replaceChildren(...this.store.queue.map(j=>element('li',j.name)));
    for(const b of this.root.querySelectorAll<HTMLButtonElement>('button[data-transfer-write]'))b.disabled=!this.#supported||this.store.busy;
    this.#result.replaceChildren();const u=this.store.current;
    if(u){this.#result.append(element('p',`${u.name}：${u.state}，${u.offset}/${u.length}${u.last_error?`，${u.last_error}`:''}`));if(u.result_deleted_at)this.#result.append(element('p','历史结果已删除，不会重新发布。'));else if(u.result_node_id){const a=element('a','下载已发布文件（浏览器处理）');a.href=`/api/v1/nodes/${u.result_node_id}/content`;this.#result.append(a);}}
  }
  async #loadRecords():Promise<void>{
    const epoch=++this.#epoch;
    try{
      const records=await this.store.records.list(this.store.api.binding);if(this.#dead||epoch!==this.#epoch)return;
      // 内容未变时保留文件输入节点，避免晚到刷新清掉用户刚重选的文件。
      const snapshot=JSON.stringify(records);
      if(snapshot===this.#recordSnapshot)return;
      this.#recordSnapshot=snapshot;
      this.#records.replaceChildren(element('h3','本地恢复记录'));
      if(!records.length)this.#records.append(element('p','没有本地恢复记录。'));
      for(const r of records)this.#records.append(this.#record(r));
    }catch{if(!this.#dead&&epoch===this.#epoch){this.#recordSnapshot=null;this.#records.replaceChildren(element('p','本地恢复记录不可用或损坏；不会清空或创建替代上传。'));}}
  }
  #record(r:RecordValue):HTMLElement{
    const row=element('article');row.className='transfer-record';row.append(element('h4',r.name),element('p',`${r.state}，本地偏移线索 ${r.confirmed_offset}/${r.size}`));
    const form=element('form');form.addEventListener('submit',e=>e.preventDefault());
    const file=field(form,'重新选择原文件','resume','file');
    form.append(this.#action('验证文件并恢复',()=>{const f=file.files?.[0];if(!f)throw Error('请重新选择原文件。');file.value='';return this.store.resume(r,f);}));
    row.append(form,this.#action('核对服务器状态',()=>this.store.inspect(r)));
    const change=element('form');change.addEventListener('submit',e=>e.preventDefault());const name=field(change,'完成目标名称','name');name.value=r.name;
    change.append(this.#action('使用所选目录再次完成',()=>{if(!this.#target)throw Error('请明确选用同空间目标目录。');return this.store.retarget(r,this.#target,name.value);}));row.append(change);
    const confirm=element('label','我已核对取消或清理操作');const check=element('input');check.type='checkbox';confirm.append(check);row.append(confirm);
    row.append(this.#action('取消服务器上传',()=>{if(!check.checked)throw Error('请先勾选确认。');return this.store.cancel(r);}),this.#action('仅删除本地记录',()=>{if(!check.checked)throw Error('请先勾选确认。本地删除不取消服务器上传。');return this.store.forget(r);}));return row;
  }
  async #loadServer():Promise<void>{
    const epoch=++this.#serverEpoch;
    try{
      const page=await this.store.api.list(this.#cursors.at(-1),this.#controller.signal);if(this.#dead||epoch!==this.#serverEpoch)return;
      this.#server.replaceChildren(element('h3','服务器上传记录'));
      const list=element('ul');for(const u of page.items)list.append(this.#serverRow(u));this.#server.append(list);
      if(!page.items.length)this.#server.append(element('p','服务器上传列表为空。'));
      if(this.#cursors.length>1)this.#server.append(button('服务器上一页',()=>{this.#cursors.pop();void this.#loadServer();}));
      if(page.next_cursor)this.#server.append(button('服务器下一页',()=>{this.#cursors.push(page.next_cursor!);if(this.#cursors.length>5)this.#cursors.shift();void this.#loadServer();}));
    }catch(error){if(!this.#dead&&epoch===this.#serverEpoch)this.#server.replaceChildren(element('p',describeFailure(error)));}
  }
  #serverRow(u:Upload):HTMLElement{
    const li=element('li',`${u.name}：${u.state}，${u.offset}/${u.length}${u.result_deleted_at?'，历史结果已删除':''}`);
    li.append(this.#action('查看最新上传状态',()=>this.store.inspectServer(u.id)));
    const label=element('label','确认取消此服务器上传');const check=element('input');check.type='checkbox';label.append(check);li.append(label,this.#action('明确取消此服务器上传',()=>{if(!check.checked)throw Error('请先确认取消。');return this.store.cancelServer(u.id);}));return li;
  }
}
