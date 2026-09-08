import { api, initialPipeline, write } from "./api.js";
import { GraphView } from "./graph.js";
import { createHistory, createDrafts, appendDiagnostic, appendConfigField, readConfigFields, readFormBuffer, restoreFormBuffer } from "./editor.js";
import { compatibleModels, createLatestRequestGate, modelBoundNodeIds, graphDocument, connectPorts, disconnectPorts, removeNode, compatibleBackends, modelAvailability, assertBrowsablePipeline, readPipelineFile, upsertModel, removeModel } from "./workbench.js";

const $ = selector => document.querySelector(selector);
const state = {
  pipeline: null,
  filename: "",
  sourceName: "",
  revision: "",
  dirty: false,
  catalog: { nodes: [], bizs: [], profiles: [] },
  catalogReady: false,
  profiles: [],
  catalogProfiles: [],
  assets: [],
  selected: "",
  jobId: "",
  errorNodeIds: new Set(),
  pipelineVersion: 0,
  documentVersion: 0,
  savedPipeline: "",
  selectedEdge: null,
  editing: false,
  saving: false,
  loading: false,
  enteredEditor: false,
};
const catalogRequests = createLatestRequestGate();
const history = createHistory();
const drafts = createDrafts();
let documentRequest = 0;

function snapshot() { return { pipeline: state.pipeline, selected: state.selected }; }

function updateEditorStatus() {
  const pending = drafts.pending;
  const badge = $("#dirtyBadge");
  badge.textContent = state.loading ? "加载中…" : !state.pipeline ? "未打开" : pending ? "有未应用修改" : state.dirty ? "未保存" : state.sourceName ? "已导入" : "已保存";
  badge.classList.toggle("dirty", pending || state.dirty);
  $("#undoButton").disabled = state.loading || pending || !history.canUndo;
  $("#redoButton").disabled = state.loading || pending || !history.canRedo;
  $("#saveButton").disabled = state.loading || state.saving || !state.pipeline;
  $("#saveAsButton").disabled = state.loading || state.saving || !state.pipeline;
  $("#openButton").disabled = state.loading;
  $("#browseButton").disabled = state.loading;
  $("#newButton").disabled = state.loading;
  $("#rawJson").readOnly = state.loading || drafts.pendingExcept("json").length > 0;
  for (const [kind, form] of [["node", "#nodeForm"], ["model", "#modelForm"]]) {
    for (const control of $(form).querySelectorAll("input, select, textarea, button")) {
      control.disabled = state.loading || !state.catalogReady || !state.pipeline || drafts.pendingExcept(kind).length > 0;
    }
  }
  const modelDefinition = state.catalog.models?.find(item => item.model_type === $("#modelType").value);
  if (!modelAvailability(state.catalog.backends || [], modelDefinition).available) $("#applyModel").disabled = true;
  $("#applyJson").disabled = state.loading || drafts.pendingExcept("json").length > 0;
  $("#modelSelect").disabled = state.loading || !state.catalogReady || drafts.pending;
  $("#newModel").disabled = state.loading || !state.catalogReady || drafts.pending;
  for (const [kind, hint, discard] of [["json", "rawJsonHint", "discardJson"], ["node", "nodeDraftHint", "discardNode"], ["model", "modelDraftHint", "discardModel"]]) {
    $(`#${hint}`).hidden = !drafts.has(kind);
    $(`#${discard}`).disabled = !drafts.has(kind);
  }
}

function requireApplied(except = "") {
  if (state.loading) { toast("方案加载中，请稍候"); return false; }
  const pending = drafts.pendingExcept(except);
  if (!pending.length) return true;
  const labels = { json: "原始 JSON", node: "节点属性", model: "模型" };
  toast(`请先应用或放弃${pending.map(kind => labels[kind]).join("、")}中的修改`, true);
  switchTab(pending[0] === "node" ? "properties" : pending[0] === "model" ? "models" : "json");
  return false;
}

function toast(message, error = false) {
  const element = $("#toast");
  element.textContent = message;
  element.className = `show${error ? " error" : ""}`;
  clearTimeout(toast.timer);
  toast.timer = setTimeout(() => { element.className = ""; }, 2600);
}

function effectiveNodes() {
  return state.pipeline?.pipeline || [];
}

function setDirty(value) {
  state.dirty = value;
  updateEditorStatus();
  $("#documentTitle").textContent = state.filename || (state.sourceName ? `${state.sourceName}（导入）` : "未命名方案");
}

