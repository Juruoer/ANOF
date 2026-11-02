import assert from 'node:assert/strict';
import { test } from 'node:test';
import { ApiClient } from '../../src/api/client.ts';
import { DriveApi } from '../../src/drive/api.ts';
import { DriveStore } from '../../src/drive/store.ts';
import { node, spaces, trash, createdDirectory, boundedPage, canEdit, validateName, validateSearch } from '../../src/drive/protocol.ts';
import { ID, INSTANCE, profile, json, fault } from './support.mjs';
const uuid = n => `30000000-0000-4000-8000-${String(n).padStart(12, '0')}`;
const quota = { limit: { bytes: '9007199254740993', nodes: '100' }, used: { bytes: '9007199254740994', nodes: '2' }, reserved: { bytes: '0', nodes: '0' }, revision: '1' };
const privateSpace = { id: uuid(1), kind: 'private', owner_id: ID, root_node_id: uuid(2), quota, my_contribution_quota: null };
const publicSpace = { id: uuid(3), kind: 'public', owner_id: null, root_node_id: uuid(4), quota, my_contribution_quota: quota };
const spaceList = { items: [privateSpace, publicSpace] };
const root = { id: uuid(2), space_id: uuid(1), parent_id: null, name: '', kind: 'directory', created_by: ID, created_at: profile.created_at, updated_at: profile.updated_at, etag: `"node-${uuid(2)}-1"` };
const child = { ...root, id: uuid(5), parent_id: root.id, name: '%_literal', etag: `"node-${uuid(5)}-1"` };
const file = { ...child, kind: 'file', size: '1099511627776', sha256: 'a'.repeat(64) };
const entry = { id: uuid(6), instance_id: INSTANCE, space_id: uuid(1), root_node_id: child.id, original_parent_id: root.id, name: child.name, kind: 'directory', deleted_at: '2026-10-27T00:00:00.000001Z', expires_at: '2026-11-26T00:00:00.000001Z', revision: '1', etag: `"trash-${uuid(6)}-1"`, node_count: '2', logical_bytes: '9007199254740993' };
const user = { ...profile, instance_id: INSTANCE, etag: 'unused' };
const reply = value => json(value, 200, { etag: value.etag });
const directoryRoute = { kind: 'directory', space: root.space_id, node: root.id };

