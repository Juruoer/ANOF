import { button, element, field } from '../ui/dom.ts';
import { describeFailure } from '../api/errors.ts';
import { DriveApi } from './api.ts';
import { DriveStore } from './store.ts';
import type { DriveRoute } from './store.ts';
import { canEdit, validateName, validateSearch } from './protocol.ts';
import type { Node, Quota, Space, Trash } from './protocol.ts';
import { DestinationPicker } from './picker.ts';
export class DriveView {
  readonly root = element('section');
  readonly store: DriveStore;
  #picker: DestinationPicker | null = null;
  #actions = element('div');
  #notice = element('p');
  constructor(api: DriveApi) {
    this.store = new DriveStore(api); this.root.className = 'drive';
    this.store.onChange = () => this.render(); void this.store.go({ kind: 'spaces' });
  }
  destroy(): void { this.#picker?.destroy(); this.store.destroy(); this.root.replaceChildren(); }
  #error(error: unknown): void { this.#notice.textContent = describeFailure(error); }
  #go(route: DriveRoute): void { void this.store.go(route); }
  #quota(label: string, quota: Quota): HTMLElement {
    const result = element('div'); result.append(element('h4', label));
    const list = element('dl');
    for (const [name, value] of [['上限', quota.limit], ['已用', quota.used], ['预留', quota.reserved]] as const) list.append(element('dt', name), element('dd', `${value.bytes} 字节 / ${value.nodes} 个节点`));
    result.append(list); return result;
  }
  #space(space: Space): HTMLElement {
    const box = element('article');
    box.append(element('h3', space.kind === 'private' ? '我的空间' : '公共空间'), this.#quota('空间配额', space.quota));
    if (space.my_contribution_quota) box.append(this.#quota('我的公共贡献配额', space.my_contribution_quota));
    box.append(button(space.kind === 'private' ? '打开我的空间' : '打开公共空间', () => this.#go({ kind: 'directory', space: space.id, node: space.root_node_id })),
      button(space.kind === 'private' ? '我的回收站' : '公共回收站', () => this.#go({ kind: 'trash', space: space.id })));
    return box;
  }
  render(): void {
    this.#picker?.destroy(); this.#picker = null;
    const s = this.store;
    this.#notice = element('p', s.notice); this.#notice.setAttribute('role', 'status');
    this.#actions = element('div'); this.#actions.className = 'actions';
    this.root.replaceChildren(element('h2', '文件管理'), this.#notice);
    if (s.loading || s.writing) { this.root.setAttribute('aria-busy', 'true'); this.root.append(element('p', s.writing ? '正在处理，请勿重复提交。' : '正在读取…')); return; }
    this.root.removeAttribute('aria-busy');
    const nav = element('nav'); nav.setAttribute('aria-label', '文件导航');
    nav.append(button('所有空间', () => this.#go({ kind: 'spaces' })), button('刷新列表', () => { void s.refresh(); })); this.root.append(nav);
    if (s.error) { this.root.append(element('p', '当前内容无法读取。可以刷新或返回空间入口。')); return; }
    if (s.route.kind === 'spaces') {
      for (const space of s.spaces) this.root.append(this.#space(space));
      return;
    }
    const route = s.route;
    const space = s.spaces.find(item => item.id === route.space)!;
    nav.append(button('空间根目录', () => this.#go({ kind: 'directory', space: space.id, node: space.root_node_id })), button('回收站', () => this.#go({ kind: 'trash', space: space.id })));
    const quota = element('details'); quota.append(element('summary', '查看空间与贡献配额'), this.#quota('空间配额', space.quota));
    if (space.my_contribution_quota) quota.append(this.#quota('我的公共贡献配额', space.my_contribution_quota));
    this.root.append(quota);
    if (route.kind === 'directory' || route.kind === 'search') {
      const search = element('form'); const query = field(search, '按名称搜索（字面匹配）', 'query'); query.maxLength = 128;
      if (route.kind === 'search') query.value = route.query;
      const submit = element('button', '搜索'); submit.type = 'submit'; search.append(submit);
      search.addEventListener('submit', event => { event.preventDefault(); try { this.#go({ kind: 'search', space: space.id, query: validateSearch(query.value) }); } catch (error) { this.#error(error); } });
      this.root.append(search);
      if (s.directory) {
        const directory = s.directory;
        this.root.append(element('h3', `目录：${directory.name || '空间根目录'}`));
        if (directory.parent_id) nav.append(button('上级目录', () => this.#go({ kind: 'directory', space: space.id, node: directory.parent_id! })));
        this.root.append(button('新建目录', () => this.#form('新建目录', '', async name => {
          await s.mutate(() => s.api.mkdir(directory, name), [() => s.api.children(directory)], '目录已创建。', route);
        })));
      } else this.root.append(element('h3', `搜索结果：${route.kind === 'search' ? route.query : ''}`));
      if (!s.nodes.length) this.root.append(element('p', '没有可显示的文件或目录。'));
      const list = element('ul'); list.className = 'entries';
      for (const node of s.nodes) {
        const row = element('li'); row.append(element('h4', node.name), element('p', node.kind === 'directory' ? '目录' : `文件 · ${node.size} 字节`));
        if (node.kind === 'directory') row.append(button('打开目录', () => this.#go({ kind: 'directory', space: space.id, node: node.id })));
        else {
          const download = element('a', '下载（浏览器处理）'); download.href = `/api/v1/nodes/${node.id}/content`; row.append(download);
          row.append(button('复制文件', () => this.#targetForm('复制文件', node.name, s.spaces, async (name, target) => {
            await s.mutate(() => s.api.copy(node, target, name), [() => s.api.children(target)], '文件已复制。', { kind: 'directory', space: target.space_id, node: target.id });
          })));
        }
        if (route.kind === 'search' && node.parent_id) row.append(button('打开所在目录', () => this.#go({ kind: 'directory', space: space.id, node: node.parent_id! })));
        if (canEdit(node, space, s.api.user)) {
          row.append(button('重命名', () => this.#form(`重命名：${node.name}`, node.name, async name => {
            await s.mutate(() => s.api.update(node, name), [() => s.api.node(node.id)], '名称已更新。', route);
          })), button('移动', () => this.#targetForm(`移动：${node.name}`, node.name, [space], async (name, target) => {
            await s.mutate(() => s.api.update(node, name, target), [() => s.api.node(node.id)], '节点已移动。', { kind: 'directory', space: target.space_id, node: target.id });
          })), button('移入回收站', () => this.#confirm(`移入回收站：${node.name}`, '整个条目将不可见，但仍占用配额；公共混合树可能被服务器拒绝。', '确认移入回收站', async () => {
            await s.mutate(() => s.api.remove(node), [() => s.api.trashList(space.id, undefined, undefined, node.id)], '已移入回收站，仍占用配额。', { kind: 'trash', space: space.id });
          })));
        } else row.append(element('p', '此节点只读；最终权限由服务器核对。'));
        list.append(row);
      }
      this.root.append(list, element('p', '下载由浏览器直接处理；点击链接不代表下载成功。目录操作的最终权限由服务器核对，公共目录可能包含他人的节点。'));
    } else if (route.kind === 'trash') {
      this.root.append(element('h3', '回收站'), element('p', '仅显示当前账户可操作的条目。回收站仍占用配额，永久删除不代表物理空间已回收。'));
      if (!s.trash.length) this.root.append(element('p', '回收站为空。'));
      const list = element('ul'); list.className = 'entries';
      for (const item of s.trash) { const row = element('li'); row.append(element('h4', item.name), element('p', `${item.node_count} 个节点 · ${item.logical_bytes} 字节`), button('查看回收条目', () => this.#go({ kind: 'detail', space: space.id, item: item.id }))); list.append(row); }
      this.root.append(list);
    } else if (s.detail) this.#trashDetail(s.detail, space);
    if (route.kind !== 'detail') {
      const pages = element('nav'); pages.setAttribute('aria-label', '列表分页');
      const prev = button('上一页', () => { void s.previousPage(); }); prev.disabled = s.previous.length === 0;
      const next = button('下一页', () => { void s.nextPage(); }); next.disabled = s.next === null;
      pages.append(button('列表首页', () => { void s.refresh(); }), prev, next, element('span', '每页 100 项；仅保留当前页和最近四个返回游标。')); this.root.append(pages);
    }
    this.root.append(this.#actions);
  }
  #trashDetail(item: Trash, space: Space): void {
    const s = this.store, after: DriveRoute = { kind: 'trash', space: space.id };
    const queries = [() => s.api.trash(item.id, space.id), () => s.api.node(item.root_node_id)];
    this.root.append(element('h3', `回收条目：${item.name}`), element('p', `${item.node_count} 个节点 · ${item.logical_bytes} 字节`), element('p', `删除时间：${item.deleted_at}；到期时间：${item.expires_at}`),
      button('恢复到原位置', () => this.#form(`恢复：${item.name}`, item.name, async name => {
        await s.mutate(() => s.api.restore(item, name), queries, '条目已恢复；可返回空间查看。', after);
      })), button('恢复到指定目录', () => this.#targetForm(`恢复：${item.name}`, item.name, [space], async (name, target) => {
        await s.mutate(() => s.api.restore(item, name, target), queries, '条目已恢复。', { kind: 'directory', space: target.space_id, node: target.id });
      })), button('永久删除', () => this.#confirm(`永久删除：${item.name}`, `将不可恢复地删除此条目的 ${item.node_count} 个节点。逻辑配额释放后，物理回收仍可能等待 GC。`, '确认永久删除', async () => {
        await s.mutate(() => s.api.purge(item), queries, '永久删除已确认；物理回收可能尚未发生。', after);
      })));
  }
  #reset(title: string): void { this.#picker?.destroy(); this.#picker = null; this.#actions.replaceChildren(element('h3', title)); }
  #form(title: string, value: string, action: (name: string) => Promise<void>): void {
    this.#reset(title); const form = element('form'), name = field(form, '名称', 'name'); name.maxLength = 255; name.value = value;
    const send = element('button', '提交'); send.type = 'submit'; form.append(send, button('取消操作', () => { this.#actions.replaceChildren(); }));
    form.addEventListener('submit', event => { event.preventDefault(); if (send.disabled) return; try { validateName(name.value); } catch (error) { this.#error(error); return; } send.disabled = true; void action(name.value).catch(error => this.#error(error)).finally(() => { send.disabled = false; }); });
    this.#actions.append(form); name.focus();
  }
  #targetForm(title: string, value: string, spaces: Space[], action: (name: string, parent: Node) => Promise<void>): void {
    this.#reset(title);
    const form = element('form'), name = field(form, '名称', 'name'); name.maxLength = 255; name.value = value;
    const status = element('p', '尚未选择目标目录。'); status.setAttribute('role', 'status');
    const send = element('button', '提交到所选目录'); send.type = 'submit'; send.disabled = true;
    let selected: Node | null = null;
    this.#picker = new DestinationPicker(this.store.api, spaces, node => { selected = node; send.disabled = node === null; status.textContent = node ? `已选择：${node.name || '空间根目录'}` : '尚未选择目标目录。'; });
    form.append(this.#picker.root, status, send, button('取消操作', () => { this.#picker?.destroy(); this.#actions.replaceChildren(); }));
    form.addEventListener('submit', event => { event.preventDefault(); if (send.disabled || !selected) return; try { validateName(name.value); } catch (error) { this.#error(error); return; } const target = selected; send.disabled = true; void action(name.value, target).catch(error => this.#error(error)); });
    this.#actions.append(form); name.focus();
  }
  #confirm(title: string, warning: string, label: string, action: () => Promise<void>): void {
    this.#reset(title); const form = element('form'); form.append(element('p', warning));
    const check = field(form, '我已核对并确认此操作', 'confirm', 'checkbox');
    const send = element('button', label); send.type = 'submit'; form.append(send, button('取消操作', () => { this.#actions.replaceChildren(); }));
    form.addEventListener('submit', event => { event.preventDefault(); if (!check.checked || send.disabled) return; send.disabled = true; void action().catch(error => this.#error(error)); });
    this.#actions.append(form); check.focus();
  }
}
