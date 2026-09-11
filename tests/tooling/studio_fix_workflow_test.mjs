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
let previewResponse, previewCalls = 0, confirmations = 0;
const context = vm.createContext({
  document, structuredClone, console, URL, URLSearchParams,
  setTimeout() {}, clearTimeout() {}, requestAnimationFrame() {},
  localStorage: { getItem() { return null; }, setItem() {} },
  window: { addEventListener() {}, confirm() { confirmations++; return true; },
    location: { search: "", hash: "" } },
  Option: class extends Element { constructor(text, value) { super("option"); this.textContent = text; this.value = value; } },
  async testWrite(path) {
    if (path === "/fixes/preview") { previewCalls++; return await previewResponse; }
    if (path === "/validate") return { ok: true, diagnostics: [], plan: { topological_order: [], layers: [] } };
    throw new Error(`Unexpected API request: ${path}`);
  },
});
const web = new URL("../../tools/pipeline_studio/web/", import.meta.url);
const modules = new Map();
async function load(name) {
  if (modules.has(name)) return modules.get(name);
  let code = readFileSync(new URL(name, web), "utf8");
  if (name === "api.js") code = "export const initialPipeline = ''; export const api = async () => ({}); export const write = globalThis.testWrite;";
  if (name === "graph.js") code = "export class GraphView { constructor() { this.positions = {}; } render() {} fit() {} focusNode() {} }";
  if (name === "app.js") {
    const startup = code.indexOf("\ntry {\n  await refreshLists();");
    assert.notEqual(startup, -1, "Locate and omit browser startup, without extracting the handler");
    code = code.slice(0, startup);
    code += "\nrenderAll = () => {}; updateEditorStatus = () => {};\nexport { state, history, drafts, handleApplyFix, restoreHistory };";
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
    savedPipeline: JSON.stringify(initial), dirty: false,
    validationReport: { revision: "revision-1", tool_fingerprint: "tool-1", diagnostics: [
      { code: "TEST", remediation: { schema_version: 1, fixes: [fix] } },
    ] },
  });
  drafts.clear(); history.reset({ pipeline: state.pipeline, selected: "" });
  previewCalls = 0; confirmations = 0;
  previewResponse = Promise.resolve({ ok: true, patched: structuredClone(patched), report: { ok: true } });
}
const plain = value => JSON.parse(JSON.stringify(value));
reset();
await handleApplyFix(fix);
assert.deepEqual(plain(state.pipeline), patched, "Apply replaces the current draft");
assert.equal(state.dirty, true, "Applying a repair marks the document unsaved");
assert.equal(previewCalls, 1);
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
