import { mkdirSync, writeFileSync } from 'node:fs';
// 不输出原始错误/调用日志：表单填充失败可能包含密码和邀请秘密。
export default class SafeReporter {
  results = [];
  onTestEnd(test, result) {
    this.results.push({ project: test.parent.project().name, title: test.title, status: result.status, retry: result.retry });
    console.log(`${test.parent.project().name}: ${test.title}: ${result.status}`);
    for (const annotation of test.annotations) if (annotation.type === 'safe-failure') console.log(annotation.description);
    for (const error of result.errors) {
      if ((error.message ?? '').startsWith('Error: UI diagnostic:')) console.log(error.message);
      if (/^Error: Fixture (startup|shutdown failed): Browser fixture failed: [A-Za-z0-9 :]+\s*$/u.test(error.message ?? '')) console.log(error.message.trim());
      const location = error.stack?.split('\n').find(line => /at .*tests\/e2e\/.*:\d+:\d+/u.test(line));
      if (location) console.log(location.trim());
    }
  }
  onError() { console.log('Browser runner error; inspect executable prerequisites and fixture lifecycle.'); }
  onEnd(result) {
    mkdirSync('test-results/e2e', { recursive: true });
    writeFileSync('test-results/e2e/summary.json', JSON.stringify({ status: result.status, cases: this.results }, null, 2) + '\n');
    console.log(`Browser suite: ${result.status}`);
  }
}
