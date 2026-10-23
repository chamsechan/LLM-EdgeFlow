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
const requests = [];
page.on('request', request => requests.push(new URL(request.url()).pathname));
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
async function clickEdge(edge) {
  // Horizontal SVG groups have zero-height bounds. Click the actual stroked path.
  const point = await edge.locator('.edge-hit').evaluate(path => {
    const middle = path.getPointAtLength(path.getTotalLength() / 2);
    const screen = new DOMPoint(middle.x, middle.y).matrixTransform(path.getScreenCTM());
    return {x: screen.x, y: screen.y};
  });
  await page.mouse.click(point.x, point.y);
  assert.match(await edge.getAttribute('class'), /\bselected\b/);
}
try {
  await page.goto(`${base}/index.html`);
  await page.locator('#pipelineSelect option[value="pipeline_keyword_match_rules.json"]').waitFor({state: 'attached'});
  assert.equal(await page.locator('#welcomePanel').isVisible(), true);
  await page.locator('#welcomeCreate').click();
  assert.equal(await page.locator('.create-panel').evaluate(element => element.open), true);
  assert.equal(await page.locator('#createMode').inputValue(), 'template');
  assert.equal(await page.locator('#cloneProfile').inputValue(), '');
  assert.equal(await page.locator('#createIo').isHidden(), true);
  assert.equal(await page.locator('#newButton').isDisabled(), true);
  await page.locator('#createMode').selectOption('blank');
  assert.equal(await page.locator('#createIo').isVisible(), true);
  await page.locator('#createMode').selectOption('template');
  assert.equal(await page.locator('#createIo').isHidden(), true);
  assert.equal(await page.locator('#newButton').isDisabled(), true);

  if (await page.locator('#editModeButton').getAttribute('aria-pressed') === 'true') await page.locator('#editModeButton').click();
  await open('pipeline_keyword_match_rules.json');
  const original = await jsonValue();
  assert.equal(await page.locator('#welcomePanel').isHidden(), true);
  assert.equal(await page.locator('#editModeButton').getAttribute('aria-pressed'), 'false');
  assert.ok(await page.locator('#graph .port.disabled').count() > 0);
  await clickEdge(page.locator('#edges .edge').first());
  assert.equal(await page.locator('#deleteEdgeButton').isDisabled(), true);

  // View operations are independent of document history and saved bytes.
  const transform = await page.locator('#viewport').getAttribute('transform');
  await page.locator('#zoomInButton').click();
  assert.notEqual(await page.locator('#viewport').getAttribute('transform'), transform);
  await page.locator('#actualSizeButton').click();
  assert.equal(await page.locator('#zoomLabel').textContent(), '100%');
  await page.locator('#layoutButton').click();
  assert.equal(await page.locator('#focusNodeButton').isDisabled(), true);
  await page.locator('#nodes .node[data-node-id="match_keywords"]').click();
  assert.equal(await page.locator('#focusNodeButton').isEnabled(), true);
  const savedBadge = await page.locator('#dirtyBadge').textContent();
  await page.locator('#focusNodeButton').click();
  assert.ok(Number.parseInt(await page.locator('#zoomLabel').textContent(), 10) >= 100);
  assert.equal(await page.locator('#dirtyBadge').textContent(), savedBadge);
  await page.locator('#fitButton').click();
  assert.deepEqual(await jsonValue(), original);
  assert.deepEqual(await savedDocument('pipeline_keyword_match_rules.json'), original);
  const canvas = await page.locator('#graph').boundingBox();
  assert.ok(canvas.width > 600 && canvas.height > 300);

  // Start from a real template, then edit its rule through the node form.
  await page.locator('#newEntryButton').click();
  await page.locator('#cloneProfile').selectOption('keyword_match_rules');
  assert.equal(await page.locator('#newButton').isEnabled(), true);
  const initialized = page.waitForResponse(response => response.url().endsWith('/api/init'));
  await page.locator('#newButton').click();
  const initializedResponse = await initialized;
  assert.equal(initializedResponse.status(), 200);
  assert.deepEqual(initializedResponse.request().postDataJSON(), {profile: 'keyword_match_rules'});
  await page.waitForFunction(() => document.querySelector('#dirtyBadge').textContent === '未保存');
  assert.deepEqual(await jsonValue(), original);
  if (await page.locator('#editModeButton').getAttribute('aria-pressed') === 'false') await page.locator('#editModeButton').click();
  assert.equal(await page.locator('#editModeButton').getAttribute('aria-pressed'), 'true');
  await page.locator('#nodes .node[data-node-id="match_keywords"]').click();
  await page.locator('#nodeName').fill('renamed');
  const categories = {BROWSER_VIP: ['VIP']};
  await page.locator('#configFields [data-field="categories"]').fill(JSON.stringify(categories));
  assert.equal((await jsonValue()).pipeline[0].name, 'match_keywords');
  await page.locator('#openRunButton').click();
  assert.equal(await page.locator('#runTab').isVisible(), true);
  assert.deepEqual(await jsonValue(), original, 'opening run settings must not apply node drafts');
  await tab('properties');
  assert.equal(await page.locator('#nodeName').inputValue(), 'renamed');
  await page.locator('#applyNode').click();
  await page.locator('#nodes .node[data-node-id="renamed"]').waitFor();
  const renamed = await jsonValue();
  assert.equal(renamed.pipeline[0].name, 'renamed');
  assert.deepEqual(renamed.pipeline[0].params.categories, categories);
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
  const validationsBeforeApply = requests.filter(path => path === '/api/validate').length;
  await page.locator('#applyReviewedFix').click();
  await page.waitForFunction(() => document.querySelector('#validationOutput .diagnostic.ok'));
  assert.equal(requests.filter(path => path === '/api/validate').length, validationsBeforeApply);
  assert.deepEqual(await jsonValue(), complete);
  await page.locator('#undoButton').click();
  assert.deepEqual(await jsonValue(), invalid);
  await page.locator('#redoButton').click();
  assert.deepEqual(await jsonValue(), complete);

  // Deleting a displayed data edge uses native authoring and is reversible.
  const inbound = page.locator('#edges .edge').filter({has: page.locator('title', {hasText: 'input.sentence_text'})}).first();
  await page.locator('#fitButton').click();
  await clickEdge(inbound);
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

  // An unavailable converter remains lossless when another I/O entry is edited.
  const unknown = structuredClone(complete);
  const input = {type: 'future_input', name: 'future_request', params: {nested: {values: ['保留', 7]}}};
  const output = {type: 'future_output', name: 'future_response', params: {capacity: 2048, custom: true}};
  unknown.io.input.push(input);
  unknown.io.output.push(output);
  await applyDocument(unknown);
  await page.locator('#fitButton').click();
  for (const id of ['input', 'output']) {
    await page.locator('#nodes .node[data-node-id="' + id + '"]').click();
    assert.equal(await page.locator('#ioSettings').evaluate(element => element.open), true);
    await page.locator('#ioSettings > summary').click();
  }
  await page.locator('#ioSettings > summary').click();
  await page.locator('#ioInputs').getByRole('button', {name: '添加输入', exact: true}).click();
  await page.locator('#applyIo').click();
  assert.deepEqual((await jsonValue()).io.input[1], input);
  assert.deepEqual((await jsonValue()).io.output[1], output);
  await page.locator('#applyIo').click();
  assert.deepEqual((await jsonValue()).io.input[1], input);
  assert.deepEqual((await jsonValue()).io.output[1], output);
  await tab('run');
  assert.equal(await page.locator('#runProfile').inputValue(), '');
  assert.equal(await page.locator('#runProfileHint').isVisible(), true);
  assert.match(await page.locator('#runProfileHint').textContent(), /示例/);
  assert.equal(await page.locator('#runButton').isDisabled(), true);
  await applyDocument(complete);

  // Preflight and real Demo results retain the request snapshot after later edits.
  await page.locator('#quickValidateButton').click();
  await page.locator('#validationOutput .diagnostic.ok').waitFor();
  await tab('run');
  await page.locator('#runProfile').selectOption('keyword_match_rules');
  await page.locator('#preflightButton').click();
  await page.locator('#preflightSummary .preflight-status').waitFor();
  await page.locator('#runButton').click();
  await page.waitForFunction(() => /运行已完成|运行失败/.test(document.querySelector('#runSummary').textContent), null, {timeout: 60000});
  assert.match(await page.locator('#runSummary').textContent(), /运行已完成/);
  assert.equal(await page.locator('#runSamples .run-sample').count(), 2);
  const result = JSON.parse(await page.locator('#runResult').textContent());
  assert.equal(result['summary.json'].success_count, 2);
  assert.equal(result['summary.json'].failed_count, 0);
  const [hit, miss] = result['results.jsonl'].map(record => record.output);
  assert.equal(hit.is_hit, 1);
  assert.equal(hit.intent, 'BROWSER_VIP');
  assert.deepEqual(hit.matches, [{category: 'BROWSER_VIP', matched_word: 'VIP'}]);
  assert.equal(miss.is_hit, 0);
  assert.deepEqual(miss.matches, []);
  // The fixture blocks command attachments; committed files must still be shown as saved.
  page.once('dialog', dialog => dialog.accept('pipeline_browser_solution.json'));
  const solutionResponse = page.waitForResponse(response => response.url().endsWith('/api/solutions'));
  await page.locator('#saveSolutionButton').click();
  assert.equal((await solutionResponse).status(), 200);
  await page.waitForFunction(() => document.querySelector('#dirtyBadge').textContent === '已保存');
  assert.match(await page.locator('#operationFeedback').textContent(), /已保存.*运行命令未生成/);
  assert.match(await page.locator('#savedCommand').textContent(), /运行命令未生成/);
  assert.deepEqual(await savedDocument('pipeline_browser_solution.json'), complete);
  const runText = await page.locator('#runSummary').textContent();
  await applyDocument({...complete, comment: 'edited after run'});
  await tab('run');
  assert.equal(await page.locator('#runSummary').textContent(), runText);
  assert.equal(await page.locator('#runFreshness').isVisible(), true);
  // The primary controls stay reachable on ordinary desktop widths.
  if (process.env.STUDIO_SCREENSHOT_DIR) await mkdir(process.env.STUDIO_SCREENSHOT_DIR, {recursive: true});
  for (const {width, height} of [{width: 1280, height: 768}, {width: 1366, height: 768}, {width: 1920, height: 1080}]) {
    await page.setViewportSize({width, height});
    await page.locator('#fitButton').click();
    await page.locator('#nodes .node[data-node-id="renamed"]').click();
    await page.locator('#focusNodeButton').click();
    assert.equal(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth), true, `horizontal overflow at ${width}`);
    for (const id of ['newEntryButton', 'openButton', 'saveButton', 'editModeButton', 'quickValidateButton', 'openRunButton', 'focusNodeButton', 'fitButton']) {
      const button = page.locator('#' + id);
      assert.equal(await button.isVisible(), true, `${id} is hidden at ${width}`);
      const box = await button.boundingBox();
      assert.ok(box.x >= 0 && box.y >= 0 && box.x + box.width <= width && box.y + box.height <= height, `${id} is off-screen at ${width}`);
    }
    if (process.env.STUDIO_SCREENSHOT_DIR) await page.screenshot({path: resolve(process.env.STUDIO_SCREENSHOT_DIR, `studio-contract-${width}.png`), fullPage: true});
  }
  assert.equal(requests.filter(path => path === '/api/catalog').length, 1);
  assert.equal(requests.filter(path => path === '/api/profiles').length, 0);
  assert.deepEqual(errors, []);
  console.log('PASS actual Studio browser creation, focused reading, editing, history, fix review, saves and edited-rule Demo results');
} finally {
  await browser.close();
}
