import assert from 'node:assert/strict';
import { test } from 'node:test';
import { ApiClient } from '../../src/api/client.ts';
import { ApiError } from '../../src/api/errors.ts';
import { AdminApi } from '../../src/admin/api.ts';
import { AdminStore } from '../../src/admin/store.ts';
import { adminUser, managedQuota, invite, createdInvite, storage, committedUsage, inputQuantity, inputInteger } from '../../src/admin/protocol.ts';
import { ID, INSTANCE, profile, tag, json, fault } from './support.mjs';
const SPACE = '30000000-0000-4000-8000-000000000001';
const OTHER = '30000000-0000-4000-8000-000000000002';
const administrator = { ...profile, role: 'administrator', instance_id: INSTANCE, etag: tag };
const info = { user: profile, private_space_id: SPACE };
const q = { limit: { bytes: '9007199254740993', nodes: '100' }, used: { bytes: '0', nodes: '1' }, reserved: { bytes: '0', nodes: '0' }, revision: '9007199254740993' };
const managed = { space_id: SPACE, user_id: null, quota: q };
const quotaTag = `"quota-space-${SPACE}-${q.revision}"`;
const invitation = { id: OTHER, created_by: ID, created_at: '2026-10-27T00:00:00.000001Z', expires_at: '2026-10-28T00:00:00.000001Z', max_uses: 2, used_count: 0, revoked_at: null };
const secret = `anof_invite_${'A'.repeat(43)}`;
const record = { instance_id: INSTANCE, content_budget_bytes: '9007199254740993', retained_bytes: '9007199254740990', reserved_bytes: '3',
  pending_gc_bytes: '5', pending_gc_objects: '2', deleting_bytes: '2', deleting_objects: '1', quarantined_bytes: '10', quarantined_objects: '1', unavailable_bytes: '20', unavailable_objects: '1',
  used_nodes: '7', reserved_nodes: '3', node_limit: '100000', disk_status: 'UNAVAILABLE', free_bytes: null, safety_margin_bytes: null, sampled_at: null };
const storageTag = `"storage-${INSTANCE}-9"`;
const headers = value => new Headers({ etag: value });
const detail = () => adminUser(info, headers(tag));

