import assert from 'node:assert/strict';
import { test } from 'node:test';
import { ApiClient, MAX_RESPONSE_BYTES } from '../../src/api/client.ts';
import { ApiError } from '../../src/api/errors.ts';
import { decimal, etag, INT64_MAX, page, timestamp } from '../../src/api/decode.ts';
import { PageWindow, pageQuery } from '../../src/api/pagination.ts';
import { currentUser, cookieLogin, user } from '../../src/auth/protocol.ts';
import { parseRoute } from '../../src/ui/router.ts';

import { ID, INSTANCE, profile, tag, json, fault } from './support.mjs';
const options = { status: 200, decode: (value) => value };
const kind = (expected) => (error) => error instanceof ApiError && error.kind === expected;

test('uint64 and wire int64 quantities never round or accept noncanonical values', () => {
  assert.equal(decimal('18446744073709551615'), '18446744073709551615');
  assert.equal(decimal('9007199254740993'), '9007199254740993');
  for (const value of [1, '01', '-1', '+1', '1e3', '1.0', '18446744073709551616']) assert.throws(() => decimal(value));
  assert.throws(() => decimal('9223372036854775808', INT64_MAX));
});
test('ETags remain fully quoted and bound to ID, kind and revision', () => {
  assert.equal(etag(tag, 'user', [ID], profile.revision), tag);
  for (const value of [tag.slice(1, -1), `W/${tag}`, `${tag}, ${tag}`, '*', `"node-${ID}-1"`]) assert.throws(() => etag(value, 'user', [ID]));
  assert.throws(() => etag(tag, 'user', [ID], '2'));
});
test('identity decoders distinguish user/current-user/Cookie login and reject secret fields', () => {
  assert.equal(user(profile).revision, '9007199254740993');
  assert.throws(() => currentUser(profile, new Headers({ etag: tag })));
  assert.equal(currentUser({ ...profile, instance_id: INSTANCE }, new Headers({ etag: tag })).etag, tag);
  assert.throws(() => currentUser({ ...profile, instance_id: INSTANCE }, new Headers({ etag: '"wrong"' })));
  assert.throws(() => user({ ...profile, instance_id: INSTANCE }));
  assert.throws(() => user({ ...profile, display_name: '\ud800' }));
  assert.throws(() => user({ ...profile, updated_at: '2025-01-01T00:00:00.000000Z' }));
  const login = { user: profile, expires_at: '2026-11-03T00:00:00.000000Z', credential_mode: 'cookie' };
  assert.equal(cookieLogin(login).id, ID);
  assert.throws(() => cookieLogin({ ...login, access_token: 'secret' }));
  assert.throws(() => timestamp('2026-02-30T00:00:00.000000Z'));
});
test('pages and retained navigation have fixed bounds', () => {
  assert.equal(pageQuery(), 'limit=100');
  assert.throws(() => pageQuery(201));
  assert.throws(() => pageQuery(1, 'bad cursor'));
  assert.throws(() => page({ items: Array(201).fill(0), next_cursor: null }, Number));
  assert.throws(() => page({ items: [], next_cursor: 'cursor' }, Number));
  const window = new PageWindow();
  for (let i = 0; i < 9; i++) window.push({ items: [i], next_cursor: null });
  assert.deepEqual(window.pages.map((p) => p.items[0]), [4, 5, 6, 7, 8]);
  window.clear(); assert.equal(window.pages.length, 0);
  assert.equal(parseRoute('#/account'), 'account');
  assert.equal(parseRoute('#/login?password=secret'), null);
});
test('adapter uses relative URLs, Cookie credentials, CSRF marker and verbatim If-Match', async () => {
  let calls = 0;
  const api = new ApiClient('https://localhost', async (path, init) => {
    calls++; assert.equal(path, '/api/v1/users/me');
    assert.equal(init.credentials, 'same-origin');
    assert.equal(init.referrerPolicy, 'same-origin'); assert.equal(init.mode, 'same-origin');
    assert.equal(init.redirect, 'error'); assert.equal(init.cache, 'no-store');
    assert.equal(init.headers.get('X-ANOF-Request'), '1'); assert.equal(init.headers.get('If-Match'), tag);
    assert.equal(init.headers.has('Authorization'), false); assert.equal(init.headers.has('Origin'), false);
    return json(profile);
  });
  await api.request('/api/v1/users/me', { ...options, method: 'PATCH', body: { display_name: 'x' }, etag: tag });
  for (const path of ['https://evil.invalid/api/v1/users/me', '//evil.invalid/api/v1/users/me', '/api/v1/../../secret']) {
    await assert.rejects(api.request(path, options), kind('local'));
  }
  await assert.rejects(api.request('/api/v1/nodes', { ...options, method: 'POST', body: { number: 1n } }), kind('local'));
  await assert.rejects(api.request('/api/v1/nodes', { ...options, method: 'POST', body: { number: 9007199254740992 } }), kind('local'));
  assert.equal(calls, 1);
});
test('512 KiB JSON bound is enforced on streamed bytes, not merely Content-Length', async () => {
  const make = (size) => new Response(new ReadableStream({ start(controller) {
    controller.enqueue(new TextEncoder().encode('"' + 'a'.repeat(size - 2) + '"')); controller.close();
  } }), { headers: { 'Content-Type': 'application/json' } });
  const good = new ApiClient('https://localhost', async () => make(MAX_RESPONSE_BYTES));
  assert.equal((await good.request('/api/v1/spaces', options)).length, MAX_RESPONSE_BYTES - 2);
  const bad = new ApiClient('https://localhost', async () => make(MAX_RESPONSE_BYTES + 1));
  await assert.rejects(bad.request('/api/v1/spaces', options), kind('protocol'));
  await assert.rejects(bad.request('/api/v1/nodes', { ...options, method: 'POST' }), kind('unknown'));
});
test('malformed UTF-8, missing JSON type and incorrect Content-Length fail closed', async () => {
  for (const response of [new Response('<html>error</html>'),
    new Response(new Uint8Array([34, 255, 34]), { headers: { 'Content-Type': 'application/json' } }),
    json({}, 200, { 'Content-Length': '999999' }), json({}, 200, { 'Content-Length': '1' })]) {
    const api = new ApiClient('https://localhost', async () => response);
    await assert.rejects(api.request('/api/v1/users/me', options), kind('protocol'));
  }
});
test('412, session cap and rate limiting remain business rejections with no replay', async () => {
  for (const [status, code] of [[412, 'PRECONDITION_FAILED'], [409, 'SESSION_LIMIT_REACHED'], [429, 'RATE_LIMITED']]) {
    let calls = 0;
    const api = new ApiClient('https://localhost', async () => { calls++; return fault(status, code); });
    await assert.rejects(api.request('/api/v1/users/me', { ...options, method: 'PATCH' }), (e) => e.kind === 'rejected' && e.status === status && e.code === code);
    assert.equal(calls, 1);
  }
});
test('network loss, 5xx, malformed success and explicit unknown commits never replay mutations', async () => {
  for (const make of [() => { throw new Error('network'); }, () => fault(500, 'COMMIT_OUTCOME_UNKNOWN'), () => new Response('bad', { status: 200 })]) {
    let calls = 0;
    const api = new ApiClient('https://localhost', async () => { calls++; return make(); });
    await assert.rejects(api.request('/api/v1/nodes', { ...options, method: 'POST' }), kind('unknown'));
    assert.equal(calls, 1);
  }
});
test('requests have deadlines and distinguish cancellation before/after sending a write', async () => {
  let calls = 0;
  const api = new ApiClient('https://localhost', async (_path, init) => {
    calls++; return new Promise((_resolve, reject) => init.signal.addEventListener('abort', () => reject(new Error('aborted')), { once: true }));
  }, 15);
  const pre = new AbortController(); pre.abort();
  await assert.rejects(api.request('/api/v1/nodes', { ...options, method: 'POST', signal: pre.signal }), kind('cancelled'));
  assert.equal(calls, 0);
  await assert.rejects(api.request('/api/v1/nodes', { ...options, method: 'POST' }), kind('unknown'));
  await assert.rejects(api.request('/api/v1/spaces', options), kind('transport'));
});
test('deadline also bounds stalled response consumption', async () => {
  let cancelled = false;
  const api = new ApiClient('https://localhost', async () => new Response(new ReadableStream({ cancel() { cancelled = true; } }),
    { headers: { 'Content-Type': 'application/json' } }), 15);
  await assert.rejects(api.request('/api/v1/spaces', options), kind('transport'));
  assert.equal(cancelled, true);
});
test('in-flight work and mutation admission are bounded', async () => {
  const resolvers = [];
  const api = new ApiClient('https://localhost', () => new Promise((resolve) => resolvers.push(resolve)));
  const reads = Array.from({ length: 6 }, () => api.request('/api/v1/spaces', options));
  await assert.rejects(api.request('/api/v1/spaces', options), kind('busy'));
  for (const resolve of resolvers.splice(0)) resolve(json({}));
  await Promise.all(reads);
  const write = api.request('/api/v1/nodes', { ...options, method: 'POST' });
  await assert.rejects(api.request('/api/v1/nodes', { ...options, method: 'POST' }), kind('busy'));
  resolvers[0](json({})); await write;
});
test('late reads cannot invalidate or restore a changed identity', async () => {
  let resolve;
  const api = new ApiClient('https://localhost', () => new Promise((done) => { resolve = done; }));
  let invalidations = 0; api.onUnauthenticated = () => invalidations++;
  const pending = api.request('/api/v1/users/me', options);
  api.cancelReads(); resolve(fault(401, 'UNAUTHENTICATED'));
  await assert.rejects(pending, kind('stale'));
  assert.equal(invalidations, 0);
});

test('native fetch is invoked without a client receiver', async () => {
  const client = new ApiClient('https://anof.test', function () {
    assert.equal(this, undefined);
    return Promise.resolve(new Response(null, { status: 204 }));
  });
  await client.request('/api/v1/auth/session', { method: 'DELETE', status: 204, decode: value => value });
});
