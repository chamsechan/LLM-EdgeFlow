import assert from 'node:assert/strict';
import {readFile} from 'node:fs/promises';

const web = new URL('../../tools/pipeline_studio/web/', import.meta.url);
const load = async name => import(`data:text/javascript;base64,${Buffer.from(await readFile(new URL(name, web))).toString('base64')}`);
const editor = await load('editor.js');
const workflow = await load('workflow.js');

// A small DOM carrier for exported renderers; browser interaction has its own consumer.
class Element {
  constructor(tag) {
    this.tagName = tag.toUpperCase();
    this.children = [];
    this.dataset = {};
    this.style = {};
    this.events = {};
    this.textContent = '';
    this._value = '';
  }
  set value(value) {
    this._value = this.tagName === 'TEXTAREA' ? String(value).replace(/\r\n?/g, '\n') : String(value);
  }
  get value() { return this._value; }
  append(...elements) {
    for (const element of elements) {
      element.parent = this;
      this.children.push(element);
    }
  }
  add(option, index) {
    option.parent = this;
    if (index === undefined) this.children.push(option);
    else this.children.splice(index, 0, option);
  }
  replaceChildren(...elements) { this.children = []; this.append(...elements); }
  addEventListener(type, callback) { this.events[type] = callback; }
  closest(selector) {
    for (let element = this; element; element = element.parent) {
      if (selector === '[data-io-entry]' && element.dataset.ioEntry) return element;
      if (selector === '#backendConfigFields' && element.id === 'backendConfigFields') return element;
    }
    return null;
  }
  querySelectorAll(selector) {
    const matches = element => selector === '[data-field]'
      ? element.dataset.field !== undefined
      : ['INPUT', 'SELECT', 'TEXTAREA'].includes(element.tagName);
    return this.children.flatMap(child => [...(matches(child) ? [child] : []), ...child.querySelectorAll(selector)]);
  }
  setCustomValidity(value) { this.validityMessage = value; }
  reportValidity() { this.validityReported = true; }
  focus() { this.focused = true; }
}
globalThis.document = {createElement: tag => new Element(tag)};
globalThis.Option = class extends Element {
  constructor(label, value) { super('option'); this.textContent = label; this.value = value; }
};

const history = editor.createHistory(3);
const original = {pipeline: [{name: 'first'}]};
history.reset(original);
original.pipeline[0].name = 'external mutation';
history.record({pipeline: [{name: 'second'}]});
assert.equal(history.undo().pipeline[0].name, 'first');
const second = history.redo();
second.pipeline[0].name = 'returned mutation';
assert.equal(history.undo().pipeline[0].name, 'first');
history.record({pipeline: [{name: 'branch'}]});
assert.equal(history.canRedo, false);
history.record({pipeline: [{name: 'third'}]});
history.record({pipeline: [{name: 'fourth'}]});
assert.equal(history.undo().pipeline[0].name, 'third');
assert.equal(history.undo().pipeline[0].name, 'branch');
assert.equal(history.undo(), null);

const drafts = editor.createDrafts();
const buffer = {prompt: '  untouched\r\n提示  '};
drafts.set('node', buffer);
buffer.prompt = 'external mutation';
assert.equal(drafts.get('node').prompt, '  untouched\r\n提示  ');
drafts.set('json', '{broken draft');
assert.deepEqual(drafts.pendingExcept('node'), ['json']);
drafts.clear('node');
assert.equal(drafts.has('json'), true);
drafts.clear();
assert.equal(drafts.pending, false);

