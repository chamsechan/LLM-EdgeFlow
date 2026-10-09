import assert from "node:assert/strict";
import { readFileSync } from "node:fs";

const source = readFileSync(new URL("../../tools/pipeline_studio/web/editor.js", import.meta.url), "utf8");
const { createHistory, createDrafts, appendDiagnostic, appendConfigField, readConfigFields, readFormBuffer, restoreFormBuffer } = await import(`data:text/javascript;base64,${Buffer.from(source).toString("base64")}`);
const history = createHistory(3);
const initial = { pipeline: { pipeline: [{ name: "root", depends_on: [] }] }, selected: "root" };
const saved = JSON.stringify(initial.pipeline);
history.reset(initial);
const removed = { pipeline: { pipeline: [] }, selected: "" };
history.record(removed);
removed.pipeline.pipeline.push({ name: "outside_mutation" });
assert.equal(JSON.stringify(history.undo().pipeline), saved, "undo must restore the saved pipeline");
const redone = history.redo();
assert.deepEqual(redone.pipeline.pipeline, [], "caller mutations must not alter recorded history");
redone.pipeline.pipeline.push({ name: "another_mutation" });
history.undo();
assert.deepEqual(history.redo().pipeline.pipeline, []);
history.undo();
history.record({ pipeline: { comment: "new branch", pipeline: [] }, selected: "" });
assert.equal(history.canRedo, false, "a new edit must discard the abandoned redo branch");
history.record({ pipeline: { comment: "second", pipeline: [] } });
history.record({ pipeline: { comment: "third", pipeline: [] } });
assert.equal(history.undo().pipeline.comment, "second");
assert.equal(history.undo().pipeline.comment, "new branch");
assert.equal(history.undo(), null, "bounded history must evict oldest snapshots");
history.reset(initial);
assert.equal(history.canUndo, false, "opening a document must reset history");
assert.equal(history.canRedo, false);

const drafts = createDrafts();
drafts.set("json", "{ incomplete JSON");
assert.equal(drafts.pending, true);
assert.equal(drafts.get("json"), "{ incomplete JSON", "even invalid text must survive repainting");
assert.deepEqual(drafts.pendingExcept("node"), ["json"]);
assert.deepEqual(drafts.pendingExcept("json"), []);
const nodeBuffer = { nodeName: "renamed", "config.temperature": "0.2" };
drafts.set("node", nodeBuffer);
nodeBuffer.nodeName = "mutated";
assert.equal(drafts.get("node").nodeName, "renamed");
drafts.clear("json");
assert.equal(drafts.has("node"), true, "discarding JSON must not discard another form");
drafts.clear();
assert.equal(drafts.pending, false);
console.log("Studio document history and pending editor buffer checks passed");

