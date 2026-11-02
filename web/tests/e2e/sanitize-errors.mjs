// Playwright 的 NO_COPY_PROMPT 只关闭页面快照；普通断言仍可能生成含源码/填充值的 error-context。
// 在其内置报告收尾之前移除原始错误文本，保留失败数量和安全位置；不更改测试状态或重试策略。
export function sanitizeErrors(info) {
  for (const error of info.errors) {
    const location = error.stack?.split('\n').find(line => /^\s*at .*tests\/e2e\/[^\n]+:\d+:\d+\)?$/u.test(line));
    const category = /timeout/iu.test(error.message ?? '') ? 'timeout' : /Fixture/iu.test(error.message ?? '') ? 'fixture failure' : 'assertion or operation failure';
    info.annotations.push({ type: 'safe-failure', description: category + (location ? `; ${location.trim()}` : '') });
    for (const key of Object.keys(error)) delete error[key];
    if (location) error.stack = location.trim();
  }
}
