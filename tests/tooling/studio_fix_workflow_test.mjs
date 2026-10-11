import assert from 'node:assert/strict';
import {readFile} from 'node:fs/promises';

const base = process.argv[2];
assert.ok(base, 'HTTP consumer requires the running isolated Studio URL');
globalThis.location = {href: `${base}/index.html`, hash: ''};
const web = new URL('../../tools/pipeline_studio/web/', import.meta.url);
const load = async name => import(`data:text/javascript;base64,${Buffer.from(await readFile(new URL(name, web))).toString('base64')}`);
const {write, api} = await load('api.js');
const {createHistory} = await load('editor.js');
const opened = await api('/pipeline?filename=pipeline_keyword_match_rules.json');
const beforeFile = structuredClone(opened.pipeline);
const draft = structuredClone(opened.pipeline);
delete draft.pipeline[0].inputs.text;
const report = await write('/validate', 'POST', {pipeline: draft, explain: true}, true);
assert.equal(report.ok, false);
assert.ok(report.revision && report.tool_fingerprint);
const snapshot = structuredClone(draft);
const patch = [{op: 'add', path: '/pipeline/0/inputs/text', value: 'input.sentence_text'}];
const body = {pipeline: draft, patch, revision: report.revision, tool_fingerprint: report.tool_fingerprint};
const preview = await write('/fixes/preview', 'POST', body);
assert.equal(preview.ok, true);
assert.equal(preview.report.ok, true);
assert.equal(preview.patched.pipeline[0].inputs.text, 'input.sentence_text');
assert.deepEqual(draft, snapshot, 'preview cannot mutate the caller draft');
assert.deepEqual((await api('/pipeline?filename=pipeline_keyword_match_rules.json')).pipeline, beforeFile);
assert.notEqual(preview.revision, report.revision);
const secondPreview = await write('/fixes/preview', 'POST', body);
assert.deepEqual(secondPreview.patched, preview.patched);
const history = createHistory();
history.reset(snapshot);
history.record(secondPreview.patched);
assert.deepEqual(history.undo(), snapshot);
assert.equal(history.undo(), null);
assert.deepEqual(history.redo(), beforeFile);

await assert.rejects(write('/fixes/preview', 'POST', {...body, revision: 'old'}),
  error => error.status === 409 && error.payload.error.code === 'REVISION_CONFLICT');
await assert.rejects(write('/fixes/preview', 'POST', {...body, tool_fingerprint: 'old'}),
  error => error.status === 409 && error.payload.error.code === 'TOOL_OUTDATED');
const changed = structuredClone(draft);
changed.comment = 'changed after validation';
await assert.rejects(write('/fixes/preview', 'POST', {...body, pipeline: changed}),
  error => error.status === 409 && error.payload.error.code === 'REVISION_CONFLICT');
await assert.rejects(write('/fixes/preview', 'POST', {...body, patch: [{op: 'remove', path: '/absent'}]}),
  error => error.status === 400 && error.payload.error.code === 'PATCH_APPLICATION_FAILED');
assert.deepEqual((await api('/pipeline?filename=pipeline_keyword_match_rules.json')).pipeline, beforeFile);

// Remediation is native Validator data, preserved by the service for UI review.
const fixes = report.diagnostics.flatMap(item => item.remediation?.fixes || []);
assert.ok(fixes.length > 0, 'missing required input should offer a native repair');
const nativeFix = fixes.find(fix => fix.patch.some(operation => operation.path === '/pipeline/0/inputs' && operation.value?.text === 'input.sentence_text'));
assert.ok(nativeFix, 'native remediation connects the required logical input');
const nativePreview = await write('/fixes/preview', 'POST', {...body, patch: nativeFix.patch});
assert.equal(nativePreview.report.ok, true);
assert.deepEqual(nativePreview.patched, beforeFile);
console.log('PASS Studio native fix preview, revisions, fingerprint and one-step history');
