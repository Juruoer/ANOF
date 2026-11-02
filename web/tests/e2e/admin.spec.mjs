import { test, expect, login } from './fixture.mjs';
async function request(page, path, method = 'GET', body, etag, expected = 200) {
  const result = await page.evaluate(async ({ path, method, body, etag }) => {
    const response = await fetch(path, { method, mode: 'same-origin', credentials: 'same-origin', referrerPolicy: 'same-origin', headers: {
      ...(method === 'GET' ? {} : { 'X-ANOF-Request': '1' }), ...(body === undefined ? {} : { 'Content-Type': 'application/json' }), ...(etag ? { 'If-Match': etag } : {}),
    }, ...(body === undefined ? {} : { body: JSON.stringify(body) }) });
    return { status: response.status, etag: response.headers.get('etag'), body: response.status === 204 ? null : await response.json() };
  }, { path, method, body, etag });
  expect(result.status).toBe(expected); return result;
}
const admin = page => page.locator('.admin');
const status = page => admin(page).getByRole('status');
const memberRow = (page, name = 'member_one') => admin(page).locator('.entries > li').filter({ has: page.getByRole('heading', { name, exact: true }) });
async function open(page) {
  await page.getByRole('link', { name: '管理员管理', exact: true }).click();
  await expect(admin(page).getByRole('heading', { name: '用户列表', exact: true })).toBeVisible();
}
async function member(page, site) {
  const secret = (await request(page, '/api/v1/admin/invites', 'POST', {}, undefined, 201)).body.token;
  const password = 'member-admin-test-password'; site.secrets.push(secret, password);
  return (await request(page, '/api/v1/auth/register', 'POST', { login: 'member_one', display_name: '<img src=x onerror=alert(1)>', password, invite_token: secret, credential_mode: 'cookie' }, undefined, 201)).body;
}
async function confirm(page, button) {
  await admin(page).getByLabel('我已核对并确认此操作').check();
  await admin(page).getByRole('button', { name: `确认${button}`, exact: true }).click();
}

test('administrator invitation handover, revocation and enable/disable invalidate old member sessions', async ({ page, site }) => {
  await login(page, site.origin); await open(page);
  await admin(page).getByRole('button', { name: '邀请管理', exact: true }).click();
  await admin(page).getByLabel('最多使用次数（1–10000）').fill('2');
  await admin(page).getByRole('button', { name: '创建邀请', exact: true }).click();
  await expect(status(page)).toContainText('邀请已创建');
  await expect(admin(page).getByLabel('一次性邀请码', { exact: true })).toHaveCount(0);
  await admin(page).getByRole('button', { name: '显示一次性邀请码' }).click();
  const secret = await admin(page).getByLabel('一次性邀请码', { exact: true }).inputValue();
  const password = 'enabled-member-test-password'; site.secrets.push(secret, password);
  expect(await page.evaluate(() => localStorage.length + sessionStorage.length)).toBe(0);
  expect(await page.evaluate(async () => (await indexedDB.databases()).length)).toBe(0);
  expect(page.url().includes(secret)).toBe(false);
  await admin(page).getByRole('button', { name: '已完成交接，清除秘密' }).click();
  await expect(admin(page).getByLabel('一次性邀请码', { exact: true })).toHaveCount(0);
  await expect(admin(page).getByRole('button', { name: '显示一次性邀请码' })).toHaveCount(0);
  const memberContext = await site.context(); const memberPage = await memberContext.newPage();
  await memberPage.goto(site.origin + '/#/register');
  await expect(memberPage.getByRole('heading', { name: '邀请注册', exact: true })).toBeVisible();
  await memberPage.getByLabel('登录名', { exact: true }).fill('member_one');
  await memberPage.getByLabel('显示名称', { exact: true }).fill('Member One');
  await memberPage.getByLabel('邀请码', { exact: true }).fill(secret);
  await memberPage.getByLabel('新密码', { exact: true }).fill(password);
  await memberPage.getByRole('button', { name: '注册', exact: true }).click();
  await expect(memberPage.getByRole('status')).toContainText('注册成功');
  await login(memberPage, site.origin, 'member_one', password);
  await expect(memberPage.getByRole('link', { name: '管理员管理', exact: true })).toHaveCount(0);
  await memberPage.goto(site.origin + '/#/admin');
  await expect(memberPage.getByText('此入口仅供管理员使用。', { exact: true })).toBeVisible();
  await request(memberPage, '/api/v1/admin/users', 'GET', undefined, undefined, 403);
  await admin(page).getByRole('button', { name: '刷新管理资料', exact: true }).click();
  await expect(admin(page).locator('.entries')).toContainText('1 / 2');
  await admin(page).getByRole('button', { name: '撤销邀请', exact: true }).click();
  await confirm(page, '撤销邀请'); await expect(status(page)).toContainText('邀请撤销已确认');
  await request(memberPage, '/api/v1/auth/register', 'POST', { login: 'unused_member', display_name: 'Unused', password, invite_token: secret, credential_mode: 'cookie' }, undefined, 409);
  await admin(page).getByRole('button', { name: '用户列表', exact: true }).click();
  await memberRow(page).getByRole('button', { name: '查看用户' }).click();
  await admin(page).getByRole('button', { name: '禁用用户', exact: true }).click();
  let patches = 0; page.on('request', r => { if (r.method() === 'PATCH') patches++; });
  await admin(page).getByRole('button', { name: '确认禁用用户', exact: true }).click(); expect(patches).toBe(0);
  await confirm(page, '禁用用户'); await expect(status(page)).toContainText('用户已禁用'); expect(patches).toBe(1);
  await memberPage.reload();
  await expect(memberPage.getByRole('heading', { name: '登录', exact: true })).toBeVisible();
  // 普通 GET 401 清除本地身份，但浏览器仍保留 HttpOnly Cookie；禁用后的旧会话不能复活。
  expect((await memberContext.cookies()).some(c => c.name === '__Host-anof_session')).toBe(true);
  await admin(page).getByRole('button', { name: '启用用户', exact: true }).click();
  await confirm(page, '启用用户'); await expect(status(page)).toContainText('用户已启用');
  await memberPage.getByRole('button', { name: '重新核对身份', exact: true }).click();
  await expect(memberPage.getByRole('heading', { name: '登录', exact: true })).toBeVisible();
  await request(memberPage, '/api/v1/users/me', 'GET', undefined, undefined, 401);
});

