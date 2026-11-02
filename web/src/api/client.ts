import { ApiError } from './errors.ts';
import { id, invalid, object, text } from './decode.ts';

export const MAX_RESPONSE_BYTES = 512 * 1024;
export type Decoder<T> = (value: unknown, headers: Headers, status: number) => T;
export interface RequestOptions<T> {
  method?: 'GET' | 'HEAD' | 'POST' | 'PATCH' | 'PUT' | 'DELETE';
  body?: unknown;
  etag?: string;
  status: number | readonly number[];
  rawBody?: Blob;
  uploadOffset?: string;
  timeoutMs?: number;
  decode: Decoder<T>;
  signal?: AbortSignal;
  identity?: boolean;
}

export async function readJson(response: Response, signal: AbortSignal): Promise<unknown> {
  if (!/^application\/json(?:\s*;\s*charset\s*=\s*"?utf-8"?)?$/iu.test(response.headers.get('content-type') ?? '')) return invalid();
  const declared = response.headers.get('content-length');
  if (declared !== null && (!/^[0-9]+$/u.test(declared) || BigInt(declared) > BigInt(MAX_RESPONSE_BYTES))) return invalid();
  if (!response.body) return invalid();
  const reader = response.body.getReader();
  const bytes = new Uint8Array(MAX_RESPONSE_BYTES);
  let size = 0;
  const abort = () => { void reader.cancel().catch(() => {}); };
  signal.addEventListener('abort', abort, { once: true });
  try {
    while (true) {
      signal.throwIfAborted();
      const part = await reader.read();
      signal.throwIfAborted();
      if (part.done) break;
      if (part.value.byteLength > MAX_RESPONSE_BYTES - size) return invalid();
      bytes.set(part.value, size);
      size += part.value.byteLength;
    }
    if (declared !== null && BigInt(size) !== BigInt(declared)) return invalid();
    return JSON.parse(new TextDecoder('utf-8', { fatal: true }).decode(bytes.subarray(0, size))) as unknown;
  } finally {
    signal.removeEventListener('abort', abort);
    void reader.cancel().catch(() => {});
    reader.releaseLock();
  }
}

