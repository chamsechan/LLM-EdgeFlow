// 可选的浏览器覆盖测试，由现有的 Python Studio 测试套件调用。
// 需提供已安装的 Playwright 模块，Chromium 可执行文件可选。
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
const authoringRequests = [];
const initializationRequests = [];
page.on("request", request => {
  if (request.url().endsWith("/api/authoring/preview")) authoringRequests.push(request.postDataJSON());
  if (request.url().endsWith("/api/init")) initializationRequests.push(request.postDataJSON());
});
page.on("pageerror", error => errors.push(error.message));
const screenshot = async name => { if (screenshotRoot) await page.screenshot({ path: join(screenshotRoot, `${name}.png`) }); };
const open = async filename => {
  await page.selectOption("#pipelineSelect", filename);
  await page.click("#openButton");
  await page.waitForFunction(name => document.querySelector("#documentTitle").textContent === name && !document.querySelector("#openButton").disabled, filename);
};
const rule = () => page.locator('.node').filter({ hasText: 'text_rule_match' });
const categories = () => page.locator('#configFields [data-field="categories"]');
const json = async () => JSON.parse(await page.locator("#rawJson").inputValue());
const contractValues = label => page.locator("#bizContractFields dt").evaluateAll(
  (terms, name) => terms.filter(term => term.textContent === name).map(term => term.nextElementSibling.textContent), label);
