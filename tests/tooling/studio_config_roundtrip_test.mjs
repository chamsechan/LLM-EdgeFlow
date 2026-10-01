// 用法：node studio_config_roundtrip_test.mjs <catalog.json> <pipeline.json>...
// 逐个渲染 Node、Model、Backend 表单，不修改直接读回，断言与配置中的原值完全一致。
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";

const source = readFileSync(new URL("../../tools/pipeline_studio/web/editor.js", import.meta.url), "utf8");
const { appendConfigField, readConfigFields } = await import(`data:text/javascript;base64,${Buffer.from(source).toString("base64")}`);

class FormElement {
  constructor(tag) {
    this.tagName = tag.toUpperCase(); this.type = "text"; this.dataset = {};
    this.children = []; this.rawValue = ""; this.listeners = {};
  }
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

const [catalogPath, ...pipelinePaths] = process.argv.slice(2);
const catalog = JSON.parse(readFileSync(catalogPath, "utf8"));
const list = value => Array.isArray(value) ? value : Object.values(value ?? {});
const find = (kind, key, name) => list(catalog[kind]).find(item => item[key] === name);
const roundTrip = (formId, fields, values, choicesFor = () => null) => {
  const form = new FormElement("form"); form.id = formId;
  for (const field of fields) appendConfigField(form, field, values, choicesFor(field));
  return readConfigFields(form);
};

let failures = 0;
for (const path of pipelinePaths) {
  const pipeline = JSON.parse(readFileSync(path, "utf8"));
  const modelIds = (pipeline.models ?? []).map(model => model.model_id);
  const check = (label, actual, expected) => {
    try { assert.deepStrictEqual(actual, expected); } catch {
      failures += 1;
      console.log(`FAIL ${path} ${label}\n  expected ${JSON.stringify(expected)}\n  actual   ${JSON.stringify(actual)}`);
    }
  };
  for (const node of pipeline.pipeline ?? []) {
    const definition = find("nodes", "node_type", node.node_type);
    if (!definition) continue;
    const dependencies = definition.model_dependencies ?? [];
    const choicesFor = field =>
      dependencies.some(dep => dep.config_field === field.name) || field.semantic === "model_ref" ? modelIds : null;
    check(`node ${node.id}`, roundTrip("configFields", definition.config_fields ?? [], node.config ?? {}, choicesFor), node.config ?? {});
  }
  for (const model of pipeline.models ?? []) {
    const modelDefinition = find("models", "model_type", model.model_type);
    const backendDefinition = find("backends", "backend_type", model.backend);
    if (modelDefinition) {
      check(`model ${model.model_id}.model_config`,
            roundTrip("modelConfigFields", modelDefinition.config_fields ?? [], model.model_config ?? {}),
            model.model_config ?? {});
    }
    if (backendDefinition) {
      check(`model ${model.model_id}.backend_config`,
            roundTrip("backendConfigFields", backendDefinition.config_fields ?? [], model.backend_config ?? {}),
            model.backend_config ?? {});
    }
  }
}
console.log(failures ? `${failures} form(s) changed on untouched apply` : "All forms round-trip unchanged");
process.exit(failures ? 1 : 0);