test('drive decoders preserve independent quota quantities without rounding', () => {
  assert.equal(spaces(spaceList, ID)[0].quota.used.bytes, '9007199254740994');
  assert.throws(() => spaces(spaceList, INSTANCE));
  assert.throws(() => spaces({ items: [privateSpace, privateSpace] }, ID));
  assert.equal(node(file).size, '1099511627776');
  assert.throws(() => node({ ...file, size: '1099511627777' }));
  assert.throws(() => node({ ...root, name: 'not empty' }));
  assert.throws(() => node({ ...child, etag: root.etag }));
  assert.throws(() => node({ ...child, updated_at: '2026-10-26T00:00:00.000000Z' }));
});
test('mkdir response has header-only ETag; reads require body/header equality', () => {
  const { etag, ...body } = { ...child, updated_at: child.created_at };
  assert.equal(createdDirectory(body, new Headers({ etag, location: `/api/v1/nodes/${child.id}` })).etag, etag);
  assert.throws(() => createdDirectory({ ...body, etag }, new Headers({ etag })));
  assert.throws(() => createdDirectory(body, new Headers({ etag, location: '/wrong' })));
});
test('trash validates instance IDs, exact retention, counts and ETags', () => {
  assert.equal(trash(entry).logical_bytes, '9007199254740993');
  for (const change of [{ node_count: '1001' }, { kind: 'file' }, { expires_at: '2026-11-26T00:00:00.000002Z' }, { original_parent_id: child.id }, { revision: '2' }]) assert.throws(() => trash({ ...entry, ...change }));
});
test('names and literal search have distinct byte and symbol rules', () => {
  assert.equal(validateName('<script>name</script>'.replaceAll('/', '')), '<script>name<script>');
  for (const value of ['', '.', '..', 'a/b', 'a\\b', '\0', '中'.repeat(86)]) assert.throws(() => validateName(value));
  for (const value of ['%_\\/', '.', '..', ' ']) assert.equal(validateSearch(value), value);
  assert.throws(() => validateSearch('中'.repeat(43)));
});
test('pages reject duplicate and excessive entries, permissions never grant private admin access', () => {
  assert.throws(() => boundedPage({ items: [child, child], next_cursor: null }, node));
  assert.throws(() => boundedPage({ items: Array.from({ length: 101 }, (_, i) => ({ ...child, id: uuid(100+i), etag: `"node-${uuid(100+i)}-1"` })), next_cursor: null }, node));
  assert.equal(canEdit(child, privateSpace, { id: INSTANCE, role: 'administrator' }), false);
  assert.equal(canEdit(root, privateSpace, user), false);
  assert.equal(canEdit({ ...child, created_by: INSTANCE }, publicSpace, user), false);
  assert.equal(canEdit({ ...child, created_by: INSTANCE }, publicSpace, { ...user, role: 'administrator' }), true);
});
test('search encodes literal percent/underscore/backslash; directory results remain bound', async () => {
  const calls = [];
  const api = new DriveApi(new ApiClient('https://localhost', async path => { calls.push(path); return json({ items: [child], next_cursor: null }); }), user);
  await api.search(root.space_id, '%_\\/');
  assert.match(calls[0], /q=%25_%5C%2F&limit=100$/);
  await assert.rejects(api.children({ ...root, id: uuid(99) }), e => e.kind === 'protocol');
});
test('copy only admits files and preserves full source tag; response loss does not retry', async () => {
  let calls = 0;
  const api = new DriveApi(new ApiClient('https://localhost', async (_path, init) => { calls++; assert.equal(init.headers.get('if-match'), file.etag); throw Error('fault'); }), user);
  assert.throws(() => api.copy(child, root, 'copy'));
  await assert.rejects(api.copy(file, root, 'copy'), e => e.kind === 'unknown'); assert.equal(calls, 1);
});
function fixture(write) {
  const calls = [];
  const api = new DriveApi(new ApiClient('https://localhost', async (path, init) => {
    calls.push([path, init.method]);
    if (init.method !== 'GET') return write(path, init);
    if (path === '/api/v1/spaces') return json(spaceList);
    if (path.includes('/children')) return json({ items: [child], next_cursor: null });
    return reply(path.endsWith(root.id) ? root : child);
  }), user);
  return { api, calls, store: new DriveStore(api) };
}
test('412 refreshes the source view and requires a new explicit action', async () => {
  const { api, store, calls } = fixture(() => fault(412, 'PRECONDITION_FAILED'));
  await store.go(directoryRoute);
  await store.mutate(() => api.update(child, 'new'), [() => api.node(child.id)], 'success', { kind: 'spaces' });
  assert.deepEqual(store.route, directoryRoute); assert.match(store.notice, /重新决定/);
  assert.equal(calls.filter(([, method]) => method === 'PATCH').length, 1);
});
test('unknown writes reconcile once and never become confirmed from matching reads', async () => {
  const { api, store, calls } = fixture(() => { throw Error('fault'); });
  await store.go(directoryRoute);
  await store.mutate(() => api.update(child, child.name), [() => api.node(child.id)], 'success', directoryRoute);
  assert.match(store.notice, /结果未知/); assert.equal(store.nodes[0].name, child.name);
  assert.equal(calls.filter(([, method]) => method === 'PATCH').length, 1);
});
test('late reads after navigation or identity destruction cannot restore protected data', async () => {
  let resolve;
  const api = { spaces: () => new Promise(done => { resolve = done; }) };
  const store = new DriveStore(api);
  const pending = store.go({ kind: 'spaces' }); store.destroy(); resolve([privateSpace, publicSpace]); await pending;
  assert.deepEqual(store.spaces, []);
});
test('page navigation retains only four return cursors and one current page', async () => {
  let calls = 0;
  const api = { spaces: async () => [privateSpace, publicSpace], node: async () => root,
    children: async () => ({ items: [child], next_cursor: `cursor${++calls}` }) };
  const store = new DriveStore(api); await store.go(directoryRoute);
  for (let i = 0; i < 12; i++) await store.nextPage();
  assert.equal(store.previous.length, 4); assert.equal(store.nodes.length, 1);
  await store.previousPage(); assert.equal(store.previous.length, 3);
  await store.refresh(); assert.equal(store.previous.length, 0); assert.equal(store.cursor, undefined);
});
test('duplicate mutation and navigation are blocked while a write is outstanding', async () => {
  let resolve;
  const { store } = fixture(() => {}); await store.go(directoryRoute);
  const pending = store.mutate(() => new Promise(done => { resolve = done; }), [], 'confirmed', directoryRoute);
  await assert.rejects(store.mutate(async () => {}, [], 'bad', directoryRoute), e => e.kind === 'busy');
  await store.go({ kind: 'spaces' }); assert.deepEqual(store.route, directoryRoute);
  resolve(); await pending; assert.equal(store.writing, false);
});

test('ordinary business rejection keeps the source view without implying deletion', async () => {
  const { api, store } = fixture(() => fault(403, 'FORBIDDEN')); await store.go(directoryRoute);
  await store.mutate(() => api.remove(child), [], 'removed', { kind: 'trash', space: root.space_id });
  assert.deepEqual(store.route, directoryRoute); assert.match(store.notice, /没有执行此操作的权限/);
});
test('unknown purge followed by 404 remains unknown and is never replayed', async () => {
  let writes = 0;
  const api = { spaces: async () => [privateSpace, publicSpace], trashList: async () => ({ items: [], next_cursor: null }) };
  const store = new DriveStore(api); const route = { kind: 'trash', space: root.space_id }; await store.go(route);
  const { ApiError } = await import('../../src/api/errors.ts');
  await store.mutate(async () => { writes++; throw new ApiError('unknown'); }, [async () => { throw new ApiError('rejected', 404, 'NOT_FOUND'); }], 'purged', route);
  assert.equal(writes, 1); assert.match(store.notice, /结果未知/); assert.match(store.notice, /不能据此判断/);
});
test('a stale navigation response cannot replace the new directory', async () => {
  let release; let reads = 0;
  const api = { spaces: () => ++reads === 1 ? new Promise(done => { release = done; }) : Promise.resolve([privateSpace, publicSpace]),
    node: async () => root, children: async () => ({ items: [child], next_cursor: null }) };
  const store = new DriveStore(api); const old = store.go(directoryRoute); await store.go({ kind: 'spaces' });
  release([privateSpace, publicSpace]); await old;
  assert.deepEqual(store.route, { kind: 'spaces' }); assert.deepEqual(store.nodes, []); assert.equal(store.directory, null);
});
test('late writes after identity destruction do not trigger reconciliation or repopulation', async () => {
  let release; let reads = 0;
  const { store } = fixture(() => {}); await store.go(directoryRoute);
  const pending = store.mutate(() => new Promise(done => { release = done; }), [async () => { reads++; }], 'confirmed', directoryRoute);
  store.destroy(); release(); await pending;
  assert.equal(reads, 0); assert.deepEqual(store.spaces, []); assert.deepEqual(store.nodes, []);
});
