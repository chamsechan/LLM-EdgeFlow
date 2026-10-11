import assert from 'node:assert/strict';
import {mkdir} from 'node:fs/promises';
import {createRequire} from 'node:module';
import {resolve} from 'node:path';

const base = process.argv[2];
const modulePath = process.env.STUDIO_PLAYWRIGHT_MODULE;
if (!modulePath) {
  console.log('SKIP Studio browser: STUDIO_PLAYWRIGHT_MODULE is not configured');
  process.exit(0);
}
assert.ok(base, 'browser consumer requires the isolated Studio HTTP URL');
const require = createRequire(import.meta.url);
const {chromium} = require(resolve(modulePath));
const browser = await chromium.launch({
  headless: true,
  executablePath: process.env.STUDIO_CHROMIUM_PATH || undefined,
  args: process.env.STUDIO_BROWSER_NO_SANDBOX === '1' ? ['--no-sandbox'] : [],
});
const page = await browser.newPage({viewport: {width: 1440, height: 960}});
page.setDefaultTimeout(15000);
const errors = [];
page.on('pageerror', error => errors.push(error.message));
const jsonValue = () => page.locator('#rawJson').evaluate(element => JSON.parse(element.value));
const tab = name => page.locator(`[data-tab="${name}"]`).click();
async function applyDocument(document) {
  await tab('json');
  await page.locator('#rawJson').fill(JSON.stringify(document, null, 2));
  await page.locator('#applyJson').click();
  await page.waitForFunction(() => document.querySelector('#rawJsonHint').hidden);
}
async function savedDocument(name) {
  const response = await fetch(`${base}/api/pipeline?filename=${name}`);
  assert.equal(response.status, 200);
  return (await response.json()).pipeline;
}
async function open(name) {
  await page.locator('#pipelineSelect').selectOption(name);
  await page.locator('#openButton').click();
  await page.waitForFunction(name => document.querySelector('#documentTitle').textContent.includes(name), name);
  await page.locator('#nodes .node[data-node-id="match_keywords"]').waitFor();
}
try {
  await page.goto(`${base}/index.html`);
  await page.locator('#pipelineSelect option[value="pipeline_keyword_match_rules.json"]').waitFor({state: 'attached'});
  await open('pipeline_keyword_match_rules.json');
  const original = await jsonValue();
  assert.equal(await page.locator('#editModeButton').getAttribute('aria-pressed'), 'false');
  assert.ok(await page.locator('#graph .port.disabled').count() > 0);
  await page.locator('#edges .edge').first().click();
  assert.equal(await page.locator('#deleteEdgeButton').isDisabled(), true);

  // View operations are independent of document history and saved bytes.
  const transform = await page.locator('#viewport').getAttribute('transform');
  await page.locator('#zoomInButton').click();
  assert.notEqual(await page.locator('#viewport').getAttribute('transform'), transform);
  await page.locator('#actualSizeButton').click();
  assert.equal(await page.locator('#zoomLabel').textContent(), '100%');
  await page.locator('#layoutButton').click();
  assert.deepEqual(await jsonValue(), original);
  assert.deepEqual(await savedDocument('pipeline_keyword_match_rules.json'), original);
  const canvas = await page.locator('#graph').boundingBox();
  assert.ok(canvas.width > 600 && canvas.height > 300);

  await page.locator('#editModeButton').click();
  assert.equal(await page.locator('#editModeButton').getAttribute('aria-pressed'), 'true');
  await page.locator('#nodes .node[data-node-id="match_keywords"]').click();
  await page.locator('#nodeName').fill('renamed');
  assert.equal((await jsonValue()).pipeline[0].name, 'match_keywords');
  await page.locator('#applyNode').click();
  await page.locator('#nodes .node[data-node-id="renamed"]').waitFor();
  const renamed = await jsonValue();
  assert.equal(renamed.pipeline[0].name, 'renamed');
  assert.equal(renamed.io.output[0].inputs.matches, 'renamed.matches');
  await page.locator('#undoButton').click();
  assert.deepEqual(await jsonValue(), original);
  await page.locator('#redoButton').click();
  assert.deepEqual(await jsonValue(), renamed);

  // Invalid JSON remains an unapplied draft and retains the rendered graph.
  await tab('json');
  await page.locator('#rawJson').fill('{broken <script>');
  await page.locator('#applyJson').click();
  assert.equal(await page.locator('#rawJson').inputValue(), '{broken <script>');
  assert.ok(await page.locator('#nodes .node[data-node-id="renamed"]').count());
  await page.locator('#discardJson').click();
  assert.deepEqual(await jsonValue(), renamed);

  // Save-as calls the real HTTP transaction; complete unknown values survive.
  const complete = {...renamed, comment: '完整 Unicode\n第二行'};
  await applyDocument(complete);
  page.once('dialog', dialog => dialog.accept('pipeline_browser.json'));
  const saveResponse = page.waitForResponse(response => response.url().endsWith('/api/pipelines') && response.request().method() === 'POST');
  await page.locator('#saveAsButton').click();
  assert.equal((await saveResponse).status(), 200);
  await page.waitForFunction(() => document.querySelector('#dirtyBadge').textContent.includes('已保存'));
  assert.deepEqual(await savedDocument('pipeline_browser.json'), complete);
  assert.deepEqual(await savedDocument('pipeline_keyword_match_rules.json'), original);

  // Native fix candidates have a separate review step and one undo operation.
  const invalid = structuredClone(complete);
  delete invalid.pipeline[0].inputs.text;
  await applyDocument(invalid);
  await page.locator('#quickValidateButton').click();
  await page.locator('#validationOutput .fix-apply-btn').first().waitFor();
  await page.locator('#validationOutput .fix-apply-btn').first().click();
  await page.locator('#applyReviewedFix').waitFor();
  assert.deepEqual(await jsonValue(), invalid, 'review does not apply the candidate');
  assert.deepEqual(await savedDocument('pipeline_browser.json'), complete);
  await page.locator('#applyReviewedFix').click();
  await page.waitForFunction(() => document.querySelector('#validationOutput .diagnostic.ok'));
  assert.deepEqual(await jsonValue(), complete);
  await page.locator('#undoButton').click();
  assert.deepEqual(await jsonValue(), invalid);
  await page.locator('#redoButton').click();
  assert.deepEqual(await jsonValue(), complete);

  // Deleting a displayed data edge uses native authoring and is reversible.
  const inbound = page.locator('#edges .edge').filter({has: page.locator('title', {hasText: 'input.sentence_text'})}).first();
  await inbound.click();
  assert.equal(await page.locator('#deleteEdgeButton').isEnabled(), true);
  const disconnect = page.waitForResponse(response => response.url().endsWith('/api/authoring/preview'));
  await page.locator('#deleteEdgeButton').click();
  await disconnect;
  await page.waitForFunction(() => !JSON.parse(document.querySelector('#rawJson').value).pipeline[0].inputs?.text);
  await page.locator('#undoButton').click();
  assert.deepEqual(await jsonValue(), complete);

  // A new raw draft invalidates the previous fix review instead of applying it.
  await applyDocument(invalid);
  await page.locator('#quickValidateButton').click();
  await page.locator('#validationOutput .fix-apply-btn').first().waitFor();
  await page.locator('#validationOutput .fix-apply-btn').first().click();
  await page.locator('#applyReviewedFix').waitFor();
  const changed = {...invalid, comment: 'changed since review'};
  await applyDocument(changed);
  assert.equal(await page.locator('#fixReviewPanel').isHidden(), true);
  assert.deepEqual(await jsonValue(), changed);
  await applyDocument(complete);

  // Preflight and real Demo results retain the request snapshot after later edits.
  await tab('run');
  await page.locator('#runProfile').selectOption('keyword_match_rules');
  await page.locator('#preflightButton').click();
  await page.locator('#preflightSummary .preflight-status').waitFor();
  await page.locator('#runButton').click();
  await page.waitForFunction(() => /运行已完成|运行失败/.test(document.querySelector('#runSummary').textContent), null, {timeout: 60000});
  assert.match(await page.locator('#runSummary').textContent(), /运行已完成/);
  assert.ok(await page.locator('#runSamples .run-sample').count() > 0);
  const runText = await page.locator('#runSummary').textContent();
  await applyDocument({...complete, comment: 'edited after run'});
  await tab('run');
  assert.equal(await page.locator('#runSummary').textContent(), runText);
  assert.equal(await page.locator('#runFreshness').isVisible(), true);
  assert.deepEqual(errors, []);
  if (process.env.STUDIO_SCREENSHOT_DIR) {
    await mkdir(process.env.STUDIO_SCREENSHOT_DIR, {recursive: true});
    await page.screenshot({path: resolve(process.env.STUDIO_SCREENSHOT_DIR, 'studio-contract.png'), fullPage: true});
  }
  console.log('PASS actual Studio browser editing, history, fix review, saves and Demo rendering');
} finally {
  await browser.close();
}
