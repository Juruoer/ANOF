import { decimal, etag, id, INT64_MAX, invalid, keys, object, page, text, timestamp } from '../api/decode.ts';
import type { Page } from '../api/decode.ts';
import { ApiError } from '../api/errors.ts';
export interface Amount { bytes: string; nodes: string }
export interface Quota { limit: Amount; used: Amount; reserved: Amount; revision: string }
export interface Space { id: string; kind: 'private' | 'public'; owner_id: string | null; root_node_id: string; quota: Quota; my_contribution_quota: Quota | null }
export interface Node { id: string; space_id: string; parent_id: string | null; name: string; kind: 'file' | 'directory'; created_by: string; created_at: string; updated_at: string; etag: string; size?: string; sha256?: string }
export interface Trash { id: string; instance_id: string; space_id: string; root_node_id: string; original_parent_id: string; name: string; kind: 'file' | 'directory'; deleted_at: string; expires_at: string; revision: string; etag: string; node_count: string; logical_bytes: string }
function name(value: unknown): string {
  const result = text(value, 255, 1);
  if (result === '.' || result === '..' || /[\/\\\p{Cc}]/u.test(result)) invalid();
  return result;
}
export function validateName(value: string): string { try { return name(value); } catch { throw new ApiError('local'); } }
export function validateSearch(value: string): string {
  try { text(value, 128, 1); if (/\p{Cc}/u.test(value)) invalid(); return value; } catch { throw new ApiError('local'); }
}
function amount(value: unknown): Amount {
  const r = object(value); keys(r, ['bytes', 'nodes']); return { bytes: decimal(r.bytes, INT64_MAX), nodes: decimal(r.nodes, INT64_MAX) };
}
export function quota(value: unknown): Quota {
  const r = object(value); keys(r, ['limit', 'used', 'reserved', 'revision']);
  // 分别精确解码上限和计量；写入时的额度约束由服务端事务核对。
  return { limit: amount(r.limit), used: amount(r.used), reserved: amount(r.reserved), revision: decimal(r.revision, INT64_MAX, 1n) };
}
export function spaces(value: unknown, userId: string): Space[] {
  const r = object(value); keys(r, ['items']);
  if (!Array.isArray(r.items) || r.items.length !== 2) return invalid();
  const result = r.items.map((value): Space => {
    const s = object(value); keys(s, ['id', 'kind', 'owner_id', 'root_node_id', 'quota', 'my_contribution_quota']);
    if (s.kind !== 'private' && s.kind !== 'public') return invalid();
    const owner = s.owner_id === null ? null : id(s.owner_id);
    const contribution = s.my_contribution_quota === null ? null : quota(s.my_contribution_quota);
    if (s.kind === 'private' ? owner !== userId || contribution !== null : owner !== null || contribution === null) invalid();
    return { id: id(s.id), kind: s.kind, owner_id: owner, root_node_id: id(s.root_node_id), quota: quota(s.quota), my_contribution_quota: contribution };
  });
  if (result[0]!.id === result[1]!.id || result[0]!.kind === result[1]!.kind || result[0]!.root_node_id === result[1]!.root_node_id) invalid();
  return result;
}
const nodeFields = ['id', 'space_id', 'parent_id', 'name', 'kind', 'created_by', 'created_at', 'updated_at', 'etag'];
export function node(value: unknown): Node {
  const r = object(value);
  if (r.kind !== 'file' && r.kind !== 'directory') return invalid();
  keys(r, r.kind === 'file' ? [...nodeFields, 'size', 'sha256'] : nodeFields);
  const parent = r.parent_id === null ? null : id(r.parent_id);
  const nodeId = id(r.id), created = timestamp(r.created_at), updated = timestamp(r.updated_at);
  if (updated < created || parent === nodeId || (parent === null && (r.name !== '' || r.kind !== 'directory'))) invalid();
  const result: Node = { id: nodeId, space_id: id(r.space_id), parent_id: parent, name: parent === null ? '' : name(r.name), kind: r.kind,
    created_by: id(r.created_by), created_at: created, updated_at: updated, etag: etag(r.etag, 'node', [nodeId]) };
  if (r.kind === 'file') {
    result.size = decimal(r.size, 1099511627776n);
    result.sha256 = text(r.sha256, 64, 64); if (!/^[0-9a-f]{64}$/u.test(result.sha256)) invalid();
  }
  return result;
}
export function nodeResponse(value: unknown, headers: Headers): Node {
  const result = node(value); if (headers.get('etag') !== result.etag) invalid(); return result;
}
export function createdDirectory(value: unknown, headers: Headers): Node {
  const r = object(value); keys(r, nodeFields.filter(key => key !== 'etag'));
  const result = nodeResponse({ ...r, etag: headers.get('etag') }, headers);
  if (result.kind !== 'directory' || result.parent_id === null || result.created_at !== result.updated_at || result.etag !== `"node-${result.id}-1"`) invalid();
  locationHeader(result.id, headers); return result;
}
export function locationHeader(nodeId: string, headers: Headers): void { if (headers.get('location') !== `/api/v1/nodes/${nodeId}`) invalid(); }
export function trash(value: unknown): Trash {
  const r = object(value); keys(r, ['id', 'instance_id', 'space_id', 'root_node_id', 'original_parent_id', 'name', 'kind', 'deleted_at', 'expires_at', 'revision', 'etag', 'node_count', 'logical_bytes']);
  if (r.kind !== 'file' && r.kind !== 'directory') return invalid();
  const result: Trash = { id: id(r.id), instance_id: id(r.instance_id), space_id: id(r.space_id), root_node_id: id(r.root_node_id), original_parent_id: id(r.original_parent_id), name: name(r.name), kind: r.kind,
    deleted_at: timestamp(r.deleted_at), expires_at: timestamp(r.expires_at), revision: decimal(r.revision, INT64_MAX, 1n), etag: '', node_count: decimal(r.node_count, 1000n, 1n), logical_bytes: decimal(r.logical_bytes, INT64_MAX) };
  result.etag = etag(r.etag, 'trash', [result.id], result.revision);
  if (result.root_node_id === result.original_parent_id || (result.kind === 'file' && result.node_count !== '1') ||
    Date.parse(result.expires_at) - Date.parse(result.deleted_at) !== 30 * 86400000 || result.expires_at.slice(20) !== result.deleted_at.slice(20)) invalid();
  return result;
}
export function trashResponse(value: unknown, headers: Headers): Trash {
  const result = trash(value); if (headers.get('etag') !== result.etag) invalid(); return result;
}
export function boundedPage<T extends { id: string }>(value: unknown, decode: (v: unknown) => T): Page<T> {
  const result = page(value, decode);
  if (result.items.length > 100 || new Set(result.items.map(item => item.id)).size !== result.items.length) invalid();
  return result;
}
export function canEdit(node: Node, space: Space, user: { id: string; role: string }): boolean {
  return node.parent_id !== null && (space.kind === 'private' ? space.owner_id === user.id : user.role === 'administrator' || node.created_by === user.id);
}
