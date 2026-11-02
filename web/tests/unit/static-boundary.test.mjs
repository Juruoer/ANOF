import assert from 'node:assert/strict';
import {after, before, test} from 'node:test';
import {get} from 'node:http';
import {createServer} from 'vite';

let server;
let origin;

before(async () => {
    server = await createServer({
        server: {port: 0},
        logLevel: 'silent',
    });
    await server.listen();
    origin = `http://127.0.0.1:${server.httpServer.address().port}`;
});

after(async () => {
    await server?.close();
});

test('static server never substitutes HTML for API or health requests', {timeout: 10_000}, async () => {
    for (const path of ['/api', '/api/v1/users/me', '/api/missing.html', '/health', '/health/ready', '/%61pi/v1/users/me']) {
        const response = await fetch(`${origin}${path}`, {signal: AbortSignal.timeout(2_000)});
        assert.equal(response.status, 404, path);
        assert.equal(response.headers.get('cache-control'), 'no-store');
        assert.doesNotMatch(await response.text(), /<html|<script/iu);
    }
});

test('static server provides no cross-origin access grant', {timeout: 10_000}, async () => {
    const response = await fetch(origin, {
        headers: {Origin: 'https://untrusted.invalid'},
        signal: AbortSignal.timeout(2_000),
    });
    assert.equal(response.headers.get('access-control-allow-origin'), null);
});

test('project instructions and parent source files cannot be served', {timeout: 10_000}, async () => {
    for (const path of [
        '/AGENTS.md',
        '/.tools/node-v24.22.1-linux-x64/LICENSE',
        '/tests/unit/static-boundary.test.mjs',
        `/@fs${new URL('../../../README.md', import.meta.url).pathname}`,
    ]) {
        const response = await fetch(`${origin}${path}`, {signal: AbortSignal.timeout(2_000)});
        assert.ok([403, 404].includes(response.status), `${path}: ${response.status}`);
        assert.doesNotMatch(await response.text(), /# (?:Repository|Web Session) Guidelines/u);
    }
});

test('static server rejects an untrusted Host', {timeout: 10_000}, async () => {
    // fetch 的 Host 由传输层管理；畸形来源检查需要能原样发送该头的 HTTP 客户端。
    const status = await new Promise((resolve, reject) => {
        const request = get(origin, {headers: {Host: 'untrusted.invalid'}}, (response) => {
            response.on('error', reject);
            response.on('end', () => resolve(response.statusCode));
            response.resume();
        });
        request.setTimeout(2_000, () => request.destroy(new Error('HTTP test request timed out')));
        request.on('error', reject);
    });
    assert.equal(status, 403);
});
