import { test, expect, login } from './fixture.mjs';
import { setTimeout as delay } from 'node:timers/promises';

test('private CA trust is required; Cookie bootstrap, text rendering and logout', async ({ page, context, site }) => {
  const untrusted = await site.context(false);
  let rejected = false;
  try { await untrusted.pages()[0].goto(site.origin); } catch { rejected = true; }
  expect(rejected).toBe(true);
  await untrusted.close();
  await login(page, site.origin);
  const cookies = await context.cookies();
  const session = cookies.find(cookie => cookie.name === '__Host-anof_session');
  expect(Boolean(session?.secure && session.httpOnly && session.sameSite === 'Strict' && session.path === '/')).toBe(true);
  expect(await page.evaluate(() => document.cookie.includes('__Host-anof_session'))).toBe(false);
  await page.reload();
  await expect(page.getByRole('heading', { name: '账户资料' })).toBeVisible();
  const malicious = '<img src=x onerror="window.compromised=1">';
  await page.getByLabel('新的显示名称').fill(malicious);
  await page.getByRole('button', { name: '保存显示名称' }).click();
  await expect(page.locator('dd').filter({ hasText: malicious })).toBeVisible();
  expect(await page.evaluate(() => !window.compromised && document.querySelectorAll('#app img').length === 0)).toBe(true);
  expect(await page.evaluate(() => localStorage.length + sessionStorage.length)).toBe(0);
  expect(await page.evaluate(async () => (await indexedDB.databases()).length)).toBe(0);
  const logout = page.waitForResponse(response => response.url().endsWith('/api/v1/auth/session') && response.request().method() === 'DELETE');
  await page.getByRole('button', { name: '登出', exact: true }).click();
  await expect(page.getByRole('status')).toHaveText('已确认登出。');
  const clear = await (await logout).headerValue('set-cookie');
  expect(Boolean(clear?.includes('__Host-anof_session=;') && clear.includes('Max-Age=0'))).toBe(true);
  await expect.poll(async () => (await context.cookies()).some(cookie => cookie.name === '__Host-anof_session'), { timeout: 5000 }).toBe(false);
});

test('invitation registration requires separate login; password revokes session', async ({ page, context, site }) => {
  await login(page, site.origin);
  const invite = await page.evaluate(async () => {
    const response = await fetch('/api/v1/admin/invites', { method: 'POST', headers: { 'Content-Type': 'application/json', 'X-ANOF-Request': '1' }, body: '{}' });
    if (response.status !== 201) throw Error('Invitation fixture setup');
    return (await response.json()).token;
  });
  const password = 'member-password-fixture'; const next = 'changed-password-fixture';
  site.secrets.push(invite, password, next);
  const logout = page.waitForResponse(response => response.url().endsWith('/api/v1/auth/session') && response.request().method() === 'DELETE');
  await page.getByRole('button', { name: '登出', exact: true }).click();
  await expect(page.getByRole('status')).toHaveText('已确认登出。');
  const clear = await (await logout).headerValue('set-cookie');
  expect(Boolean(clear?.includes('__Host-anof_session=;') && clear.includes('Max-Age=0'))).toBe(true);
  await page.getByRole('link', { name: '邀请注册' }).click();
  await expect(page.getByRole('heading', { name: '邀请注册' })).toBeVisible();
  await page.getByLabel('登录名', { exact: true }).fill('member_one');
  await page.getByLabel('显示名称', { exact: true }).fill('Member One');
  await page.getByLabel('邀请码').fill(invite);
  await page.getByLabel('新密码', { exact: true }).fill(password);
  await page.getByRole('button', { name: '注册', exact: true }).click();
  await expect(page.getByRole('status')).toContainText('注册成功');
  await expect.poll(async () => (await context.cookies()).some(cookie => cookie.name === '__Host-anof_session'), { timeout: 5000 }).toBe(false);
  await login(page, site.origin, 'member_one', password);
  const oldSession = (await context.cookies()).find(c => c.name === '__Host-anof_session');
  site.secrets.push(oldSession.value);
  await page.getByRole('link', { name: '修改密码' }).click();
  await page.getByLabel('当前密码', { exact: true }).fill(password);
  await page.getByLabel('新密码', { exact: true }).fill(next);
  await page.getByLabel('确认新密码', { exact: true }).fill(next);
  await page.getByRole('button', { name: '确认修改密码' }).click();
  await expect(page.getByRole('status')).toContainText('密码已更新');
  await expect.poll(async () => (await context.cookies()).some(cookie => cookie.name === '__Host-anof_session'), { timeout: 5000 }).toBe(false);
  await context.addCookies([oldSession]);
  expect(await page.evaluate(async () => (await fetch('/api/v1/users/me')).status)).toBe(401);
  await context.clearCookies();
  // 注册、登录、改密共享每来源/登录名突发 3、每分钟 5 次；预先等一个令牌，不重试写入。
  await delay(12_100);
  await login(page, site.origin, 'member_one', next);
});

