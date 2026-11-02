import { recordKey } from './protocol.ts';
import type { Binding } from './protocol.ts';
export type LockRunner = <T>(key: Binding & {client_upload_id:string}, action: () => Promise<T>) => Promise<T>;
export const withRecordLock: LockRunner = async (key, action) => {
  if (!navigator.locks) throw Error('浏览器不支持 Web Locks，已禁用上传写入和恢复。');
  return navigator.locks.request(`anof-upload|${recordKey(key)}`, { ifAvailable: true }, async lock => {
    if (!lock) throw Error('另一标签页正在处理此记录，请等待其停止后再明确恢复。');
    return action();
  });
};
