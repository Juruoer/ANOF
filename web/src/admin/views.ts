import { button, element, field } from '../ui/dom.ts';
import { describeFailure } from '../api/errors.ts';
import type { AdminApi } from './api.ts';
import { AdminStore } from './store.ts';
import type { AdminRoute } from './store.ts';
import { committedUsage, inputInteger, inputQuantity } from './protocol.ts';
import type { Quota } from '../drive/protocol.ts';
export class AdminView {
  readonly root = element('section');
  readonly store: AdminStore;
  #notice = element('p');
  #actions = element('div');
  #onHidden = () => { if (document.visibilityState === 'hidden') this.store.clearSecret(); };
  constructor(api: AdminApi) {
    this.store = new AdminStore(api); this.root.className = 'admin';
    document.addEventListener('visibilitychange', this.#onHidden);
    this.store.onChange = () => this.render(); void this.store.go({ kind: 'users' });
  }
  destroy(): void { document.removeEventListener('visibilitychange', this.#onHidden); this.store.destroy(); this.root.replaceChildren(); }
  #error(error: unknown): void { this.#notice.textContent = describeFailure(error); }
  #go(route: AdminRoute): void { void this.store.go(route); }
  #facts(values: [string, string][]): HTMLElement {
    const list = element('dl'); for (const [label, value] of values) list.append(element('dt', label), element('dd', value)); return list;
  }
  #quota(value: Quota): HTMLElement {
    return this.#facts([['上限', `${value.limit.bytes} 字节 / ${value.limit.nodes} 个节点`], ['已用', `${value.used.bytes} 字节 / ${value.used.nodes} 个节点`], ['预留', `${value.reserved.bytes} 字节 / ${value.reserved.nodes} 个节点`]]);
  }
  render(): void {
    const s = this.store;
    this.#notice = element('p', s.notice); this.#notice.setAttribute('role', 'status');
    this.#actions = element('div'); this.#actions.className = 'actions';
    this.root.replaceChildren(element('h2', '管理员管理'), this.#notice);
    if (s.loading || s.writing) { this.root.setAttribute('aria-busy', 'true'); this.root.append(element('p', s.writing ? '正在处理，请勿重复提交。' : '正在读取…')); return; }
    this.root.removeAttribute('aria-busy');
    const nav = element('nav'); nav.setAttribute('aria-label', '管理导航');
    for (const [label, kind] of [['用户列表', 'users'], ['邀请管理', 'invites'], ['公共空间配额', 'publicQuota'], ['存储预算', 'storage']] as const) nav.append(button(label, () => this.#go({ kind })));
    nav.append(button('刷新管理资料', () => { void s.refresh(); })); this.root.append(nav);
    if (s.hasSecret) this.#handover();
    if (s.error) { this.root.append(element('p', '管理资料读取失败。请刷新或重新选择管理入口。')); return; }
    if (s.route.kind === 'users') {
      this.root.append(element('h3', '用户列表'), element('p', '管理账户和配额不赋予访问其他用户私有文件的权限。'));
      if (!s.users.length) this.root.append(element('p', '本页没有用户。'));
      const list = element('ul'); list.className = 'entries';
      for (const user of s.users) {
        const row = element('li'); row.append(element('h4', user.login), element('p', user.display_name), element('p', `${user.role === 'administrator' ? '管理员' : '成员'} · ${user.enabled ? '已启用' : '已禁用'}`), button('查看用户', () => this.#go({ kind: 'user', user: user.id }))); list.append(row);
      }
      this.root.append(list);
    } else if (s.route.kind === 'user' && s.detail) {
      const before = s.detail, user = before.user;
      this.root.append(element('h3', `用户：${user.login}`), this.#facts([['显示名称', user.display_name], ['角色', user.role === 'administrator' ? '管理员' : '成员'], ['状态', user.enabled ? '已启用' : '已禁用'], ['注册时间', user.created_at], ['更新时间', user.updated_at]]));
      if (user.role !== 'administrator') {
        const enabled = !user.enabled, label = enabled ? '启用用户' : '禁用用户';
        this.root.append(button(label, () => this.#confirm(`${label}：${user.login}`, enabled ? '重新启用不会恢复旧会话，用户需要重新登录。' : '禁用将使该用户的现有会话失效，但不删除文件或返还配额。', label, () => s.run(() => s.api.setEnabled(before, enabled), enabled ? '用户已启用；旧会话不会恢复。' : '用户已禁用，现有会话已失效。'))));
      } else this.root.append(element('p', '唯一管理员不能禁用。'));
      this.root.append(button('管理私有空间配额', () => this.#go({ kind: 'privateQuota', user: user.id })), button('管理公共贡献配额', () => this.#go({ kind: 'contributionQuota', user: user.id })));
    } else if (s.quota) {
      const before = s.quota;
      const label = s.route.kind === 'publicQuota' ? '公共空间配额' : `${s.detail!.user.login} · ${s.route.kind === 'privateQuota' ? '私有空间配额' : '公共贡献配额'}`;
      this.root.append(element('h3', label), this.#quota(before.quota), element('p', '提交同时替换字节和节点上限，不修改已用或预留。上限不能低于服务器提交时的已用加预留。'));
      if (s.detail) this.root.append(button('返回用户详情', () => this.#go({ kind: 'user', user: s.detail!.user.id })));
      this.#limitsForm('保存配额', [['字节上限', before.quota.limit.bytes], ['节点上限', before.quota.limit.nodes]], values => s.run(() => s.api.setQuota(before, values[0]!, values[1]!), '配额上限已更新。'));
    } else if (s.route.kind === 'invites') this.#invites();
    else if (s.storage) {
      const before = s.storage;
      this.root.append(element('h3', '实例存储预算'), this.#facts([
        ['内容预算', `${before.content_budget_bytes} 字节`], ['已计费物理内容 P', `${before.retained_bytes} 字节`], ['预留 R', `${before.reserved_bytes} 字节`], ['P + R', `${committedUsage(before)} 字节`],
        ['待回收（P 的子集）', `${before.pending_gc_bytes} 字节 / ${before.pending_gc_objects} 个对象`], ['正在删除（待回收的子集）', `${before.deleting_bytes} 字节 / ${before.deleting_objects} 个对象`],
        ['隔离（P 的子集）', `${before.quarantined_bytes} 字节 / ${before.quarantined_objects} 个对象`], ['不可用（P 的子集）', `${before.unavailable_bytes} 字节 / ${before.unavailable_objects} 个对象`],
        ['实例节点', `已用 ${before.used_nodes} / 预留 ${before.reserved_nodes} / 上限 ${before.node_limit}`],
        ['磁盘采样', before.disk_status === 'AVAILABLE' ? `可用 ${before.free_bytes} 字节；安全余量 ${before.safety_margin_bytes} 字节；采样于 ${before.sampled_at}` : '暂不可用（不是零剩余空间）'],
      ]), element('p', '子集统计不能再加到 P 上，也不代表即时可释放容量。预算版本不绑定这些派生统计；提交时由服务器重新核对。'));
      this.#limitsForm('保存内容预算', [['内容预算（字节）', before.content_budget_bytes]], values => s.run(() => s.api.setStorage(before, values[0]!), '内容预算已更新。'));
    }
    if (s.route.kind === 'users' || s.route.kind === 'invites') {
      const pages = element('nav'); pages.setAttribute('aria-label', '管理分页');
      const prev = button('上一页', () => { void s.previousPage(); }); prev.disabled = !s.previous.length;
      const next = button('下一页', () => { void s.nextPage(); }); next.disabled = !s.next;
      pages.append(button('列表首页', () => { void s.refresh(); }), prev, next, element('span', '每页 100 项；仅保留当前页和最近四个返回游标。')); this.root.append(pages);
    }
    this.root.append(this.#actions);
  }
  #handover(): void {
    const box = element('aside'); box.className = 'handover';
    box.append(element('h3', '一次性邀请交接'), element('p', `邀请 ID：${this.store.secretId}`), element('p', '仅在本次页面中展示一次；两分钟、隐藏页面或离开后清除。请通过你选择的安全方式交给接收者。'));
    box.append(button('显示一次性邀请码', () => {
      const secret = this.store.takeSecret(); if (!secret) return;
      const label = element('label', '一次性邀请码');
      const output = element('input'); output.type = 'text'; output.readOnly = true; output.autocomplete = 'off'; output.spellcheck = false; output.value = secret;
      label.append(output); box.replaceChildren(element('h3', '一次性邀请交接'), label, button('已完成交接，清除秘密', () => this.store.clearSecret())); output.focus(); output.select();
    }), button('放弃展示并清除', () => this.store.clearSecret()));
    this.root.append(box);
  }
  #invites(): void {
    const s = this.store;
    this.root.append(element('h3', '邀请管理'), element('p', '列表只含邀请元信息，不能再次取回秘密。撤销不会删除已经注册的账户。'));
    const form = element('form');
    const count = field(form, '最多使用次数（1–10000）', 'max_uses'); count.inputMode = 'numeric'; count.maxLength = 5; count.value = '1';
    const expiry = field(form, '有效期秒数（1–2592000）', 'valid_for_seconds'); expiry.inputMode = 'numeric'; expiry.maxLength = 7; expiry.value = '604800';
    const send = element('button', '创建邀请'); send.type = 'submit'; form.append(send);
    form.addEventListener('submit', event => {
      event.preventDefault(); if (send.disabled) return;
      try {
        const max = inputInteger(count.value, 10000), seconds = inputInteger(expiry.value, 2592000);
        send.disabled = true; void s.createInvite(max, seconds).catch(error => this.#error(error)).finally(() => { send.disabled = false; });
      } catch (error) { this.#error(error); }
    }); this.root.append(form);
    if (!s.invites.length) this.root.append(element('p', '本页没有邀请。'));
    const list = element('ul'); list.className = 'entries';
    for (const invite of s.invites) {
      const row = element('li'); row.append(element('h4', `邀请 ${invite.id}`), this.#facts([['使用次数', `${invite.used_count} / ${invite.max_uses}`], ['创建时间', invite.created_at], ['到期时间', invite.expires_at], ['撤销时间', invite.revoked_at ?? '未撤销']]));
      if (invite.revoked_at === null) row.append(button('撤销邀请', () => this.#confirm(`撤销邀请 ${invite.id}`, '撤销后此邀请不能再用于注册。', '撤销邀请', () => s.run(() => s.api.revoke(invite.id), '邀请撤销已确认。'))));
      list.append(row);
    }
    this.root.append(list);
  }
  #limitsForm(label: string, fields: [string, string][], action: (values: string[]) => Promise<void>): void {
    const form = element('form');
    const inputs = fields.map(([label, value], index) => { const input = field(form, label, `limit_${index}`); input.inputMode = 'numeric'; input.maxLength = 19; input.value = value; return input; });
    const send = element('button', label); send.type = 'submit'; form.append(send);
    form.addEventListener('submit', event => {
      event.preventDefault(); if (send.disabled) return;
      try { const values = inputs.map(input => inputQuantity(input.value)); send.disabled = true; void action(values).catch(error => this.#error(error)).finally(() => { send.disabled = false; }); }
      catch (error) { this.#error(error); }
    }); this.root.append(form);
  }
  #confirm(title: string, warning: string, label: string, action: () => Promise<void>): void {
    this.store.clearSecret(false);
    this.root.querySelector('.handover')?.remove();
    const form = element('form'); form.append(element('p', warning));
    const check = field(form, '我已核对并确认此操作', 'confirm', 'checkbox');
    const send = element('button', `确认${label}`); send.type = 'submit';
    form.append(send, button('取消操作', () => this.#actions.replaceChildren()));
    form.addEventListener('submit', event => { event.preventDefault(); if (send.disabled || !check.checked) return; send.disabled = true; void action().catch(error => this.#error(error)); });
    this.#actions.replaceChildren(element('h3', title), form); check.focus();
  }
}
