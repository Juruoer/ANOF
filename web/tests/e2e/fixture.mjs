import { sanitizeErrors } from './sanitize-errors.mjs';
import { test as base, expect, chromium, firefox } from '@playwright/test';
import { spawn, execFileSync } from 'node:child_process';
import { mkdirSync, mkdtempSync, accessSync, constants } from 'node:fs';
import { resolve, join } from 'node:path';

export const adminPassword = 'fixture-admin-password-only';
export const test = base.extend({
  transferDownloads: [false, { option: true }],
  safeErrors: [async ({}, use, info) => {
    try { await use(); } finally { sanitizeErrors(info); }
  }, { auto: true }],
  site: async ({ browserName, transferDownloads }, use) => {
    const executable = (key) => {
      const path = process.env[key];
      if (!path || !path.startsWith('/')) throw Error(`Explicit executable required: ${key}`);
      accessSync(path, constants.X_OK); return path;
    };
    const certutil = executable('ANOF_TEST_CERTUTIL');
    const child = spawn('/usr/bin/python3', [resolve('tests/e2e/server.py'),
      '--anof', executable('ANOF_TEST_ANOF'), '--nginx', executable('ANOF_TEST_NGINX'),
      '--openssl', executable('ANOF_TEST_OPENSSL'), '--dist', resolve('dist')], { stdio: ['pipe', 'pipe', 'pipe'] });
    let diagnostic = '';
    child.stderr.on('data', chunk => { diagnostic = (diagnostic + chunk).slice(-4096); });
    const ended = new Promise(resolve => child.on('exit', code => resolve(code)));
    const contexts = [];
    const secrets = [];
    const secretReads = [];
    let secretOverflow = false;
    try {
      const info = await new Promise((resolve, reject) => {
        const timer = setTimeout(() => reject(Error('Fixture readiness deadline')), 30_000);
        let output = '';
        child.once('exit', () => { clearTimeout(timer); reject(Error(`Fixture startup: ${diagnostic}`)); });
        child.stdout.on('data', chunk => {
          output += chunk;
          if (output.length > 8192) { clearTimeout(timer); reject(Error('Fixture output limit')); }
          if (output.includes('\n')) { clearTimeout(timer); try { resolve(JSON.parse(output.trim())); } catch { reject(Error('Invalid fixture readiness')); } }
        });
      });
      const site = { ...info, secrets, async context(trust = true) {
        const directory = mkdtempSync(join(info.base, 'browser-'));
        const profile = join(directory, 'profile'); mkdirSync(profile);
        const browserHome = join(directory, 'home'); mkdirSync(browserHome);
        const database = browserName === 'firefox' ? profile : join(browserHome, '.local/share/pki/nssdb');
        mkdirSync(database, { recursive: true });
        if (trust) {
          execFileSync(certutil, ['-N', '--empty-password', '-d', `sql:${database}`], { timeout: 5000, stdio: 'pipe' });
          execFileSync(certutil, ['-A', '-d', `sql:${database}`, '-n', 'ANOF private test CA', '-t', 'C,,', '-i', info.ca], { timeout: 5000, stdio: 'pipe' });
        }
        const context = await ({ chromium, firefox }[browserName]).launchPersistentContext(profile, {
          headless: true, ignoreHTTPSErrors: false, acceptDownloads: transferDownloads, serviceWorkers: 'block',
          env: { ...process.env, HOME: browserHome, XDG_CACHE_HOME: join(directory, 'cache'), XDG_CONFIG_HOME: join(directory, 'config') },
          timeout: 15_000,
        });
        context.setDefaultTimeout(8000); context.setDefaultNavigationTimeout(10_000);
        context.on('response', response => {
          if (!/\/api\/v1\/(auth\/sessions?|users\/me\/password)$/u.test(new URL(response.url()).pathname)) return;
          if (secretReads.length >= 128) { secretOverflow = true; return; }
          secretReads.push(response.headerValue('set-cookie').then(value => {
            const token = value?.match(/__Host-anof_session=([A-Za-z0-9_-]{43});/u)?.[1];
            if (token) secrets.push(token);
          }));
        });
        contexts.push(context); return context;
      }};
      await use(site);
    } finally {
      let closeFailed = false;
      if ((await Promise.allSettled(secretReads)).some(r => r.status === 'rejected') || secretOverflow) closeFailed = true;
      for (const context of contexts.reverse()) {
        try { await context.close(); } catch { closeFailed = true; }
      }
      if (child.exitCode === null) child.stdin.end(JSON.stringify({ stop: true, secrets }) + '\n');
      const timer = setTimeout(() => child.kill('SIGTERM'), 25_000);
      const code = await ended; clearTimeout(timer);
      if (code !== 0) throw Error(`Fixture shutdown failed: ${diagnostic}`);
      if (closeFailed) throw Error('Browser context cleanup failed');
    }
  },
  context: async ({ site }, use) => { await use(await site.context()); },
});
export { expect };
export async function login(page, origin, name = 'administrator', password = adminPassword) {
  let bootstrapStatus = null, loginStatus = null;
  const observe = response => {
    if (response.url() === origin + '/api/v1/users/me') bootstrapStatus = response.status();
    if (response.url() === origin + '/api/v1/auth/sessions' && response.request().method() === 'POST') loginStatus = response.status();
  };
  page.on('response', observe);
  try {
  await page.goto(origin + '/#/login');
  await page.getByLabel('登录名', { exact: true }).fill(name);
  await page.getByLabel('密码', { exact: true }).fill(password);
  const request = page.waitForRequest(request => request.url().endsWith('/api/v1/auth/sessions') && request.method() === 'POST');
  await page.getByRole('button', { name: '登录', exact: true }).click();
  const headers = await (await request).allHeaders();
  expect(headers.origin === origin && headers['x-anof-request'] === '1').toBe(true);
  try { await expect(page.getByRole('heading', { name: '账户资料' })).toBeVisible(); }
  catch { throw Error('Login view was not confirmed'); }
  } catch (error) {
    // 仅固定字段和 HTTP 状态，不输出表单、Cookie、URL 或服务端正文。
    test.info().annotations.push({type:'safe-failure',description:`Login diagnostic: bootstrap=${bootstrapStatus}, login=${loginStatus}`});
    throw error;
  } finally { page.off('response', observe); }
}