// 保留导致原始缺陷的浏览器值清洗行为：
// 文本输入框会去掉换行；textarea 的值会把 CR/CRLF 规范化为 LF。
class FormElement {
  constructor(tag) {
    this.tagName = tag.toUpperCase(); this.type = "text"; this.dataset = {};
    this.children = []; this.rawValue = ""; this.listeners = {};
  }
  set innerHTML(value) { throw new Error("Render user-provided content with textContent"); }
  addEventListener(name, callback) { this.listeners[name] = callback; }
  set value(value) {
    value = String(value);
    this.rawValue = this.tagName === "INPUT" && this.type === "text" ? value.replace(/[\r\n]/g, "")
      : this.tagName === "TEXTAREA" ? value.replace(/\r\n?/g, "\n") : value;
  }
  get value() { return this.rawValue; }
  append(...children) { for (const child of children) { child.parent = this; this.children.push(child); } }
  add(child, index) {
    if (index === undefined) this.append(child);
    else { child.parent = this; this.children.splice(index, 0, child); }
  }
  closest(selector) { return (selector === "[data-io-entry]" ? this.dataset.ioEntry !== undefined : this.id === selector.slice(1)) ? this : this.parent?.closest(selector); }
  querySelectorAll(selector) {
    return this.children.flatMap(child => [
      ...(selector === "[data-field]" ? child.dataset.field
        : ["INPUT", "SELECT", "TEXTAREA"].includes(child.tagName)) ? [child] : [],
      ...child.querySelectorAll(selector),
    ]);
  }
}
globalThis.document = { createElement: tag => new FormElement(tag) };
globalThis.Option = class extends FormElement {
  constructor(text, value) { super("option"); this.textContent = text; this.value = value; }
};
const diagnostics = new FormElement("div");
const unsafeText = '<img src=x onerror="alert(1)">';
let selectedDiagnosticNode = "";
appendDiagnostic(diagnostics, {
  code: "PORT_TYPE_MISMATCH", path: `/pipeline/${unsafeText}`, message: unsafeText,
  node_name: "consumer", port: "input", related_nodes: ["producer", unsafeText],
  suggestions: ["连接类型兼容的输出", unsafeText],
}, id => { selectedDiagnosticNode = id; });
const diagnostic = diagnostics.children[0];
const diagnosticText = element => [element.textContent || "", ...element.children.map(diagnosticText)].join("\n");
assert.match(diagnosticText(diagnostic), /PORT_TYPE_MISMATCH/);
assert.match(diagnosticText(diagnostic), /节点：consumer/);
assert.match(diagnosticText(diagnostic), /端口：input/);
assert.match(diagnosticText(diagnostic), /相关节点：producer/);
assert.equal(diagnostic.children.find(child => child.tagName === "P").textContent, unsafeText);
assert.deepEqual(diagnostic.children.find(child => child.tagName === "UL").children.map(child => child.textContent), ["连接类型兼容的输出", unsafeText]);
diagnostic.listeners.click();
assert.equal(selectedDiagnosticNode, "consumer", "diagnostics must retain navigation to the affected node");
appendDiagnostic(diagnostics, { code: "ROOT_TYPE", path: "/", message: "expected object" }, () => assert.fail("root diagnostics have no node"));
assert.equal(diagnostics.children[1].listeners.click, undefined);
console.log("Studio diagnostic details and safe text rendering checks passed");