export class ApiClient {
  readonly origin: string;
  readonly fetcher: typeof fetch;
  readonly timeoutMs: number;
  onUnauthenticated: () => void = () => {};
  #active = 0;
  #writing = false;
  #mutationRetired: Promise<void> = Promise.resolve();
  #epoch = 0;
  #reads = new Set<AbortController>();
  constructor(origin: string, fetcher: typeof fetch = fetch, timeoutMs = 45_000) {
    const parsed = new URL(origin);
    if (parsed.origin !== origin || parsed.protocol !== 'https:' || timeoutMs < 1 || timeoutMs > 60_000) throw new ApiError('local');
    this.origin = origin;
    this.fetcher = fetcher;
    this.timeoutMs = timeoutMs;
  }
  async waitForMutation(): Promise<void> {
    let timer: ReturnType<typeof setTimeout> | undefined;
    try { await Promise.race([this.#mutationRetired, new Promise<never>((_resolve,reject) => { timer=setTimeout(()=>reject(new ApiError('busy')),5000); })]); }
    finally { clearTimeout(timer); }
  }
  cancelReads(): void {
    this.#epoch++;
    for (const controller of this.#reads) controller.abort();
  }
  async request<T>(path: string, options: RequestOptions<T>): Promise<T> {
    const method = options.method ?? 'GET';
    const writing = method !== 'GET' && method !== 'HEAD';
    const timeoutMs = options.timeoutMs ?? this.timeoutMs;
    if (!Number.isInteger(timeoutMs) || timeoutMs < 1 || timeoutMs > 375_000) throw new ApiError('local');
    if (!path.startsWith('/api/v1/') || path.includes('\\') || path.includes('#') || new URL(path, this.origin).pathname !== path.split('?')[0]) throw new ApiError('local');
    if (this.#active >= 6 || (writing && this.#writing)) throw new ApiError('busy');
    if (options.signal?.aborted) throw new ApiError('cancelled');
    const headers = new Headers({ Accept: 'application/json' });
    let body: string | Blob | undefined;
    try {
      if (options.rawBody !== undefined) {
        if (method !== 'PATCH' || options.body !== undefined || options.rawBody.size < 1 || options.rawBody.size > 8 * 1024 * 1024 || !/^(0|[1-9][0-9]*)$/u.test(options.uploadOffset ?? '')) throw new Error();
        body = options.rawBody;
        headers.set('Content-Type', 'application/offset+octet-stream');
        headers.set('Upload-Offset', options.uploadOffset!);
      }
      if (options.body !== undefined) {
        if (!writing) throw new Error();
        body = JSON.stringify(options.body, (_key, value: unknown) => {
          if (typeof value === 'bigint' || (typeof value === 'number' && !Number.isSafeInteger(value))) throw new Error();
          return value;
        });
        if (body === undefined || new TextEncoder().encode(body).byteLength > 65536) throw new Error();
        headers.set('Content-Type', 'application/json');
      }
      if (options.etag !== undefined) {
        if (!/^"[^"\r\n]{1,158}"$/u.test(options.etag)) throw new Error();
        headers.set('If-Match', options.etag);
      }
    } catch { throw new ApiError('local'); }
    if (writing) headers.set('X-ANOF-Request', '1');
    const controller = new AbortController();
    const abort = () => controller.abort();
    options.signal?.addEventListener('abort', abort, { once: true });
    const timer = setTimeout(abort, timeoutMs);
    const epoch = this.#epoch;
    this.#active++;
    let retire: (() => void) | undefined;
    if (writing) { this.#writing = true; this.#mutationRetired = new Promise<void>(resolve => { retire=resolve; }); } else this.#reads.add(controller);
    let response: Response | undefined;
    try {
      const fetcher = this.fetcher;
      response = await fetcher(path, { method, headers, ...(body === undefined ? {} : { body }),
        // same-origin 模式配合 no-referrer 会使部分浏览器发送 Origin: null。
        credentials: 'same-origin', mode: 'same-origin', referrerPolicy: 'same-origin', cache: 'no-store', redirect: 'error', signal: controller.signal });
      if (!writing && epoch !== this.#epoch) throw new ApiError('stale');
      if (!response.ok) {
        if (method === 'HEAD') {
          if (response.status === 401 && options.identity !== false && epoch === this.#epoch) this.onUnauthenticated();
          throw new ApiError('rejected', response.status);
        }
        const raw = object(await readJson(response, controller.signal));
        const error = object(raw.error);
        const code = text(error.code, 80, 1);
        text(error.message, 4096);
        id(error.request_id);
        object(error.details);
        if (!/^[A-Z][A-Z0-9_]*$/u.test(code)) invalid();
        if (!writing && epoch !== this.#epoch) throw new ApiError('stale');
        if (response.status === 401 && code === 'UNAUTHENTICATED' && options.identity !== false && epoch === this.#epoch) this.onUnauthenticated();
        throw new ApiError(writing && (response.status >= 500 || code === 'COMMIT_OUTCOME_UNKNOWN') ? 'unknown' : 'rejected', response.status, code);
      }
      if (!(typeof options.status === 'number' ? [options.status] : options.status).includes(response.status)) return invalid();
      const value: unknown = response.status === 204 || method === 'HEAD' ? undefined : await readJson(response, controller.signal);
      if (!writing && epoch !== this.#epoch) throw new ApiError('stale');
      controller.signal.throwIfAborted();
      return options.decode(value, response.headers, response.status);
    } catch (error) {
      if (error instanceof ApiError && ['rejected', 'unknown', 'stale'].includes(error.kind)) throw error;
      if (writing) throw new ApiError('unknown', response?.status ?? 0);
      if (controller.signal.aborted) throw new ApiError(epoch !== this.#epoch ? 'stale' : options.signal?.aborted ? 'cancelled' : 'transport');
      throw new ApiError(response ? 'protocol' : 'transport');
    } finally {
      clearTimeout(timer);
      options.signal?.removeEventListener('abort', abort);
      this.#reads.delete(controller);
      this.#active--;
      if (writing) { this.#writing = false; retire?.(); }
      if (response?.body && !response.body.locked) void response.body.cancel().catch(() => {});
    }
  }
}
