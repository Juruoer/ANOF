import { decimal, etag, id, INT64_MAX, invalid, keys, object, timestamp } from '../api/decode.ts';
import { ApiError } from '../api/errors.ts';
import { user, validateInvite } from '../auth/protocol.ts';
import type { User } from '../auth/protocol.ts';
import { quota } from '../drive/protocol.ts';
import type { Quota } from '../drive/protocol.ts';
export interface AdminUser { user: User; private_space_id: string; etag: string }
export interface QuotaTarget { space: string; user: string | null }
export interface ManagedQuota { space_id: string; user_id: string | null; quota: Quota; etag: string }
export interface Invite { id: string; created_by: string; created_at: string; expires_at: string; max_uses: number; used_count: number; revoked_at: string | null }
export interface CreatedInvite { invite: Invite; token: string }
export const storageFields = ['content_budget_bytes', 'retained_bytes', 'reserved_bytes', 'pending_gc_bytes', 'pending_gc_objects', 'deleting_bytes', 'deleting_objects', 'quarantined_bytes', 'quarantined_objects', 'unavailable_bytes', 'unavailable_objects', 'used_nodes', 'reserved_nodes', 'node_limit'] as const;
export type Storage = Record<typeof storageFields[number], string> & { instance_id: string; etag: string; disk_status: 'AVAILABLE' | 'UNAVAILABLE'; free_bytes: string | null; safety_margin_bytes: string | null; sampled_at: string | null };
export function inputQuantity(value: string): string { try { return decimal(value, INT64_MAX); } catch { throw new ApiError('local'); } }
function integer(value: unknown, min: number, max: number): number {
  if (typeof value !== 'number' || !Number.isSafeInteger(value) || value < min || value > max) return invalid(); return value;
}
export function inputInteger(value: string, max: number): number {
  try { return Number(decimal(value, BigInt(max), 1n)); } catch { throw new ApiError('local'); }
}
export function adminUser(value: unknown, headers: Headers): AdminUser {
  const r = object(value); keys(r, ['user', 'private_space_id']); const profile = user(r.user);
  return { user: profile, private_space_id: id(r.private_space_id), etag: etag(headers.get('etag'), 'user', [profile.id], profile.revision) };
}
export function managedQuota(value: unknown, headers: Headers, target: QuotaTarget): ManagedQuota {
  const r = object(value); keys(r, ['space_id', 'user_id', 'quota']);
  const space = id(r.space_id), owner = r.user_id === null ? null : id(r.user_id), q = quota(r.quota);
  if (space !== target.space || owner !== target.user) invalid();
  return { space_id: space, user_id: owner, quota: q, etag: etag(headers.get('etag'), owner === null ? 'quota-space' : 'quota-contribution', owner === null ? [space] : [space, owner], q.revision) };
}
export function invite(value: unknown): Invite {
  const r = object(value); keys(r, ['id', 'created_by', 'created_at', 'expires_at', 'max_uses', 'used_count', 'revoked_at']);
  const created = timestamp(r.created_at), expires = timestamp(r.expires_at), revoked = r.revoked_at === null ? null : timestamp(r.revoked_at);
  const max = integer(r.max_uses, 1, 10000), used = integer(r.used_count, 0, max);
  if (expires <= created || (revoked !== null && revoked < created)) invalid();
  return { id: id(r.id), created_by: id(r.created_by), created_at: created, expires_at: expires, max_uses: max, used_count: used, revoked_at: revoked };
}
export function createdInvite(value: unknown): CreatedInvite {
  const r = object(value); keys(r, ['invite', 'token']);
  const info = invite(r.invite);
  if (typeof r.token !== 'string') return invalid();
  try { validateInvite(r.token); } catch { return invalid(); }
  if (info.used_count !== 0 || info.revoked_at !== null) invalid();
  return { invite: info, token: r.token };
}
export function storage(value: unknown, headers: Headers, instance: string): Storage {
  const r = object(value); keys(r, ['instance_id', ...storageFields, 'disk_status', 'free_bytes', 'safety_margin_bytes', 'sampled_at']);
  const instanceId = id(r.instance_id); if (instanceId !== instance) invalid();
  const counts = {} as Record<typeof storageFields[number], string>;
  for (const key of storageFields) counts[key] = decimal(r[key], INT64_MAX);
  const n = (key: typeof storageFields[number]) => BigInt(counts[key]);
  if (counts.node_limit !== '100000' || n('used_nodes') + n('reserved_nodes') > 100000n ||
    n('deleting_bytes') > n('pending_gc_bytes') || n('deleting_objects') > n('pending_gc_objects') ||
    n('pending_gc_bytes') + n('quarantined_bytes') + n('unavailable_bytes') > n('retained_bytes') ||
    n('pending_gc_objects') + n('quarantined_objects') + n('unavailable_objects') > INT64_MAX) invalid();
  for (const prefix of ['pending_gc', 'deleting', 'quarantined', 'unavailable'] as const) {
    if (n(`${prefix}_objects`) === 0n && n(`${prefix}_bytes`) !== 0n) invalid();
  }
  if (r.disk_status !== 'AVAILABLE' && r.disk_status !== 'UNAVAILABLE') return invalid();
  if (r.disk_status === 'UNAVAILABLE' && (r.free_bytes !== null || r.safety_margin_bytes !== null || r.sampled_at !== null)) invalid();
  return { ...counts, instance_id: instanceId, etag: etag(headers.get('etag'), 'storage', [instanceId]), disk_status: r.disk_status,
    free_bytes: r.disk_status === 'AVAILABLE' ? decimal(r.free_bytes, INT64_MAX) : null,
    safety_margin_bytes: r.disk_status === 'AVAILABLE' ? decimal(r.safety_margin_bytes, INT64_MAX) : null,
    sampled_at: r.disk_status === 'AVAILABLE' ? timestamp(r.sampled_at) : null };
}
export function committedUsage(value: Storage): string { return (BigInt(value.retained_bytes) + BigInt(value.reserved_bytes)).toString(); }