const stringFields = [
  { name: "template", type: "string", default: "{{primary}}" },
  { name: "separator", type: "string", default: "\n" },
  { name: "optional_prefix", type: "string" },
];
const configuredStrings = { template: "  第一行\r\n{{primary}}\n\t尾行\r", separator: "" };
const renderFields = (values, fields = stringFields, id = "configFields") => {
  const form = new FormElement("form"); form.id = id;
  for (const field of fields) appendConfigField(form, field, values);
  return form;
};
const field = (form, name) => form.querySelectorAll("[data-field]").find(input => input.dataset.field === name);
const describedForm = renderFields({}, [
  { name: "threshold", type: "number", semantic: "最低匹配分数，越高越严格。" },
  { name: "template", type: "string", semantic: unsafeText },
  { name: "model", type: "string", semantic: "model_ref" },
  { name: "undocumented", type: "string" },
]);
assert.equal(describedForm.children[0].children[1].textContent, "最低匹配分数，越高越严格。");
assert.equal(describedForm.children[1].children[1].textContent, unsafeText);
assert.equal(describedForm.children[2].children.length, 1, "model_ref is a binding marker, not user help");
assert.equal(describedForm.children[3].children.length, 1);
const stringForm = renderFields(configuredStrings);
assert.equal(field(stringForm, "template").tagName, "TEXTAREA");
assert.equal(field(stringForm, "template").rows, 4);
assert.equal(field(stringForm, "separator").rows, 1);
assert.deepEqual(readConfigFields(stringForm), configuredStrings, "Apply must preserve whitespace, CRLF and explicit empty strings");
const unconfiguredForm = renderFields({});
assert.deepEqual(readConfigFields(unconfiguredForm), {}, "Apply must keep unset strings omitted, including those with defaults");
field(unconfiguredForm, "separator").value = "";
assert.deepEqual(readConfigFields(unconfiguredForm), { separator: "" }, "clearing a nonempty default must set an empty string");
field(unconfiguredForm, "optional_prefix").value = "new prefix";
assert.deepEqual(readConfigFields(unconfiguredForm), { separator: "", optional_prefix: "new prefix" });
field(stringForm, "template").value = "Edited\n{{primary}}\n";
const repaintedForm = renderFields(configuredStrings);
restoreFormBuffer(repaintedForm, readFormBuffer(stringForm));
assert.deepEqual(readConfigFields(repaintedForm), { template: "Edited\n{{primary}}\n", separator: "" });
const untouchedRepaint = renderFields(configuredStrings);
restoreFormBuffer(untouchedRepaint, readFormBuffer(renderFields(configuredStrings)));
assert.deepEqual(readConfigFields(untouchedRepaint), configuredStrings);
const requiredForm = renderFields({ value: "" }, [{ name: "value", type: "string", required: true }]);
assert.deepEqual(readConfigFields(requiredForm), { value: "" });
assert.equal(field(requiredForm, "value").required, false, "required means present, not nonempty");
const enumForm = renderFields({ value: "fail" }, [{ name: "value", type: "string", enum: ["fail", "empty"] }]);
assert.equal(field(enumForm, "value").dataset.originalValue, undefined, "select fields do not need original-text metadata");
field(enumForm, "value").value = "empty";
assert.deepEqual(readConfigFields(enumForm), { value: "empty" });
const combinedForm = new FormElement("form");
const modelForm = renderFields({ separator: "\n" }, [stringFields[1]], "modelConfigFields");
const backendForm = renderFields({ separator: "" }, [stringFields[1]], "backendConfigFields");
combinedForm.append(modelForm, backendForm);
field(modelForm, "separator").value = "model separator";
field(backendForm, "separator").value = "backend separator";
const combinedBuffer = readFormBuffer(combinedForm);
assert.equal(combinedBuffer["config.separator"], "model separator");
assert.equal(combinedBuffer["backend.separator"], "backend separator");
field(modelForm, "separator").value = "";
field(backendForm, "separator").value = "";
restoreFormBuffer(combinedForm, combinedBuffer);
assert.deepEqual(readConfigFields(modelForm), { separator: "model separator" });
assert.deepEqual(readConfigFields(backendForm), { separator: "backend separator" });
const ioForm = new FormElement("form");
const ioRows = ["output.0", "output.1"].map((scope, index) => {
  const row = new FormElement("fieldset"); row.dataset.ioEntry = scope;
  const params = renderFields({capacity: index + 1}, [{name: "capacity", type: "integer", default: 10}]);
  row.append(params); ioForm.append(row); return params;
});
const ioBuffer = readFormBuffer(ioForm);
assert.equal(ioBuffer["output.0.capacity"], "1");
assert.equal(ioBuffer["output.1.capacity"], "2", "each converter registration needs its own draft parameter scope");
field(ioRows[0], "capacity").value = "11";
field(ioRows[1], "capacity").value = "12";
restoreFormBuffer(ioForm, ioBuffer);
assert.deepEqual(ioRows.map(readConfigFields), [{capacity: 1}, {capacity: 2}]);
console.log("Studio config string and form repaint checks passed");

