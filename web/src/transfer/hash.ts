import { BLOCK, digest, FILE_LIMIT } from './protocol.ts';
export type HashFile = (file: Blob, signal: AbortSignal, progress: (offset: number) => void) => Promise<string>;
export const hashFile: HashFile = (file, signal, progress) => new Promise((resolve,reject) => {
  if (signal.aborted || !Number.isSafeInteger(file.size) || BigInt(file.size) > FILE_LIMIT) { reject(Error('文件长度无效或哈希已取消。')); return; }
  const worker = new Worker(new URL('../workers/sha256.ts', import.meta.url), { type:'module' });
  let settled = false;
  let offset = 0; let timer: ReturnType<typeof setTimeout>;
  const total = setTimeout(() => finish(undefined), 6*60*60*1000);
  const finish = (value: string | undefined) => { if (settled) return; settled = true; clearTimeout(timer); clearTimeout(total); signal.removeEventListener('abort',abort); worker.terminate(); value === undefined ? reject(Error('哈希已停止或失败，请重新选择文件。')) : resolve(value); };
  const abort = () => finish(undefined);
  const deadline = () => { clearTimeout(timer); timer = setTimeout(abort,30_000); };
  signal.addEventListener('abort',abort,{once:true}); deadline();
  worker.onerror = worker.onmessageerror = abort;
  worker.onmessage = event => {
    if (settled) return;
    try {
      if (event.data?.type === 'progress' && event.data.offset === Math.min(offset+BLOCK,file.size) && offset < file.size) {
        offset = event.data.offset; progress(offset); deadline(); worker.postMessage({type:'ack'});
      } else if (event.data?.type === 'done' && offset === file.size) finish(digest(event.data.digest));
      else abort();
    } catch { abort(); }
  };
  worker.postMessage({type:'start',file});
});