test('412 refresh requires another decision; committed response loss never replays', async ({ page, site }) => {
  await login(page, site.origin);
  await page.evaluate(async () => {
    const current = await fetch('/api/v1/users/me');
    const result = await fetch('/api/v1/users/me', { method: 'PATCH', headers: { 'Content-Type': 'application/json', 'X-ANOF-Request': '1', 'If-Match': current.headers.get('etag') }, body: JSON.stringify({ display_name: 'Concurrent edit' }) });
    if (result.status !== 200) throw Error('Conflict fixture setup');
  });
  let writes = 0;
  page.on('request', request => { if (request.method() === 'PATCH') writes++; });
  await page.getByLabel('新的显示名称').fill('Conflict choice');
  await page.getByRole('button', { name: '保存显示名称' }).click();
  await expect(page.getByRole('status')).toContainText('资料已发生变化');
  await expect(page.getByLabel('新的显示名称')).toHaveValue('Concurrent edit');
  expect(writes).toBe(1);
  // 在浏览器 fetch 边界丢弃已收到的成功响应；真实写入仍只发送一次。
  await page.addInitScript(() => {
    const original = window.fetch;
    window.fetch = async (...args) => {
      const response = await original(...args);
      if (args[1]?.method === 'PATCH') {
        await response.arrayBuffer();
        throw new TypeError('Controlled response loss after commit');
      }
      return response;
    };
  });
  await page.reload();
  await expect(page.getByRole('heading', { name: '账户资料' })).toBeVisible();
  await page.getByLabel('新的显示名称').fill('Committed but response lost');
  await page.getByRole('button', { name: '保存显示名称' }).click();
  await expect(page.getByRole('status')).toContainText('操作结果未知');
  await expect(page.getByLabel('新的显示名称')).toHaveValue('Committed but response lost');
  expect(writes).toBe(2);
});

// 受控生命周期事件只验证应用接线，不代表真实桌面切换或浏览器 BFCache 命中。
async function lifecycle(page) {
  await page.evaluate(() => {
    Object.defineProperty(document, 'visibilityState', { configurable: true, value: 'visible' });
    document.dispatchEvent(new Event('visibilitychange'));
    Reflect.deleteProperty(document, 'visibilityState');
    window.dispatchEvent(new PageTransitionEvent('pagehide', { persisted: true }));
    window.dispatchEvent(new PageTransitionEvent('pageshow', { persisted: true }));
  });
}

