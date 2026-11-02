import { ApiClient } from '../api/client.ts';
import { id, invalid } from '../api/decode.ts';
import { ApiError } from '../api/errors.ts';
import { pageQuery } from '../api/pagination.ts';
import { empty, user } from '../auth/protocol.ts';
import type { CurrentUser } from '../auth/protocol.ts';
import { DriveApi } from '../drive/api.ts';
import { boundedPage } from '../drive/protocol.ts';
import { adminUser, createdInvite, inputQuantity, invite, managedQuota, storage } from './protocol.ts';
import type { AdminUser, ManagedQuota, QuotaTarget, Storage } from './protocol.ts';
export class AdminApi {
  readonly client: ApiClient;
  readonly identity: CurrentUser;
  constructor(client: ApiClient, identity: CurrentUser) {
    if (identity.role !== 'administrator' || !identity.enabled) throw new ApiError('local');
    this.client = client; this.identity = identity;
  }
  users(cursor?: string, signal?: AbortSignal) { return this.client.request(`/api/v1/admin/users?${pageQuery(100, cursor)}`, { status: 200, ...(signal ? { signal } : {}), decode: v => boundedPage(v, user) }); }
  user(userId: string, signal?: AbortSignal) {
    return this.client.request(`/api/v1/admin/users/${id(userId)}`, { status: 200, ...(signal ? { signal } : {}), decode: (v, h) => {
      const result = adminUser(v, h); if (result.user.id !== userId) invalid(); return result;
    } });
  }
  setEnabled(before: AdminUser, enabled: boolean) {
    if (before.user.role === 'administrator' && !enabled) throw new ApiError('local');
    return this.client.request(`/api/v1/admin/users/${id(before.user.id)}`, { method: 'PATCH', status: 200, etag: before.etag, body: { enabled }, decode: (v, h) => {
      const result = adminUser(v, h);
      if (result.user.id !== before.user.id || result.private_space_id !== before.private_space_id || result.user.role !== before.user.role || result.user.login !== before.user.login || result.user.enabled !== enabled) invalid();
      return result;
    } });
  }
  async publicSpace(signal?: AbortSignal): Promise<string> {
    const spaces = await new DriveApi(this.client, this.identity).spaces(signal);
    const found = spaces.find(space => space.kind === 'public'); if (!found) return invalid(); return found.id;
  }
  #quotaPath(target: QuotaTarget): string {
    return `/api/v1/admin/spaces/${id(target.space)}${target.user === null ? '' : `/contributions/${id(target.user)}`}/quota`;
  }
  quota(target: QuotaTarget, signal?: AbortSignal) {
    return this.client.request(this.#quotaPath(target), { status: 200, ...(signal ? { signal } : {}), decode: (v, h) => managedQuota(v, h, target) });
  }
  setQuota(before: ManagedQuota, bytes: string, nodes: string) {
    const target = { space: before.space_id, user: before.user_id };
    const limit = { bytes: inputQuantity(bytes), nodes: inputQuantity(nodes) };
    return this.client.request(this.#quotaPath(target), { method: 'PUT', status: 200, etag: before.etag, body: { limit }, decode: (v, h) => {
      const result = managedQuota(v, h, target); if (result.quota.limit.bytes !== bytes || result.quota.limit.nodes !== nodes) invalid(); return result;
    } });
  }
  invites(cursor?: string, signal?: AbortSignal) { return this.client.request(`/api/v1/admin/invites?${pageQuery(100, cursor)}`, { status: 200, ...(signal ? { signal } : {}), decode: v => boundedPage(v, invite) }); }
  createInvite(maxUses: number, seconds: number) {
    if (!Number.isSafeInteger(maxUses) || maxUses < 1 || maxUses > 10000 || !Number.isSafeInteger(seconds) || seconds < 1 || seconds > 2592000) throw new ApiError('local');
    return this.client.request('/api/v1/admin/invites', { method: 'POST', status: 201, body: { max_uses: maxUses, valid_for_seconds: seconds }, decode: v => {
      const result = createdInvite(v);
      if (result.invite.created_by !== this.identity.id || result.invite.max_uses !== maxUses || Date.parse(result.invite.expires_at) - Date.parse(result.invite.created_at) !== seconds * 1000 || result.invite.expires_at.slice(20) !== result.invite.created_at.slice(20)) invalid();
      return result;
    } });
  }
  revoke(inviteId: string) { return this.client.request(`/api/v1/admin/invites/${id(inviteId)}`, { method: 'DELETE', status: 204, decode: empty }); }
  storage(signal?: AbortSignal) { return this.client.request('/api/v1/admin/storage', { status: 200, ...(signal ? { signal } : {}), decode: (v, h) => storage(v, h, this.identity.instance_id) }); }
  setStorage(before: Storage, bytes: string) {
    const budget = inputQuantity(bytes);
    return this.client.request('/api/v1/admin/storage', { method: 'PUT', status: 200, etag: before.etag, body: { content_budget_bytes: budget }, decode: (v, h) => {
      const result = storage(v, h, this.identity.instance_id); if (result.content_budget_bytes !== budget) invalid(); return result;
    } });
  }
}
