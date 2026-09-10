// Optional browser coverage, invoked by the existing Python Studio suite.
// Supply an installed Playwright module and, optionally, a Chromium executable.
import assert from "node:assert/strict";
import { createRequire } from "node:module";
import { readFileSync, writeFileSync, mkdirSync } from "node:fs";
import { join } from "node:path";
const require = createRequire(import.meta.url);
const { chromium } = require(process.env.STUDIO_PLAYWRIGHT_MODULE);
const [url, configRoot] = process.argv.slice(2);
const browser = await chromium.launch({ headless: true,
  ...(process.env.STUDIO_CHROMIUM_PATH ? { executablePath: process.env.STUDIO_CHROMIUM_PATH } : {}),
  ...(process.env.STUDIO_BROWSER_NO_SANDBOX === "1" ? { args: ["--no-sandbox"] } : {}),
});
const screenshotRoot = process.env.STUDIO_SCREENSHOT_DIR;
if (screenshotRoot) mkdirSync(screenshotRoot, { recursive: true });
const page = await browser.newPage({ viewport: { width: 1366, height: 768 } });
page.setDefaultTimeout(10000);
const errors = [];
page.on("pageerror", error => errors.push(error.message));
const screenshot = async name => { if (screenshotRoot) await page.screenshot({ path: join(screenshotRoot, `${name}.png`) }); };
const open = async filename => {
  await page.selectOption("#pipelineSelect", filename);
  await page.click("#openButton");
  await page.waitForFunction(name => document.querySelector("#documentTitle").textContent === name && !document.querySelector("#openButton").disabled, filename);
};
const rule = () => page.locator('.node').filter({ hasText: 'TextRuleMatchNode' });
const categories = () => page.locator('#configFields [data-field="categories"]');
const json = async () => JSON.parse(await page.locator("#rawJson").inputValue());
try {
  await page.goto(url);
  await page.waitForFunction(() => document.querySelector("#pipelineSelect").options.length > 1);
  assert.equal(await page.locator("#toast").evaluate(el => el.classList.contains("error")), false);
  assert.equal(await page.locator("#newEntryButton").isVisible(), true);
  await page.click("#newEntryButton");
  assert.equal(await page.locator("#newButton").isVisible(), true, "one click exposes creation");
  await page.selectOption("#bizSelect", "keyword_match_v1");
  await page.selectOption("#cloneProfile", "keyword_match_rules");
  await page.click("#newButton");
  await page.waitForFunction(() => document.querySelectorAll('.node').length === 3 && !document.querySelector('#newButton').disabled);
  assert.equal((await json()).biz_name, "keyword_match_v1");
  page.once("dialog", dialog => dialog.accept());
  await open("pipeline_browser.json");
  await page.click("#editModeButton"); // browse
  for (const width of [1280, 1366, 1920]) {
    await page.setViewportSize({ width, height: width === 1920 ? 1080 : 768 });
    for (const filename of ["pipeline_browser.json", "pipeline_browser_multi.json"]) {
      await open(filename);
      await page.click("#fitButton");
      assert.equal(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth), true);
      for (const id of ["newEntryButton", "quickValidateButton", "openRunButton", "editModeButton"]) {
        const box = await page.locator(`#${id}`).boundingBox();
        assert.ok(box && box.x >= 0 && box.x + box.width <= width, `${id} reachable at ${width}`);
      }
      const box = await page.locator('#graph').boundingBox(); assert.ok(box.width > 400);
      await screenshot(`${width}-${filename.replace('.json', '')}`);
      await page.click('#editModeButton');
      for (const id of ['saveButton', 'saveAsButton', 'quickValidateButton', 'openRunButton']) {
        const editBox = await page.locator(`#${id}`).boundingBox();
        assert.ok(editBox && editBox.x >= 0 && editBox.x + editBox.width <= width, `${id} reachable when editing at ${width}`);
      }
      await screenshot(`${width}-edit-${filename.replace('.json', '')}`);
      await page.click('#editModeButton');
    }
  }
  await page.setViewportSize({ width: 1366, height: 768 });
  await open("pipeline_browser.json");
  await rule().click();
  assert.equal(await page.locator("#nodeForm").isVisible(), true);
  assert.equal(await categories().isDisabled(), true, "browsing never edits parameters");
  assert.equal(await page.locator('body').evaluate(el => el.classList.contains('editing')), false);
  await page.click("#inspectorToggle"); assert.equal(await page.locator(".inspector").isVisible(), false);
  await page.click("#quickValidateButton");
  await page.waitForFunction(() => document.querySelector("#validationOutput").textContent.includes("校验通过"));
  await page.click("#openRunButton"); await page.click("#runButton");
  await page.waitForFunction(() => document.querySelector("#runSummary").textContent.includes("运行已完成"));
  assert.match(await page.locator("#runSummary").textContent(), /成功 2 条/);
  assert.equal(await page.locator(".run-sample").count(), 2);
  const originalResult = await page.locator("#runResult").textContent();
  await screenshot("run-results");
  await page.click("#editModeButton"); await rule().click();
  await categories().fill("{invalid");
  await page.click("#quickValidateButton"); await page.click("#applyContinue");
  assert.equal(await categories().inputValue(), "{invalid", "invalid input survives continuation");
  assert.equal(await page.locator("#draftActions").isVisible(), true);
  assert.match(await page.locator("#operationFeedback").textContent(), /categories/);
  await categories().fill('{"BROWSER":["VIP"]}'); await page.click("#applyContinue");
  await page.waitForFunction(() => document.querySelector("#validationOutput").textContent.includes("校验通过"));
  await page.click("#openRunButton");
  assert.equal(await page.locator("#runFreshness").isVisible(), true);
  assert.equal(await page.locator("#runResult").textContent(), originalResult);
  await page.click("#undoButton");
  assert.equal(await page.locator("#runFreshness").isVisible(), false);
  await page.click("#redoButton"); assert.equal(await page.locator("#runFreshness").isVisible(), true);
  await rule().click(); await categories().fill('{"SAVED_BROWSER":["VIP"]}');
  await page.click("#saveButton"); await page.click("#applyContinue");
  await page.waitForFunction(() => document.querySelector("#operationFeedback").textContent.includes("已保存"));
  assert.deepEqual(JSON.parse(readFileSync(join(configRoot, "pipeline_browser.json"))).pipeline[0].config.categories, { SAVED_BROWSER: ["VIP"] });
  assert.match(await page.locator("#saveScope").textContent(), /pipeline_browser.json/);
  assert.doesNotMatch(await page.locator("#saveScope").textContent(), /\.conf/);
  await page.click("#openRunButton"); await page.click("#runButton");
  await page.waitForFunction(() => document.querySelector("#runSummary").textContent.includes("运行已完成"));
  const editedRecords = JSON.parse(await page.locator('#runResult').textContent())['results.jsonl'];
  assert.equal(editedRecords[0].output.match_result.intent, 'SAVED_BROWSER');
  assert.equal(editedRecords[1].output.is_hit, false);
  await screenshot('edited-run-results');
  page.once("dialog", dialog => dialog.accept("pipeline_browser_pair.json"));
  await page.click("#saveSolutionButton");
  await page.waitForFunction(() => document.querySelector("#saveScope").textContent.includes("pipeline_browser_pair.conf"));
  const confPath = join(configRoot, "pipeline_browser_pair.conf");
  assert.ok(JSON.parse(readFileSync(confPath)).data.pipe_path.endsWith("pipeline_browser_pair.json"));
  await open("pipeline_browser_other.json"); await open("pipeline_browser_pair.json");
  assert.match(await page.locator("#saveScope").textContent(), /pipeline_browser_pair.conf/);
  await rule().click(); await categories().fill('{"PAIR_UPDATE":["VIP"]}');
  await page.click("#saveButton"); await page.click("#applyContinue");
  await page.waitForFunction(() => document.querySelector("#operationFeedback").textContent.includes("已保存"));
  assert.deepEqual(JSON.parse(readFileSync(join(configRoot, "pipeline_browser_pair.json"))).pipeline[0].config.categories, { PAIR_UPDATE: ["VIP"] });
  const savedPair = readFileSync(join(configRoot, "pipeline_browser_pair.json"), "utf8");
  writeFileSync(confPath, readFileSync(confPath, "utf8") + "\n");
  await categories().fill('{"CONFLICT":["VIP"]}'); await page.click("#saveButton"); await page.click("#applyContinue");
  await page.waitForFunction(() => document.querySelector("#operationFeedback").textContent.includes("其他编辑器修改"));
  assert.equal(readFileSync(join(configRoot, "pipeline_browser_pair.json"), "utf8"), savedPair);
  page.once("dialog", dialog => dialog.accept());
  await open("pipeline_browser_multi.json");
  const ids = await page.locator('.node').evaluateAll(nodes => nodes.map(n => n.dataset.nodeId).filter(id => !id.startsWith('$')));
  await page.locator(`.node[data-node-id="${ids[0]}"]`).click();
  await page.locator('#nodeId').fill('browser_renamed');
  await page.locator(`.node[data-node-id="${ids[1]}"]`).click();
  await page.click('#applyContinue');
  assert.equal(await page.locator('#nodeId').inputValue(), ids[1]);
  assert.ok((await json()).pipeline.some(n => n.id === 'browser_renamed'));
  await page.locator('#nodeId').fill('discarded_name');
  await page.locator('.node[data-node-id="browser_renamed"]').click();
  await page.click('#discardContinue');
  assert.equal(await page.locator('#nodeId').inputValue(), 'browser_renamed');
  assert.ok(!(await json()).pipeline.some(n => n.id === 'discarded_name'));
  await page.click('#undoButton'); assert.ok(!(await json()).pipeline.some(n => n.id === 'browser_renamed'));
  // Model and raw JSON buffers use the same explicit continuation path.
  const originalMulti = await json();
  await page.click('[data-tab="models"]');
  await page.selectOption('#modelSelect', originalMulti.models[0].model_id);
  await page.locator('#modelId').fill('browser_model');
  await page.click('#quickValidateButton'); await page.click('#applyContinue');
  await page.waitForFunction(() => document.querySelector('#validationOutput').textContent.includes('校验通过'));
  assert.ok((await json()).models.some(model => model.model_id === 'browser_model'));
  await page.click('#undoButton');
  assert.deepEqual(await json(), originalMulti);
  await page.click('[data-tab="json"]'); await page.locator('#rawJson').fill('{ broken');
  await page.click('#quickValidateButton'); await page.click('#applyContinue');
  assert.equal(await page.locator('#rawJson').inputValue(), '{ broken');
  await page.locator('#rawJson').fill(JSON.stringify({ ...originalMulti, comment: 'browser JSON' }, null, 2));
  await page.click('#applyContinue');
  await page.waitForFunction(() => document.querySelector('#validationOutput').textContent.includes('校验通过'));
  assert.equal((await json()).comment, 'browser JSON');
  await page.click('#undoButton'); assert.deepEqual(await json(), originalMulti);
  await open('pipeline_browser.json');
  // Hold the POST response until another document is open. Even this earliest
  // race must retain the originating document identity, not the active editor.
  let releaseStart, started;
  const heldStart = new Promise(resolve => { releaseStart = resolve; });
  const sawStart = new Promise(resolve => { started = resolve; });
  await page.route('**/api/v1/runs', async route => {
    started(); await heldStart;
    await route.fulfill({ json: { ok: true, job_id: 'delayed', status: 'queued' } });
  });
  await page.route('**/api/v1/runs/delayed', route => route.fulfill({ json: { ok: true, job: {
    status: 'completed', logs: 'ONLY_A', result: { 'results.jsonl': [{ request_id: 999, status: 0, output: { answer: 'ONLY_A' } }] },
  } } }));
  await page.click('#openRunButton'); await page.click('#runButton'); await sawStart;
  await open('pipeline_browser_other.json'); releaseStart();
  await page.waitForFunction(() => document.querySelector('#runContext').textContent.includes('当前方案尚未运行'));
  await page.click('#openRunButton');
  assert.equal(await page.locator('#runResult').textContent(), '');
  assert.equal(await page.locator('.run-sample').count(), 0);
  assert.doesNotMatch(await page.locator('#runLog').textContent(), /ONLY_A/);
  await page.unroute('**/api/v1/runs'); await page.unroute('**/api/v1/runs/delayed');
  await page.click('#runButton');
  await page.waitForFunction(() => document.querySelector('#runSummary').textContent.includes('运行已完成'));
  assert.equal(await page.locator('.run-sample').count(), 2);
  // Native Validator reports use an object-valued error.message.
  await page.click('[data-tab="json"]');
  const invalidPipeline = await json(); invalidPipeline.pipeline[0].node_type = 'MissingNode';
  await page.locator('#rawJson').fill(JSON.stringify(invalidPipeline, null, 2));
  await page.click('#applyJson'); await page.click('#saveButton');
  await page.waitForFunction(() => document.querySelector('#validationOutput .diagnostic'));
  assert.equal(await page.locator('#validationTab').isVisible(), true);
  assert.match(await page.locator('#operationFeedback').textContent(), /方案校验未通过/);
  await page.click('#undoButton');
  // Startup failures must remain readable after the old 2.6-second timeout.
  const startupPage = await browser.newPage();
  try {
    await startupPage.route('**/api/v1/assets', route => route.fulfill({ status: 503,
      json: { ok: false, error: { code: 'ASSET_CATALOG_FAILED', message: '启动故障测试' } },
    }));
    await startupPage.goto(url);
    await startupPage.waitForFunction(() => document.querySelector('#toast').classList.contains('error'));
    await startupPage.waitForTimeout(3000);
    assert.equal(await startupPage.locator('#toast').evaluate(el => el.classList.contains('show')), true);
    assert.match(await startupPage.locator('#toast').textContent(), /api\/v1\/assets.*HTTP 503.*ASSET_CATALOG_FAILED/);
    assert.match(await startupPage.locator('#toast').textContent(), /启动故障测试/);
    await startupPage.click('#toastDismiss');
    assert.equal(await startupPage.locator('#toast').evaluate(el => el.classList.contains('show')), false);
    await startupPage.unroute('**/api/v1/assets'); await startupPage.reload();
    await startupPage.waitForFunction(() => document.querySelector('#pipelineSelect').options.length > 1);
    assert.equal(await startupPage.locator('#toast').evaluate(el => el.classList.contains('error')), false);
  } finally { await startupPage.close(); }
  assert.deepEqual(errors, []);
  console.log('Browser workflow passed: desktop layouts, read-only browsing, creation, drafts, undo, save scope/conflicts, real Demo and cross-document run isolation.');
} finally { await browser.close(); }
