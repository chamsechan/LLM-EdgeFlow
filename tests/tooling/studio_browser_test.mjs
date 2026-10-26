import assert from 'node:assert/strict';
import {mkdir, readFile} from 'node:fs/promises';
import {createRequire} from 'node:module';
import {resolve} from 'node:path';

const base = process.argv[2], modulePath = process.env.STUDIO_PLAYWRIGHT_MODULE;
if (!modulePath) { console.log('SKIP Studio browser: STUDIO_PLAYWRIGHT_MODULE is not configured'); process.exit(0); }
const {chromium} = createRequire(import.meta.url)(resolve(modulePath));
const browser = await chromium.launch({headless: true, executablePath: process.env.STUDIO_CHROMIUM_PATH || undefined,
  args: process.env.STUDIO_BROWSER_NO_SANDBOX === '1' ? ['--no-sandbox'] : []});
const page = await browser.newPage({viewport: {width: 1366, height: 768}});
page.setDefaultTimeout(15000);
const errors = [];
page.on('pageerror', error => errors.push(error.message));
let saveName = 'browser_keywords';
const savedFile = () => `pipeline_${saveName}.json`;
page.on('dialog', dialog => dialog.accept(dialog.type() === 'prompt' ? saveName : undefined));
const jsonValue = () => page.locator('#rawJson').evaluate(element => JSON.parse(element.value));
async function tab(name) {
  if (['json', 'models'].includes(name)) await page.locator('.advanced-menu').evaluate(element => { element.open = true; });
  await page.locator(`[data-tab="${name}"]`).click();
}
async function applyDocument(value) {
  await tab('json'); await page.locator('#rawJson').fill(JSON.stringify(value, null, 2));
  await page.locator('#applyJson').click(); await page.waitForFunction(() => document.querySelector('#rawJsonHint').hidden);
}
async function savedDocument(name) {
  const response = await fetch(`${base}/api/pipeline?filename=${encodeURIComponent(name)}`);
  assert.equal(response.status, 200); return (await response.json()).pipeline;
}
async function clickEdge(edge) {
  const point = await edge.locator('.edge-hit').evaluate(path => {
    const p = path.getPointAtLength(path.getTotalLength() / 2), s = new DOMPoint(p.x, p.y).matrixTransform(path.getScreenCTM());
    return {x: s.x, y: s.y};
  });
  await page.mouse.click(point.x, point.y);
}
async function save(as = false) {
  const response = page.waitForResponse(r => r.url().endsWith(as ? '/api/pipelines' : '/api/pipeline') && ['PUT', 'POST'].includes(r.request().method()));
  await page.locator(as ? '#saveAsButton' : '#saveButton').click();
  assert.equal((await response).status(), 200);
  await page.waitForFunction(() => document.querySelector('#dirtyBadge').textContent === '已保存');
}
try {
  await page.goto(base);
  await page.locator('#pipelineSelect option[value="pipeline_keyword_match_rules.json"]').waitFor({state: 'attached'});
  assert.equal(await page.locator('#welcomePanel').isVisible(), true);
  await page.locator('#welcomeCreate').click();
  await page.locator('#createMode').selectOption('blank');
  assert.equal(await page.locator('#createIo').isVisible(), true);
  await page.locator('#createMode').selectOption('template');
  assert.equal(await page.locator('#createIo').isHidden(), true);
  await page.locator('#cloneProfile').selectOption('keyword_match_rules');
  assert.match(await page.locator('#cloneProfile').textContent(), /keyword_match_rules/);
  await page.locator('#newButton').click();
  await page.locator('.node[data-node-id="match_keywords"]').waitFor();
  await page.locator('#operatorsToggle').click();
  const original = await jsonValue();
  await page.locator('.node[data-node-id="match_keywords"]').click();
  assert.equal(await page.locator('#nodeForm').isVisible(), true);
  assert.equal(await page.locator('#configFields [data-collection="categories"]').isVisible(), true);
  const category = page.locator('[data-collection="categories"]');
  await category.getByLabel('键', {exact: true}).fill('BROWSER_VIP');
  await category.getByLabel('值，每行一个').fill('VIP');
  await category.getByLabel('值，每行一个').press('Tab');
  await page.waitForFunction(() => JSON.parse(document.querySelector('#rawJson').value).pipeline[0].params.categories.BROWSER_VIP?.[0] === 'VIP');
  const edited = await jsonValue();
  await page.locator('#undoButton').click();
  assert.notDeepEqual((await jsonValue()).pipeline[0].params.categories, edited.pipeline[0].params.categories);
  await page.locator('#redoButton').click();
  assert.deepEqual(await jsonValue(), edited);

  // A rule can be added, edited and removed without writing JSON; unset optional fields sit under 更多参数.
  await page.locator('#configFields > details > summary', {hasText: '更多参数'}).click();
  await page.locator('[data-collection="rules"] > button').click();
  const rule = page.locator('[data-collection="rules"] .collection-row');
  await rule.locator('[data-field="pattern"]').fill('专席');
  await rule.locator('[data-field="pattern"]').press('Tab');
  assert.equal((await jsonValue()).pipeline[0].params.rules[0].pattern, '专席');
  const constants = rule.locator('[data-field="constants"]');
  await constants.fill('{invalid');
  await page.locator('#saveButton').click();
  assert.equal(await constants.evaluate(input => input.checkValidity()), false);
  await constants.fill('{"route":"vip"}'); await constants.press('Tab');
  assert.deepEqual((await jsonValue()).pipeline[0].params.rules[0].constants, {route: 'vip'});
  await rule.getByRole('button', {name: '删除此项', exact: true}).click();
  const complete = await jsonValue();
  await save(true);
  assert.deepEqual(await savedDocument(savedFile()), complete);
  assert.deepEqual(await savedDocument('pipeline_keyword_match_rules.json'), original);

  // One Save click also completes a rename whose native preview is still pending.
  await page.locator('#nodeForm details').filter({has: page.locator('#nodeName')}).evaluate(element => { element.open = true; });
  let releasePreview, previewSeen;
  const previewBlocked = new Promise(resolve => { previewSeen = resolve; });
  const previewGate = new Promise(resolve => { releasePreview = resolve; });
  const holdPreview = async route => { previewSeen(); await previewGate; await route.continue(); };
  await page.route('**/api/authoring/preview', holdPreview);
  let saveRequests = 0;
  const countSave = request => { if (request.url().endsWith('/api/pipeline') && request.method() === 'PUT') saveRequests++; };
  page.on('request', countSave);
  await page.locator('#nodeName').fill('renamed_keywords');
  const renameSaved = page.waitForResponse(response => response.url().endsWith('/api/pipeline') && response.request().method() === 'PUT');
  await page.locator('#saveButton').click();
  await previewBlocked;
  assert.equal(saveRequests, 0, 'saving must wait for the rename preview');
  releasePreview();
  assert.equal((await renameSaved).status(), 200);
  await page.waitForFunction(() => document.querySelector('#dirtyBadge').textContent === '已保存');
  assert.equal(saveRequests, 1, 'one click must produce exactly one save');
  page.off('request', countSave); await page.unroute('**/api/authoring/preview', holdPreview);
  const renamed = await savedDocument(savedFile());
  assert.equal(renamed.pipeline[0].name, 'renamed_keywords');
  assert.equal(renamed.io.output[0].inputs.matches, 'renamed_keywords.matches');
  await applyDocument(complete); await save();

  // Invalid collection drafts can be abandoned without losing an earlier completed edit.
  const retained = {...structuredClone(complete), comment: 'keep this completed edit'};
  await applyDocument(retained);
  await page.locator('.node[data-node-id="match_keywords"]').click();
  await category.getByLabel('键', {exact: true}).fill('');
  await tab('run'); await page.locator('#saveButton').click();
  assert.equal(await page.locator('#nodeForm').isVisible(), true);
  assert.equal(await category.getByLabel('键', {exact: true}).evaluate(input => input === document.activeElement && !input.checkValidity()), true);
  assert.equal(await page.locator('#undoButton').isDisabled(), true);
  await page.locator('#discardNode').click();
  assert.deepEqual(await jsonValue(), retained);
  assert.equal(await page.locator('#undoButton').isEnabled(), true);
  await tab('json'); assert.equal(await page.locator('#rawJson').isEditable(), true);
  await page.locator('#undoButton').click(); assert.deepEqual(await jsonValue(), complete);
  await page.locator('#redoButton').click(); assert.deepEqual(await jsonValue(), retained);
  const twoCategories = structuredClone(retained);
  twoCategories.pipeline[0].params.categories.SECOND = ['second'];
  await applyDocument(twoCategories); await page.locator('.node[data-node-id="match_keywords"]').click();
  await category.getByLabel('键', {exact: true}).nth(1).fill('BROWSER_VIP');
  await tab('run'); await page.locator('#saveButton').click();
  assert.equal(await page.locator('#nodeForm').isVisible(), true);
  assert.equal(await category.getByLabel('键', {exact: true}).evaluateAll(inputs => inputs.some(input => input === document.activeElement && !input.checkValidity())), true);
  await page.locator('#discardNode').click(); assert.deepEqual(await jsonValue(), twoCategories);
  await applyDocument(complete);

  // Abandoning invalid I/O input restores all converters and does not add history.
  await applyDocument(retained); await page.locator('.node[data-node-id="output"]').click();
  const outputCapacity = page.locator('#ioOutputs [data-field="result_json_max_bytes"]');
  await outputCapacity.fill('-1'); await tab('run'); await page.locator('#saveButton').click();
  assert.equal(await outputCapacity.evaluate(input => input === document.activeElement && !input.checkValidity()), true);
  await page.locator('#discardIo').click(); assert.deepEqual(await jsonValue(), retained);
  assert.equal(await page.locator('#undoButton').isEnabled(), true);
  await page.locator('#undoButton').click(); assert.deepEqual(await jsonValue(), complete);
  await page.locator('#redoButton').click(); assert.deepEqual(await jsonValue(), retained);
  await applyDocument(complete);

  // Node discovery uses Catalog names and descriptions, with an explicit no-results state.
  await page.locator('#operatorsToggle').click();
  await page.locator('#operatorSearch').fill('keyword');
  assert.ok(await page.locator('#operatorList .operator').count() > 0);
  await page.locator('#operatorSearch').fill('不存在的操作');
  assert.match(await page.locator('#operatorList').textContent(), /没有找到/);
  await page.locator('#operatorSearch').fill('');
  await page.locator('#operatorsToggle').click();

  // Incomplete work saves, but cannot execute. Connecting works by clicks and near-port drops.
  await page.locator('#fitButton').click();
  await clickEdge(page.locator('#edges .edge').first());
  await page.locator('#deleteEdgeButton').click();
  await page.waitForFunction(() => !JSON.parse(document.querySelector('#rawJson').value).pipeline[0].inputs?.text);
  await save();
  assert.equal((await savedDocument(savedFile())).pipeline[0].inputs?.text, undefined);
  await page.locator('#openRunButton').click();
  const rejected = page.waitForResponse(r => r.url().endsWith('/api/runs'));
  await page.locator('#runButton').click();
  assert.equal((await rejected).status(), 400);
  await page.locator('#fitButton').click();
  const source = page.locator('.node[data-node-id="input"] .port.output');
  const target = page.locator('.node[data-node-id="match_keywords"] .port.input');
  // The second click uses the same snapping as a drop: near a highlighted port, not only on it.
  await source.click();
  assert.match(await target.getAttribute('class'), /candidate/);
  const near = await target.boundingBox();
  await page.mouse.click(near.x + near.width / 2, near.y + near.height / 2 - 18);
  await page.waitForFunction(() => JSON.parse(document.querySelector('#rawJson').value).pipeline[0].inputs?.text === 'input.sentence_text');
  await page.locator('#fitButton').click();
  const a = await source.boundingBox(), b = await target.boundingBox();
  const connected = page.waitForResponse(r => r.url().endsWith('/api/authoring/preview'));
  await page.mouse.move(a.x + a.width / 2, a.y + a.height / 2); await page.mouse.down();
  await page.mouse.move(b.x + b.width / 2, b.y + b.height / 2 - 20, {steps: 8}); await page.mouse.up();
  assert.equal((await connected).status(), 200);
  await page.waitForFunction(() => document.querySelector('#dirtyBadge').textContent !== '加载中…');
  await source.click(); await page.keyboard.press('Escape');
  assert.equal(await page.locator('.port.candidate').count(), 0);
  // Keyboard selection keeps focus on the chosen port so Tab continues from there.
  await source.focus(); await page.keyboard.press('Enter');
  assert.equal(await source.evaluate(port => port === document.activeElement), true);
  await page.keyboard.press('Escape');
  assert.equal(await page.locator('.port.candidate').count(), 0);

  // Reviewed native fixes remain reversible and do not commit during preview.
  const broken = structuredClone(complete); delete broken.pipeline[0].inputs.text;
  await applyDocument(broken); await page.locator('#quickValidateButton').click();
  await page.locator('.fix-apply-btn').first().click();
  await page.locator('#applyReviewedFix').waitFor();
  assert.deepEqual(await jsonValue(), broken);
  await page.locator('#applyReviewedFix').click();
  await page.locator('#validationOutput .diagnostic.ok').waitFor();
  await page.locator('#undoButton').click(); assert.deepEqual(await jsonValue(), broken);
  await page.locator('#redoButton').click(); assert.deepEqual(await jsonValue(), complete);

  // Actual Demo uses edited rules and reports results against this document version.
  await page.locator('#openRunButton').click();
  await page.locator('#preflightButton').click();
  await page.locator('#preflightSummary .preflight-status').waitFor();
  await page.locator('#runButton').click();
  await page.waitForFunction(() => /运行已完成|运行失败/.test(document.querySelector('#runSummary').textContent), null, {timeout: 60000});
  assert.match(await page.locator('#runSummary').textContent(), /运行已完成/);
  const result = JSON.parse(await page.locator('#runResult').textContent());
  assert.equal(result['summary.json'].success_count, 2);
  assert.equal(result['results.jsonl'][0].output.intent, 'BROWSER_VIP');
  assert.equal(result['results.jsonl'][1].output.is_hit, 0);
  await applyDocument({...complete, comment: 'modified after run'});
  await tab('run'); assert.equal(await page.locator('#runFreshness').isVisible(), true);

  await page.locator('#undoButton').click();
  assert.deepEqual(await jsonValue(), complete);
  assert.equal(await page.locator('#runFreshness').isHidden(), true);
  assert.deepEqual(JSON.parse(await page.locator('#runResult').textContent()), result);
  await page.locator('#redoButton').click();
  assert.equal(await page.locator('#runFreshness').isVisible(), true);

  // Invalid JSON blocks saving visibly and keeps the unfinished text.
  await tab('json'); await page.locator('#rawJson').fill('{unfinished');
  await page.locator('#saveButton').click();
  assert.equal(await page.locator('#rawJson').inputValue(), '{unfinished');
  assert.match(await page.locator('#operationFeedback').textContent(), /JSON 错误/);
  await applyDocument(complete); await save();

  // Structured parameters preserve unknown values, and invalid primitive input blocks save visibly.
  const withUnknown = structuredClone(complete); withUnknown.pipeline[0].params.future_parameter = {nested: [1, '保留']};
  await applyDocument(withUnknown); await page.locator('.node[data-node-id="match_keywords"]').click();
  await category.getByLabel('值，每行一个').fill('VIP\n专席');
  await category.getByLabel('值，每行一个').press('Tab');
  assert.deepEqual((await jsonValue()).pipeline[0].params.future_parameter, {nested: [1, '保留']});
  await applyDocument(complete);

  // A file changed by another editor is never silently overwritten.
  await applyDocument({...complete, comment: 'unsaved local change'});
  const current = await (await fetch(`${base}/api/pipeline?filename=${encodeURIComponent(savedFile())}`)).json();
  const external = {...complete, comment: 'written by another editor'};
  const replaced = await fetch(`${base}/api/pipeline`, {method: 'PUT', headers: {'Content-Type': 'application/json'},
    body: JSON.stringify({filename: savedFile(), revision: current.revision, pipeline: external})});
  assert.equal(replaced.status, 200);
  const conflict = page.waitForResponse(r => r.url().endsWith('/api/pipeline') && r.request().method() === 'PUT');
  await page.locator('#saveButton').click(); assert.equal((await conflict).status(), 409);
  assert.deepEqual(await savedDocument(savedFile()), external);
  assert.equal((await jsonValue()).comment, 'unsaved local change');
  saveName = 'Browser Keywords Copy'; await save(true);
  assert.equal((await savedDocument('pipeline_browser_keywords_copy.json')).comment, 'unsaved local change');

  const unknownIo = structuredClone(complete);
  const extraInput = {type: 'future_input', name: 'future_request', params: {nested: ['保留', 7]}};
  unknownIo.io.input.push(extraInput);
  await applyDocument(unknownIo); await page.locator('#fitButton').click();
  await page.locator('.node[data-node-id="input"]').click();
  await page.locator('#ioInputs').getByRole('button', {name: '添加输入', exact: true}).click();
  assert.deepEqual((await jsonValue()).io.input[1], extraInput);
  await tab('run'); assert.equal(await page.locator('#runButton').isDisabled(), true);
  await applyDocument(complete);

  // Larger diagrams open at readable scale; overview is an explicit action.
  const complex = JSON.parse(await readFile(new URL('../../configs/pipeline_doc_qa_rerank_cpu.json', import.meta.url)));
  delete complex.pipeline.find(node => node.name === 'retrieve').params.top_k;
  await page.locator('#pipelineFile').setInputFiles({name: 'complex.json', mimeType: 'application/json', buffer: Buffer.from(JSON.stringify(complex))});
  await page.waitForFunction(() => document.querySelectorAll('#nodes .node').length === 10);
  assert.ok(Number.parseInt(await page.locator('#zoomLabel').textContent(), 10) >= 65);
  await page.locator('#fitButton').click();
  // At overview scale every visible port dot still hits its own port, not a neighbour.
  assert.ok(Number.parseInt(await page.locator('#zoomLabel').textContent(), 10) < 53);
  assert.deepEqual(await page.evaluate(() => [...document.querySelectorAll('.port-dot')].flatMap(dot => {
    const box = dot.getBoundingClientRect(), hit = document.elementFromPoint(box.x + box.width / 2, box.y + box.height / 2);
    return hit === dot.previousElementSibling ? [] : [`${dot.previousElementSibling.dataset.nodeId}.${dot.previousElementSibling.dataset.port}`];
  })), []);
  await page.locator('.node[data-node-id="retrieve"]').click();
  await page.locator('#focusNodeButton').click();
  assert.ok(Number.parseInt(await page.locator('#zoomLabel').textContent(), 10) >= 100);
  const topK = page.locator('#configFields [data-field="top_k"]');
  await topK.evaluate(input => { for (let ancestor = input.parentElement; ancestor; ancestor = ancestor.parentElement) if (ancestor.tagName === 'DETAILS') ancestor.open = true; });
  await topK.fill('-1');
  await topK.evaluate(input => { for (let ancestor = input.parentElement; ancestor; ancestor = ancestor.parentElement) if (ancestor.tagName === 'DETAILS') ancestor.open = false; });
  await tab('run'); await page.locator('#saveButton').click();
  assert.equal(await topK.isVisible(), true);
  assert.equal(await topK.evaluate(input => input.checkValidity()), false);
  assert.equal(await topK.evaluate(input => input === document.activeElement), true);
  await topK.fill('5'); await topK.press('Tab');
  assert.equal((await jsonValue()).pipeline.find(node => node.name === 'retrieve').params.top_k, 5);
  await page.locator('#inspectorResize').focus(); const oldWidth = (await page.locator('.inspector').boundingBox()).width;
  await page.keyboard.press('ArrowLeft'); assert.ok((await page.locator('.inspector').boundingBox()).width > oldWidth);
  if (process.env.STUDIO_SCREENSHOT_DIR) await mkdir(process.env.STUDIO_SCREENSHOT_DIR, {recursive: true});
  for (const viewport of [{width: 1280, height: 768}, {width: 1366, height: 768}, {width: 1920, height: 1080}]) {
    await page.setViewportSize(viewport);
    assert.equal(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth), true);
    for (const id of ['saveButton', 'openRunButton', 'newEntryButton', 'fitButton', 'focusNodeButton']) {
      const box = await page.locator('#' + id).boundingBox();
      assert.ok(box.x >= 0 && box.x + box.width <= viewport.width && box.y + box.height <= viewport.height, id);
    }
    if (process.env.STUDIO_SCREENSHOT_DIR) await page.screenshot({path: resolve(process.env.STUDIO_SCREENSHOT_DIR, `studio-${viewport.width}.png`)});
  }
  assert.deepEqual(errors, []);
  console.log('PASS Studio JSON editor: forms, history, draft saves, connections, fix review, real Demo and desktop layouts');
} finally { await browser.close(); }
