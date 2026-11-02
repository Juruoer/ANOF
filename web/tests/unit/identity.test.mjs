import assert from 'node:assert/strict';
import { test } from 'node:test';
import { ApiClient } from '../../src/api/client.ts';
import { IdentityStore } from '../../src/auth/store.ts';
import { ID, INSTANCE, profile, tag, json, fault, me } from './support.mjs';

const PASSWORD = 'unit-only-long-password';
const loginResult = () => json({ user: profile, credential_mode: 'cookie', expires_at: '2026-11-03T00:00:00.000000Z' }, 201);

test('bootstrap binds only a verified me result to origin, instance and user', async () => {
  const paths = [];
  const store = new IdentityStore(new ApiClient('https://localhost', async (path) => { paths.push(path); return me(); }));
  await store.refresh();
  assert.equal(store.state.binding, `https://localhost|${INSTANCE}|${ID}`);
  assert.deepEqual(paths, ['/api/v1/users/me']);
  store.invalidate(); assert.equal(store.state.user, null); assert.equal(store.state.binding, null);
});
test('login obtains instance identity through me; no bearer token or credential persistence', async () => {
  const calls = [];
  const store = new IdentityStore(new ApiClient('https://localhost', async (path, init) => {
    calls.push(path);
    if (path.endsWith('/sessions')) { assert.equal(JSON.parse(init.body).credential_mode, 'cookie'); return loginResult(); }
    return me();
  }));
  await store.login('alice', PASSWORD);
  assert.deepEqual(calls, ['/api/v1/auth/sessions', '/api/v1/users/me']);
  assert.equal(store.state.phase, 'authenticated');
  assert.equal(JSON.stringify(store.state).includes(PASSWORD), false);
});
test('registration does not login or reuse the submitted secret in state', async () => {
  let calls = 0;
  const store = new IdentityStore(new ApiClient('https://localhost', async (path, init) => {
    calls++; assert.equal(path, '/api/v1/auth/register');
    assert.equal(JSON.parse(init.body).credential_mode, 'cookie'); return json(profile, 201);
  }));
  await store.register('alice', 'Alice', PASSWORD, `anof_invite_${'A'.repeat(43)}`);
  assert.equal(calls, 1); assert.equal(store.state.phase, 'anonymous'); assert.equal(store.state.user, null);
  assert.match(store.state.notice, /注册成功/u);
});
test('identity switch clears the protected view before fetching the new identity', async () => {
  const resolvers = [];
  const store = new IdentityStore(new ApiClient('https://localhost', () => new Promise((resolve) => resolvers.push(resolve))));
  const first = store.refresh(); resolvers[0](me()); await first;
  const second = store.refresh(); assert.equal(store.state.user, null);
  resolvers[1](me({ ...profile, id: INSTANCE, login: 'bob' })); await second;
  assert.equal(store.state.user.login, 'bob');
  assert.notEqual(store.state.binding, `https://localhost|${INSTANCE}|${ID}`);
});
test('a late bootstrap read cannot restore invalidated identity', async () => {
  let resolve;
  const store = new IdentityStore(new ApiClient('https://localhost', () => new Promise((done) => { resolve = done; })));
  const pending = store.refresh(); store.invalidate(); resolve(me()); await pending;
  assert.equal(store.state.phase, 'anonymous'); assert.equal(store.state.user, null);
});
test('a late login completion cannot restore a locally invalidated identity', async () => {
  let resolve; let calls = 0;
  const store = new IdentityStore(new ApiClient('https://localhost', () => { calls++; return new Promise((done) => { resolve = done; }); }));
  const pending = store.login('alice', PASSWORD); store.invalidate(); resolve(loginResult()); await pending;
  assert.equal(calls, 1); assert.equal(store.state.phase, 'anonymous'); assert.equal(store.state.user, null);
});
test('unknown logout clears memory, queries once, never claims revocation or replays DELETE', async () => {
  const calls = [];
  const store = new IdentityStore(new ApiClient('https://localhost', async (path, init) => {
    calls.push(init.method);
    if (init.method === 'DELETE') throw new Error('lost response');
    return me();
  }));
  await store.refresh(); await store.logout();
  assert.deepEqual(calls, ['GET', 'DELETE', 'GET']);
  assert.equal(store.state.phase, 'anonymous'); assert.equal(store.state.binding, null);
  assert.match(store.state.notice, /结果未知/u); assert.match(store.state.notice, /未确认服务端撤销/u);
});
test('confirmed logout requires 204 and clears protected identity', async () => {
  const store = new IdentityStore(new ApiClient('https://localhost', async (_path, init) => init.method === 'GET' ? me() : new Response(null, { status: 204 })));
  await store.refresh(); await store.logout();
  assert.equal(store.state.user, null); assert.equal(store.state.notice, '已确认登出。');
});
test('profile 412 refreshes but requires a second deliberate action with the new ETag', async () => {
  let patches = 0; let reads = 0; const tags = [];
  const store = new IdentityStore(new ApiClient('https://localhost', async (_path, init) => {
    if (init.method === 'PATCH') { patches++; tags.push(init.headers.get('If-Match')); return fault(412, 'PRECONDITION_FAILED'); }
    return me(reads++ === 0 ? profile : { ...profile, revision: '9007199254740994' });
  }));
  await store.refresh(); await store.updateName('Updated');
  assert.equal(patches, 1); assert.deepEqual(tags, [tag]);
  assert.equal(store.state.user.revision, '9007199254740994'); assert.match(store.state.notice, /重新决定/u);
});
test('unknown profile update reads back without declaring that matching values prove success', async () => {
  let patches = 0;
  const store = new IdentityStore(new ApiClient('https://localhost', async (_path, init) => {
    if (init.method === 'PATCH') { patches++; throw new Error(); }
    return me({ ...profile, display_name: patches ? 'Updated' : 'Alice' });
  }));
  await store.refresh(); await store.updateName('Updated');
  assert.equal(patches, 1); assert.equal(store.state.user.display_name, 'Updated'); assert.match(store.state.notice, /结果未知/u);
});
test('password change sends current ETag, clears local identity even on business rejection', async () => {
  const store = new IdentityStore(new ApiClient('https://localhost', async (path, init) => {
    if (path.endsWith('/password')) { assert.equal(init.headers.get('If-Match'), tag); return fault(401, 'UNAUTHENTICATED'); }
    return me();
  }));
  await store.refresh(); await store.changePassword(PASSWORD, 'new-unit-only-password');
  assert.equal(store.state.phase, 'anonymous'); assert.equal(store.state.user, null);
  assert.match(store.state.notice, /重新登录/u);
});
test('duplicate identity mutations are rejected while the first is outstanding', async () => {
  let resolve;
  const store = new IdentityStore(new ApiClient('https://localhost', () => new Promise((done) => { resolve = done; })));
  const pending = store.logout();
  await assert.rejects(store.login('alice', PASSWORD), (e) => e.kind === 'busy');
  resolve(new Response(null, { status: 204 })); await pending;
  assert.equal(store.state.busy, false);
});

