import assert from "node:assert/strict";
import { readFileSync } from "node:fs";

const source = readFileSync(new URL("../../tools/pipeline_studio/web/editor.js", import.meta.url), "utf8");
const { createHistory, createDrafts } = await import(`data:text/javascript;base64,${Buffer.from(source).toString("base64")}`);
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
