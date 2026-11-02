import assert from 'node:assert/strict';
import { execFileSync } from 'node:child_process';
import { readFile } from 'node:fs/promises';

const root = new URL('../', import.meta.url);
const json = async (path) => JSON.parse(await readFile(new URL(path, root), 'utf8'));
const manifest = await json('package.json');
const lock = await json('package-lock.json');
const tools = await json('toolchain-lock.json');

assert.equal(`${process.platform}-${process.arch}`, tools.platform);
assert.equal(process.versions.node, tools.node.version);
assert.equal((await readFile(new URL('.node-version', root), 'utf8')).trim(), tools.node.version);
assert.equal(manifest.engines.node, tools.node.version);
assert.equal(manifest.engines.npm, tools.node.npm);
assert.equal(manifest.packageManager, `npm@${tools.node.npm}`);
assert.ok(process.env.npm_execpath, 'Run tools:check via npm');
assert.equal(execFileSync(process.execPath, [process.env.npm_execpath, '--version'], {encoding:'utf8',timeout:5000}).trim(), tools.node.npm);
assert.deepEqual(lock.packages[''].devDependencies, manifest.devDependencies);
assert.deepEqual(lock.packages[''].dependencies, manifest.dependencies);
assert.equal(Object.keys(manifest.devDependencies).length + Object.keys(manifest.dependencies ?? {}).length, tools.packages.length);

for (const pkg of tools.packages) {
  const installed = await json(`node_modules/${pkg.name}/package.json`);
  const resolved = lock.packages[`node_modules/${pkg.name}`];
  assert.equal((pkg.runtime ? manifest.dependencies : manifest.devDependencies)[pkg.name], pkg.version);
  assert.equal(installed.version, pkg.version);
  assert.equal(installed.license, pkg.license);
  assert.equal(resolved.version, pkg.version);
  assert.equal(resolved.resolved, pkg.tarball);
  assert.equal(resolved.integrity, pkg.integrity);
}
for (const [path, pkg] of Object.entries(lock.packages)) {
  if (path === '') continue;
  assert.equal(pkg.dev === true, path !== 'node_modules/hash-wasm', path);
  assert.ok(pkg.resolved.startsWith('https://registry.npmjs.org/'), path);
  assert.ok(pkg.integrity.startsWith('sha512-'), path);
}

const browserManifest = await json('node_modules/playwright-core/browsers.json');
for (const browser of tools.browsers) {
  assert.deepEqual(browserManifest.browsers.find((item) => item.name === browser.name), browser);
}
console.log(`Linux Node ${tools.node.version}, npm ${tools.node.npm}: toolchain locks match installed packages`);