test('unknown logout blocks automatic checks across invalidation until explicit recovery succeeds', async () => {
  const calls = [];
  const store = new IdentityStore(new ApiClient('https://localhost', async (_path, init) => {
    calls.push(init.method);
    if (init.method === 'DELETE') throw new Error('controlled loss');
    return me();
  }));
  await store.refreshAutomatically(); await store.logout();
  const notice = store.state.notice;
  store.invalidate(notice);
  await store.refreshAutomatically(); await store.refreshAutomatically();
  assert.deepEqual(calls, ['GET', 'DELETE', 'GET']);
  assert.equal(store.state.phase, 'anonymous'); assert.equal(store.state.notice, notice);
  await store.refresh();
  assert.equal(store.state.phase, 'authenticated');
  assert.match(store.state.notice, /未确认服务端撤销/u);
  await store.refreshAutomatically();
  assert.deepEqual(calls, ['GET', 'DELETE', 'GET', 'GET', 'GET']);
});

test('failed explicit recovery keeps automatic recovery blocked; explicit login can restore it', async () => {
  let failRead = false; let reads = 0;
  const store = new IdentityStore(new ApiClient('https://localhost', async (path, init) => {
    if (init.method === 'DELETE') throw new Error();
    if (path.endsWith('/sessions')) return loginResult();
    reads++; if (failRead) throw new Error(); return me();
  }));
  await store.logout(); failRead = true; await store.refresh();
  const before = reads; await store.refreshAutomatically(); assert.equal(reads, before);
  failRead = false; await store.login('alice', PASSWORD);
  assert.equal(store.state.phase, 'authenticated');
  await store.refreshAutomatically(); assert.equal(reads, before + 2);
});

test('explicit second logout confirms revocation without automatic replay or recovery', async () => {
  let deletes = 0; let reads = 0;
  const store = new IdentityStore(new ApiClient('https://localhost', async (_path, init) => {
    if (init.method === 'DELETE') { if (++deletes === 1) throw new Error(); return new Response(null, { status: 204 }); }
    reads++; return me();
  }));
  await store.logout(); await store.refreshAutomatically(); await store.logout();
  await store.refreshAutomatically();
  assert.equal(deletes, 2); assert.equal(reads, 1);
  assert.equal(store.state.phase, 'anonymous'); assert.equal(store.state.notice, '已确认登出。');
});

test('automatic revalidation still works normally but a late read cannot undo logout', async () => {
  let finish; let reads = 0;
  const store = new IdentityStore(new ApiClient('https://localhost', async (_path, init) => {
    if (init.method === 'DELETE') throw new Error();
    if (++reads === 2) return new Promise(resolve => { finish = resolve; });
    return me();
  }));
  await store.refreshAutomatically();
  assert.equal(store.state.phase, 'authenticated');
  const pending = store.refreshAutomatically();
  await store.logout(); finish(me()); await pending;
  await store.refreshAutomatically();
  assert.equal(reads, 3); assert.equal(store.state.phase, 'anonymous');
  assert.match(store.state.notice, /未确认服务端撤销/u);
});
