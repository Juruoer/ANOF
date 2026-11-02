import {readdirSync, readFileSync} from 'node:fs';
import {defineConfig} from '@playwright/test';

process.env.PLAYWRIGHT_NO_COPY_PROMPT = '1';
// 两引擎的各用例最大期限之和，加一分钟生命周期余量；不延长单例或增加重试。
const suiteBudget = readdirSync(new URL('.', import.meta.url)).filter(f => f.endsWith('.spec.mjs')).reduce((n, f) => n + (readFileSync(new URL(f, import.meta.url), 'utf8').match(/^test\(/gm)?.length ?? 0) * (f === 'transfer.spec.mjs' ? 120_000 : 60_000), 0) * 2 + 60_000;

export default defineConfig({
    testDir: '.',
    testMatch: '**/*.spec.mjs',
    fullyParallel: false,
    workers: 1,
    retries: 0,
    forbidOnly: true,
    timeout: 60_000,
    globalTimeout: suiteBudget,
    maxFailures: 1,
    reporter: [['./safe-reporter.mjs']],
    outputDir: '../../test-results/e2e',
    use: {
        headless: true,
        ignoreHTTPSErrors: false,
        actionTimeout: 10_000,
        navigationTimeout: 15_000,
        trace: 'off',
        screenshot: 'off',
        video: 'off',
        serviceWorkers: 'block',
    },
    projects: [
        {name: 'chromium', use: {browserName: 'chromium'}},
        {name: 'firefox', use: {browserName: 'firefox'}, dependencies: ['chromium']},
    ],
});
