import { button, element } from '../ui/dom.ts';
import { describeFailure } from '../api/errors.ts';
import type { DriveApi } from './api.ts';
import type { Node, Space } from './protocol.ts';
// 单页目标目录浏览器；读取消仅释放本地等待，不与文件写入的事务结果混同。
export class DestinationPicker {
  readonly root = element('fieldset');
  #api: DriveApi;
  #spaces: Space[];
  #select: (node: Node | null) => void;
  #epoch = 0;
  #controller = new AbortController();
  constructor(api: DriveApi, spaces: Space[], selected: (node: Node | null) => void) {
    this.#api = api; this.#spaces = spaces; this.#select = selected; this.showSpaces();
  }
  destroy(): void { this.#epoch++; this.#controller.abort(); this.#select(null); }
  showSpaces(): void {
    this.#epoch++; this.#controller.abort(); this.#select(null);
    this.root.replaceChildren(element('legend', '选择目标目录'));
    for (const space of this.#spaces) this.root.append(button(space.kind === 'private' ? '目标：我的空间' : '目标：公共空间', () => { void this.open(space.root_node_id); }));
  }
  async open(id: string, cursor?: string): Promise<void> {
    const epoch = ++this.#epoch;
    this.#controller.abort(); this.#controller = new AbortController(); this.#select(null);
    this.root.replaceChildren(element('legend', '选择目标目录'), element('p', '正在读取目标目录…'));
    try {
      const parent = await this.#api.node(id, this.#controller.signal);
      if (parent.kind !== 'directory' || !this.#spaces.some(s => s.id === parent.space_id)) throw Error('Invalid destination');
      const page = await this.#api.children(parent, cursor, this.#controller.signal);
      if (epoch !== this.#epoch) return;
      this.root.replaceChildren(element('legend', '选择目标目录'), element('p', `当前目标：${parent.name || '空间根目录'}`));
      this.root.append(button('返回目标空间选择', () => this.showSpaces()));
      if (parent.parent_id) this.root.append(button('目标上级目录', () => { void this.open(parent.parent_id!); }));
      this.root.append(button('选用此目录', () => { this.#select(parent); }));
      const list = element('ul');
      for (const item of page.items) if (item.kind === 'directory') { const li = element('li'); li.append(button(`进入目标目录：${item.name}`, () => { void this.open(item.id); })); list.append(li); }
      this.root.append(list);
      if (!list.children.length) this.root.append(element('p', '本页没有子目录。'));
      if (cursor) this.root.append(button('目标列表首页', () => { void this.open(id); }));
      if (page.next_cursor) this.root.append(button('目标列表下一页', () => { void this.open(id, page.next_cursor!); }));
    } catch (error) {
      if (epoch !== this.#epoch) return;
      this.root.replaceChildren(element('legend', '选择目标目录'), element('p', describeFailure(error)), button('重新选择目标空间', () => this.showSpaces()));
    }
  }
}