function clearValidation(message = "") {
  state.errorNodeIds = new Set();
  $("#validationOutput").textContent = message;
}

function markPipelineChanged(record = true) {
  state.pipelineVersion += 1;
  clearValidation("方案已修改，请重新校验");
  $("#selectionReport").textContent = "方案已修改，资产与效果验收需要重新检查。";
  state.selectedEdge = null;
  if (record) history.record(snapshot());
  setDirty(JSON.stringify(state.pipeline) !== state.savedPipeline);
}

function positionsKey() { return `edgeflow.positions.${state.filename || state.sourceName || state.pipeline?.biz_name || "draft"}`; }
function restorePositions() {
  try {
    const saved = JSON.parse(localStorage.getItem(positionsKey()) || "{}");
    graph.positions = Object.fromEntries(Object.entries(saved).filter(([, point]) => Number.isFinite(point?.x) && Number.isFinite(point?.y)));
  }
  catch { graph.positions = {}; }
}

function savePositions(positions) {
  try { localStorage.setItem(positionsKey(), JSON.stringify(positions)); }
  catch { /* A full/disabled browser store must not interrupt editing. */ }
}

function selectNode(id) {
  if (state.loading) return;
  if (drafts.has("node") && state.selected !== id) {
    requireApplied(); return;
  }
  state.selected = id; state.selectedEdge = null; renderAll();
}

const graph = new GraphView($("#graph"), {
  select: selectNode,
  connect: (source, sourcePort, target, targetPort) => {
    if (!state.editing || !requireApplied()) return;
    try { connectPorts(state.pipeline, state.catalog, source, sourcePort, target, targetPort); markPipelineChanged(); renderAll(); }
    catch (error) { toast(error.message, true); }
  },
  selectEdge: edge => {
    if (state.loading) return;
    state.selectedEdge = edge; renderAll();
  },
  positionsChanged: savePositions,
  viewChanged: ({ scale }) => { $("#zoomLabel").textContent = `${Math.round(scale * 100)}%`; },
});

function renderAll() {
  graph.editable = state.editing && !state.loading && state.catalogReady;
  const nodes = effectiveNodes();
  const modelIds = modelBoundNodeIds(nodes, state.catalog.nodes);
  const document = graphDocument(state.pipeline, state.catalog);
  graph.render(document.nodes, state.selected, state.errorNodeIds, modelIds, document.definitions, document.edges, state.selectedEdge);
  if (!drafts.has("json")) $("#rawJson").value = state.pipeline ? JSON.stringify(state.pipeline, null, 2) : "";
  $("#graphSummary").textContent = state.pipeline ? `${nodes.length} 个节点 · ${document.edges.length} 条连线` : "打开方案，查看算法流程";
  $("#edgeSelection").hidden = !state.selectedEdge;
  $("#edgeSelectionLabel").textContent = state.selectedEdge ? state.selectedEdge.dependency ? "已选中执行依赖" : `${state.selectedEdge.sourcePort} → ${state.selectedEdge.targetPort}` : "";
  $("#deleteEdgeButton").disabled = !state.editing || !state.selectedEdge;
  renderInspector(nodes.find(node => node.id === state.selected));
  filterProfiles();
  refreshModelList();
  updateEditorStatus();
}

function renderInspector(node) {
  $("#emptyInspector").hidden = Boolean(node);
  $("#nodeForm").hidden = !node;
  if (!node) return;
  $("#nodeId").value = node.id;
  $("#nodeType").textContent = node.node_type;
  const definition = state.catalog.nodes.find(item => item.node_type === node.node_type);
  const container = $("#configFields");
  container.replaceChildren();
  for (const field of definition?.config_fields || []) {
    const modelRef = field.semantic === "model_ref" || field.name === definition.model_config_field;
    const choices = modelRef ? compatibleModels(state.pipeline.models, state.catalog.models, definition).map(model => model.model_id) : null;
    appendConfigField(container, field, node.config || {}, choices);
  }
  if (drafts.has("node")) restoreFormBuffer($("#nodeForm"), drafts.get("node"));
}

async function loadCatalog(biz = "") {
  return catalogRequests.run(
    () => api(`/catalog${biz ? `?biz=${encodeURIComponent(biz)}` : ""}`),
    catalog => { state.catalog = catalog; state.catalogReady = true; renderOperators(); renderAll(); }
  );
}

