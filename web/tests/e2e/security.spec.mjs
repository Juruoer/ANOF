import { test, expect, login } from './fixture.mjs';

const me = '/api/v1/users/me';

test('cross-site navigation omits Strict Cookie; forms and opaque origins cannot authenticate', async ({ page, context, site }) => {
  await login(page, site.origin);
  const attacker = await context.newPage();
  await attacker.goto(site.otherOrigin);
  const target = site.origin + me;
  const read = attacker.waitForResponse(r => r.url() === target);
  await attacker.evaluate(target => {
    const a = document.createElement('a'); a.href = target; a.textContent = 'Navigate'; document.body.append(a); a.click();
  }, target);
  const response = await read;
  await attacker.waitForURL(target);
  expect(response.status()).toBe(401);
  expect(Boolean((await response.request().allHeaders()).cookie)).toBe(false);
  expect((await context.cookies()).some(c => c.name === '__Host-anof_session')).toBe(true);
  expect(await page.evaluate(async path => (await fetch(path)).status, me)).toBe(200);

  for (const opaque of [false, true]) {
    for (const path of ['/api/v1/auth/sessions', '/api/v1/auth/register']) {
      await attacker.goto(site.otherOrigin);
      const result = attacker.waitForResponse(r => r.url() === site.origin + path && r.request().method() === 'POST');
      await attacker.evaluate(({ action, opaque }) => {
        if (opaque) {
          const frame = document.createElement('iframe');
          frame.setAttribute('sandbox', 'allow-scripts allow-forms');
          // 固定测试 HTML，仅 action 来自测试夹具的 loopback origin。
          frame.srcdoc = `<form method="post" action="${action}"><input name="credential_mode" value="cookie"></form><script>document.forms[0].submit()</script>`;
          document.body.append(frame);
        } else {
          const form = document.createElement('form'); form.method = 'POST'; form.action = action;
          const input = document.createElement('input'); input.name = 'credential_mode'; input.value = 'cookie';
          form.append(input); document.body.append(form); form.submit();
        }
      }, { action: site.origin + path, opaque });
      const denied = await result;
      if (!opaque) await attacker.waitForURL(site.origin + path);
      expect(denied.status()).toBe(403);
      expect((await denied.request().allHeaders()).origin).toBe(opaque ? 'null' : site.otherOrigin);
      expect(await denied.headerValue('access-control-allow-origin')).toBeNull();
      expect(await denied.headerValue('set-cookie')).toBeNull();
    }
  }
  expect(await page.evaluate(async path => (await fetch(path)).status, me)).toBe(200);
});

test('CSP blocks inline execution; assets stay same-origin and API errors never become HTML', async ({ page, site }) => {
  const navigation = await page.goto(site.origin);
  const csp = await navigation.headerValue('content-security-policy');
  expect(Boolean(csp?.includes("default-src 'none'") && csp.includes("script-src 'self'") && csp.includes("frame-ancestors 'none'"))).toBe(true);
  expect(await navigation.headerValue('x-content-type-options')).toBe('nosniff');
  expect(await navigation.headerValue('referrer-policy')).toBe('no-referrer');
  await expect(page.getByRole('heading', { name: '登录', exact: true })).toBeVisible();
  await page.evaluate(() => {
    window.cspViolations = [];
    document.addEventListener('securitypolicyviolation', e => window.cspViolations.push(e.effectiveDirective));
    const script = document.createElement('script'); script.textContent = 'window.inlineExecuted = true'; document.body.append(script);
  });
  await expect.poll(() => page.evaluate(() => window.cspViolations.includes('script-src-elem'))).toBe(true);
  expect(await page.evaluate(() => window.inlineExecuted === undefined)).toBe(true);
  expect(await page.evaluate(() => performance.getEntriesByType('resource').every(r => new URL(r.name).origin === location.origin))).toBe(true);
  const invalid = await page.evaluate(async () => {
    const response = await fetch('/api/v1/not-a-route');
    return { status: response.status, type: response.headers.get('content-type'), html: (await response.text()).includes('<!doctype html>') };
  });
  expect(invalid.status).toBe(404); expect(invalid.html).toBe(false);
  expect(invalid.type?.includes('text/html') ?? false).toBe(false);
});

test('revoked Cookie cannot be reused; browser-expired Cookie clears protected views on refresh', async ({ page, context, site }) => {
  await login(page, site.origin);
  const old = (await context.cookies()).find(c => c.name === '__Host-anof_session');
  expect(Boolean(old)).toBe(true); site.secrets.push(old.value);
  await page.getByRole('button', { name: '登出', exact: true }).click();
  await expect(page.getByRole('status')).toHaveText('已确认登出。');
  // 测试驱动重新放入旧 Cookie；产品 JavaScript 无法读取或写入 HttpOnly Cookie。
  await context.addCookies([old]);
  await page.reload();
  await expect(page.getByRole('heading', { name: '登录', exact: true })).toBeVisible();
  expect(await page.evaluate(async path => (await fetch(path)).status, me)).toBe(401);
  await login(page, site.origin);
  await page.getByRole('link', { name: '文件管理', exact: true }).click();
  await expect(page.locator('.drive')).toBeVisible();
  const live = (await context.cookies()).find(c => c.name === '__Host-anof_session');
  // 控制浏览器 Cookie 过期，不修改主机时钟，也不冒充服务端绝对七天过期。
  await context.addCookies([{ ...live, expires: 1 }]);
  await page.reload();
  await expect(page.getByRole('heading', { name: '登录', exact: true })).toBeVisible();
  await expect(page.locator('.drive')).toHaveCount(0);
  await expect(page.getByRole('link', { name: '管理员管理', exact: true })).toHaveCount(0);
  expect(await page.evaluate(async () => localStorage.length + sessionStorage.length + (await indexedDB.databases()).length)).toBe(0);
});

test('unmarked protected writes preserve the live HttpOnly session and do not mutate identity', async ({ page, context, site }) => {
  await login(page, site.origin);
  const before = (await context.cookies()).find(c => c.name === '__Host-anof_session');
  await page.evaluate(() => { document.cookie = '__Host-anof_session=invalid; Path=/; Secure; SameSite=Strict'; });
  expect((await context.cookies()).find(c => c.name === '__Host-anof_session')?.value === before.value).toBe(true);
  for (const [path, method] of [['/api/v1/auth/session', 'DELETE'], ['/api/v1/users/me/password', 'POST']]) {
    const denied = await page.evaluate(async ({ path, method }) => {
      const response = await fetch(path, { method, credentials: 'same-origin', mode: 'same-origin', referrerPolicy: 'same-origin', headers: { 'Content-Type': 'application/json' }, body: '{}' });
      return { status: response.status, code: (await response.json()).error.code };
    }, { path, method });
    expect(denied).toEqual({ status: 403, code: 'CSRF_REQUIRED' });
  }
  expect(await page.evaluate(async path => (await fetch(path)).status, me)).toBe(200);
  expect((await context.cookies()).find(c => c.name === '__Host-anof_session')?.value === before.value).toBe(true);
});
