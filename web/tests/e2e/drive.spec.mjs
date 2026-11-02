import { test, expect, login } from './fixture.mjs';

// 仅准备小型测试数据；上传队列、断点记录及完整下载验收不属于这些用例。
async function request(page, path, method = 'GET', body, etag, expected = 200) {
  const result = await page.evaluate(async ({ path, method, body, etag }) => {
    const response = await fetch(path, { method, credentials: 'same-origin', mode: 'same-origin', referrerPolicy: 'same-origin', headers: {
      ...(method === 'GET' ? {} : { 'X-ANOF-Request': '1' }), ...(body === undefined ? {} : { 'Content-Type': 'application/json' }), ...(etag ? { 'If-Match': etag } : {}),
    }, ...(body === undefined ? {} : { body: JSON.stringify(body) }) });
    return { status: response.status, etag: response.headers.get('etag'), body: response.status === 204 ? null : await response.json() };
  }, { path, method, body, etag });
  expect(result.status).toBe(expected); return result;
}
const drive = page => page.locator('.drive');
const row = (page, name) => drive(page).locator('.entries > li').filter({ has: page.getByRole('heading', { name, exact: true }) });
async function open(page, kind = '我的') {
  await page.getByRole('link', { name: '文件管理', exact: true }).click();
  await drive(page).getByRole('button', { name: `打开${kind}空间`, exact: true }).click();
  await expect(drive(page).getByRole('heading', { name: '目录：空间根目录', exact: true })).toBeVisible();
}
async function submit(page, name, expected) {
  await drive(page).getByLabel('名称', { exact: true }).fill(name);
  await drive(page).getByRole('button', { name: '提交', exact: true }).click();
  await expect(drive(page).getByRole('status')).toContainText(expected);
}
async function chooseRoot(page, kind = '我的') {
  await drive(page).getByRole('button', { name: `目标：${kind}空间`, exact: true }).click();
  await drive(page).getByRole('button', { name: '选用此目录', exact: true }).click();
}
async function mkdir(page, parent, name) {
  return (await request(page, '/api/v1/nodes', 'POST', { space_id: parent.space_id ?? parent.id, parent_id: parent.root_node_id ?? parent.id, name }, undefined, 201)).body;
}

test('daily directories, file copy, literal search, trash restore and explicit purge', async ({ page, site }) => {
  await login(page, site.origin);
  const spaces = (await request(page, '/api/v1/spaces')).body.items;
  const space = spaces.find(item => item.kind === 'private');
  const upload = (await request(page, '/api/v1/uploads', 'POST', {
    client_upload_id: '70000000-0000-4000-8000-000000000001', space_id: space.id, parent_id: space.root_node_id,
    name: 'small-file.txt', length: '0', sha256: 'e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855',
  }, undefined, 201)).body;
  await request(page, `/api/v1/uploads/${upload.id}/complete`, 'POST', {}, undefined, 202);
  await expect.poll(async () => (await request(page, `/api/v1/uploads/${upload.id}`)).body.state, { timeout: 8000 }).toBe('COMPLETED');
  await open(page);
  await drive(page).getByRole('button', { name: '新建目录', exact: true }).click();
  await submit(page, 'folder%_', '目录已创建');
  await row(page, 'folder%_').getByRole('button', { name: '打开目录' }).click();
  await expect(drive(page).getByRole('heading', { name: '目录：folder%_' })).toBeVisible();
  await drive(page).getByRole('button', { name: '新建目录', exact: true }).click();
  await submit(page, 'inner', '目录已创建');
  await row(page, 'inner').getByRole('button', { name: '移动', exact: true }).click();
  await chooseRoot(page);
  await drive(page).getByRole('button', { name: '提交到所选目录' }).click();
  await expect(drive(page).getByRole('status')).toContainText('节点已移动');
  await row(page, 'inner').getByRole('button', { name: '重命名', exact: true }).click();
  await submit(page, '<img src=x onerror=alert(1)>', '名称已更新');
  expect(await drive(page).locator('img').count()).toBe(0);
  await row(page, 'small-file.txt').getByRole('button', { name: '复制文件' }).click();
  await drive(page).getByLabel('名称', { exact: true }).fill('copied.txt');
  await chooseRoot(page, '公共');
  await drive(page).getByRole('button', { name: '提交到所选目录' }).click();
  await expect(drive(page).getByRole('status')).toContainText('文件已复制');
  await expect(row(page, 'copied.txt')).toBeVisible();
  await expect(row(page, 'copied.txt').getByRole('link', { name: '下载（浏览器处理）' })).toHaveAttribute('href', /^\/api\/v1\/nodes\/[0-9a-f-]+\/content$/);
  await drive(page).getByRole('button', { name: '所有空间', exact: true }).click();
  await drive(page).getByRole('button', { name: '打开我的空间', exact: true }).click();
  await drive(page).getByLabel('按名称搜索（字面匹配）').fill('%_');
  await drive(page).getByRole('button', { name: '搜索', exact: true }).click();
  await expect(row(page, 'folder%_')).toBeVisible();
  await expect(drive(page).locator('.entries > li')).toHaveCount(1);
  await row(page, 'folder%_').getByRole('button', { name: '移入回收站', exact: true }).click();
  await drive(page).getByLabel('我已核对并确认此操作').check();
  await drive(page).getByRole('button', { name: '确认移入回收站' }).click();
  await expect(drive(page).getByRole('status')).toContainText('已移入回收站');
  await row(page, 'folder%_').getByRole('button', { name: '查看回收条目' }).click();
  await drive(page).getByRole('button', { name: '恢复到原位置' }).click();
  await submit(page, 'restored', '条目已恢复');
  await drive(page).getByRole('button', { name: '空间根目录', exact: true }).click();
  await row(page, 'restored').getByRole('button', { name: '移入回收站', exact: true }).click();
  await drive(page).getByLabel('我已核对并确认此操作').check();
  await drive(page).getByRole('button', { name: '确认移入回收站' }).click();
  await row(page, 'restored').getByRole('button', { name: '查看回收条目' }).click();
  await drive(page).getByRole('button', { name: '永久删除', exact: true }).click();
  let deletions = 0; page.on('request', r => { if (r.method() === 'DELETE') deletions++; });
  await drive(page).getByRole('button', { name: '确认永久删除' }).click();
  expect(deletions).toBe(0);
  await drive(page).getByLabel('我已核对并确认此操作').check();
  await drive(page).getByRole('button', { name: '确认永久删除' }).click();
  await expect(drive(page).getByRole('status')).toContainText('永久删除已确认');
  expect(deletions).toBe(1);
  await expect(drive(page).getByText('回收站为空。', { exact: true })).toBeVisible();
});

