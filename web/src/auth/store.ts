import { ApiClient } from '../api/client.ts';
import { ApiError, CONFLICT, describeFailure, RECONCILE } from '../api/errors.ts';
import { cookieLogin, currentUser, empty, updatedUser, user, validateDisplayName, validateInvite, validateLogin, validatePassword } from './protocol.ts';
import type { CurrentUser } from './protocol.ts';

export interface IdentityState {
  phase: 'checking' | 'anonymous' | 'authenticated' | 'error';
  user: CurrentUser | null;
  binding: string | null;
  busy: boolean;
  notice: string;
}
export class IdentityStore {
  readonly api: ApiClient;
  #state: IdentityState = { phase: 'checking', user: null, binding: null, busy: false, notice: '' };
  #listeners = new Set<() => void>();
  #generation = 0;
  // 登出意图跨 pagehide/pageshow 保留；只有显式核对或登录成功才能重新开放自动复验。
  #blockAutomaticRefresh = false;
  constructor(api: ApiClient) {
    this.api = api;
    api.onUnauthenticated = () => this.invalidate('会话已失效，请重新登录。');
  }
  get state(): Readonly<IdentityState> { return this.#state; }
  subscribe(listener: () => void): () => void { this.#listeners.add(listener); return () => { this.#listeners.delete(listener); }; }
  #publish(change: Partial<IdentityState>): void {
    this.#state = { ...this.#state, ...change };
    for (const listener of this.#listeners) listener();
  }
  invalidate(notice = ''): void {
    this.#generation++;
    this.api.cancelReads();
    this.#publish({ phase: 'anonymous', user: null, binding: null, notice });
  }
  async refresh(): Promise<void> {
    if (this.#state.busy) return;
    await this.#readIdentity();
  }
  async refreshAutomatically(): Promise<void> {
    if (this.#blockAutomaticRefresh) return;
    await this.refresh();
  }
  async #readIdentity(expectedId?: string): Promise<void> {
    const generation = ++this.#generation;
    this.api.cancelReads();
    this.#publish({ phase: 'checking', user: null, binding: null });
    try {
      const result = await this.api.request('/api/v1/users/me', { status: 200, decode: currentUser, identity: false });
      if (generation !== this.#generation) return;
      if (expectedId !== undefined && expectedId !== result.id) throw new ApiError('protocol');
      this.#blockAutomaticRefresh = false;
      this.#publish({ phase: 'authenticated', user: result, binding: `${this.api.origin}|${result.instance_id}|${result.id}` });
    } catch (error) {
      if (generation !== this.#generation) return;
      if (error instanceof ApiError && error.status === 401) this.#publish({ phase: 'anonymous', user: null, binding: null });
      else this.#publish({ phase: 'error', user: null, binding: null, notice: describeFailure(error) });
    }
  }
  async #mutation(action: () => Promise<void>): Promise<void> {
    if (this.#state.busy) throw new ApiError('busy');
    this.#publish({ busy: true, notice: '' });
    try { await action(); } finally { this.#publish({ busy: false }); }
  }
  // 核对不会把未知写入改判为成功，也不会在未知登出后自动恢复受保护视图。
  async #reconcileAnonymous(): Promise<string> {
    try {
      await this.api.request('/api/v1/users/me', { status: 200, decode: currentUser, identity: false });
      return '服务器仍报告有效登录。请明确重新核对身份或再次发起登出。';
    } catch (error) {
      return error instanceof ApiError && error.status === 401 ? '当前请求未获身份认证；这不证明此前的写入未生效。' : '当前身份也未能核实。';
    }
  }
  async login(login: string, password: string): Promise<void> {
    validateLogin(login); validatePassword(password);
    await this.#mutation(async () => {
      this.invalidate();
      const generation = this.#generation;
      try {
        const result = await this.api.request('/api/v1/auth/sessions', { method: 'POST', status: 201,
          body: { login, password, credential_mode: 'cookie' }, decode: cookieLogin, identity: false });
        if (generation !== this.#generation) return;
        await this.#readIdentity(result.id);
      } catch (error) {
        if (generation !== this.#generation) return;
        const notice = describeFailure(error) + (error instanceof ApiError && error.kind === 'unknown' ? ` ${await this.#reconcileAnonymous()}` : '');
        if (generation === this.#generation) this.#publish({ notice });
      }
    });
  }
  async register(login: string, displayName: string, password: string, invite: string): Promise<void> {
    validateLogin(login); validateDisplayName(displayName); validatePassword(password); validateInvite(invite);
    await this.#mutation(async () => {
      this.invalidate();
      const generation = this.#generation;
      try {
        await this.api.request('/api/v1/auth/register', { method: 'POST', status: 201,
          body: { login, display_name: displayName, password, invite_token: invite, credential_mode: 'cookie' }, decode: user, identity: false });
        if (generation !== this.#generation) return;
        this.#publish({ notice: '注册成功，请使用新账户登录。注册不会自动登录。' });
      } catch (error) {
        if (generation !== this.#generation) return;
        this.#publish({ notice: describeFailure(error) + (error instanceof ApiError && error.kind === 'unknown' ? ' 请通过登录核对账户是否已创建，或请管理员核对；不要重复消耗邀请。' : '') });
        if (error instanceof ApiError && error.kind === 'unknown') await this.#reconcileAnonymous();
      }
    });
  }
  async logout(): Promise<void> {
    await this.#mutation(async () => {
      this.#blockAutomaticRefresh = true;
      this.invalidate();
      const generation = this.#generation;
      try {
        await this.api.waitForMutation();
        await this.api.request('/api/v1/auth/session', { method: 'DELETE', status: 204, decode: empty, identity: false });
        if (generation === this.#generation) this.#publish({ notice: '已确认登出。' });
      } catch (error) {
        if (generation !== this.#generation) return;
        const notice = `${describeFailure(error)} 未确认服务端撤销；本地身份已清除。 ${await this.#reconcileAnonymous()}`;
        if (generation === this.#generation) this.#publish({ notice });
      }
    });
  }
  async updateName(name: string): Promise<void> {
    validateDisplayName(name);
    const before = this.#state.user;
    if (!before) throw new ApiError('local');
    const generation = this.#generation;
    await this.#mutation(async () => {
      try {
        const result = await this.api.request('/api/v1/users/me', { method: 'PATCH', status: 200, etag: before.etag,
          body: { display_name: name }, decode: updatedUser });
        if (generation !== this.#generation) return;
        if (result.id !== before.id) throw new ApiError('unknown');
        await this.#readIdentity(before.id);
        this.#publish({ notice: '显示名称已更新。' });
      } catch (error) {
        if (generation !== this.#generation) return;
        if (error instanceof ApiError && (error.status === 412 || error.kind === 'unknown')) {
          await this.#readIdentity(before.id);
          this.#publish({ notice: error.status === 412 ? CONFLICT : RECONCILE });
        } else this.#publish({ notice: describeFailure(error) });
      }
    });
  }
  async changePassword(current: string, next: string): Promise<void> {
    validatePassword(current); validatePassword(next);
    const before = this.#state.user;
    if (!before) throw new ApiError('local');
    await this.#mutation(async () => {
      this.invalidate();
      const generation = this.#generation;
      try {
        await this.api.waitForMutation();
        await this.api.request('/api/v1/users/me/password', { method: 'POST', status: 204, etag: before.etag,
          body: { current_password: current, new_password: next }, decode: empty, identity: false });
        if (generation === this.#generation) this.#publish({ notice: '密码已更新，原会话已失效，请重新登录。' });
      } catch (error) {
        if (generation !== this.#generation) return;
        this.#publish({ notice: `${describeFailure(error)} 请重新登录核对密码与资料状态。` });
        if (error instanceof ApiError && (error.kind === 'unknown' || error.status === 412)) await this.#reconcileAnonymous();
      }
    });
  }
}