function clearCatalogSelection() {
  state.catalogReady = false;
  state.catalog = { ...state.catalog, nodes: [], models: [], profiles: [] };
  renderOperators();
}

function renderOperators() {
  const query = $("#operatorSearch").value.trim().toLowerCase();
  const list = $("#operatorList"); list.replaceChildren();
  for (const node of state.catalog.nodes.filter(item => `${item.node_type} ${item.description}`.toLowerCase().includes(query))) {
    const button = document.createElement("button"); button.className = "operator";
    button.innerHTML = `<strong>${node.node_type}</strong><small>${node.category} · ${node.description}</small>`;
    button.addEventListener("click", () => addNode(node)); list.append(button);
  }
}

function addNode(definition) {
  if (!state.pipeline) return toast("请先打开或新建方案", true);
  if (!state.editing || !requireApplied()) return;
  const base = definition.node_type.replace(/Node$/, "").replace(/([A-Z])/g, "_$1").toLowerCase().replace(/^_/, "");
  let index = 1, id = base;
  const ids = new Set(state.pipeline.pipeline.map(node => node.id));
  while (ids.has(id)) id = `${base}_${index++}`;
  const config = {};
  for (const field of definition.config_fields || []) if (field.default !== undefined) config[field.name] = field.default;
  const outputs = Object.fromEntries((definition.outputs || []).map(port => [port.key, `${id}__${port.key}`]));
  state.pipeline.pipeline.push({ id, node_type: definition.node_type, depends_on: [], ports: { inputs: {}, outputs }, config });
  state.selected = id; markPipelineChanged(); renderAll();
}

async function refreshLists() {
  const [allCatalog, profiles, pipelines, assets] = await Promise.all([api("/catalog"), api("/profiles"), api("/pipelines"), api("/assets")]);
  state.assets = assets.selections;
  const variants = $("#buildVariant"); variants.replaceChildren(new Option("按当前 Catalog 检查", ""));
  for (const variant of assets.variants) variants.add(new Option(variant.name, variant.name));
  state.profiles = profiles.profiles;
  state.catalogProfiles = allCatalog.profiles || [];
  const biz = $("#bizSelect"); biz.replaceChildren();
  for (const item of allCatalog.bizs) biz.add(new Option(`${item.display_name} · ${item.biz_name}`, item.biz_name));
  if (state.pipeline) biz.value = state.pipeline.biz_name;
  const schemes = $("#pipelineSelect"); schemes.replaceChildren(new Option("选择方案", ""));
  for (const item of pipelines.pipelines) schemes.add(new Option(`${item.filename} · ${item.biz_name}`, item.filename));
  if (state.pipeline) {
    await loadCatalog(state.pipeline.biz_name);
  } else {
    catalogRequests.invalidate();
    state.catalog = allCatalog;
    state.catalogReady = true;
    renderOperators();
  }
  filterProfiles();
}

function filterProfiles() {
  for (const selector of [$("#cloneProfile"), $("#runProfile")]) {
    const cloning = selector.id === "cloneProfile";
    const bizName = cloning ? $("#bizSelect").value : state.pipeline?.biz_name;
    const matching = cloning ? state.catalogProfiles : state.catalog.profiles || [];
    const previous = selector.value; selector.replaceChildren();
    if (cloning) selector.add(new Option("空图", ""));
    for (const profile of matching.filter(item => item.pipeline_biz === bizName)) selector.add(new Option(`${profile.name} · ${profile.suite}`, profile.name));
    if ([...selector.options].some(item => item.value === previous)) selector.value = previous;
  }
}

async function openPipeline(filename) {
  if (!filename) return;
  return openDocument(() => api(`/pipeline?filename=${encodeURIComponent(filename)}`));
}