test('administrator manages private/public/contribution quotas and physical budget without private file access', async ({ page, site }) => {
  await login(page, site.origin); const user = await member(page, site); await open(page);
  await expect(admin(page).locator('img')).toHaveCount(0);
  await memberRow(page).getByRole('button', { name: '查看用户' }).click();
  let privateNodeReads = 0; page.on('request', r => { if (/\/api\/v1\/nodes\//u.test(r.url())) privateNodeReads++; });
  await admin(page).getByRole('button', { name: '管理私有空间配额', exact: true }).click();
  await admin(page).getByLabel('字节上限', { exact: true }).fill('2097152');
  await admin(page).getByLabel('节点上限', { exact: true }).fill('200');
  await admin(page).getByRole('button', { name: '保存配额', exact: true }).click();
  await expect(status(page)).toContainText('配额上限已更新');
  const detail = (await request(page, `/api/v1/admin/users/${user.id}`)).body;
  const privateQuota = await request(page, `/api/v1/admin/spaces/${detail.private_space_id}/quota`);
  expect(privateQuota.body.quota.limit).toEqual({ bytes: '2097152', nodes: '200' });
  await admin(page).getByLabel('节点上限', { exact: true }).fill('0');
  await admin(page).getByRole('button', { name: '保存配额', exact: true }).click();
  await expect(status(page)).toContainText('低于当前已用加预留');
  await expect(admin(page).getByLabel('节点上限', { exact: true })).toHaveValue('200');
  await admin(page).getByRole('button', { name: '返回用户详情', exact: true }).click();
  await admin(page).getByRole('button', { name: '管理公共贡献配额', exact: true }).click();
  await admin(page).getByLabel('字节上限', { exact: true }).fill('1048576');
  await admin(page).getByLabel('节点上限', { exact: true }).fill('15');
  await admin(page).getByRole('button', { name: '保存配额', exact: true }).click();
  await expect(status(page)).toContainText('配额上限已更新');
  await admin(page).getByRole('button', { name: '公共空间配额', exact: true }).click();
  await admin(page).getByLabel('字节上限', { exact: true }).fill('2097152');
  await admin(page).getByLabel('节点上限', { exact: true }).fill('300');
  await admin(page).getByRole('button', { name: '保存配额', exact: true }).click();
  await expect(status(page)).toContainText('配额上限已更新'); expect(privateNodeReads).toBe(0);
  const spaces = (await request(page, '/api/v1/spaces')).body.items;
  const pub = spaces.find(s => s.kind === 'public'), own = spaces.find(s => s.kind === 'private');
  expect((await request(page, `/api/v1/admin/spaces/${pub.id}/contributions/${user.id}/quota`)).body.quota.limit.nodes).toBe('15');
  expect((await request(page, `/api/v1/admin/spaces/${pub.id}/quota`)).body.quota.limit.nodes).toBe('300');
  // 只准备 16 字节预留来验证预算下界，不实现或验收上传 UI。
  await request(page, '/api/v1/uploads', 'POST', { client_upload_id: '70000000-0000-4000-8000-000000000004', space_id: own.id, parent_id: own.root_node_id, name: 'budget-fixture', length: '16' }, undefined, 201);
  await admin(page).getByRole('button', { name: '存储预算', exact: true }).click();
  await expect(admin(page).locator('dt').filter({ hasText: /^P \+ R$/u }).locator('+ dd')).toHaveText('16 字节');
  await admin(page).getByLabel('内容预算（字节）', { exact: true }).fill('0');
  await admin(page).getByRole('button', { name: '保存内容预算', exact: true }).click();
  await expect(status(page)).toContainText('低于当前已计费内容加预留');
  await admin(page).getByLabel('内容预算（字节）', { exact: true }).fill('20971520');
  await admin(page).getByRole('button', { name: '保存内容预算', exact: true }).click();
  await expect(status(page)).toContainText('内容预算已更新');
  expect((await request(page, '/api/v1/admin/storage')).body.content_budget_bytes).toBe('20971520');
  for (const label of ['隔离（P 的子集）', '不可用（P 的子集）']) await expect(admin(page).getByText(label, { exact: true })).toBeVisible();
});

test('administrator 412 refresh requires a new decision; lost budget response is never replayed', async ({ page, site }) => {
  await login(page, site.origin); const user = await member(page, site); await open(page);
  await memberRow(page).getByRole('button', { name: '查看用户' }).click();
  await expect(admin(page).getByRole('heading', { name: '用户：member_one' })).toBeVisible();
  const current = await request(page, `/api/v1/admin/users/${user.id}`);
  await request(page, `/api/v1/admin/users/${user.id}`, 'PATCH', { enabled: false }, current.etag);
  let writes = 0; page.on('request', r => { if (['PATCH', 'PUT'].includes(r.method())) writes++; });
  await admin(page).getByRole('button', { name: '禁用用户', exact: true }).click();
  await confirm(page, '禁用用户'); await expect(status(page)).toContainText('资料已发生变化'); expect(writes).toBe(1);
  await admin(page).getByRole('button', { name: '启用用户', exact: true }).click();
  await confirm(page, '启用用户'); await expect(status(page)).toContainText('用户已启用'); expect(writes).toBe(2);
  await page.addInitScript(() => {
    const original = window.fetch;
    window.fetch = async (...args) => {
      const response = await original(...args);
      if (args[0] === '/api/v1/admin/storage' && args[1]?.method === 'PUT') { await response.arrayBuffer(); throw new TypeError('Controlled response loss'); }
      return response;
    };
  });
  await page.reload();
  await admin(page).getByRole('button', { name: '存储预算', exact: true }).click();
  await admin(page).getByLabel('内容预算（字节）', { exact: true }).fill('20971520');
  await admin(page).getByRole('button', { name: '保存内容预算', exact: true }).click();
  await expect(status(page)).toContainText('操作结果未知');
  await expect(admin(page).getByLabel('内容预算（字节）', { exact: true })).toHaveValue('20971520'); expect(writes).toBe(3);
});

test('bounded invitation paging and late secret delivery after navigation', async ({ page, site }) => {
  await login(page, site.origin);
  for (let i = 0; i < 101; i++) site.secrets.push((await request(page, '/api/v1/admin/invites', 'POST', {}, undefined, 201)).body.token);
  await open(page); await admin(page).getByRole('button', { name: '邀请管理', exact: true }).click();
  await expect(admin(page).locator('.entries > li')).toHaveCount(100);
  await admin(page).getByRole('button', { name: '下一页', exact: true }).click();
  await expect(admin(page).locator('.entries > li')).toHaveCount(1);
  await admin(page).getByRole('button', { name: '上一页', exact: true }).click();
  await expect(admin(page).locator('.entries > li')).toHaveCount(100);
  await page.addInitScript(() => {
    const original = window.fetch;
    window.fetch = async (...args) => {
      const response = await original(...args);
      if (args[0] === '/api/v1/admin/invites' && args[1]?.method === 'POST') {
        // 先完成真实小响应的消费，再延迟应用可见结果，避免测试自身等待响应体形成循环。
        await response.clone().arrayBuffer();
        await new Promise(resolve => { window.releaseInviteResponse = resolve; });
      }
      return response;
    };
  });
  await page.reload(); await admin(page).getByRole('button', { name: '邀请管理', exact: true }).click();
  const response = page.waitForResponse(r => r.url().endsWith('/api/v1/admin/invites') && r.request().method() === 'POST');
  await admin(page).getByRole('button', { name: '创建邀请', exact: true }).click();
  site.secrets.push((await (await response).json()).token);
  await page.waitForFunction(() => typeof window.releaseInviteResponse === 'function');
  await page.getByRole('link', { name: '账户资料', exact: true }).click();
  await expect(page.getByRole('heading', { name: '账户资料', exact: true })).toBeVisible();
  await page.evaluate(() => window.releaseInviteResponse());
  await open(page); await admin(page).getByRole('button', { name: '邀请管理', exact: true }).click();
  await expect(admin(page).getByRole('button', { name: '显示一次性邀请码' })).toHaveCount(0);
  expect(await page.evaluate(() => localStorage.length + sessionStorage.length)).toBe(0);
});

test('lost invitation creation response leaves metadata only and never creates a replacement', async ({ page, site }) => {
  await page.addInitScript(() => {
    const original = window.fetch;
    window.fetch = async (...args) => {
      const response = await original(...args);
      if (args[0] === '/api/v1/admin/invites' && args[1]?.method === 'POST') { await response.arrayBuffer(); throw new TypeError('Controlled invitation response loss'); }
      return response;
    };
  });
  await login(page, site.origin); await open(page); await admin(page).getByRole('button', { name: '邀请管理', exact: true }).click();
  let creates = 0; page.on('request', r => { if (r.url().endsWith('/api/v1/admin/invites') && r.method() === 'POST') creates++; });
  const response = page.waitForResponse(r => r.url().endsWith('/api/v1/admin/invites') && r.request().method() === 'POST');
  await admin(page).getByRole('button', { name: '创建邀请', exact: true }).click();
  site.secrets.push((await (await response).json()).token);
  await expect(status(page)).toContainText('操作结果未知');
  await expect(admin(page).locator('.entries > li')).toHaveCount(1); expect(creates).toBe(1);
  await expect(admin(page).getByRole('button', { name: '显示一次性邀请码' })).toHaveCount(0);
});

test('controlled hidden event clears shown invitation secrets; real logout clears them again', async ({ page, site }) => {
  await login(page, site.origin); await open(page);
  async function reveal() {
    await admin(page).getByRole('button', { name: '邀请管理', exact: true }).click();
    await admin(page).getByRole('button', { name: '创建邀请', exact: true }).click();
    await admin(page).getByRole('button', { name: '显示一次性邀请码' }).click();
    site.secrets.push(await admin(page).getByLabel('一次性邀请码', { exact: true }).inputValue());
  }
  await reveal();
  // Headless Shell 不随 bringToFront 改变可见性；明确注入事件，仅验证应用处理，不冒充桌面切换证据。
  await page.evaluate(() => {
    Object.defineProperty(document, 'visibilityState', { configurable: true, value: 'hidden' });
    document.dispatchEvent(new Event('visibilitychange'));
  });
  await expect(page.getByLabel('一次性邀请码', { exact: true })).toHaveCount(0);
  await page.evaluate(() => {
    Reflect.deleteProperty(document, 'visibilityState');
    document.dispatchEvent(new Event('visibilitychange'));
  });
  await expect(admin(page).getByRole('heading', { name: '用户列表', exact: true })).toBeVisible();
  await reveal();
  await page.getByRole('button', { name: '登出', exact: true }).click();
  await expect(page.getByRole('status')).toHaveText('已确认登出。');
  await expect(page.getByLabel('一次性邀请码', { exact: true })).toHaveCount(0);
  await expect(page.getByRole('link', { name: '管理员管理', exact: true })).toHaveCount(0);
});
