import { bool, decimal, etag, id, INT64_MAX, invalid, keys, object, text, timestamp } from '../api/decode.ts';
import { ApiError } from '../api/errors.ts';

export interface User {
  id: string; login: string; display_name: string; role: 'member' | 'administrator';
  enabled: boolean; revision: string; created_at: string; updated_at: string;
}
export interface CurrentUser extends User { instance_id: string; etag: string }
const fields = ['id', 'login', 'display_name', 'role', 'enabled', 'revision', 'created_at', 'updated_at'];
function decodeFields(raw: Record<string, unknown>): User {
  const login = text(raw.login, 32, 3);
  const display = text(raw.display_name, 128, 1);
  if (!/^[a-z][a-z0-9_-]{2,31}$/u.test(login) || /\p{Cc}/u.test(display) || !['member', 'administrator'].includes(String(raw.role))) return invalid();
  const created = timestamp(raw.created_at), updated = timestamp(raw.updated_at);
  if (updated < created) return invalid();
  return { id: id(raw.id), login, display_name: display, role: raw.role as User['role'],
    enabled: bool(raw.enabled), revision: decimal(raw.revision, INT64_MAX, 1n), created_at: created, updated_at: updated };
}
export function user(value: unknown): User {
  const raw = object(value); keys(raw, fields); return decodeFields(raw);
}
export function currentUser(value: unknown, headers: Headers): CurrentUser {
  const raw = object(value); keys(raw, [...fields, 'instance_id']);
  const result = decodeFields(raw);
  if (!result.enabled) return invalid();
  return { ...result, instance_id: id(raw.instance_id), etag: etag(headers.get('etag'), 'user', [result.id], result.revision) };
}
export function cookieLogin(value: unknown): User {
  const raw = object(value); keys(raw, ['user', 'expires_at', 'credential_mode']);
  if (raw.credential_mode !== 'cookie') return invalid();
  timestamp(raw.expires_at);
  const result = user(raw.user);
  if (!result.enabled) return invalid();
  return result;
}
export function updatedUser(value: unknown, headers: Headers): User {
  const result = user(value); etag(headers.get('etag'), 'user', [result.id], result.revision); return result;
}
export const empty = (value: unknown): void => { if (value !== undefined) invalid(); };

export function validateLogin(login: string): void {
  if (!/^[A-Za-z][A-Za-z0-9_-]{2,31}$/u.test(login)) throw new ApiError('local');
}
export function validateDisplayName(name: string): void {
  try { text(name, 128, 1); if (/\p{Cc}/u.test(name)) invalid(); } catch { throw new ApiError('local'); }
}
export function validatePassword(password: string): void {
  try { text(password, 1024, 12); if (password.includes('\0')) invalid(); } catch { throw new ApiError('local'); }
}
export function validateInvite(invite: string): void {
  if (!/^anof_invite_[A-Za-z0-9_-]{42}[AEIMQUYcgkosw048]$/u.test(invite)) throw new ApiError('local');
}
