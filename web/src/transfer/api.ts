import type { ApiClient } from '../api/client.ts';
import { id, invalid } from '../api/decode.ts';
import { boundedPage } from '../drive/protocol.ts';
import { pageQuery } from '../api/pagination.ts';
import { empty } from '../auth/protocol.ts';
import { bindUpload, head, size, upload } from './protocol.ts';
import type { Binding, RecordValue, Upload } from './protocol.ts';
// PATCH 覆盖服务端 body 300 秒 + response 60 秒及 15 秒传输余量；控制/读取不无限等待。
export const PATCH_TIMEOUT = 375_000;
export const CONTROL_TIMEOUT = 45_000;
export class TransferApi {
  readonly client: ApiClient;
  readonly binding: Binding;
  constructor(client: ApiClient, binding: Binding) { if (binding.origin !== client.origin) invalid(); id(binding.instance_id); id(binding.user_id); this.client=client; this.binding=binding; }
  #decode(value: unknown): Upload { const u = upload(value); if (u.instance_id !== this.binding.instance_id) invalid(); return u; }
  list(cursor?: string, signal?: AbortSignal, clientId?: string) {
    if (cursor && clientId) invalid();
    return this.client.request(`/api/v1/uploads?${pageQuery(100,cursor)}${clientId ? `&client_upload_id=${id(clientId)}` : ''}`, { status: 200, timeoutMs: CONTROL_TIMEOUT, ...(signal ? { signal } : {}), decode: v => {
      const p = boundedPage(v, x => this.#decode(x));
      if (clientId && (p.items.length > 1 || p.next_cursor || p.items.some(u => u.client_upload_id !== clientId))) invalid(); return p;
    } });
  }
  get(uploadId: string, signal: AbortSignal) { return this.client.request(`/api/v1/uploads/${id(uploadId)}`, { status: 200, signal, timeoutMs: CONTROL_TIMEOUT, decode: v => { const u = this.#decode(v); if (u.id !== uploadId) invalid(); return u; } }); }
  head(uploadId: string, signal: AbortSignal) { return this.client.request(`/api/v1/uploads/${id(uploadId)}`, { method: 'HEAD', status: 200, signal, timeoutMs: CONTROL_TIMEOUT, decode: (_v,h) => head(h) }); }
  create(r: RecordValue, signal: AbortSignal) {
    return this.client.request('/api/v1/uploads', { method: 'POST', status: [200,201,202], signal, timeoutMs: CONTROL_TIMEOUT,
      body: {client_upload_id:r.client_upload_id,space_id:r.space_id,parent_id:r.parent_id,name:r.name,length:r.size,sha256:r.sha256}, decode: (v,_h,status) => { const u = this.#decode(v); bindUpload(u,r); if (u.parent_id !== r.parent_id || u.name !== r.name || (status === 201 && u.state !== 'OPEN') || (status === 202 && u.state !== 'CREATING') || (status === 200 && u.state === 'CREATING')) invalid(); return u; } });
  }
  patch(u: Upload, body: Blob, signal: AbortSignal) {
    const expected = (BigInt(u.offset) + BigInt(body.size)).toString();
    if (body.size > u.max_patch_bytes || BigInt(expected) > BigInt(u.length)) invalid();
    return this.client.request(`/api/v1/uploads/${u.id}`, { method: 'PATCH', rawBody: body, uploadOffset: u.offset, status: 204, timeoutMs: PATCH_TIMEOUT, signal, decode: (_v,h) => { const offset = size(h.get('Upload-Offset')); if (offset !== expected) invalid(); return offset; } });
  }
  complete(u: Upload, signal: AbortSignal, target?: {parent_id:string;name:string}) {
    return this.client.request(`/api/v1/uploads/${u.id}/complete`, { method: 'POST', status: [200,202], timeoutMs: CONTROL_TIMEOUT, signal, body: target ?? {}, decode: (v,_h,status) => { const next = this.#decode(v); if (next.id !== u.id || next.client_upload_id !== u.client_upload_id || next.length !== u.length || next.space_id !== u.space_id || next.expected_sha256 !== u.expected_sha256 || next.created_at !== u.created_at || next.parent_id !== (target?.parent_id ?? u.parent_id) || next.name !== (target?.name ?? u.name) || (status === 200) !== (next.state === 'COMPLETED') || !['VERIFYING','READY','COMMITTING','COMPLETED'].includes(next.state)) invalid(); return next; } });
  }
  cancel(uploadId: string, signal: AbortSignal) { return this.client.request(`/api/v1/uploads/${id(uploadId)}`, { method: 'DELETE', status: 204, signal, timeoutMs: CONTROL_TIMEOUT, decode: empty }); }
}
