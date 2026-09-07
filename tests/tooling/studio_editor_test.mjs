import assert from "node:assert/strict";
import { readFileSync } from "node:fs";

const source = readFileSync(new URL("../../tools/pipeline_studio/web/editor.js", import.meta.url), "utf8");
const { createHistory, createDrafts, appendConfigField, readConfigFields, readFormBuffer, restoreFormBuffer } = await import(`data:text/javascript;base64,${Buffer.from(source).toString("base64")}`);
const history = createHistory(3);
const initial = { pipeline: { pipeline: [{ id: "root", depends_on: [] }] }, selected: "root" };
const saved = JSON.stringify(initial.pipeline);
history.reset(initial);
const removed = { pipeline: { pipeline: [] }, selected: "" };
history.record(removed);
removed.pipeline.pipeline.push({ id: "outside_mutation" });
assert.equal(JSON.stringify(history.undo().pipeline), saved, "undo must restore the saved pipeline");
const redone = history.redo();
assert.deepEqual(redone.pipeline.pipeline, [], "caller mutations must not alter recorded history");
redone.pipeline.pipeline.push({ id: "another_mutation" });
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
const nodeBuffer = { nodeId: "renamed", "config.temperature": "0.2" };
drafts.set("node", nodeBuffer);
nodeBuffer.nodeId = "mutated";
assert.equal(drafts.get("node").nodeId, "renamed");
drafts.clear("json");
assert.equal(drafts.has("node"), true, "discarding JSON must not discard another form");
drafts.clear();
assert.equal(drafts.pending, false);
console.log("Studio document history and pending editor buffer checks passed");

// Keep the browser value-sanitization behavior that caused the original bug:
// text inputs remove newlines; textarea values normalize CR/CRLF to LF.
class FormElement {
  constructor(tag) {
    this.tagName = tag.toUpperCase(); this.type = "text"; this.dataset = {};
    this.children = []; this.rawValue = "";
  }
  set value(value) {
    value = String(value);
    this.rawValue = this.tagName === "INPUT" && this.type === "text" ? value.replace(/[\r\n]/g, "")
      : this.tagName === "TEXTAREA" ? value.replace(/\r\n?/g, "\n") : value;
  }
  get value() { return this.rawValue; }
  append(...children) { for (const child of children) { child.parent = this; this.children.push(child); } }
  add(child) { this.append(child); }
  closest(selector) { return this.id === selector.slice(1) ? this : this.parent?.closest(selector); }
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
console.log("Studio config string and form repaint checks passed");

const workbenchSource = readFileSync(new URL("../../tools/pipeline_studio/web/workbench.js", import.meta.url), "utf8");
const { readPipelineFile, modelAvailability, upsertModel } = await import(`data:text/javascript;base64,${Buffer.from(workbenchSource).toString("base64")}`);
const input = { biz_name: "unknown_biz_is_still_viewable", pipeline: [], models: [] };
const file = { name: "selected.json", size: 30, text: async () => JSON.stringify(input) };
assert.deepEqual(await readPipelineFile(file), { filename: "selected.json", revision: "", imported: true, pipeline: input });
assert.deepEqual((await readPipelineFile({ ...file, text: async () => "\uFEFF" + JSON.stringify(input) })).pipeline, input);
await assert.rejects(readPipelineFile({ ...file, name: "directory" }), /JSON/);
await assert.rejects(readPipelineFile({ ...file, size: 4 * 1024 * 1024 + 1 }), /4 MiB/);
await assert.rejects(readPipelineFile({ ...file, text: async () => "{" }), SyntaxError);
await assert.rejects(readPipelineFile({ ...file, text: async () => "[]" }), /pipeline/);
await assert.rejects(readPipelineFile({ ...file, text: async () => '{"pipeline":{}}' }), /pipeline/);
for (const malformed of [{ pipeline: [null] }, { pipeline: [{ depends_on: 1 }] }, { pipeline: [], models: {} }]) {
  await assert.rejects(readPipelineFile({ ...file, text: async () => JSON.stringify(malformed) }), /pipeline|models/);
}

const modelDefinition = { model_type: "vision", capability: "ocr", required_protocol: "image_text_generation" };
assert.match(modelAvailability([], modelDefinition).message, /当前构建无兼容 Backend.*image_text_generation/);
assert.equal(modelAvailability([{ supported_protocols: ["text_generation"] }], modelDefinition).available, false);
assert.deepEqual(modelAvailability([{ supported_protocols: ["image_text_generation"] }], modelDefinition), { available: true, message: "" });
assert.match(modelAvailability([], null).message, /未注册/);
const pipeline = { models: [], pipeline: [] };
assert.throws(() => upsertModel(pipeline, { models: [modelDefinition], backends: [], nodes: [] }, "", {
  model_id: "vision", model_type: "vision", model_path: "model.bin", backend: "",
}), /当前构建无兼容 Backend/);
assert.deepEqual(pipeline, { models: [], pipeline: [] }, "unavailable model rejection must preserve the document");
console.log("Studio single-file import and model availability checks passed");