async function openDocument(load) {
  if ((state.dirty || drafts.pending) && !confirm("当前修改尚未保存，确认丢弃并打开其他方案？")) return;
  const request = ++documentRequest;
  state.loading = true; graph.editable = false; updateEditorStatus();
  try {
    const result = await load();
    if (request !== documentRequest) return;
    assertBrowsablePipeline(result.pipeline);
    state.pipeline = result.pipeline;
    state.filename = result.imported ? "" : result.filename;
    state.sourceName = result.imported ? result.filename : "";
    state.revision = result.revision; state.selected = "";
    $("#pipelineSelect").value = state.filename;
    drafts.clear(); state.selectedEdge = null; state.documentVersion += 1;
    state.savedPipeline = JSON.stringify(state.pipeline); history.reset(snapshot());
    state.pipelineVersion += 1;
    restorePositions(); clearValidation(); setDirty(false); clearCatalogSelection();
    $("#bizSelect").value = state.pipeline.biz_name;
    try { await loadCatalog(state.pipeline.biz_name); }
    catch (error) { toast(`文件已打开，Catalog 暂不可用：${error.message}`, true); }
  } finally {
    if (request === documentRequest) { state.loading = false; graph.editable = state.editing; renderAll(); }
  }
}

async function createPipeline() {
  if ((state.dirty || drafts.pending) && !confirm("当前修改尚未保存，确认丢弃并新建？")) return;
  const request = ++documentRequest;
  state.loading = true; graph.editable = false; updateEditorStatus();
  const biz = $("#bizSelect").value, profile = $("#cloneProfile").value;
  try {
    const result = await write("/init", "POST", { biz, profile, empty: !profile });
    if (request !== documentRequest) return;
    state.pipeline = result.pipeline; state.filename = ""; state.sourceName = ""; state.revision = ""; state.selected = "";
    drafts.clear(); state.selectedEdge = null; state.documentVersion += 1;
    state.savedPipeline = ""; history.reset(snapshot());
    state.pipelineVersion += 1;
    restorePositions(); clearValidation(); setDirty(true); clearCatalogSelection();
    await loadCatalog(biz);
  } finally {
    if (request === documentRequest) { state.loading = false; graph.editable = state.editing; renderAll(); }
  }
}

async function save(saveAs) {
  if (!state.pipeline) return toast("没有可保存的方案", true);
  if (state.saving || !requireApplied()) return;
  let filename = state.filename;
  if (saveAs || !filename) {
    const suggested = /^pipeline_[a-z0-9_]+\.json$/.test(state.sourceName) ? state.sourceName : "pipeline_new_solution.json";
    filename = prompt("另存到 configs：方案文件名（pipeline_[a-z0-9_]+.json）", filename || suggested);
    if (!filename) return;
    saveAs = true;
  }
  const documentVersion = state.documentVersion;
  const pipeline = structuredClone(state.pipeline);
  state.saving = true; updateEditorStatus();
  try {
    const result = await write(saveAs ? "/pipelines" : "/pipeline", saveAs ? "POST" : "PUT", {
      filename, pipeline, revision: state.revision,
    });
    if (documentVersion !== state.documentVersion) return;
    state.filename = result.filename; state.sourceName = ""; state.revision = result.revision;
    state.savedPipeline = JSON.stringify(pipeline);
    savePositions(graph.positions);
    setDirty(JSON.stringify(state.pipeline) !== state.savedPipeline);
    const pipelines = await api("/pipelines");
    const schemes = $("#pipelineSelect"); schemes.replaceChildren(new Option("选择方案", ""));
    for (const item of pipelines.pipelines) schemes.add(new Option(`${item.filename} · ${item.biz_name}`, item.filename));
    $("#pipelineSelect").value = state.filename;
    toast(state.dirty || drafts.pending ? "已保存提交时的版本，当前仍有新修改" : "方案已保存");
  } catch (error) { toast(error.status === 409 ? "保存冲突：请重新加载或另存" : error.message, true); }
  finally { state.saving = false; updateEditorStatus(); }
}

async function validate() {
  if (!state.pipeline || !requireApplied()) return;
  const output = $("#validationOutput");
  const pipelineVersion = state.pipelineVersion;
  clearValidation("校验中…"); renderAll();
  try {
    const report = await write("/validate", "POST", { pipeline: state.pipeline }, true);
    if (pipelineVersion !== state.pipelineVersion) return false;
    output.replaceChildren();
    state.errorNodeIds = new Set();
    if (report.ok) {
      const block = document.createElement("div"); block.className = "diagnostic ok";
      block.textContent = `校验通过 · ${report.plan.topological_order.length} 个节点 · ${report.plan.layers.length} 个波前`;
      output.append(block);
    }
    for (const item of report.diagnostics || []) {
      if (item.node_id) state.errorNodeIds.add(item.node_id);
      appendDiagnostic(output, item, id => { selectNode(id); switchTab("properties"); graph.focusNode?.(id); });
    }
    renderAll();
    return report.ok;
  } catch (error) {
    if (pipelineVersion === state.pipelineVersion) output.textContent = error.message;
    return false;
  }
}

