import { ApiError, CONFLICT, describeFailure, RECONCILE } from '../api/errors.ts';
import type { User } from '../auth/protocol.ts';
import type { AdminApi } from './api.ts';
import type { AdminUser, Invite, ManagedQuota, Storage } from './protocol.ts';
export type AdminRoute = { kind: 'users' } | { kind: 'invites' } | { kind: 'storage' } | { kind: 'publicQuota' } | { kind: 'user'; user: string } | { kind: 'privateQuota'; user: string } | { kind: 'contributionQuota'; user: string };
export class AdminStore {
  readonly api: AdminApi;
  route: AdminRoute = { kind: 'users' };
  users: User[] = [];
  invites: Invite[] = [];
  detail: AdminUser | null = null;
  quota: ManagedQuota | null = null;
  storage: Storage | null = null;
  next: string | null = null;
  cursor: string | undefined;
  previous: (string | undefined)[] = [];
  loading = false;
  writing = false;
  error = false;
  notice = '';
  onChange: () => void = () => {};
  #epoch = 0;
  #alive = true;
  #controller = new AbortController();
  #secret = '';
  #secretId: string | null = null;
  #secretEpoch = 0;
  #secretTimer: ReturnType<typeof setTimeout> | undefined;
  constructor(api: AdminApi) { this.api = api; }
  get hasSecret(): boolean { return this.#secret !== ''; }
  get secretId(): string | null { return this.#secretId; }
  takeSecret(): string {
    if (!this.#alive || this.loading || this.writing) return '';
    const secret = this.#secret; this.#secret = ''; return secret;
  }
  clearSecret(notify = true): void {
    this.#secretEpoch++; this.#secret = ''; this.#secretId = null;
    clearTimeout(this.#secretTimer); this.#secretTimer = undefined;
    if (notify && this.#alive) this.onChange();
  }
  #clearData(): void { this.users = []; this.invites = []; this.detail = null; this.quota = null; this.storage = null; this.next = null; }
  destroy(): void { this.#alive = false; this.#epoch++; this.#controller.abort(); this.clearSecret(false); this.#clearData(); this.previous = []; this.onChange = () => {}; }
  async go(route: AdminRoute): Promise<void> {
    if (this.writing || !this.#alive) return;
    this.previous = []; this.notice = ''; await this.#read(route);
  }
  async refresh(): Promise<void> { if (!this.writing && this.#alive) { this.previous = []; this.notice = ''; await this.#read(this.route); } }
  async nextPage(): Promise<void> {
    if (!this.next || this.loading || this.writing || !this.#alive) return;
    this.previous = [...this.previous.slice(-3), this.cursor]; await this.#read(this.route, this.next);
  }
  async previousPage(): Promise<void> {
    if (!this.previous.length || this.loading || this.writing || !this.#alive) return;
    await this.#read(this.route, this.previous.pop());
  }
  async #read(route: AdminRoute, cursor?: string, preserveSecret = false): Promise<void> {
    if (!this.#alive) return;
    if (!preserveSecret) this.clearSecret(false);
    const epoch = ++this.#epoch;
    this.#controller.abort(); this.#controller = new AbortController(); const signal = this.#controller.signal;
    this.route = route; this.cursor = cursor; this.loading = true; this.error = false; this.#clearData(); this.onChange();
    try {
      let users: User[] = [], invites: Invite[] = [], detail: AdminUser | null = null, quota: ManagedQuota | null = null, storage: Storage | null = null, next: string | null = null;
      if (route.kind === 'users') { const page = await this.api.users(cursor, signal); users = page.items; next = page.next_cursor; }
      else if (route.kind === 'invites') { const page = await this.api.invites(cursor, signal); invites = page.items; next = page.next_cursor; }
      else if (route.kind === 'storage') storage = await this.api.storage(signal);
      else if (route.kind === 'publicQuota') quota = await this.api.quota({ space: await this.api.publicSpace(signal), user: null }, signal);
      else {
        detail = await this.api.user(route.user, signal);
        if (route.kind === 'privateQuota') quota = await this.api.quota({ space: detail.private_space_id, user: null }, signal);
        if (route.kind === 'contributionQuota') quota = await this.api.quota({ space: await this.api.publicSpace(signal), user: detail.user.id }, signal);
      }
      if (!this.#alive || epoch !== this.#epoch) return;
      this.users = users; this.invites = invites; this.detail = detail; this.quota = quota; this.storage = storage; this.next = next;
    } catch (error) {
      if (!this.#alive || epoch !== this.#epoch) return;
      this.error = true; this.notice = describeFailure(error);
    } finally {
      if (this.#alive && epoch === this.#epoch) { this.loading = false; this.onChange(); }
    }
  }
  async run(action: () => Promise<unknown>, success: string): Promise<void> {
    if (!this.#alive || this.loading || this.writing) throw new ApiError('busy');
    this.clearSecret(false); this.writing = true; this.notice = ''; this.onChange();
    let notice = success;
    try { await action(); }
    catch (error) {
      notice = error instanceof ApiError && error.status === 412 ? CONFLICT : describeFailure(error);
      if (error instanceof ApiError && error.kind === 'unknown' && this.route.kind === 'invites') notice = `${RECONCILE} 请核对邀请列表；创建响应丢失时无法再次取回秘密，不要自动补建邀请。`;
      if (error instanceof ApiError && error.status === 409 && error.code === 'QUOTA_EXCEEDED') notice = '配额上限低于当前已用加预留，服务器已拒绝修改。请核对最新计量后重新决定。';
    } finally {
      if (this.#alive) {
        // 对当前资源重新读取只是核对，不将未知写入改判为成功，也不重新发送写请求。
        await this.#read(this.route, this.cursor, true);
        if (this.#alive) { this.writing = false; this.notice = notice + (this.error ? ' 当前资源也未能重新读取。' : ''); this.onChange(); }
      }
    }
  }
  async createInvite(max: number, seconds: number): Promise<void> {
    await this.run(async () => {
      const epoch = this.#secretEpoch;
      const result = await this.api.createInvite(max, seconds);
      if (!this.#alive || epoch !== this.#secretEpoch) return;
      this.#secret = result.token; this.#secretId = result.invite.id;
      this.#secretTimer = setTimeout(() => this.clearSecret(), 120_000);
    }, '邀请已创建。请明确打开一次性秘密完成交接；离开、隐藏或刷新页面会清除它。');
  }
}