test('unknown logout stays anonymous through controlled lifecycle events until explicit recovery or logout', async ({ page, context, site }) => {
  await login(page, site.origin);
  const old = (await context.cookies()).find(c => c.name === '__Host-anof_session');
  site.secrets.push(old.value);
  let deletes = 0, reads = 0;
  page.on('request', r => {
    if (r.method() === 'DELETE' && r.url().endsWith('/api/v1/auth/session')) deletes++;
    if (r.method() === 'GET' && r.url().endsWith('/api/v1/users/me')) reads++;
  });
  await page.route('**/api/v1/auth/session', route => route.abort('failed'));
  async function remainAnonymous() {
    const before = reads;
    for (const route of ['account', 'files', 'admin']) {
      await page.evaluate(route => { location.hash = '#/' + route; }, route);
      await lifecycle(page);
      await expect(page.getByRole('heading', { name: '登录', exact: true })).toBeVisible();
      await expect(page.getByRole('status')).toContainText('未确认服务端撤销');
      await expect(page.locator('.drive, .admin')).toHaveCount(0);
      await expect(page.getByRole('heading', { name: '账户资料', exact: true })).toHaveCount(0);
    }
    // 有界负向观测窗口，检查生命周期没有发出读取或重发写入。
    await page.waitForTimeout(200);
    expect(reads).toBe(before);
  }
  await page.getByRole('button', { name: '登出', exact: true }).click();
  await expect(page.getByRole('status')).toContainText('未确认服务端撤销');
  await remainAnonymous(); expect(deletes).toBe(1);
  expect((await context.cookies()).some(c => c.value === old.value)).toBe(true);
  await page.getByRole('button', { name: '重新核对身份' }).click();
  await expect(page.locator('.admin')).toBeVisible();
  await expect(page.getByRole('status').first()).toContainText('未确认服务端撤销');
  expect(deletes).toBe(1);
  await page.getByRole('button', { name: '登出', exact: true }).click();
  await expect(page.getByRole('status')).toContainText('未确认服务端撤销');
  await remainAnonymous(); expect(deletes).toBe(2);
  await page.unroute('**/api/v1/auth/session');
  await page.getByRole('button', { name: '再次发起登出', exact: true }).click();
  await expect(page.getByRole('status')).toHaveText('已确认登出。');
  await lifecycle(page);
  await expect(page.getByRole('status')).toHaveText('已确认登出。');
  expect(deletes).toBe(3);
  await context.addCookies([old]);
  expect(await page.evaluate(async () => (await fetch('/api/v1/users/me')).status)).toBe(401);
  await expect(page.getByRole('heading', { name: '登录', exact: true })).toBeVisible();
});

test('authenticated identity is revalidated by controlled visibility and persisted pageshow events', async ({ page, site }) => {
  await login(page, site.origin);
  for (const event of ['visibilitychange', 'pageshow']) {
    const read = page.waitForResponse(r => r.url().endsWith('/api/v1/users/me') && r.request().method() === 'GET');
    await page.evaluate(event => {
      if (event === 'visibilitychange') {
        Object.defineProperty(document, 'visibilityState', { configurable: true, value: 'visible' });
        document.dispatchEvent(new Event(event)); Reflect.deleteProperty(document, 'visibilityState');
      } else {
        window.dispatchEvent(new PageTransitionEvent('pagehide', { persisted: true }));
        window.dispatchEvent(new PageTransitionEvent('pageshow', { persisted: true }));
      }
    }, event);
    expect((await read).status()).toBe(200);
    await expect(page.getByRole('heading', { name: '账户资料', exact: true })).toBeVisible();
  }
});

test('browser rejects unmarked authentication and cross-origin fetch', async ({ page, site }) => {
  await page.goto(site.origin);
  for (const path of ['/api/v1/auth/sessions', '/api/v1/auth/register']) {
    const status = await page.evaluate(async path => (await fetch(path, { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: '{}' })).status, path);
    expect(status).toBe(403);
  }
  const attacker = await page.context().newPage();
  await attacker.goto(site.otherOrigin);
  const blocked = await attacker.evaluate(async origin => {
    try { await fetch(origin + '/api/v1/auth/sessions', { method: 'POST', credentials: 'include', headers: { 'Content-Type': 'application/json', 'X-ANOF-Request': '1' }, body: '{}' }); return false; }
    catch { return true; }
  }, site.origin);
  expect(blocked).toBe(true);
});