async function runDraft() {
  if (!state.pipeline || !requireApplied()) return;
  try {
    const result = await write("/runs", "POST", { pipeline: state.pipeline, profile: $("#runProfile").value });
    state.jobId = result.job_id; $("#cancelButton").disabled = false; pollRun();
  } catch (error) { toast(error.message, true); }
}

async function pollRun() {
  if (!state.jobId) return;
  try {
    const { job } = await api(`/runs/${state.jobId}`);
    $("#runLog").textContent = `${job.status}\n${job.logs || ""}${job.error ? `\n${JSON.stringify(job.error, null, 2)}` : ""}`;
    $("#runResult").textContent = job.result ? JSON.stringify(job.result, null, 2) : "";
    if (["completed", "failed", "cancelled"].includes(job.status)) { $("#cancelButton").disabled = true; return; }
    setTimeout(pollRun, 700);
  } catch (error) { toast(error.message, true); }
}


let editingModelId = "";
let modelDocument = null;

function refreshModelList() {
  const select = $("#modelSelect"), previous = select.value;
  select.replaceChildren(new Option("新增模型", ""));
  for (const model of state.pipeline?.models || []) select.add(new Option(model.model_id, model.model_id));
  select.value = previous;
  if (!drafts.has("model") && (modelDocument !== state.pipeline || !$("#modelType").options.length)) {
    modelDocument = state.pipeline;
    loadModelEditor(state.pipeline?.models?.[0]?.model_id || "");
  }
}

function loadModelEditor(id = "") {
  editingModelId = id;
  $("#modelSelect").value = id;
  const model = state.pipeline?.models?.find(item => item.model_id === id);
  const assets = $("#assetSelect"); assets.replaceChildren(new Option("手动配置", ""));
  for (const asset of state.assets) {
    const supported = state.catalog.backends?.some(backend => backend.backend_type === asset.model.backend);
    const availability = asset.availability === "missing" ? "资产缺失" : "文件存在，待验 SHA";
    const option = new Option(`${asset.label} · ${supported ? availability : "此构建未启用"}`, asset.id);
    option.disabled = !supported; assets.add(option);
  }
  $("#modelId").value = model?.model_id || "";
  $("#modelPath").value = model?.model_path || "";
  const type = $("#modelType"); type.replaceChildren();
  for (const definition of state.catalog.models || []) {
    const availability = modelAvailability(state.catalog.backends || [], definition);
    const option = new Option(`${definition.model_type} · ${definition.capability}${availability.available ? "" : " · 当前构建无兼容 Backend"}`, definition.model_type);
    option.title = availability.message;
    type.add(option);
  }
  if (model && !state.catalog.models?.some(item => item.model_type === model.model_type)) type.add(new Option(`${model.model_type} · 当前构建未注册`, model.model_type));
  if (model) type.value = model.model_type;
  renderModelFields(model);
  updateEditorStatus();
}

function renderModelFields(model = null) {
  const definition = state.catalog.models?.find(item => item.model_type === $("#modelType").value);
  const backend = $("#modelBackend"); backend.replaceChildren();
  const availability = modelAvailability(state.catalog.backends || [], definition);
  const compatible = compatibleBackends(state.catalog.backends || [], definition);
  for (const item of compatible) backend.add(new Option(item.backend_type, item.backend_type));
  const hint = $("#modelAvailability"); hint.textContent = availability.message; hint.hidden = availability.available;
  if (model && !compatible.some(item => item.backend_type === model.backend)) {
    const option = new Option(`${model.backend} · 当前构建不可用或不兼容`, model.backend);
    option.disabled = true; backend.add(option);
    if (availability.available) { hint.hidden = false; hint.textContent = "当前 Backend 不可用或不兼容，请选择列表中的兼容 Backend。"; }
  } else if (!compatible.length) {
    const option = new Option("当前构建无兼容 Backend", ""); option.disabled = true; backend.add(option); option.selected = true;
  }
  if (model) backend.value = model.backend;
  const container = $("#modelConfigFields"); container.replaceChildren();
  for (const field of definition?.config_fields || []) appendConfigField(container, field, model?.model_config || {});
  renderBackendFields(model?.backend_config || {});
}

