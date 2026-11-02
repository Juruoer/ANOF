import { ApiError } from './errors.ts';

export const UINT64_MAX = 18446744073709551615n;
export const INT64_MAX = 9223372036854775807n;
export function invalid(): never { throw new ApiError('protocol'); }
export function object(value: unknown): Record<string, unknown> {
  if (typeof value !== 'object' || value === null || Array.isArray(value)) return invalid();
  return value as Record<string, unknown>;
}
export function keys(value: Record<string, unknown>, expected: readonly string[]): void {
  if (Object.keys(value).length !== expected.length || expected.some((key) => !Object.hasOwn(value, key))) invalid();
}
export function text(value: unknown, maxBytes = 1024, minBytes = 0): string {
  if (typeof value !== 'string' || value.length > maxBytes || /[\uD800-\uDFFF]/u.test(value)) return invalid();
  const bytes = new TextEncoder().encode(value).byteLength;
  if (bytes < minBytes || bytes > maxBytes) return invalid();
  return value;
}
export function decimal(value: unknown, max = UINT64_MAX, min = 0n): string {
  const raw = text(value, 20, 1);
  if (!/^(0|[1-9][0-9]*)$/u.test(raw)) return invalid();
  const number = BigInt(raw);
  if (number < min || number > max) return invalid();
  return raw;
}
export function id(value: unknown): string {
  const raw = text(value, 36, 36);
  if (!/^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/u.test(raw) || raw === '00000000-0000-0000-0000-000000000000') return invalid();
  return raw;
}
export function timestamp(value: unknown): string {
  const raw = text(value, 27, 27);
  if (!/^(?!0000)[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}\.[0-9]{6}Z$/u.test(raw)) return invalid();
  const date = new Date(raw);
  if (!Number.isFinite(date.getTime()) || date.toISOString() !== `${raw.slice(0, 23)}Z`) return invalid();
  return raw;
}
export function bool(value: unknown): boolean {
  if (typeof value !== 'boolean') return invalid();
  return value;
}
export function etag(value: unknown, kind: string, ids: readonly string[], revision?: string): string {
  const raw = text(value, 160, 1);
  const prefix = `"${kind}-${ids.map(id).join('-')}-`;
  if (!raw.startsWith(prefix) || !raw.endsWith('"')) return invalid();
  const version = decimal(raw.slice(prefix.length, -1), INT64_MAX, 1n);
  if (revision !== undefined && version !== revision) return invalid();
  return raw;
}
export interface Page<T> { items: T[]; next_cursor: string | null }
export function page<T>(value: unknown, decode: (item: unknown) => T): Page<T> {
  const raw = object(value);
  keys(raw, ['items', 'next_cursor']);
  if (!Array.isArray(raw.items) || raw.items.length > 200) return invalid();
  const cursor = raw.next_cursor === null ? null : text(raw.next_cursor, 2048, 1);
  if (cursor !== null && (!/^[A-Za-z0-9_-]+$/u.test(cursor) || raw.items.length === 0)) return invalid();
  return { items: raw.items.map(decode), next_cursor: cursor };
}