const fields = new Element('div');
const declarations = [
  {name: 'prompt', type: 'string', required: true},
  {name: 'optional', type: 'string', default: 'untouched'},
  {name: 'count', type: 'integer', default: 4},
  {name: 'enabled', type: 'boolean', default: true},
  {name: 'options', type: 'object', default: {nested: ['default']}},
];
for (const field of declarations) editor.appendConfigField(fields, field, {prompt: '  line\r\n提示  '});
assert.deepEqual(editor.readConfigFields(fields), {prompt: '  line\r\n提示  '});
assert.deepEqual(editor.readConfigFields(fields, {future: {nested: ['保留', 1]}}), {
  prompt: '  line\r\n提示  ', future: {nested: ['保留', 1]},
});
const [prompt, optional, count, enabled, options] = fields.querySelectorAll('[data-field]');
assert.equal(prompt.required, false);
count.value = '0';
enabled.value = 'false';
options.value = '{"nested":[1,false,{"unicode":"中文"}]}';
optional.value = '   ';
assert.deepEqual(editor.readConfigFields(fields), {
  prompt: '  line\r\n提示  ', optional: '   ', count: 0, enabled: false,
  options: {nested: [1, false, {unicode: '中文'}]},
});
prompt.value = '';
assert.equal(editor.readConfigFields(fields).prompt, '');
count.value = '9007199254740992';
assert.throws(() => editor.readConfigFields(fields), /count/);
assert.equal(count.focused, true);
assert.equal(count.validityReported, true);
count.value = '2';
options.value = '{broken';
assert.throws(() => editor.readConfigFields(fields), /options/);
assert.equal(options.value, '{broken');

const form = new Element('form');
const backend = new Element('div');
backend.id = 'backendConfigFields';
const entry = new Element('div');
entry.dataset.ioEntry = 'output:0';
for (const container of [backend, entry]) {
  editor.appendConfigField(container, {name: 'value', type: 'string'}, {value: container === backend ? 'backend' : 'output'});
  form.append(container);
}
const snapshot = editor.readFormBuffer(form);
assert.deepEqual(snapshot, {'backend.value': 'backend', 'output:0.value': 'output'});
for (const input of form.querySelectorAll('input, select, textarea')) input.value = 'changed';
editor.restoreFormBuffer(form, snapshot);
assert.deepEqual(editor.readFormBuffer(form), snapshot);

const diagnostics = new Element('div');
let selected, previewed, stopped = false;
const fix = {title: '<script>title</script>', effect: 'literal', verification: 'pipeline_valid'};
editor.appendDiagnostic(diagnostics, {
  code: 'MISSING', path: '/pipeline/0/inputs', message: '<img onerror=alert(1)>',
  node_name: 'node', suggestions: ['<b>suggestion</b>'], remediation: {fixes: [fix]},
}, name => selected = name, candidate => previewed = candidate);
const card = diagnostics.children[0];
assert.equal(card.children[2].textContent, '<img onerror=alert(1)>');
card.events.click();
assert.equal(selected, 'node');
const button = card.children.find(child => child.className === 'fix-candidate').children[1];
button.events.click({stopPropagation() { stopped = true; }});
assert.equal(previewed, fix);
assert.equal(stopped, true);

const state = {documentVersion: 7, pipeline: {pipeline: []}, filename: 'pipeline.json', profile: 'keyword'};
const run = workflow.captureRun(state);
assert.equal(workflow.runIsCurrent(run, {...state, pending: false}), true);
assert.equal(workflow.runIsCurrent(run, {...state, pending: true}), false);
assert.equal(workflow.runIsCurrent(run, {...state, documentVersion: 8}), false);
state.pipeline.pipeline.push({name: 'changed'});
assert.equal(JSON.parse(run.pipeline).pipeline.length, 0);
assert.equal(workflow.runIsCurrent(run, state), false);
const summary = workflow.runSummary({status: 'completed', result: {'summary.json': {
  total_samples: 3, success_count: 2, failed_count: 1,
}}});
assert.match(summary, /成功 2 条/);
assert.match(summary, /失败 1 条/);
const samples = new Element('div');
workflow.renderSamples(samples, {'results.jsonl': Array.from({length: 52}, () => ({status: 0, output: {text: '<img>'}}))});
assert.equal(samples.children.length, 51);
assert.equal(samples.children[0].open, true);
assert.equal(samples.children[0].children[1].children[1].textContent, '<img>');
assert.match(samples.children[50].textContent, /50 \/ 52/);
workflow.renderSamples(samples, {});
assert.equal(samples.children.length, 0);
console.log('PASS Studio editor, drafts, diagnostics and run snapshots');