const tuningFields = [
  { name: "top_p", type: "number", default: 0.9 },
  { name: "ratio", type: "number" },
  { name: "normalize", type: "boolean", default: true },
  { name: "policy", type: "string", enum: ["fail", "truncate"], default: "fail" },
  { name: "stop_words", type: "array", default: [] },
  { name: "options", type: "object", default: {} },
];
for (const formId of ["configFields", "backendConfigFields"]) {
  const unset = renderFields({}, tuningFields, formId);
  assert.equal(field(unset, "normalize").children[0].value, "");
  assert.equal(field(unset, "normalize").children[0].textContent, "默认（true）");
  assert.equal(field(unset, "top_p").placeholder, "默认 0.9");
  assert.equal(field(unset, "ratio").placeholder, "");
  assert.equal(field(unset, "options").placeholder, "{}");
  assert.deepEqual(readConfigFields(unset), {}, "untouched defaults must stay unset");
  const repainted = renderFields({}, tuningFields, formId);
  restoreFormBuffer(repainted, readFormBuffer(unset));
  assert.deepEqual(readConfigFields(repainted), {}, "draft repaint must keep fields unset");
  const explicit = { top_p: 0.9, normalize: true, policy: "fail", stop_words: [], options: {} };
  const pinned = renderFields(explicit, tuningFields, formId);
  assert.deepEqual(readConfigFields(pinned), explicit, "explicit values equal to defaults stay explicit");
  for (const name of Object.keys(explicit)) field(pinned, name).value = "";
  assert.deepEqual(readConfigFields(pinned), {}, "clearing an override restores the default");
  field(unset, "top_p").value = "0.5";
  field(unset, "normalize").value = "false";
  field(unset, "policy").value = "truncate";
  assert.deepEqual(readConfigFields(unset), { top_p: 0.5, normalize: false, policy: "truncate" });
}
for (const type of ["integer", "number"]) {
  assert.throws(() => readConfigFields(renderFields({}, [{ name: "dim", type, required: true }])), /dim：请输入数值/);
}
const optionalBoolean = renderFields({}, [{ name: "enabled", type: "boolean" }]);
assert.equal(field(optionalBoolean, "enabled").children[0].textContent, "未设置");
assert.deepEqual(readConfigFields(optionalBoolean), {});
const modelReference = new FormElement("form");
appendConfigField(modelReference, { name: "model", type: "string", required: true }, { model: "m1" }, ["m1"]);
assert.equal(field(modelReference, "model").dataset.unsetOption, undefined);
assert.deepEqual(readConfigFields(modelReference), { model: "m1" });

const structuredFields = [
  {name: "endpoints", type: "map", required: true, items: {type: "object", fields: [{name: "prompt", type: "string", default: "{{input}}"}]}},
  {name: "policies", type: "map", enum: ["keep", "drop"], default: {}},
  {name: "payload", type: "json"},
  {name: "records", type: "array", items: {type: "object", fields: [{name: "labels", type: "map", items: {type: "array", items: {type: "string"}}}]}},
];
const structuredValues = {endpoints: {answer: {prompt: "首行\r\n{{input}}"}}, policies: {answer: "keep"}, payload: [true, {message: unsafeText}], records: [{labels: {tags: ["A", "B"]}}]};
const structuredForm = renderFields(structuredValues, structuredFields);
assert.equal(field(structuredForm, "policies").tagName, "TEXTAREA", "element enums must not turn a whole map into a scalar select");
assert.deepEqual(readConfigFields(structuredForm), structuredValues, "map/json/nested structure must round-trip without changing payloads");
field(structuredForm, "payload").value = JSON.stringify("JSON scalar string");
assert.equal(readConfigFields(structuredForm).payload, "JSON scalar string");
assert.deepEqual(readConfigFields(renderFields({}, structuredFields.slice(1))), {}, "unset structured defaults remain omitted");
field(structuredForm, "endpoints").value = "{";
assert.throws(() => readConfigFields(structuredForm), /endpoints：/);
const fileForm = renderFields({}, [{name: "tokenizer_file", type: "string", file: true}]);
assert.match(fileForm.children[0].children[1].textContent, /相对方案所在目录/);
console.log("Studio map/json/nested parameter and file hint checks passed");

