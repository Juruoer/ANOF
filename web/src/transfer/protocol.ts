import { decimal, id, invalid, keys, object, text, timestamp } from '../api/decode.ts';
import { validateName } from '../drive/protocol.ts';
export const FILE_LIMIT = 1099511627776n;
export const BLOCK = 1024 * 1024;
export const states = ['CREATING', 'OPEN', 'VERIFYING', 'READY', 'COMMITTING', 'COMPLETED', 'CANCELLED', 'EXPIRED', 'FAILED'] as const;
export type UploadState = typeof states[number];
export const terminal = (state: string) => ['COMPLETED', 'CANCELLED', 'EXPIRED', 'FAILED'].includes(state);
export function digest(value: unknown): string { const s = text(value, 64, 64); if (!/^[0-9a-f]{64}$/u.test(s)) invalid(); return s; }
export function size(value: unknown): string { return decimal(value, FILE_LIMIT); }
export function offsetNumber(value: unknown): number { return Number(size(value)); }
export function state(value: unknown): UploadState { if (!states.includes(value as UploadState)) invalid(); return value as UploadState; }
export function now(): string { return new Date().toISOString().replace('Z', '000Z'); }
export interface Upload {
  id: string; instance_id: string; client_upload_id: string; space_id: string; parent_id: string; name: string;
  length: string; offset: string; expected_sha256: string | null; actual_sha256: string | null; state: UploadState;
  result_node_id: string | null; result_deleted_at: string | null; created_at: string; last_progress_at: string;
  expires_at: string; terminal_at: string | null; last_error: string | null; max_patch_bytes: number;
}
const nullable = (v: unknown, decode: (v: unknown) => string): string | null => v === null ? null : decode(v);
export function upload(value: unknown): Upload {
  const r = object(value);
  keys(r, ['id','instance_id','client_upload_id','space_id','parent_id','name','length','offset','expected_sha256','actual_sha256','state','result_node_id','result_deleted_at','created_at','last_progress_at','expires_at','terminal_at','last_error','max_patch_bytes']);
  const u: Upload = {
    id: id(r.id), instance_id: id(r.instance_id), client_upload_id: id(r.client_upload_id), space_id: id(r.space_id), parent_id: id(r.parent_id), name: validateName(text(r.name)),
    length: size(r.length), offset: size(r.offset), expected_sha256: nullable(r.expected_sha256, digest), actual_sha256: nullable(r.actual_sha256, digest), state: state(r.state),
    result_node_id: nullable(r.result_node_id, id), result_deleted_at: nullable(r.result_deleted_at, timestamp), created_at: timestamp(r.created_at), last_progress_at: timestamp(r.last_progress_at),
    expires_at: timestamp(r.expires_at), terminal_at: nullable(r.terminal_at, timestamp), last_error: nullable(r.last_error, v => text(v, 80, 1)), max_patch_bytes: r.max_patch_bytes as number,
  };
  if (!Number.isInteger(u.max_patch_bytes) || u.max_patch_bytes < 1 || u.max_patch_bytes > 8 * BLOCK || BigInt(u.offset) > BigInt(u.length) || u.last_progress_at < u.created_at || u.expires_at < u.created_at) invalid();
  if (terminal(u.state) !== (u.terminal_at !== null) || (u.terminal_at && u.terminal_at < u.created_at) || (u.state === 'COMPLETED') !== (u.result_node_id !== null)) invalid();
  if (u.result_deleted_at && (u.state !== 'COMPLETED' || u.result_deleted_at < u.terminal_at!)) invalid();
  if (['READY','COMMITTING','COMPLETED'].includes(u.state) && (!u.actual_sha256 || u.offset !== u.length || (u.expected_sha256 !== null && u.actual_sha256 !== u.expected_sha256))) invalid();
  if (u.last_error !== null && !['NAME_CONFLICT','PARENT_NOT_DIRECTORY','DEPTH_LIMIT_EXCEEDED','NOT_FOUND','CHECKSUM_MISMATCH','UPLOAD_CANCELLED','UPLOAD_EXPIRED','USER_DISABLED','STORAGE_FULL','TEMPORARILY_UNAVAILABLE','INTERNAL_ERROR','COMMIT_OUTCOME_UNKNOWN'].includes(u.last_error)) invalid();
  return u;
}
export interface UploadHead { offset: string; length: string; state: UploadState; expires_at: string }
export function head(headers: Headers): UploadHead {
  return { offset: size(headers.get('Upload-Offset')), length: size(headers.get('Upload-Length')), state: state(headers.get('Upload-State')), expires_at: timestamp(headers.get('Upload-Expires')) };
}
export function matchHead(u: Upload, h: UploadHead): void {
  if (u.offset !== h.offset || u.length !== h.length || u.state !== h.state || u.expires_at !== h.expires_at) throw Error('上传状态发生变化，请重新核对后继续。');
}
export interface Binding { origin: string; instance_id: string; user_id: string }
export interface RecordValue extends Binding {
  record_version: 1; client_upload_id: string; upload_id: string | null; space_id: string; parent_id: string; name: string;
  size: string; sha256: string; confirmed_offset: string; state: UploadState | 'INITIAL' | 'UNCERTAIN'; created_at: string; updated_at: string;
}
export function record(value: unknown): RecordValue {
  const r = object(value);
  keys(r, ['record_version','origin','instance_id','user_id','client_upload_id','upload_id','space_id','parent_id','name','size','sha256','confirmed_offset','state','created_at','updated_at']);
  const origin = text(r.origin, 512, 1);
  if (r.record_version !== 1 || new URL(origin).origin !== origin || !origin.startsWith('https://')) invalid();
  const out: RecordValue = { record_version: 1, origin, instance_id: id(r.instance_id), user_id: id(r.user_id), client_upload_id: id(r.client_upload_id), upload_id: nullable(r.upload_id, id), space_id: id(r.space_id), parent_id: id(r.parent_id), name: validateName(text(r.name)), size: size(r.size), sha256: digest(r.sha256), confirmed_offset: size(r.confirmed_offset), state: r.state === 'INITIAL' || r.state === 'UNCERTAIN' ? r.state : state(r.state), created_at: timestamp(r.created_at), updated_at: timestamp(r.updated_at) };
  if (BigInt(out.confirmed_offset) > BigInt(out.size) || out.updated_at < out.created_at || new TextEncoder().encode(JSON.stringify(out)).length > 4096) invalid();
  if (!['INITIAL','UNCERTAIN'].includes(out.state) && out.upload_id === null) invalid();
  if (out.state === 'INITIAL' && (out.upload_id !== null || out.confirmed_offset !== '0')) invalid();
  return out;
}
export function recordKey(r: Binding & {client_upload_id: string}): string { return `${r.origin}|${r.instance_id}|${r.user_id}|${r.client_upload_id}`; }
export function bound(r: Binding, b: Binding): boolean { return r.origin === b.origin && r.instance_id === b.instance_id && r.user_id === b.user_id; }
export function bindUpload(u: Upload, r: RecordValue): void {
  if (r.upload_id === null && (u.parent_id !== r.parent_id || u.name !== r.name)) invalid();
  if (u.instance_id !== r.instance_id || u.client_upload_id !== r.client_upload_id || u.space_id !== r.space_id || u.length !== r.size || u.expected_sha256 !== r.sha256 || (r.upload_id !== null && u.id !== r.upload_id)) invalid();
}