function renderBackendFields(values = {}) {
  const definition = state.catalog.backends?.find(item => item.backend_type === $("#modelBackend").value);
  const container = $("#backendConfigFields"); container.replaceChildren();
  for (const field of definition?.config_fields || []) appendConfigField(container, field, values);
}

function updateBackendAvailability() {
  const definition = state.catalog.models?.find(item => item.model_type === $("#modelType").value);
  const availability = modelAvailability(state.catalog.backends || [], definition);
  const selectedIsCompatible = compatibleBackends(state.catalog.backends || [], definition).some(item => item.backend_type === $("#modelBackend").value);
  const hint = $("#modelAvailability");
  hint.hidden = selectedIsCompatible;
  hint.textContent = availability.available ? "当前 Backend 不可用或不兼容，请选择列表中的兼容 Backend。" : availability.message;
}

$("#assetSelect").addEventListener("change", event => {
  const asset = state.assets.find(item => item.id === event.target.value);
  if (!asset) return;
  const model = structuredClone(asset.model);
  $("#modelType").value = model.model_type; $("#modelPath").value = model.model_path;
  if (!$("#modelId").value) $("#modelId").value = asset.id;
  renderModelFields(model);
});
$("#verifySelection").addEventListener("click", async () => {
  if (!state.pipeline || !requireApplied()) return;
  const version = state.pipelineVersion;
  $("#selectionReport").textContent = "正在计算资产散列并检查编译产物…";
  try {
    const report = await write("/selection", "POST", {pipeline: state.pipeline, variant: $("#buildVariant").value}, true);
    if (version !== state.pipelineVersion) return;
    const summary = [report.ok ? "配置、资产与构建检查通过。" : "选择检查未通过。", "业务效果：尚未验收。请使用 verify_selection.py evaluate 生成数据集验收记录。"];
    $("#selectionReport").textContent = summary.join("\n") + "\n" + JSON.stringify(report, null, 2);
  } catch (error) { if (version === state.pipelineVersion) $("#selectionReport").textContent = error.message; }
});
$("#modelSelect").addEventListener("change", event => {
  if (!requireApplied()) { event.target.value = editingModelId; return; }
  loadModelEditor(event.target.value);
});
$("#newModel").addEventListener("click", () => { if (requireApplied()) loadModelEditor(); });
$("#modelType").addEventListener("change", () => renderModelFields());
$("#modelBackend").addEventListener("change", () => { renderBackendFields(); updateBackendAvailability(); });
$("#modelForm").addEventListener("submit", event => {
  event.preventDefault();
  if (!state.pipeline) return toast("请先新建或打开方案", true);
  if (!requireApplied("model")) return;
  try {
    const model = {
      model_id: $("#modelId").value.trim(), model_type: $("#modelType").value,
      backend: $("#modelBackend").value, model_path: $("#modelPath").value.trim(),
      model_config: readConfigFields($("#modelConfigFields")), backend_config: readConfigFields($("#backendConfigFields")),
    };
    state.pipeline.models ??= [];
    upsertModel(state.pipeline, state.catalog, editingModelId, model);
    drafts.clear("model");
    markPipelineChanged(); renderAll(); loadModelEditor(model.model_id); toast("模型已应用，请校验方案");
  } catch (error) { toast(error.message, true); }
});
$("#deleteModel").addEventListener("click", () => {
  if (!state.pipeline || !editingModelId || !requireApplied()) return;
  try {
    removeModel(state.pipeline, state.catalog, editingModelId);
    markPipelineChanged(); renderAll(); loadModelEditor();
  } catch (error) { toast(error.message, true); }
});

function switchTab(name) {
  document.querySelectorAll(".tabs button").forEach(button => {
    button.classList.toggle("active", button.dataset.tab === name);
    button.setAttribute("aria-selected", String(button.dataset.tab === name));
  });
  for (const tab of ["properties", "models", "json", "validation", "run"]) $(`#${tab}Tab`).hidden = tab !== name;
}