try {
  await page.goto(url);
  await page.waitForFunction(() => document.querySelector("#pipelineSelect").options.length > 1);
  assert.equal(await page.locator("#toast").evaluate(el => el.classList.contains("error")), false);
  assert.equal(await page.locator("#newEntryButton").isVisible(), true);
  await page.click("#newEntryButton");
  assert.equal(await page.locator("#newButton").isVisible(), true, "one click exposes creation");
  await page.locator("#createInputs button").filter({hasText: "添加输入"}).click();
  await page.selectOption("#create_input_0_type", "keyword_in");
  await page.locator("#createOutputs button").filter({hasText: "添加输出"}).click();
  await page.selectOption("#create_output_0_type", "keyword_out");
  await page.click("#newButton");
  await page.waitForFunction(() => document.querySelectorAll(".node").length === 2 && !document.querySelector("#newButton").disabled);
  assert.deepEqual(initializationRequests.at(-1), {input: [{type: "keyword_in", name: "keyword_match"}], output: [{type: "keyword_out", name: "keyword_match"}]});
  assert.deepEqual((await json()).pipeline, [], "explicit I/O selections create an empty native draft");
  page.once("dialog", dialog => dialog.accept());
  await page.selectOption("#cloneProfile", "keyword_match_rules");
  await page.click("#newButton");
  await page.waitForFunction(() => document.querySelectorAll('.node').length === 3 && !document.querySelector('#newButton').disabled);
  assert.deepEqual((await json()).io.input, [{type: "keyword_in", name: "keyword_match"}]);
  assert.deepEqual(initializationRequests.at(-1), {profile: "keyword_match_rules"});
  page.once("dialog", dialog => dialog.accept());
  await open("pipeline_browser.json");
  await page.click("#editModeButton"); // 浏览模式
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
      if (!(await page.locator("#operatorSearch").isVisible())) await page.click("#operatorsToggle");
      const operatorBox = await page.locator("#operatorList .operator").first().boundingBox();
      const viewportHeight = width === 1920 ? 1080 : 768;
      assert.ok(operatorBox && operatorBox.y >= 0 && operatorBox.y + operatorBox.height <= viewportHeight,
        `capability buttons remain reachable with expanded I/O creation at ${width}`);
      await screenshot(`${width}-edit-${filename.replace('.json', '')}`);
      await page.click('#editModeButton');
    }
  }
  await page.setViewportSize({ width: 1366, height: 768 });
  await open("pipeline_browser.json");
  await page.click("#openRunButton");
  await page.locator("#bizContract > summary").click();
  for (const [label, expected] of [
    ["输入 → 输出", ["keyword_in/keyword_match → keyword_out/keyword_match"]],
    ["输入 Converter", ["keyword_in/keyword_match"]],
    ["输出 Converter", ["keyword_out/keyword_match"]],
    ["宿主类型", ["CompanyOperatorKeywordInput", "CompanyOperatorKeywordOutput"]],
    ["类型注册后缀", ["keyword_in", "keyword_out"]],
  ]) assert.deepEqual(await contractValues(label), expected, label);
  await open("pipeline_browser_multi.json");
  await page.click("#openRunButton");
  assert.deepEqual(await contractValues("输入 → 输出"), ["doc_in/doc_qa → doc_out/doc_qa"]);
  assert.doesNotMatch(await page.locator("#bizContractFields").textContent(), /keyword/,
    "Changing documents must replace every previous contract field");
  await page.setViewportSize({ width: 390, height: 844 });
  await page.locator("#bizContract > summary").scrollIntoViewIfNeeded();
  assert.equal(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth), true);
  assert.equal(await page.locator("#bizContract").evaluate(element => {
    const box = element.getBoundingClientRect();
    return box.left >= 0 && box.right <= innerWidth && element.scrollWidth <= element.clientWidth;
  }), true, "Contract details must fit the narrow viewport");
  assert.equal(await page.locator("#bizContractFields dt, #bizContractFields dd").evaluateAll(
    fields => fields.every(field => field.scrollWidth <= field.clientWidth)), true,
  "Contract labels and identifiers must wrap within their fields");
  await screenshot("390-contract-details");
  await page.setViewportSize({ width: 1366, height: 768 });
  await page.route("**/api/catalog", async route => {
    const response = await route.fetch();
    const catalog = await response.json();
    for (const converter of [...catalog.input_converters, ...catalog.output_converters]) {
      delete converter.slot.type_id; delete converter.slot.type_suffix;
    }
    await route.fulfill({ response, json: catalog });
  }, { times: 1 });
  await open("pipeline_browser.json");
  await page.click("#openRunButton");
  for (const label of ["宿主类型", "类型注册后缀"]) {
    assert.deepEqual(await contractValues(label), ["未提供", "未提供"],
      "Missing Catalog slot fields must not be inferred from converter selection");
  }
  assert.deepEqual(await contractValues("输入 Converter"), ["keyword_in/keyword_match"]);
  await open("pipeline_browser_multi.json");
  await open("pipeline_browser.json");
  await rule().click();
  assert.equal(await page.locator("#nodeForm").isVisible(), true);
  assert.equal(await categories().isDisabled(), true, "browsing never edits parameters");
  assert.equal(await page.locator('body').evaluate(el => el.classList.contains('editing')), false);
  const beforeIoEdit = await json();
  await page.locator("#ioSettings > summary").click();
  const outputCapacity = page.locator('#ioOutputs [data-field="match_result_json_max_bytes"]');
  assert.equal(await outputCapacity.isDisabled(), true, "browsing never edits converter parameters");
  assert.equal(await outputCapacity.inputValue(), "", "converter defaults remain unset in the draft");
  await page.click("#editModeButton");
  await outputCapacity.fill("4096");
  await page.click("#quickValidateButton");
  await page.waitForFunction(() => document.querySelector("#validationOutput").textContent.includes("校验通过"));
  const afterIoEdit = await json();
  assert.deepEqual(afterIoEdit.io.input, beforeIoEdit.io.input);
  assert.deepEqual(afterIoEdit.io.output[0].inputs, beforeIoEdit.io.output[0].inputs);
  assert.equal(afterIoEdit.io.output[0].params.match_result_json_max_bytes, 4096);
  assert.ok(await page.locator("#runProfile option[value=keyword_match_rules]").count(), "parameter overrides preserve the Profile contract");
  await page.click("#undoButton");
  assert.deepEqual(await json(), beforeIoEdit, "I/O Apply records one complete undo transaction");
  await page.click("#editModeButton");
  await rule().click(); // 返回浏览会收起详情；选中节点恢复原隐藏断言的前置状态。
  assert.equal(await page.locator(".inspector").isVisible(), true);
  assert.equal(await categories().isDisabled(), true);
  assert.deepEqual(await json(), beforeIoEdit, "opening node details in browse mode preserves the document");
  await page.click("#inspectorToggle"); assert.equal(await page.locator(".inspector").isVisible(), false);
  await page.click("#quickValidateButton");
  await page.waitForFunction(() => document.querySelector("#validationOutput").textContent.includes("校验通过"));
  await page.click("#openRunButton"); await page.selectOption("#runProfile", "keyword_match_rules"); await page.click("#runButton");
  await page.waitForFunction(() => document.querySelector("#runSummary").textContent.includes("运行已完成"));
  assert.match(await page.locator("#runSummary").textContent(), /成功 2 条/);
  assert.equal(await page.locator(".run-sample").count(), 2);
  const originalResult = await page.locator("#runResult").textContent();
  await page.click("#preflightButton");
  await page.waitForFunction(() => !document.querySelector("#preflightSummary").classList.contains("loading"));
  assert.match(await page.locator("#preflightSummary").textContent(), /alg_pipeline_tool=✓.*alg_demo=✓/,
    "Real server field names must render both tools as present");
  const profileBefore = await page.locator("#runProfile").inputValue();
  await page.selectOption("#runProfile", "keyword_match_control");
  assert.match(await page.locator("#preflightSummary").textContent(), /过期|重新预检/);
  await page.selectOption("#runProfile", profileBefore);
  let releasePreflight, sawPreflight;
  const heldPreflight = new Promise(resolve => { releasePreflight = resolve; });
  const preflightStarted = new Promise(resolve => { sawPreflight = resolve; });
  await page.route("**/api/preflight", async route => {
    sawPreflight(); await heldPreflight;
    await route.fulfill({ json: { ok: true, summary: { status: "ready", next_step: "STALE_PREFLIGHT_SENTINEL" } } });
  });
  await page.click("#preflightButton"); await preflightStarted;
  await page.selectOption("#runProfile", "keyword_match_control");
  releasePreflight();
  await page.waitForFunction(() => !document.querySelector("#preflightSummary").classList.contains("loading"));
  assert.doesNotMatch(await page.locator("#preflightSummary").textContent(), /STALE_PREFLIGHT_SENTINEL/);
  await page.unroute("**/api/preflight");
  await page.selectOption("#runProfile", profileBefore);

  await screenshot("run-results");
  await page.click("#editModeButton"); await rule().click();
  await categories().fill("{invalid");
  await page.click("#quickValidateButton");
  assert.equal(await categories().inputValue(), "{invalid", "invalid input survives continuation");
  assert.equal(await page.locator("#draftActions").isVisible(), false, "one-click validation must not require a second continuation");
  assert.match(await page.locator("#operationFeedback").textContent(), /categories/);
  await categories().fill('{"BROWSER":["VIP"]}'); await page.click("#quickValidateButton");
  await page.waitForFunction(() => document.querySelector("#validationOutput").textContent.includes("校验通过"));
  await page.click("#openRunButton");
  assert.equal(await page.locator("#runFreshness").isVisible(), true);
  assert.equal(await page.locator("#runResult").textContent(), originalResult);
  await page.click("#undoButton");
  assert.equal(await page.locator("#runFreshness").isVisible(), false);
  await page.click("#redoButton"); assert.equal(await page.locator("#runFreshness").isVisible(), true);
  await rule().click(); await categories().fill('{"SAVED_BROWSER":["VIP"]}');
  await page.click("#saveButton");
  await page.waitForFunction(() => document.querySelector("#operationFeedback").textContent.includes("已保存"));
  const savedPipeline = JSON.parse(readFileSync(join(configRoot, "pipeline_browser.json")));
  assert.deepEqual(savedPipeline.pipeline[0].params.categories, { SAVED_BROWSER: ["VIP"] });
  assert.equal(savedPipeline.io.output[0].name, "keyword_match");
  assert.equal(savedPipeline.io.output[0].inputs.matches, `${savedPipeline.pipeline[0].name}.matches`);
  assert.match(await page.locator("#saveScope").textContent(), /pipeline_browser.json/);
  assert.doesNotMatch(await page.locator("#saveScope").textContent(), /\.conf/);
  await page.click("#openRunButton"); await page.selectOption("#runProfile", "keyword_match_rules"); await page.click("#runButton");
  await page.waitForFunction(() => document.querySelector("#runSummary").textContent.includes("运行已完成"));
  const editedRecords = JSON.parse(await page.locator('#runResult').textContent())['results.jsonl'];
  assert.equal(editedRecords[0].output.match_result.intent, 'SAVED_BROWSER');
  assert.equal(editedRecords[1].output.is_hit, false);
  await screenshot('edited-run-results');
  page.once("dialog", dialog => dialog.accept("pipeline_browser_pair.json"));
  await page.click("#saveSolutionButton");
  await page.waitForFunction(() => document.querySelector("#saveScope").textContent.includes("pipeline_browser_pair.conf"));
  const confPath = join(configRoot, "pipeline_browser_pair.conf");
  assert.ok(JSON.parse(readFileSync(confPath)).pipe_path.endsWith("pipeline_browser_pair.json"));
  await open("pipeline_browser_other.json"); await open("pipeline_browser_pair.json");
  assert.match(await page.locator("#saveScope").textContent(), /pipeline_browser_pair.conf/);
  await rule().click(); await categories().fill('{"PAIR_UPDATE":["VIP"]}');
  await page.click("#saveButton");
  await page.waitForFunction(() => document.querySelector("#operationFeedback").textContent.includes("已保存"));
  assert.deepEqual(JSON.parse(readFileSync(join(configRoot, "pipeline_browser_pair.json"))).pipeline[0].params.categories, { PAIR_UPDATE: ["VIP"] });
  const savedPair = readFileSync(join(configRoot, "pipeline_browser_pair.json"), "utf8");
  writeFileSync(confPath, readFileSync(confPath, "utf8") + "\n");
  await categories().fill('{"CONFLICT":["VIP"]}'); await page.click("#saveButton");
  await page.waitForFunction(() => document.querySelector("#operationFeedback").textContent.includes("其他编辑器修改"));
  assert.equal(readFileSync(join(configRoot, "pipeline_browser_pair.json"), "utf8"), savedPair);
  page.once("dialog", dialog => dialog.accept());
  await open("pipeline_browser_multi.json");
  const ids = await page.locator('.node').evaluateAll(nodes => nodes.map(n => n.dataset.nodeId).filter(id => !["input", "output"].includes(id)));
  await page.locator(`.node[data-node-id="${ids[0]}"]`).click();
  await page.locator('#nodeName').fill('browser_renamed');
  await page.locator(`.node[data-node-id="${ids[1]}"]`).click();
  await page.click('#applyContinue');
  await page.waitForFunction(id => document.querySelector('#nodeName').value === id, ids[1]);
  assert.equal(await page.locator('#nodeName').inputValue(), ids[1]);
  assert.ok((await json()).pipeline.some(n => n.name === 'browser_renamed'));
  assert.ok(authoringRequests.some(request => request.operation?.kind === 'rename_node' ||
    request.operations?.some(operation => operation.kind === 'rename_node')),
    'Actual node form renames must pass through the shared authoring endpoint');
  await page.locator('#nodeName').fill('discarded_name');
  await page.locator('.node[data-node-id="browser_renamed"]').click();
  await page.click('#discardContinue');
  assert.equal(await page.locator('#nodeName').inputValue(), 'browser_renamed');
  assert.ok(!(await json()).pipeline.some(n => n.name === 'discarded_name'));
  await page.click('#undoButton'); assert.ok(!(await json()).pipeline.some(n => n.name === 'browser_renamed'));
  // 校验在同一个动作中应用模型缓冲和原始 JSON 缓冲。
  const originalMulti = await json();
  await page.click('[data-tab="models"]');
  await page.selectOption('#modelSelect', originalMulti.models[0].name);
  await page.locator('#modelName').fill('browser_model');
  await page.click('#quickValidateButton');
  await page.waitForFunction(() => document.querySelector('#validationOutput').textContent.includes('校验通过'));
  assert.ok((await json()).models.some(model => model.name === 'browser_model'));
  const renamedModel = (await json()).models.find(model => model.name === 'browser_model');
  assert.equal(renamedModel.file, originalMulti.models[0].file);
  await page.click('#undoButton');
  assert.deepEqual(await json(), originalMulti);
  await page.click('[data-tab="json"]'); await page.locator('#rawJson').fill('{ broken');
  await page.click('#quickValidateButton');
  assert.equal(await page.locator('#rawJson').inputValue(), '{ broken');
  await page.locator('#rawJson').fill(JSON.stringify({ ...originalMulti, comment: 'browser JSON' }, null, 2));
  await page.click('#quickValidateButton');
  await page.waitForFunction(() => document.querySelector('#validationOutput').textContent.includes('校验通过'));
  assert.equal((await json()).comment, 'browser JSON');
  await page.click('#undoButton'); assert.deepEqual(await json(), originalMulti);
  await open('pipeline_browser.json');
  // 在打开另一个文档前挂起 POST 响应。即使是最早的这种竞争，也必须保留
  // 发起请求的文档标识，而不是当前活动的编辑器。
  let releaseStart, started;
  const heldStart = new Promise(resolve => { releaseStart = resolve; });
  const sawStart = new Promise(resolve => { started = resolve; });
  await page.route('**/api/runs', async route => {
    started(); await heldStart;
    await route.fulfill({ json: { ok: true, job_id: 'delayed', status: 'queued' } });
  });
  await page.route('**/api/runs/delayed', route => route.fulfill({ json: { ok: true, job: {
    status: 'completed', logs: 'ONLY_A', result: { 'results.jsonl': [{ request_id: 999, status: 0, output: { answer: 'ONLY_A' } }] },
  } } }));
  await page.click('#openRunButton'); await page.click('#runButton'); await sawStart;
  await open('pipeline_browser_other.json'); releaseStart();
  await page.waitForFunction(() => document.querySelector('#runContext').textContent.includes('当前方案尚未运行'));
  await page.click('#openRunButton');
  assert.equal(await page.locator('#runResult').textContent(), '');
  assert.equal(await page.locator('.run-sample').count(), 0);
  assert.doesNotMatch(await page.locator('#runLog').textContent(), /ONLY_A/);
  await page.unroute('**/api/runs'); await page.unroute('**/api/runs/delayed');
  await page.click('#runButton');
  await page.waitForFunction(() => document.querySelector('#runSummary').textContent.includes('运行已完成'));
  assert.equal(await page.locator('.run-sample').count(), 2);
  await rule().click();
  await page.locator("#nodeForm details > summary").click();

  // 依赖控件属于图操作，而非 Node 属性草稿缓冲。
  // 添加一个独立 Node，使新的顺序可观察且无环。
  const beforeAdd = await json();
  if (!(await page.locator("#operatorSearch").isVisible())) await page.click("#operatorsToggle");
  await page.locator("#operatorSearch").fill("text_template");
  await page.locator("#operatorList button").filter({ hasText: "text_template" }).click();
  await page.waitForFunction(count => JSON.parse(document.querySelector("#rawJson").value).pipeline.length === count + 1,
    beforeAdd.pipeline.length);
  const afterAdd = await json();
  const addedNode = afterAdd.pipeline.find(node => !beforeAdd.pipeline.some(previous => previous.name === node.name));
  assert.ok(authoringRequests.some(request => request.operation?.kind === "add_node"));
  await page.locator(`.node[data-node-id="${addedNode.name}"]`).click();
  if (!(await page.locator("#dependencySource").isVisible())) await page.locator("#nodeForm details > summary").click();
  await page.selectOption("#dependencySource", beforeAdd.pipeline[0].name);
  assert.equal(await page.locator("#nodeDraftHint").isVisible(), false,
    "Selecting an execution dependency must not create a pending parameter draft");
  await page.click("#addDependencyButton");
  await page.waitForFunction(({ id, dependency }) => JSON.parse(document.querySelector("#rawJson").value)
    .pipeline.find(node => node.name === id).depends_on?.includes(dependency),
    { id: addedNode.name, dependency: beforeAdd.pipeline[0].name });
  assert.ok(authoringRequests.some(request => request.operation?.kind === "add_dependency" &&
    request.operation.node === addedNode.name));
  await page.click("#undoButton");
  assert.deepEqual(await json(), afterAdd, "One undo removes only the added ordering");
  await page.click("#undoButton");
  assert.deepEqual(await json(), beforeAdd, "The preceding undo removes the added node");
  await page.locator("#operatorSearch").fill("");

  // 数据连接无需显式排序或依赖修复。
  await open("pipeline_browser_multi.json");
  const inferredDependencies = await json();
  for (const node of inferredDependencies.pipeline) delete node.depends_on;
  await page.click('[data-tab="json"]');
  await page.locator("#rawJson").fill(JSON.stringify(inferredDependencies));
  await page.click("#quickValidateButton");
  await page.waitForFunction(() => document.querySelector("#validationOutput").textContent.includes("校验通过"));
  assert.equal(await page.locator("#validationOutput .fix-apply-btn").count(), 0);
  await open("pipeline_browser_other.json");
  // 原生 Validator 报告中的 error.message 是对象值。
  await page.click('[data-tab="json"]');
  const invalidPipeline = await json(); invalidPipeline.pipeline[0].type = 'missing_node';
  await page.locator('#rawJson').fill(JSON.stringify(invalidPipeline, null, 2));
  await page.click('#applyJson'); await page.click('#saveButton');
  await page.waitForFunction(() => document.querySelector('#validationOutput .diagnostic'));
  assert.equal(await page.locator('#validationTab').isVisible(), true);
  assert.match(await page.locator('#operationFeedback').textContent(), /方案校验未通过/);
  await page.click('#undoButton');
  // 启动失败信息在超过旧的 2.6 秒超时后仍必须可读。
  const startupPage = await browser.newPage();
  try {
    await startupPage.route('**/api/assets', route => route.fulfill({ status: 503,
      json: { ok: false, error: { code: 'ASSET_CATALOG_FAILED', message: '启动故障测试' } },
    }));
    await startupPage.goto(url);
    await startupPage.waitForFunction(() => document.querySelector('#toast').classList.contains('error'));
    await startupPage.waitForTimeout(3000);
    assert.equal(await startupPage.locator('#toast').evaluate(el => el.classList.contains('show')), true);
    assert.match(await startupPage.locator('#toast').textContent(), /api\/assets.*HTTP 503.*ASSET_CATALOG_FAILED/);
    assert.match(await startupPage.locator('#toast').textContent(), /启动故障测试/);
    await startupPage.click('#toastDismiss');
    assert.equal(await startupPage.locator('#toast').evaluate(el => el.classList.contains('show')), false);
    await startupPage.unroute('**/api/assets'); await startupPage.reload();
    await startupPage.waitForFunction(() => document.querySelector('#pipelineSelect').options.length > 1);
    assert.equal(await startupPage.locator('#toast').evaluate(el => el.classList.contains('error')), false);
  } finally { await startupPage.close(); }
  assert.deepEqual(errors, []);
  console.log('Browser workflow passed: desktop layouts, read-only browsing, creation, drafts, undo, save scope/conflicts, real Demo and cross-document run isolation.');
} catch (error) {
  console.error("Browser failure state:", JSON.stringify(await page.evaluate(() => ({
    document: document.querySelector("#documentTitle")?.textContent,
    model: document.querySelector("#modelName")?.value,
    validation: document.querySelector("#validationOutput")?.textContent,
    feedback: document.querySelector("#operationFeedback")?.textContent,
  })), null, 2));
  console.error("Browser JavaScript errors:", errors);
  await screenshot("browser-failure");
  throw error;
} finally { await browser.close(); }
