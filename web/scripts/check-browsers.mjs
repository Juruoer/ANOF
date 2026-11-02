import assert from 'node:assert/strict';
import {readFile} from 'node:fs/promises';
import {chromium, firefox} from 'playwright';

const lock = JSON.parse(await readFile(new URL('../toolchain-lock.json', import.meta.url), 'utf8'));

// 仅验证工具可启动，不访问产品端点，不保存登录状态或把它算作端到端验收。
for (const [name, engine] of Object.entries({chromium, firefox})) {
    let browser;
    try {
        browser = await engine.launch({headless: true, timeout: 15_000});
        const expected = lock.browsers.find((item) => item.name === name);
        assert.ok(expected, `Missing browser lock: ${name}`);
        assert.equal(browser.version(), expected.browserVersion);
        const page = await browser.newPage();
        const result = await page.evaluate(() => ({
            decimal: BigInt('9223372036854775807').toString(),
            abort: typeof AbortController,
            decoder: typeof TextDecoder,
        }));
        assert.deepEqual(result, {decimal: '9223372036854775807', abort: 'function', decoder: 'function'});
        console.log(`${name} ${browser.version()}: tool smoke passed`);
    } finally {
        await browser?.close();
    }
}