test('real bounded paging, stale ETag decision and simulated lost mutation response', async ({ page, site }) => {
  await login(page, site.origin);
  const space = (await request(page, '/api/v1/spaces')).body.items.find(item => item.kind === 'private');
  const quota = await request(page, `/api/v1/admin/spaces/${space.id}/quota`);
  await request(page, `/api/v1/admin/spaces/${space.id}/quota`, 'PUT', { limit: { bytes: '1048576', nodes: '300' } }, quota.etag);
  const nodes = [];
  for (let i = 0; i < 101; i++) nodes.push(await mkdir(page, space, `entry-${String(i).padStart(3, '0')}`));
  await open(page);
  await expect(drive(page).locator('.entries > li')).toHaveCount(100);
  await drive(page).getByRole('button', { name: '下一页', exact: true }).click();
  await expect(drive(page).locator('.entries > li')).toHaveCount(1);
  await expect(row(page, 'entry-100')).toBeVisible();
  await drive(page).getByRole('button', { name: '上一页', exact: true }).click();
  await expect(drive(page).locator('.entries > li')).toHaveCount(100);
  const current = await request(page, `/api/v1/nodes/${nodes[0].id}`);
  await request(page, `/api/v1/nodes/${nodes[0].id}`, 'PATCH', { name: 'entry-000-concurrent' }, current.etag);
  let writes = 0; page.on('request', r => { if (r.method() === 'PATCH') writes++; });
  await row(page, 'entry-000').getByRole('button', { name: '重命名', exact: true }).click();
  await submit(page, 'entry-000-choice', '资料已发生变化');
  expect(writes).toBe(1);
  await expect(row(page, 'entry-000-concurrent')).toBeVisible();
  await page.addInitScript(() => {
    const original = window.fetch;
    window.fetch = async (...args) => {
      const response = await original(...args);
      if (args[1]?.method === 'PATCH') { await response.arrayBuffer(); throw new TypeError('Controlled response loss'); }
      return response;
    };
  });
  await page.reload();
  await drive(page).getByRole('button', { name: '打开我的空间', exact: true }).click();
  await row(page, 'entry-000-concurrent').getByRole('button', { name: '重命名', exact: true }).click();
  await submit(page, 'entry-000-unknown', '操作结果未知');
  await expect(row(page, 'entry-000-unknown')).toBeVisible();
  expect(writes).toBe(2);
});

test('two members and administrator: private isolation and public mixed-tree denial', async ({ page, site }) => {
  await login(page, site.origin);
  const invite = (await request(page, '/api/v1/admin/invites', 'POST', { max_uses: 2 }, undefined, 201)).body.token;
  const password = 'members-only-test-password'; site.secrets.push(invite, password);
  for (const name of ['alice', 'bob']) await request(page, '/api/v1/auth/register', 'POST', { login: name, display_name: name, password, invite_token: invite, credential_mode: 'cookie' }, undefined, 201);
  const aliceContext = await site.context(), bobContext = await site.context();
  const alice = await aliceContext.newPage(), bob = await bobContext.newPage();
  await login(alice, site.origin, 'alice', password); await login(bob, site.origin, 'bob', password);
  const aSpaces = (await request(alice, '/api/v1/spaces')).body.items;
  const bSpaces = (await request(bob, '/api/v1/spaces')).body.items;
  const privateB = bSpaces.find(s => s.kind === 'private');
  await request(alice, `/api/v1/nodes/${privateB.root_node_id}`, 'GET', undefined, undefined, 404);
  await request(page, `/api/v1/nodes/${privateB.root_node_id}`, 'GET', undefined, undefined, 404);
  const pub = aSpaces.find(s => s.kind === 'public');
  const shared = await mkdir(alice, pub, 'alice-shared');
  await mkdir(bob, shared, 'bob-owned-child');
  await open(alice, '公共');
  await row(alice, 'alice-shared').getByRole('button', { name: '移入回收站', exact: true }).click();
  await drive(alice).getByLabel('我已核对并确认此操作').check();
  await drive(alice).getByRole('button', { name: '确认移入回收站' }).click();
  await expect(drive(alice).getByRole('status')).toContainText('没有执行此操作的权限');
  await row(alice, 'alice-shared').getByRole('button', { name: '打开目录' }).click();
  await expect(row(alice, 'bob-owned-child')).toBeVisible();
  await expect(row(alice, 'bob-owned-child').getByRole('button', { name: '重命名', exact: true })).toHaveCount(0);
  await drive(alice).locator('summary').click();
  await expect(drive(alice).getByRole('heading', { name: '我的公共贡献配额' })).toBeVisible();
  await request(bob, `/api/v1/nodes/${shared.id}`);
});
