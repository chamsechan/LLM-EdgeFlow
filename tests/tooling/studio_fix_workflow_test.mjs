// Execute the application's real repair handler and history. Rendering/network
// boundaries are replaced; application state transitions remain production code.
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import vm from "node:vm";

class Element {
  constructor(tag = "div") {
    this.tagName = tag.toUpperCase(); this.children = []; this.listeners = {};
    this.style = {}; this.dataset = {}; this.value = "";
    this.classList = { add() {}, remove() {}, toggle() {}, contains() { return false; } };
  }
  append(...children) { this.children.push(...children); }
  appendChild(child) { this.append(child); return child; }
  replaceChildren(...children) { this.children = children; }
  addEventListener(name, callback) { this.listeners[name] = callback; }
  setAttribute() {}
  querySelectorAll() { return []; }
  focus() {}
  add(option) { this.append(option); }
  get options() { return this.children; }
}
const elements = new Map();
const document = {
  querySelector(selector) {
    if (!elements.has(selector)) elements.set(selector, new Element());
    return elements.get(selector);
  },
  querySelectorAll() { return []; }, createElement: tag => new Element(tag),
  addEventListener() {}, documentElement: new Element(), body: new Element(),
};
let previewResponse, previewCalls = 0, confirmations = 0, validationCalls = 0;
let catalogVersion = 4;
let authoringResponse, authoringCalls = [], preflightResponse;
const requests = [];
const context = vm.createContext({
  document, structuredClone, console, URL, URLSearchParams,
  setTimeout() {}, clearTimeout() {}, requestAnimationFrame() {},
  localStorage: { getItem() { return null; }, setItem() {} },
  window: { addEventListener() {}, confirm() { confirmations++; return true; },
    location: { search: "", hash: "" } },
  Option: class extends Element { constructor(text, value) { super("option"); this.textContent = text; this.value = value; } },
  async testApi(path) {
    if (path.startsWith("/catalog")) return { schema_version: catalogVersion, nodes: [], models: [], profiles: [], bizs: [] };
    if (path === "/profiles") return { profiles: [] };
    if (path === "/pipelines") return { pipelines: [] };
    if (path === "/assets") return { selections: [], variants: [] };
    throw new Error(`Unexpected API request: ${path}`);
  },
  async testWrite(path, method, body) {
    requests.push({ path, body: structuredClone(body) });
    if (path === "/authoring/preview") { authoringCalls.push(structuredClone(body)); return await authoringResponse; }
    if (path === "/preflight") return await preflightResponse;
    if (path === "/fixes/preview") { previewCalls++; return await previewResponse; }
    if (path === "/validate") { validationCalls++; return { ok: true, revision: "revision-1", tool_fingerprint: "tool-1", diagnostics: [], plan: { topological_order: [], layers: [] } }; }
    throw new Error(`Unexpected API request: ${path}`);
  },
});
const web = new URL("../../tools/pipeline_studio/web/", import.meta.url);
const modules = new Map();
async function load(name) {
  if (modules.has(name)) return modules.get(name);
  let code = readFileSync(new URL(name, web), "utf8");
  if (name === "api.js") code = "export const initialPipeline = ''; export const api = globalThis.testApi; export const write = globalThis.testWrite;";
  if (name === "graph.js") code = "export class GraphView { constructor() { this.positions = {}; } render() {} fit() {} focusNode() {} }";
  if (name === "app.js") {
    const startup = code.indexOf("\ntry {\n  await refreshLists();");
    assert.notEqual(startup, -1, "Locate and omit browser startup, without extracting the handler");
    code = code.slice(0, startup);
    code += "\nrenderAll = () => {}; updateEditorStatus = () => {};\nexport { state, history, drafts, handleApplyFix, handleApplyReviewedFix, applyAuthoring, runPreflight, renderPreflightFreshness, markPipelineChanged, restoreHistory, refreshLists, loadCatalog, validate };";
  }
  const module = new vm.SourceTextModule(code, { context, identifier: name });
  modules.set(name, module);
  await module.link(specifier => load(specifier.replace(/^\.\//, "")));
  return module;
}
const app = await load("app.js");
await app.evaluate();
const { state, history, drafts, handleApplyFix, restoreHistory } = app.namespace;
const initial = { biz_name: "keyword_match_v1", models: [], pipeline: [], comment: "before" };
const patched = { ...initial, comment: "after" };
const fix = { id: "repair-1", title: "Repair", effect: "Replace comment", verification: "pipeline_valid",
  patch: [{ op: "replace", path: "/comment", value: "after" }] };
function reset() {
  Object.assign(state, { pipeline: structuredClone(initial), selected: "", loading: false,
    editing: true, catalogReady: true, pipelineVersion: 1, documentVersion: 1,
    savedPipeline: JSON.stringify(initial), dirty: false, deployment: null, modelPathActions: {}, preflight: null,
    validationReport: { revision: "revision-1", tool_fingerprint: "tool-1", diagnostics: [
      { code: "TEST", remediation: { schema_version: 1, fixes: [fix] } },
    ] },
  });
  drafts.clear(); history.reset({ pipeline: state.pipeline, selected: "" });
  previewCalls = 0; confirmations = 0; authoringCalls = []; requests.length = 0;
  previewResponse = Promise.resolve({ ok: true, patched: structuredClone(patched), report: { ok: true } });
}
const plain = value => JSON.parse(JSON.stringify(value));
reset();
await handleApplyFix(fix);
assert.deepEqual(plain(state.pipeline), initial, "Preview never changes the draft before page approval");
assert.equal(history.canUndo, false);
assert.equal(confirmations, 0, "Repair review must not invoke a blocking browser dialog");
assert.equal(elements.get("#fixReviewPanel").hidden, false);
await app.namespace.handleApplyReviewedFix();
assert.deepEqual(plain(state.pipeline), patched, "Apply replaces the current draft");
assert.equal(state.dirty, true, "Applying a repair marks the document unsaved");
assert.equal(previewCalls, 2, "Application revalidates the reviewed patch and tool fingerprint");
await restoreHistory("undo");
assert.deepEqual(plain(state.pipeline), initial, "Undo restores a complete application snapshot");
await restoreHistory("redo");
assert.deepEqual(plain(state.pipeline), patched, "Redo must not lose the pipeline wrapper");

for (const mutate of [
  () => { state.pipeline.comment = "user edit"; state.pipelineVersion++; },
  () => { state.pipeline = { ...initial, comment: "other document" }; state.documentVersion++; },
  () => { drafts.set("node", { prompt: "unsaved form" }); },
]) {
  reset();
  let resolve;
  previewResponse = new Promise(done => { resolve = done; });
  const running = handleApplyFix(fix);
  assert.equal(previewCalls, 1, "Preview request must actually be in flight");
  mutate();
  const expected = structuredClone(state.pipeline);
  resolve({ ok: true, patched: structuredClone(patched), report: { ok: true } });
  await running;
  assert.deepEqual(plain(state.pipeline), expected, "Late preview must preserve newer user work");
  assert.equal(history.canUndo, false, "Rejected late repair creates no history entry");
  assert.equal(confirmations, 0, "A stale repair must not ask the user to approve it");
}
reset();
drafts.set("json", "{ unfinished");
await handleApplyFix(fix);
assert.equal(previewCalls, 0, "Do not preview a repair against unapplied form buffers");


// The application must route graph changes through the native endpoint and treat
// its complete candidate as one undoable operation, including retained ordering.
reset();
const graphInitial = { ...initial, pipeline: [{ id: "a" }, { id: "b", depends_on: ["a"], inputs: { text: "shared" } }] };
state.pipeline = structuredClone(graphInitial);
history.reset({ pipeline: state.pipeline, selected: "" });
const graphCandidate = structuredClone(graphInitial);
graphCandidate.pipeline[1].inputs.text = "unconnected_1";
authoringResponse = { ok: true, pipeline: graphCandidate, validation: { ok: false, diagnostics: [] } };
const disconnect = { kind: "disconnect", source: { node_id: "a", port: "text" }, target: { node_id: "b", port: "text" } };
await app.namespace.applyAuthoring(disconnect);
assert.equal(authoringCalls.length, 1);
assert.deepEqual(authoringCalls[0].operation, disconnect);
assert.deepEqual(authoringCalls[0].pipeline, graphInitial);
assert.equal(authoringCalls[0].revision, "revision-1");
assert.equal(authoringCalls[0].tool_fingerprint, "tool-1");
assert.deepEqual(plain(state.pipeline), graphCandidate, "Use the complete native candidate without a second graph transform");
await restoreHistory("undo");
assert.deepEqual(plain(state.pipeline), graphInitial);
assert.equal(history.canUndo, false, "One action records exactly one history transaction");

for (const change of [
  () => { state.pipeline.comment = "newer"; state.pipelineVersion++; },
  () => { state.documentVersion++; },
  () => { drafts.set("json", "unfinished"); },
]) {
  reset();
  let finish;
  authoringResponse = new Promise(resolve => { finish = resolve; });
  const action = app.namespace.applyAuthoring(disconnect);
  await new Promise(resolve => setImmediate(resolve));
  assert.equal(authoringCalls.length, 1);
  change();
  const expected = structuredClone(state.pipeline);
  finish({ ok: true, pipeline: patched, validation: { ok: true, diagnostics: [] } });
  await action;
  assert.deepEqual(plain(state.pipeline), expected, "Late graph operations cannot replace newer work");
  assert.equal(history.canUndo, false);
}

// Page review can last arbitrarily long: reject draft edits and changed tools
// before committing the reviewed candidate.
reset();
await handleApplyFix(fix);
state.pipeline.comment = "edited while reviewing";
state.pipelineVersion++;
await app.namespace.handleApplyReviewedFix();
assert.equal(state.pipeline.comment, "edited while reviewing");
assert.equal(history.canUndo, false);
reset();
await handleApplyFix(fix);
previewResponse = Promise.reject(new Error("TOOL_FINGERPRINT_MISMATCH"));
await app.namespace.handleApplyReviewedFix();
assert.deepEqual(plain(state.pipeline), initial);
assert.equal(history.canUndo, false);
assert.equal(confirmations, 0);

const ready = { ok: true, summary: { status: "ready", tools: { alg_pipeline_tool: true, alg_demo: true }, assets: [] } };
const elementText = node => [node.textContent || "", ...node.children.map(elementText)].join("\n");
reset();
preflightResponse = ready;
await app.namespace.runPreflight();
assert.match(elementText(elements.get("#preflightSummary")), /alg_pipeline_tool=✓.*alg_demo=✓/);
elements.get("#runModelRoot").value = "other-models";
app.namespace.renderPreflightFreshness();
assert.match(elementText(elements.get("#preflightSummary")), /过期|重新预检/);
for (const change of [
  () => { state.pipelineVersion++; state.pipeline.comment = "newer"; },
  () => { state.documentVersion++; },
  () => { elements.get("#runProfile").value = "different-profile"; },
  () => { state.deployment = { conf_name: "different.conf", conf_revision: "new" }; },
]) {
  reset();
  let finish;
  preflightResponse = new Promise(resolve => { finish = resolve; });
  const action = app.namespace.runPreflight();
  change();
  finish({ ...ready, summary: { ...ready.summary, next_step: "STALE_RESPONSE_SENTINEL" } });
  await action;
  assert.doesNotMatch(elementText(elements.get("#preflightSummary")), /STALE_RESPONSE_SENTINEL/,
    "Delayed preflight must not display a result for another draft or deployment");
}
console.log("Studio native authoring, page approval and preflight snapshot regressions passed");

// Render the actual diagnostic card for unsupported wire-format versions.
const { appendDiagnostic } = modules.get("editor.js").namespace;
for (const schema_version of [0, 2, 999]) {
  const container = new Element();
  appendDiagnostic(container, { code: "TEST", message: "Readable old diagnostic", suggestions: ["Manual action"],
    remediation: { schema_version, summary: "future schema", fixes: [fix] } }, () => {}, () => {});
  const flatten = node => [node, ...node.children.flatMap(flatten)];
  assert.equal(flatten(container).filter(node => node.tagName === "BUTTON").length, 0,
    "Unknown remediation versions must not expose an Apply button");
  assert.ok(flatten(container).some(node => node.textContent === "Readable old diagnostic"));
}
console.log("Studio real repair handler, history, late response and schema-version checks passed");

// Exercise both Catalog entry paths and the real validation guard.
for (const opened of [false, true]) {
  reset();
  if (!opened) state.pipeline = null;
  catalogVersion = 3;
  const beforeCalls = validationCalls;
  await app.namespace.refreshLists();
  assert.equal(state.catalogReady, false, "Reject old Catalog on startup and on an open document");
  assert.match(elements.get("#toast").children[0].textContent, /Catalog v4/);
  assert.equal(await app.namespace.validate(), false);
  assert.equal(validationCalls, beforeCalls, "No request after an incompatible Catalog");
  catalogVersion = 4;
  await app.namespace.loadCatalog();
  assert.equal(state.catalogReady, true, "A compatible Catalog restores readiness");
}
console.log("Studio Catalog version rejection and recovery passed");