function setEditing(editing) {
  state.editing = editing;
  graph.editable = editing && !state.loading && state.catalogReady;
  document.body.classList.toggle("editing", editing);
  if (editing && !state.enteredEditor && state.pipeline) {
    document.body.classList.add("operators-hidden");
    $("#operatorsToggle").setAttribute("aria-pressed", "false");
  }
  if (editing) state.enteredEditor = true;
  $("#editModeButton").textContent = editing ? "浏览流程" : "编辑方案";
  $("#editModeButton").setAttribute("aria-pressed", String(editing));
  $("#canvasHint").textContent = editing
    ? "从输出端口拖到输入端口连线 · 选中连线后删除 · Ctrl/⌘ Z 撤销"
    : "拖动画布平移 · 滚轮缩放 · 双击节点放大阅读";
  renderAll();
  requestAnimationFrame(() => graph.fit());
}

async function restoreHistory(direction) {
  if (!requireApplied()) return;
  const restored = history[direction]();
  if (!restored) return;
  const bizChanged = state.pipeline?.biz_name !== restored.pipeline?.biz_name || !state.catalogReady;
  state.pipeline = restored.pipeline; state.selected = restored.selected;
  markPipelineChanged(false);
  if (bizChanged) {
    state.loading = true; graph.editable = false;
    $("#bizSelect").value = state.pipeline.biz_name;
    clearCatalogSelection();
    renderAll();
    try { await loadCatalog(state.pipeline.biz_name); }
    catch (error) { renderAll(); toast(error.message, true); }
    finally { state.loading = false; graph.editable = state.editing; renderAll(); }
  } else renderAll();
}

function deleteSelectedEdge() {
  if (!state.editing || !state.selectedEdge || !requireApplied()) return;
  disconnectPorts(state.pipeline, state.catalog, state.selectedEdge);
  markPipelineChanged(); renderAll(); toast("连线已删除，可撤销恢复");
}

$("#editModeButton").addEventListener("click", () => setEditing(!state.editing));
for (const [id, name] of [["operatorsToggle", "operators-hidden"], ["inspectorToggle", "inspector-hidden"]]) {
  $("#" + id).addEventListener("click", () => {
    const hidden = document.body.classList.toggle(name);
    $("#" + id).setAttribute("aria-pressed", String(!hidden));
  });
}
$("#fitButton").addEventListener("click", () => graph.fit());
$("#zoomInButton").addEventListener("click", () => graph.zoomBy(1.2));
$("#zoomOutButton").addEventListener("click", () => graph.zoomBy(1 / 1.2));
$("#actualSizeButton").addEventListener("click", () => graph.resetZoom());
$("#undoButton").addEventListener("click", () => restoreHistory("undo"));
$("#redoButton").addEventListener("click", () => restoreHistory("redo"));
$("#deleteEdgeButton").addEventListener("click", deleteSelectedEdge);

$("#rawJson").addEventListener("input", event => {
  if (event.target.value === JSON.stringify(state.pipeline, null, 2)) drafts.clear("json");
  else drafts.set("json", event.target.value);
  updateEditorStatus();
});
for (const [kind, selector] of [["node", "#nodeForm"], ["model", "#modelForm"]]) {
  const remember = () => { drafts.set(kind, readFormBuffer($(selector))); updateEditorStatus(); };
  $(selector).addEventListener("input", remember);
  $(selector).addEventListener("change", remember);
}
$("#discardJson").addEventListener("click", () => { drafts.clear("json"); renderAll(); });
$("#discardNode").addEventListener("click", () => { drafts.clear("node"); renderAll(); });
$("#discardModel").addEventListener("click", () => { drafts.clear("model"); loadModelEditor(editingModelId); updateEditorStatus(); });

window.addEventListener("keydown", event => {
  const typing = event.target.closest?.("input, textarea, select, [contenteditable=true]");
  if ((event.ctrlKey || event.metaKey) && event.key.toLowerCase() === "s") {
    event.preventDefault(); if (state.editing) save(false); return;
  }
  if (typing || !state.editing) return;
  if ((event.ctrlKey || event.metaKey) && ["z", "y"].includes(event.key.toLowerCase())) {
    event.preventDefault();
    restoreHistory(event.shiftKey || event.key.toLowerCase() === "y" ? "redo" : "undo");
  } else if (event.key === "Delete" || event.key === "Backspace") {
    if (state.selectedEdge) { event.preventDefault(); deleteSelectedEdge(); }
  } else if (event.key === "Escape") {
    state.selectedEdge = null; graph.cancelConnection(); renderAll();
  }
});

