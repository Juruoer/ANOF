import { ApiClient } from '../api/client.ts';
import { id, invalid } from '../api/decode.ts';
import { pageQuery } from '../api/pagination.ts';
import { empty } from '../auth/protocol.ts';
import type { CurrentUser } from '../auth/protocol.ts';
import { boundedPage, createdDirectory, locationHeader, node, nodeResponse, spaces, trash, trashResponse, validateName, validateSearch } from './protocol.ts';
import type { Node, Trash } from './protocol.ts';
export class DriveApi {
  readonly client: ApiClient;
  readonly user: CurrentUser;
  constructor(client: ApiClient, user: CurrentUser) { this.client = client; this.user = user; }
  spaces(signal?: AbortSignal) { return this.client.request('/api/v1/spaces', { status: 200, decode: value => spaces(value, this.user.id), ...(signal ? { signal } : {}) }); }
  node(nodeId: string, signal?: AbortSignal) { return this.client.request(`/api/v1/nodes/${id(nodeId)}`, { status: 200, decode: (v, h) => { const n = nodeResponse(v, h); if (n.id !== nodeId) invalid(); return n; }, ...(signal ? { signal } : {}) }); }
  children(parent: Node, cursor?: string, signal?: AbortSignal) {
    return this.client.request(`/api/v1/nodes/${id(parent.id)}/children?${pageQuery(100, cursor)}`, { status: 200, ...(signal ? { signal } : {}), decode: value => boundedPage(value, v => {
      const n = node(v); if (n.space_id !== parent.space_id || n.parent_id !== parent.id) invalid(); return n;
    }) });
  }
  search(space: string, query: string, cursor?: string, signal?: AbortSignal) {
    return this.client.request(`/api/v1/search?space_id=${id(space)}&q=${encodeURIComponent(validateSearch(query))}&${pageQuery(100, cursor)}`, { status: 200, ...(signal ? { signal } : {}), decode: value => boundedPage(value, v => {
      const n = node(v); if (n.space_id !== space || n.parent_id === null) invalid(); return n;
    }) });
  }
  trashList(space: string, cursor?: string, signal?: AbortSignal, root?: string) {
    return this.client.request(`/api/v1/trash?space_id=${id(space)}&${pageQuery(100, cursor)}${root ? `&root_node_id=${id(root)}` : ''}`, { status: 200, ...(signal ? { signal } : {}), decode: value => boundedPage(value, v => {
      const t = trash(v); this.checkTrash(t, space); if (root && t.root_node_id !== root) invalid(); return t;
    }) });
  }
  checkTrash(t: Trash, space: string): void { if (t.instance_id !== this.user.instance_id || t.space_id !== space) invalid(); }
  trash(item: string, space: string, signal?: AbortSignal) {
    return this.client.request(`/api/v1/trash/${id(item)}`, { status: 200, ...(signal ? { signal } : {}), decode: (v, h) => { const t = trashResponse(v, h); this.checkTrash(t, space); if (t.id !== item) invalid(); return t; } });
  }
  mkdir(parent: Node, name: string) {
    return this.client.request('/api/v1/nodes', { method: 'POST', status: 201, body: { space_id: parent.space_id, parent_id: parent.id, name: validateName(name) }, decode: (v, h) => {
      const n = createdDirectory(v, h); if (n.space_id !== parent.space_id || n.parent_id !== parent.id || n.name !== name || n.created_by !== this.user.id) invalid(); return n;
    } });
  }
  update(source: Node, name: string, parent?: Node) {
    if (parent && (parent.space_id !== source.space_id || parent.kind !== 'directory')) throw new Error('Invalid destination');
    return this.client.request(`/api/v1/nodes/${source.id}`, { method: 'PATCH', status: 200, etag: source.etag, body: { name: validateName(name), ...(parent ? { parent_id: parent.id } : {}) }, decode: (v, h) => {
      const n = nodeResponse(v, h); if (n.id !== source.id || n.space_id !== source.space_id || n.parent_id !== (parent?.id ?? source.parent_id) || n.name !== name || n.kind !== source.kind || n.created_by !== source.created_by || n.created_at !== source.created_at || n.size !== source.size || n.sha256 !== source.sha256) invalid(); return n;
    } });
  }
  copy(source: Node, parent: Node, name: string) {
    if (source.kind !== 'file' || parent.kind !== 'directory') throw new Error('Invalid copy');
    return this.client.request(`/api/v1/nodes/${source.id}/copy`, { method: 'POST', status: 201, etag: source.etag, body: { space_id: parent.space_id, parent_id: parent.id, name: validateName(name) }, decode: (v, h) => {
      const n = nodeResponse(v, h); locationHeader(n.id, h); if (n.kind !== 'file' || n.id === source.id || n.space_id !== parent.space_id || n.parent_id !== parent.id || n.name !== name || n.size !== source.size || n.sha256 !== source.sha256 || n.created_by !== this.user.id) invalid(); return n;
    } });
  }
  remove(source: Node) {
    return this.client.request(`/api/v1/nodes/${source.id}`, { method: 'DELETE', status: 200, etag: source.etag, decode: (v, h) => {
      const t = trashResponse(v, h); this.checkTrash(t, source.space_id);
      if (t.root_node_id !== source.id || t.name !== source.name || t.kind !== source.kind || t.original_parent_id !== source.parent_id || h.get('location') !== `/api/v1/trash/${t.id}`) invalid(); return t;
    } });
  }
  restore(source: Trash, name?: string, parent?: Node) {
    if (parent && (parent.space_id !== source.space_id || parent.kind !== 'directory')) throw new Error('Invalid destination');
    return this.client.request(`/api/v1/trash/${source.id}/restore`, { method: 'POST', status: 200, etag: source.etag,
      body: { ...(name === undefined ? {} : { name: validateName(name) }), ...(parent ? { parent_id: parent.id } : {}) }, decode: (v, h) => {
        const n = nodeResponse(v, h); locationHeader(n.id, h); if (n.id !== source.root_node_id || n.kind !== source.kind || n.space_id !== source.space_id || n.name !== (name ?? source.name) || n.parent_id !== (parent?.id ?? source.original_parent_id)) invalid(); return n;
      } });
  }
  purge(source: Trash) { return this.client.request(`/api/v1/trash/${source.id}`, { method: 'DELETE', status: 204, etag: source.etag, decode: empty }); }
}
