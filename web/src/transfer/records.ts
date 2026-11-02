import { bound, record, terminal } from './protocol.ts';
import type { Binding, RecordValue } from './protocol.ts';
export interface RecordStore {
  list(binding: Binding): Promise<RecordValue[]>;
  put(value: RecordValue): Promise<void>;
  remove(value: RecordValue): Promise<void>;
}
export function retained(values: unknown[], time = Date.now()): RecordValue[] {
  if (values.length > 64) throw Error('本地恢复记录数量超限，请检查存储。');
  const decoded = values.map(record);
  if (decoded.reduce((n, r) => n + new TextEncoder().encode(JSON.stringify(r)).length, 0) > 256 * 1024) throw Error('本地恢复记录容量超限。');
  return decoded.filter(r => !terminal(r.state) || time - Date.parse(r.updated_at) < 7 * 86400000);
}
export class Records implements RecordStore {
  #open(): Promise<IDBDatabase> {
    return new Promise((resolve, reject) => {
      const request = indexedDB.open('anof-transfers', 1);
      let expired = false;
      const timer = setTimeout(() => { expired = true; reject(Error('本地存储打开超时。')); }, 10_000);
      request.onupgradeneeded = () => { request.result.createObjectStore('records', { keyPath: ['origin','instance_id','user_id','client_upload_id'] }); };
      request.onerror = () => { clearTimeout(timer); reject(Error('本地存储不可用，不会创建替代上传。')); };
      request.onblocked = () => { clearTimeout(timer); expired = true; reject(Error('本地存储被其他页面占用。')); };
      request.onsuccess = () => {
        clearTimeout(timer);
        const db = request.result;
        if (expired) { db.close(); return; }
        db.onversionchange = () => db.close(); resolve(db);
      };
    });
  }
  async #transaction(change?: { put?: RecordValue; remove?: RecordValue }): Promise<RecordValue[]> {
    const db = await this.#open();
    return new Promise((resolve, reject) => {
      let values: RecordValue[] = []; let failure: unknown;
      let tx: IDBTransaction;
      try { tx = db.transaction('records', 'readwrite'); } catch { db.close(); reject(Error('本地恢复库结构无效。')); return; }
      const timer = setTimeout(() => { failure = Error('本地存储操作超时。'); tx.abort(); }, 10_000);
      const store = tx.objectStore('records');
      tx.oncomplete = () => { clearTimeout(timer); db.close(); resolve(values); };
      tx.onabort = tx.onerror = () => { clearTimeout(timer); db.close(); reject(failure ?? Error('本地记录保存失败，停止写入并核对服务器状态。')); };
      const keyPath = ['origin','instance_id','user_id','client_upload_id'];
      if (store.autoIncrement || !Array.isArray(store.keyPath) ||
          store.keyPath.length !== keyPath.length || store.keyPath.some((key,i) => key !== keyPath[i])) {
        failure = Error('本地恢复库结构无效。'); tx.abort(); return;
      }
      const rows: unknown[] = [];
      const get = store.openCursor();
      get.onsuccess = () => {
        try {
          const cursor = get.result;
          if (cursor) {
            const r = record(cursor.value);
            const key = cursor.primaryKey;
            if (!Array.isArray(key) || key.length !== keyPath.length ||
                key.some((value,i) => value !== r[keyPath[i] as keyof RecordValue])) throw Error('本地恢复记录主键无效。');
            rows.push(r);
            if (rows.length > 64) throw Error('本地恢复记录数量超限，请检查存储。');
            cursor.continue(); return;
          }
          values = retained(rows);
          const key = (r: RecordValue) => [r.origin,r.instance_id,r.user_id,r.client_upload_id];
          const equal = (a: RecordValue, b: RecordValue) => key(a).join('|') === key(b).join('|');
          for (const raw of rows) { const r = record(raw); if (!values.some(v => equal(v,r))) store.delete(key(r)); }
          if (change?.remove) { const r = record(change.remove); values = values.filter(v => !equal(v,r)); store.delete(key(r)); }
          if (change?.put) {
            const r = record(change.put); values = values.filter(v => !equal(v,r)); values.push(r);
            if (values.length > 64 || values.reduce((n,v) => n + new TextEncoder().encode(JSON.stringify(v)).length, 0) > 256*1024) throw Error('恢复记录已满，请明确清理本地记录后再创建。');
            store.put(r);
          }
        } catch (error) { failure = error instanceof DOMException ? Error('本地存储操作失败，已停止服务器写入。') : error; tx.abort(); }
      };

    });
  }
  async list(binding: Binding): Promise<RecordValue[]> { return (await this.#transaction()).filter(r => bound(r,binding)); }
  async put(value: RecordValue): Promise<void> { await this.#transaction({ put: value }); }
  async remove(value: RecordValue): Promise<void> { await this.#transaction({ remove: value }); }
}