const workbenchSource = readFileSync(new URL("../../tools/pipeline_studio/web/workbench.js", import.meta.url), "utf8");
const { readPipelineFile, modelAvailability, upsertModel } = await import(`data:text/javascript;base64,${Buffer.from(workbenchSource).toString("base64")}`);
const input = { io: {input: [{type: "unknown_input", name: "still_viewable"}], output: [{type: "unknown_output", name: "still_viewable"}]}, pipeline: [], models: [] };
const file = { name: "selected.json", size: 30, text: async () => JSON.stringify(input) };
assert.deepEqual(await readPipelineFile(file), { filename: "selected.json", revision: "", imported: true, pipeline: input });
assert.deepEqual((await readPipelineFile({ ...file, text: async () => "\uFEFF" + JSON.stringify(input) })).pipeline, input);
await assert.rejects(readPipelineFile({ ...file, name: "directory" }), /JSON/);
await assert.rejects(readPipelineFile({ ...file, size: 4 * 1024 * 1024 + 1 }), /4 MiB/);
await assert.rejects(readPipelineFile({ ...file, text: async () => "{" }), SyntaxError);
await assert.rejects(readPipelineFile({ ...file, text: async () => "[]" }), /pipeline/);
await assert.rejects(readPipelineFile({ ...file, text: async () => '{"pipeline":{}}' }), /pipeline/);
for (const malformed of [{ pipeline: [null] }, { pipeline: [{ depends_on: 1 }] }, { pipeline: [], models: {} }, { pipeline: [], io: [] }, { pipeline: [], io: {input: {}} }, { pipeline: [], io: {output: [null]} }]) {
  await assert.rejects(readPipelineFile({ ...file, text: async () => JSON.stringify(malformed) }), /pipeline|models|io/);
}

const modelDefinition = {impl_name: "vision_ocr", model_type: "ocr", backends: ["vision_backend"]};
assert.match(modelAvailability([], modelDefinition).message, /当前构建无兼容 Backend/);
assert.equal(modelAvailability([{backend_type: "text_backend"}], modelDefinition).available, false);
assert.deepEqual(modelAvailability([{backend_type: "vision_backend"}], modelDefinition), { available: true, message: "" });
assert.match(modelAvailability([], null).message, /未注册/);
const pipeline = { models: [], pipeline: [] };
assert.throws(() => upsertModel(pipeline, { models: [modelDefinition], backends: [], nodes: [] }, "", {
  name: "vision", type: "ocr", file: "model.bin", backend: {type: "vision_backend"},
}), /当前构建无兼容 Backend/);
assert.deepEqual(pipeline, { models: [], pipeline: [] }, "unavailable model rejection must preserve the document");
console.log("Studio single-file import and model availability checks passed");

const workflowSource = readFileSync(new URL("../../tools/pipeline_studio/web/workflow.js", import.meta.url), "utf8");
const { captureRun, runIsCurrent, runSummary, renderSamples } = await import(`data:text/javascript;base64,${Buffer.from(workflowSource).toString("base64")}`);
const runInput = { documentVersion: 1, pipeline: { pipeline: [] }, filename: "a.json", profile: "rules" };
const run = captureRun(runInput);
assert.equal(runIsCurrent(run, runInput), true);
runInput.pipeline.pipeline.push({ name: "edited" });
assert.equal(runIsCurrent(run, runInput), false, "edits cannot mutate the submitted snapshot");
runInput.pipeline.pipeline.pop();
assert.equal(runIsCurrent(run, runInput), true, "undo can restore the submitted content");
for (const changed of [{ pending: true }, { documentVersion: 2 }, { profile: "other" }]) {
  assert.equal(runIsCurrent(run, { ...runInput, ...changed }), false);
}
assert.match(runSummary({ status: "completed", result: { "summary.json": { total_samples: 2, success_count: 1, failed_count: 1 } } }), /失败 1 条/);
assert.doesNotMatch(runSummary({ status: "completed" }), /成功.*条/, "process completion must not invent sample successes");
assert.match(runSummary({ status: "failed", error: { message: "missing asset" } }), /missing asset/);
const samples = new FormElement("div"); samples.replaceChildren = () => { samples.children = []; };
renderSamples(samples, { "results.jsonl": [{ request_id: 42, status: 0, output: { answer: unsafeText } }] });
assert.equal(samples.children[0].children[1].children[1].textContent, unsafeText, "sample output must be rendered as text");
renderSamples(samples, null);
assert.equal(samples.children.length, 0, "changing documents must remove old sample cards");
console.log("Studio run snapshot, sample status and safe result rendering checks passed");
