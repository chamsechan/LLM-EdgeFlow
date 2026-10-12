import assert from 'node:assert/strict';
import {readFile} from 'node:fs/promises';

const web = new URL('../../tools/pipeline_studio/web/', import.meta.url);
const load = async name => import(`data:text/javascript;base64,${Buffer.from(await readFile(new URL(name, web))).toString('base64')}`);
const workbench = await load('workbench.js');
const base = process.argv[2];
const file = text => ({name: 'external.JSON', size: Buffer.byteLength(text), text: async () => text});
const imported = await workbench.readPipelineFile(file('\uFEFF{"pipeline":[{"name":"duplicate","type":"unknown"}],"unknown":{"keep":true}}'));
assert.equal(imported.imported, true);
assert.equal(imported.revision, '');
assert.equal(imported.filename, 'external.JSON');
assert.deepEqual(imported.pipeline.unknown, {keep: true});
await assert.rejects(workbench.readPipelineFile({...file('{}'), name: 'wrong.txt'}), /JSON/);
await assert.rejects(workbench.readPipelineFile({...file('{}'), size: 4 * 1024 * 1024 + 1}), /4 MiB/);
await assert.rejects(workbench.readPipelineFile(file('{broken')), SyntaxError);
assert.throws(() => workbench.assertBrowsablePipeline({pipeline: [{depends_on: 'bad'}]}), /depends_on/);
assert.doesNotThrow(() => workbench.assertBrowsablePipeline({pipeline: [{name: '', inputs: {text: 'malformed'}}]}));

const nested = {name: 'options', default: {values: [1, false]}};
const defaults = workbench.schemaDefaults([nested, {name: 'missing'}, {name: 'null', default: null}]);
defaults.options.values.push(2);
assert.deepEqual(nested.default, {values: [1, false]});
assert.deepEqual(defaults.null, null);
assert.equal('missing' in defaults, false);
const catalog = {
  nodes: [{node_type: 'generate', model_dependencies: [{config_field: 'encoder', model_type: 'llm'}]}],
  models: [{model_type: 'llm', backends: ['runtime']}, {model_type: 'embedding', backends: ['runtime']}],
  backends: [{backend_type: 'runtime'}, {backend_type: 'other'}],
};
const original = {name: 'old', type: 'llm', file: 'weights.bin', backend: {type: 'runtime', config: {}}};
const pipeline = {
  models: [structuredClone(original)],
  pipeline: [{name: 'consumer', type: 'generate', params: {encoder: 'old', literal: 'old'}},
    {name: 'unknown', type: 'custom_missing', params: {encoder: 'old'}}],
};
assert.deepEqual([...workbench.modelBoundNodeIds(pipeline.pipeline, catalog.nodes)], ['consumer']);
assert.deepEqual(workbench.compatibleModels([original, {type: 'embedding'}], catalog.models, catalog.nodes[0]), [original]);
assert.deepEqual(workbench.compatibleBackends(catalog.backends, catalog.models[0]), [{backend_type: 'runtime'}]);
assert.equal(workbench.modelAvailability([], catalog.models[0]).available, false);
const renamed = {...original, name: 'new'};
workbench.upsertModel(pipeline, catalog, 'old', renamed);
assert.equal(pipeline.pipeline[0].params.encoder, 'new');
assert.equal(pipeline.pipeline[0].params.literal, 'old');
assert.equal(pipeline.pipeline[1].params.encoder, 'old');
const beforeFailure = structuredClone(pipeline);
assert.throws(() => workbench.upsertModel(pipeline, catalog, 'new', {...renamed, type: 'embedding'}), /不兼容/);
assert.deepEqual(pipeline, beforeFailure);
assert.throws(() => workbench.removeModel(pipeline, catalog, 'new'), /仍被/);
assert.deepEqual(pipeline, beforeFailure);
pipeline.pipeline[0].params.encoder = '';
workbench.removeModel(pipeline, catalog, 'new');
assert.deepEqual(pipeline.models, []);
const asset = {model: {...original, backend: {type: 'runtime', config: {'a/b': {'~file': 'old'}}}},
  paths: {'/file': '/resolved/weights', '/backend/config/a~1b/~0file': '/resolved/nested'}};
const resolved = workbench.assetModel(asset);
assert.equal(resolved.file, '/resolved/weights');
assert.equal(resolved.backend.config['a/b']['~file'], '/resolved/nested');
assert.equal(asset.model.file, 'weights.bin');
const io = {io: {input: [{type: 'a', name: 'b', params: {keep: 1}}], output: [{type: 'c', name: 'd', inputs: {x: 'n.y'}}]}};
assert.deepEqual(workbench.pipelineIo(io), {input: [{type: 'a', name: 'b'}], output: [{type: 'c', name: 'd'}]});
assert.equal(workbench.ioLabel(workbench.pipelineIo(io)), 'a/b → c/d');

// The async gate owns replacement of frontend state, including stale errors.
const gate = workbench.createLatestRequestGate();
let completeOld, committed;
const old = gate.run(() => new Promise(resolve => completeOld = resolve), value => committed = value);
assert.equal(await gate.run(async () => 'current', value => committed = value), true);
completeOld('stale');
assert.equal(await old, false);
assert.equal(committed, 'current');
let rejectOld;
const obsoleteError = gate.run(() => new Promise((_, reject) => rejectOld = reject), () => assert.fail('stale commit'));
gate.invalidate();
rejectOld(new Error('old failed'));
assert.equal(await obsoleteError, false);
await assert.rejects(gate.run(async () => { throw new Error('current failed'); }, () => {}), /current failed/);

// Execute the actual API transport with proxy prefixes and native HTTP responses.
globalThis.location = {href: `${base || 'http://localhost'}/proxy/index.html`, hash: '#pipeline=external.json'};
const {api, write, initialPipeline} = await load('api.js');
assert.equal(initialPipeline, 'external.json');
const nativeFetch = globalThis.fetch;
let requested;
globalThis.fetch = async (url, options) => {
  requested = {url: String(url), options};
  return new Response('{"ok":true,"value":1}', {status: 200});
};
await write('/validate', 'POST', {pipeline: {pipeline: []}});
assert.equal(new URL(requested.url).pathname, '/proxy/api/validate');
assert.equal(requested.options.method, 'POST');
assert.deepEqual(JSON.parse(requested.options.body), {pipeline: {pipeline: []}});
globalThis.fetch = async () => new Response('<html>proxy error</html>', {status: 502});
await assert.rejects(api('/catalog'), error => error.status === 502 && /非 JSON/.test(error.message));
globalThis.fetch = async () => new Response('{"ok":false,"error":{"code":"CONFLICT","message":"changed"}}', {status: 409});
await assert.rejects(api('/pipeline'), error => error.status === 409 && error.payload.error.code === 'CONFLICT');
globalThis.fetch = nativeFetch;
if (base) {
  globalThis.location.href = `${base}/index.html`;
  const catalog = await api('/catalog');
  assert.ok(catalog.nodes.length > 0);
  const report = await write('/validate', 'POST', {pipeline: {pipeline: []}}, true);
  assert.equal(report.ok, false);
}
console.log('PASS Studio config roundtrip, model bindings and API transport');
