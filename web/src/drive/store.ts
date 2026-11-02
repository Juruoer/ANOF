import { ApiError, CONFLICT, describeFailure, RECONCILE } from '../api/errors.ts';
import { invalid } from '../api/decode.ts';
import { DriveApi } from './api.ts';
import type { Node, Space, Trash } from './protocol.ts';
export type DriveRoute = { kind: 'spaces' } | { kind: 'directory'; space: string; node: string } | { kind: 'search'; space: string; query: string } | { kind: 'trash'; space: string } | { kind: 'detail'; space: string; item: string };
export class DriveStore {
  readonly api: DriveApi;
  route: DriveRoute = { kind: 'spaces' };
  spaces: Space[] = [];
  directory: Node | null = null;
  detail: Trash | null = null;
  nodes: Node[] = [];
  trash: Trash[] = [];
  next: string | null = null;
  cursor: string | undefined;
  previous: (string | undefined)[] = [];
  loading = false;
  writing = false;
  notice = '';
  error = false;
  #epoch = 0;
  #alive = true;
  #controller = new AbortController();
  onChange: () => void = () => {};
  constructor(api: DriveApi) { this.api = api; }
  destroy(): void { this.#alive = false; this.#epoch++; this.#controller.abort(); this.spaces = []; this.nodes = []; this.trash = []; this.directory = null; this.detail = null; this.onChange = () => {}; }
  async go(route: DriveRoute): Promise<void> {
    if (this.writing || !this.#alive) return;
    this.previous = []; this.notice = ''; await this.#read(route);
  }
  async refresh(): Promise<void> { if (!this.writing) { this.previous = []; this.notice = ''; await this.#read(this.route); } }
  async nextPage(): Promise<void> {
    if (!this.next || this.loading || this.writing) return;
    this.previous = [...this.previous.slice(-3), this.cursor]; await this.#read(this.route, this.next);
  }
  async previousPage(): Promise<void> {
    if (!this.previous.length || this.loading || this.writing) return;
    await this.#read(this.route, this.previous.pop());
  }
  async #read(route: DriveRoute, cursor?: string): Promise<void> {
    if (!this.#alive) return;
    const epoch = ++this.#epoch;
    this.#controller.abort(); this.#controller = new AbortController();
    const signal = this.#controller.signal;
    this.route = route; this.cursor = cursor; this.loading = true; this.error = false;
    this.directory = null; this.detail = null; this.nodes = []; this.trash = []; this.spaces = []; this.next = null;
    this.onChange();
    try {
      const spaces = await this.api.spaces(signal);
      let directory: Node | null = null, detail: Trash | null = null, nodes: Node[] = [], trash: Trash[] = [], next: string | null = null;
      if (route.kind !== 'spaces') {
        const space = spaces.find(s => s.id === route.space); if (!space) invalid();
        if (route.kind === 'directory') {
          directory = await this.api.node(route.node, signal);
          if (directory.space_id !== route.space || directory.kind !== 'directory' || (directory.parent_id === null && directory.id !== space.root_node_id)) invalid();
          const result = await this.api.children(directory, cursor, signal); nodes = result.items; next = result.next_cursor;
        } else if (route.kind === 'search') {
          const result = await this.api.search(route.space, route.query, cursor, signal); nodes = result.items; next = result.next_cursor;
        } else if (route.kind === 'trash') {
          const result = await this.api.trashList(route.space, cursor, signal); trash = result.items; next = result.next_cursor;
        } else detail = await this.api.trash(route.item, route.space, signal);
      }
      if (epoch !== this.#epoch || !this.#alive) return;
      this.spaces = spaces; this.directory = directory; this.detail = detail; this.nodes = nodes; this.trash = trash; this.next = next;
    } catch (error) {
      if (epoch !== this.#epoch || !this.#alive) return;
      this.error = true; this.notice = describeFailure(error);
    } finally {
      if (epoch === this.#epoch && this.#alive) { this.loading = false; this.onChange(); }
    }
  }
  async mutate(action: () => Promise<unknown>, reconcile: (() => Promise<unknown>)[], success: string, after: DriveRoute): Promise<void> {
    if (this.loading || this.writing || !this.#alive) throw new ApiError('busy');
    this.writing = true; this.notice = ''; this.onChange();
    let notice = success;
    let destination = after;
    try { await action(); }
    catch (error) {
      notice = describeFailure(error);
      if (!(error instanceof ApiError) || error.kind !== 'unknown') destination = this.route;
      if (error instanceof ApiError && (error.kind === 'unknown' || error.status === 412)) {
        notice = error.status === 412 ? CONFLICT : RECONCILE;
        if (error.status === 412) destination = this.route;
        let failed = false;
        for (const query of reconcile.slice(0, 3)) {
          if (!this.#alive) return;
          try { await query(); } catch { failed = true; }
        }
        if (failed) notice += ' 部分核对未得到可见结果；请检查当前列表或重新读取，不能据此判断写入未生效。';
      }
    }
    finally {
      if (this.#alive) {
        this.previous = [];
        await this.#read(destination);
        if (this.#alive) { this.notice = notice + (this.error ? ' 当前视图也未能重新读取。' : ''); this.writing = false; this.onChange(); }
      }
    }
  }
}
