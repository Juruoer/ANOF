import { createSHA256 } from 'hash-wasm';
const scope = self as unknown as { onmessage: ((e: MessageEvent) => void) | null; postMessage(value: unknown): void };
let started = false;
let acknowledge: (() => void) | null = null;
scope.onmessage = event => {
  if (event.data?.type === 'ack' && acknowledge) { const next = acknowledge; acknowledge = null; next(); return; }
  if (event.data?.type !== 'start' || started || !(event.data.file instanceof Blob)) return;
  started = true;
  const file: Blob = event.data.file;
  void (async () => {
    const hash = await createSHA256(); hash.init();
    for (let offset = 0; offset < file.size;) {
      const end = Math.min(offset + 1024*1024, file.size);
      const block = await file.slice(offset,end).arrayBuffer(); hash.update(new Uint8Array(block)); offset = end;
      await new Promise<void>(resolve => { acknowledge = resolve; scope.postMessage({ type: 'progress', offset }); });
    }
    scope.postMessage({ type: 'done', digest: hash.digest('hex') });
  })().catch(() => scope.postMessage({ type: 'error' }));
};
