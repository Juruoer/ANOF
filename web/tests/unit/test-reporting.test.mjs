import { test } from 'node:test';
import assert from 'node:assert/strict';
import { sanitizeErrors } from '../e2e/sanitize-errors.mjs';
test('browser diagnostics retain failures and source locations without credential-bearing error context', () => {
  const secret = 'test-only-invitation-secret';
  const info = { status: 'failed', errors: [{ message: `locator.fill: ${secret}`, stack: `Error: ${secret}\n    at /workspace/tests/e2e/admin.spec.mjs:9:3`, value: secret, snippet: secret, errorContext: secret }], annotations: [] };
  sanitizeErrors(info);
  assert.equal(info.status, 'failed'); assert.equal(info.errors.length, 1);
  assert.equal(info.errors[0].message, undefined);
  assert.equal(JSON.stringify(info).includes(secret), false);
  assert.match(info.errors[0].stack, /admin.spec.mjs:9:3/);
  assert.equal(info.annotations[0].type, 'safe-failure');
});