test('admin constructor rejects members and disabled identities before networking', () => {
  const client = new ApiClient('https://localhost', () => { throw Error('unexpected fetch'); });
  assert.throws(() => new AdminApi(client, { ...administrator, role: 'member' }), e => e.kind === 'local');
  assert.throws(() => new AdminApi(client, { ...administrator, enabled: false }), e => e.kind === 'local');
});
test('administrator detail remains a user profile plus quota-only private space ID', () => {
  assert.equal(detail().private_space_id, SPACE);
  assert.throws(() => adminUser({ ...info, root_node_id: OTHER }, headers(tag)));
  assert.throws(() => adminUser(info, headers(`"user-${OTHER}-${profile.revision}"`)));
});
test('quota decoders bind scope, both IDs and revision without number coercion', () => {
  assert.equal(managedQuota(managed, headers(quotaTag), { space: SPACE, user: null }).quota.limit.bytes, '9007199254740993');
  const contribution = { ...managed, user_id: ID };
  const contributionTag = `"quota-contribution-${SPACE}-${ID}-${q.revision}"`;
  assert.equal(managedQuota(contribution, headers(contributionTag), { space: SPACE, user: ID }).etag, contributionTag);
  assert.throws(() => managedQuota(contribution, headers(quotaTag), { space: SPACE, user: ID }));
  assert.throws(() => managedQuota(contribution, headers(contributionTag), { space: SPACE, user: OTHER }));
});
test('quantities are canonical decimal strings; invitation integer ranges are explicit', () => {
  assert.equal(inputQuantity('9223372036854775807'), '9223372036854775807');
  for (const value of ['9223372036854775808', '1e3', '01', ' 1', '-1', '', 12]) assert.throws(() => inputQuantity(value));
  assert.equal(inputInteger('10000', 10000), 10000);
  for (const value of ['10001', '0', '1.1', '1e2']) assert.throws(() => inputInteger(value, 10000));
});
test('storage usage adds only retained and reserved, never subsets', () => {
  const value = storage(record, headers(storageTag), INSTANCE);
  assert.equal(committedUsage(value), '9007199254740993');
  assert.equal(value.disk_status, 'UNAVAILABLE'); assert.equal(value.free_bytes, null);
  assert.equal(storage({ ...record, content_budget_bytes: '1' }, headers(storageTag), INSTANCE).content_budget_bytes, '1');
  assert.throws(() => storage(record, headers(storageTag), OTHER));
  assert.throws(() => storage(record, headers(`"storage-${OTHER}-9"`), INSTANCE));
});
test('storage requires all four quarantine/unavailable counters and checks disjoint subsets', () => {
  for (const key of ['quarantined_bytes', 'quarantined_objects', 'unavailable_bytes', 'unavailable_objects']) { const bad = { ...record }; delete bad[key]; assert.throws(() => storage(bad, headers(storageTag), INSTANCE)); }
  for (const change of [{ deleting_bytes: '6' }, { pending_gc_objects: '0' }, { quarantined_bytes: record.retained_bytes }, { unavailable_objects: '0' }, { used_nodes: '99999' }, { node_limit: '100001' }, { free_bytes: '0' }]) assert.throws(() => storage({ ...record, ...change }, headers(storageTag), INSTANCE));
  const disk = { ...record, disk_status: 'AVAILABLE', free_bytes: '0', safety_margin_bytes: '1000', sampled_at: profile.created_at };
  assert.equal(storage(disk, headers(storageTag), INSTANCE).free_bytes, '0');
  assert.throws(() => storage({ ...disk, sampled_at: null }, headers(storageTag), INSTANCE));
});
test('invitation list cannot carry secrets; created secrets and metadata must be valid', () => {
  assert.equal(invite(invitation).used_count, 0);
  assert.throws(() => invite({ ...invitation, token: secret }));
  assert.throws(() => invite({ ...invitation, used_count: 3 }));
  assert.throws(() => invite({ ...invitation, max_uses: '2' }));
  assert.throws(() => invite({ ...invitation, revoked_at: '2026-10-26T00:00:00.000001Z' }));
  assert.equal(createdInvite({ invite: invitation, token: secret }).token, secret);
  assert.throws(() => createdInvite({ invite: invitation, token: 'wrong' }));
  assert.throws(() => createdInvite({ invite: { ...invitation, used_count: 1 }, token: secret }));
});
test('management writes send original strong tags and decimal string limits', async () => {
  const calls = [];
  const api = new AdminApi(new ApiClient('https://localhost', async (path, init) => {
    calls.push([path, init]);
    if (path.includes('/users/')) return json({ ...info, user: { ...profile, enabled: false } }, 200, { etag: tag });
    if (path.includes('/quota')) return json(managed, 200, { etag: quotaTag });
    if (path.endsWith('/storage')) return json(record, 200, { etag: storageTag });
    return new Response(null, { status: 204 });
  }), administrator);
  await api.setEnabled(detail(), false);
  await api.setQuota(managedQuota(managed, headers(quotaTag), { space: SPACE, user: null }), q.limit.bytes, q.limit.nodes);
  await api.setStorage(storage(record, headers(storageTag), INSTANCE), record.content_budget_bytes);
  await api.revoke(OTHER);
  assert.deepEqual(calls.map(([, init]) => init.headers.get('If-Match')), [tag, quotaTag, storageTag, null]);
  assert.equal(JSON.parse(calls[1][1].body).limit.bytes, '9007199254740993');
  assert.deepEqual(JSON.parse(calls[0][1].body), { enabled: false });
  assert.equal(calls[3][1].method, 'DELETE');
});
test('private quota navigation uses admin account metadata, never private node reads', async () => {
  const calls = [];
  const api = new AdminApi(new ApiClient('https://localhost', async path => { calls.push(path); return path.includes('/users/') ? json(info, 200, { etag: tag }) : json(managed, 200, { etag: quotaTag }); }), administrator);
  const store = new AdminStore(api); await store.go({ kind: 'privateQuota', user: ID });
  assert.deepEqual(calls, [`/api/v1/admin/users/${ID}`, `/api/v1/admin/spaces/${SPACE}/quota`]);
  assert.equal(store.quota.space_id, SPACE); store.destroy();
});
function inviteFixture(create = async () => ({ invite: invitation, token: secret })) {
  let reads = 0;
  const api = { invites: async () => { reads++; return { items: [invitation], next_cursor: null }; }, createInvite: create };
  return { store: new AdminStore(api), reads: () => reads };
}
test('new invitation secret is private, consumed once, and cleared by refresh', async () => {
  const { store } = inviteFixture(); await store.go({ kind: 'invites' }); await store.createInvite(2, 86400);
  assert.equal(store.hasSecret, true); assert.equal(JSON.stringify(store).includes(secret), false);
  assert.equal(store.takeSecret(), secret); assert.equal(store.takeSecret(), '');
  await store.refresh(); assert.equal(store.secretId, null); assert.equal(store.hasSecret, false); store.destroy();
});
test('secret expires after two minutes even without navigation', async t => {
  t.mock.timers.enable({ apis: ['setTimeout'] });
  const { store } = inviteFixture(); await store.go({ kind: 'invites' }); await store.createInvite(2, 86400);
  assert.equal(store.hasSecret, true); t.mock.timers.tick(120001);
  assert.equal(store.hasSecret, false); assert.equal(store.secretId, null); store.destroy();
});
test('late secret delivery after hiding or identity destruction is discarded', async () => {
  for (const destroy of [false, true]) {
    let release;
    const { store } = inviteFixture(() => new Promise(done => { release = done; }));
    await store.go({ kind: 'invites' }); const pending = store.createInvite(2, 86400);
    if (destroy) store.destroy(); else store.clearSecret();
    release({ invite: invitation, token: secret }); await pending;
    assert.equal(store.hasSecret, false); assert.equal(store.takeSecret(), ''); store.destroy();
  }
});
test('unknown invitation creation queries metadata and never recreates or invents a secret', async () => {
  let writes = 0; const { store, reads } = inviteFixture(async () => { writes++; throw new ApiError('unknown'); });
  await store.go({ kind: 'invites' }); await store.createInvite(2, 86400);
  assert.equal(writes, 1); assert.equal(reads(), 2); assert.equal(store.hasSecret, false); assert.match(store.notice, /无法再次取回秘密/); store.destroy();
});
test('412 refreshes user detail and never automatically resubmits the new version', async () => {
  let reads = 0, writes = 0;
  const api = { user: async () => { reads++; return detail(); }, setEnabled: async () => { writes++; throw new ApiError('rejected', 412, 'PRECONDITION_FAILED'); } };
  const store = new AdminStore(api); await store.go({ kind: 'user', user: ID });
  await store.run(() => api.setEnabled(store.detail, false), 'success');
  assert.equal(reads, 2); assert.equal(writes, 1); assert.match(store.notice, /重新决定/); store.destroy();
});
test('unknown storage update stays unknown after reading back the desired value', async () => {
  let reads = 0, writes = 0; const value = storage(record, headers(storageTag), INSTANCE);
  const api = { storage: async () => { reads++; return value; } }; const store = new AdminStore(api);
  await store.go({ kind: 'storage' });
  await store.run(async () => { writes++; throw new ApiError('unknown'); }, 'success');
  assert.equal(reads, 2); assert.equal(writes, 1); assert.match(store.notice, /结果未知/); store.destroy();
});
test('admin lists replace pages, retain four return cursors and reject duplicate writes', async () => {
  let reads = 0, release;
  const store = new AdminStore({ users: async () => ({ items: [profile], next_cursor: `cursor${++reads}` }) });
  await store.go({ kind: 'users' }); for (let i = 0; i < 10; i++) await store.nextPage();
  assert.equal(store.previous.length, 4); assert.equal(store.users.length, 1);
  const pending = store.run(() => new Promise(done => { release = done; }), 'success');
  await assert.rejects(store.run(async () => {}, 'bad'), e => e.kind === 'busy');
  release(); await pending; store.destroy();
});
test('a late admin read cannot repopulate state after logout', async () => {
  let release; const store = new AdminStore({ users: () => new Promise(done => { release = done; }) });
  const pending = store.go({ kind: 'users' }); store.destroy(); release({ items: [profile], next_cursor: null }); await pending;
  assert.deepEqual(store.users, []); assert.equal(store.detail, null);
});
