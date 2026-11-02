import { ApiError } from './errors.ts';
import type { Page } from './decode.ts';

export const DEFAULT_PAGE_SIZE = 100;
export function pageQuery(limit = DEFAULT_PAGE_SIZE, cursor?: string): string {
  if (!Number.isInteger(limit) || limit < 1 || limit > 200 || (cursor !== undefined && (!/^[A-Za-z0-9_-]{1,2048}$/u.test(cursor)))) throw new ApiError('local');
  return `limit=${limit}${cursor === undefined ? '' : `&cursor=${encodeURIComponent(cursor)}`}`;
}
// 仅保留最近五页；返回更早页面需显式重新读取，不自动遍历所有游标。
export class PageWindow<T> {
  #pages: Page<T>[] = [];
  get pages(): readonly Page<T>[] { return this.#pages; }
  clear(): void { this.#pages = []; }
  push(page: Page<T>): void {
    if (page.items.length > 200) throw new ApiError('protocol');
    this.#pages = [...this.#pages.slice(-4), page];
  }
}