$("#openButton").addEventListener("click", () => openPipeline($("#pipelineSelect").value).catch(error => toast(error.message, true)));
$("#browseButton").addEventListener("click", () => $("#pipelineFile").click());
$("#pipelineFile").addEventListener("change", async event => {
  const file = event.target.files?.[0];
  event.target.value = "";
  if (!file) return;
  try { await openDocument(() => readPipelineFile(file)); }
  catch (error) { toast(`打开文件失败：${error.message}`, true); }
});
$("#newButton").addEventListener("click", () => createPipeline().catch(error => toast(error.message, true)));
$("#saveButton").addEventListener("click", () => save(false));
$("#saveAsButton").addEventListener("click", () => save(true));
$("#layoutButton").addEventListener("click", () => {
  graph.layout(graphDocument(state.pipeline, state.catalog).nodes, true);
  renderAll();
});
$("#operatorSearch").addEventListener("input", renderOperators);
$("#bizSelect").addEventListener("change", filterProfiles);
$("#validateButton").addEventListener("click", validate);
$("#runButton").addEventListener("click", runDraft);
$("#cancelButton").addEventListener("click", async () => { if (state.jobId) await write(`/runs/${state.jobId}`, "DELETE", {}); });
document.querySelectorAll(".tabs button").forEach(button => button.addEventListener("click", () => switchTab(button.dataset.tab)));

$("#nodeForm").addEventListener("submit", event => {
  event.preventDefault();
  if (!requireApplied("node")) return;
  if (!state.catalog.nodes.some(definition => definition.node_type === effectiveNodes().find(node => node.id === state.selected)?.node_type)) return toast("节点定义不可用，请重新打开方案或修正 JSON", true);
  const node = state.pipeline.pipeline.find(item => item.id === state.selected);
  const newId = $("#nodeId").value.trim();
  if (!newId || state.pipeline.pipeline.some(item => item !== node && item.id === newId)) return toast("节点 ID 为空或重复", true);
  try {
    const config = readConfigFields($("#configFields"));
    for (const item of state.pipeline.pipeline) item.depends_on = item.depends_on.map(id => id === node.id ? newId : id);
    if (newId !== node.id && graph.positions[node.id]) {
      graph.positions[newId] = graph.positions[node.id];
      delete graph.positions[node.id];
      graph.callbacks.positionsChanged(graph.positions);
    }
    node.id = newId; node.config = config; state.selected = newId;
    drafts.clear("node"); markPipelineChanged(); renderAll();
  } catch (error) { toast(`配置 JSON 错误：${error.message}`, true); }
});

$("#deleteNode").addEventListener("click", () => {
  if (!requireApplied()) return;
  removeNode(state.pipeline, state.catalog, state.selected);
  delete graph.positions[state.selected];
  graph.callbacks.positionsChanged(graph.positions);
  state.selected = ""; markPipelineChanged(); renderAll();
});

$("#applyJson").addEventListener("click", async () => {
  if (!requireApplied("json")) return;
  let parsed;
  try {
    parsed = JSON.parse($("#rawJson").value);
    assertBrowsablePipeline(parsed);
  } catch (error) {
    toast(`JSON 错误：${error.message}`, true);
    return;
  }

  const bizChanged = parsed.biz_name !== state.pipeline?.biz_name || !state.catalogReady;
  drafts.clear("json");
  state.pipeline = parsed; state.selected = ""; markPipelineChanged();
  if (bizChanged) clearCatalogSelection();
  if (!bizChanged) { renderAll(); return; }

  state.loading = true; graph.editable = false;
  $("#bizSelect").value = state.pipeline.biz_name;
  renderAll();
  try {
    if (await loadCatalog(state.pipeline.biz_name)) renderAll();
  } catch (error) {
    clearCatalogSelection(); renderAll();
    toast(`Catalog 加载失败：${error.message}`, true);
  } finally { state.loading = false; graph.editable = state.editing; renderAll(); }
});

window.addEventListener("beforeunload", event => { if (state.dirty || drafts.pending) { event.preventDefault(); event.returnValue = ""; } });

try {
  await refreshLists();
} catch (error) { toast(`工具列表暂不可用，仍可浏览文件：${error.message}`, true); }

try {
  if (initialPipeline) {
    $("#pipelineSelect").value = initialPipeline;
    await openPipeline(initialPipeline);
  } else {
    const initial = await api("/initial");
    if (initial.document && documentRequest === 0) await openDocument(async () => initial.document);
  }
} catch (error) { toast(error.message, true); }
