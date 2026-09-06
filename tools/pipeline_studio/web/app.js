import { api, initialPipeline, write } from "./api.js";
import { GraphView } from "./graph.js";
import { compatibleModels, createLatestRequestGate, modelBoundNodeIds, graphDocument, connectPorts, disconnectPorts, removeNode, compatibleBackends, upsertModel, removeModel } from "./workbench.js";

const $ = selector => document.querySelector(selector);
const state = {
  pipeline: null,
  filename: "",
  revision: "",
  dirty: false,
  catalog: { nodes: [], bizs: [], profiles: [] },
  profiles: [],
  assets: [],
  selected: "",
  jobId: "",
  errorNodeIds: new Set(),
  pipelineVersion: 0,
};
const catalogRequests = createLatestRequestGate();

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
  const badge = $("#dirtyBadge");
  badge.textContent = !state.pipeline ? "未打开" : value ? "未保存" : "已保存";
  badge.classList.toggle("dirty", value);
  $("#documentTitle").textContent = state.filename || "未命名方案";
}

function clearValidation(message = "") {
  state.errorNodeIds = new Set();
  $("#validationOutput").textContent = message;
}

function markPipelineChanged() {
  state.pipelineVersion += 1;
  clearValidation("方案已修改，请重新校验");
  $("#selectionReport").textContent = "方案已修改，资产与效果验收需要重新检查。";
  setDirty(true);
}

function positionsKey() { return `edgeflow.positions.${state.filename || state.pipeline?.biz_name || "draft"}`; }
function restorePositions() {
  try { graph.positions = JSON.parse(localStorage.getItem(positionsKey()) || "{}"); }
  catch { graph.positions = {}; }
}

const graph = new GraphView($("#graph"), {
  select: id => { state.selected = id; renderAll(); },
  connect: (source, sourcePort, target, targetPort) => {
    try { connectPorts(state.pipeline, state.catalog, source, sourcePort, target, targetPort); markPipelineChanged(); renderAll(); }
    catch (error) { toast(error.message, true); }
  },
  deleteEdge: edge => {
    disconnectPorts(state.pipeline, state.catalog, edge); markPipelineChanged(); renderAll();
  },
  positionsChanged: positions => localStorage.setItem(positionsKey(), JSON.stringify(positions)),
});

function renderAll() {
  const nodes = effectiveNodes();
  const modelIds = modelBoundNodeIds(nodes, state.catalog.nodes);
  const document = graphDocument(state.pipeline, state.catalog);
  graph.render(document.nodes, state.selected, state.errorNodeIds, modelIds, document.definitions, document.edges);
  $("#rawJson").value = state.pipeline ? JSON.stringify(state.pipeline, null, 2) : "";
  renderInspector(nodes.find(node => node.id === state.selected));
  filterProfiles();
  refreshModelList();
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
    appendConfigField(container, field, node.config || {}, definition);

  }
}

function appendConfigField(container, field, values, definition = null) {
    const label = document.createElement("label"); label.textContent = field.name;
    let input;
    if (field.semantic === "model_ref" || (definition?.model_config_field && field.name === definition.model_config_field)) {
      input = document.createElement("select");
      for (const model of compatibleModels(state.pipeline.models, state.catalog.models, definition)) {
        const option = new Option(model.model_id, model.model_id); input.add(option);
      }
    } else if (Array.isArray(field.enum) && field.enum.length) {
      input = document.createElement("select");
      for (const opt of field.enum) input.add(new Option(opt, opt));
    } else if (field.type === "boolean") {
      input = document.createElement("select"); input.add(new Option("true", "true")); input.add(new Option("false", "false"));
    } else if (field.type === "object" || field.type === "array") {
      input = document.createElement("textarea"); input.rows = 3;
    } else {
      input = document.createElement("input"); input.type = field.type === "integer" || field.type === "number" ? "number" : "text";
      if (field.minimum !== undefined) input.min = field.minimum;
      if (field.maximum !== undefined) input.max = field.maximum;
    }
    input.dataset.field = field.name; input.dataset.type = field.type;
    input.required = Boolean(field.required);
    if (input.type === "number") input.step = field.type === "integer" ? "1" : "any";
    const value = values[field.name] ?? field.default;
    input.value = typeof value === "object" ? JSON.stringify(value) : value ?? "";
    label.append(input); container.append(label);
}

function parseField(input) {
  if (input.dataset.type === "integer") {
    const value = Number(input.value);
    if (!Number.isSafeInteger(value)) throw new Error("请输入有效整数");
    return value;
  }
  if (input.dataset.type === "number") return Number(input.value);
  if (input.dataset.type === "boolean") return input.value === "true";
  if (input.dataset.type === "object" || input.dataset.type === "array") return JSON.parse(input.value);
  return input.value;
}

async function loadCatalog(biz = "") {
  return catalogRequests.run(
    () => api(`/catalog${biz ? `?biz=${encodeURIComponent(biz)}` : ""}`),
    catalog => { state.catalog = catalog; renderOperators(); }
  );
}

function clearCatalogSelection() {
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
    renderOperators();
  }
  filterProfiles();
}

function filterProfiles() {
  const bizName = state.pipeline?.biz_name || $("#bizSelect").value;
  const matching = state.catalog.profiles?.length ? state.catalog.profiles : [];
  for (const selector of [$("#cloneProfile"), $("#runProfile")]) {
    const previous = selector.value; selector.replaceChildren();
    if (selector.id === "cloneProfile") selector.add(new Option("空图", ""));
    for (const profile of matching.filter(item => item.pipeline_biz === bizName)) selector.add(new Option(`${profile.name} · ${profile.suite}`, profile.name));
    if ([...selector.options].some(item => item.value === previous)) selector.value = previous;
  }
}

async function openPipeline(filename) {
  if (!filename) return;
  if (state.dirty && !confirm("当前草稿尚未保存，确认丢弃并打开其他方案？")) return;
  const result = await api(`/pipeline?filename=${encodeURIComponent(filename)}`);
  state.pipeline = result.pipeline; state.filename = result.filename; state.revision = result.revision; state.selected = "";
  state.pipelineVersion += 1;
  restorePositions(); clearValidation(); setDirty(false); clearCatalogSelection();
  $("#bizSelect").value = state.pipeline.biz_name;
  if (await loadCatalog(state.pipeline.biz_name)) renderAll();
}

async function createPipeline() {
  if (state.dirty && !confirm("当前草稿尚未保存，确认新建？")) return;
  const biz = $("#bizSelect").value, profile = $("#cloneProfile").value;
  const result = await write("/init", "POST", { biz, profile, empty: !profile });
  state.pipeline = result.pipeline; state.filename = ""; state.revision = ""; state.selected = "";
  state.pipelineVersion += 1;
  restorePositions(); clearValidation(); setDirty(true); clearCatalogSelection();
  if (await loadCatalog(biz)) renderAll();
}

async function save(saveAs) {
  if (!state.pipeline) return toast("没有可保存的方案", true);
  let filename = state.filename;
  if (saveAs || !filename) {
    filename = prompt("方案文件名（pipeline_[a-z0-9_]+.json）", filename || "pipeline_new_solution.json");
    if (!filename) return;
    saveAs = true;
  }
  try {
    const result = await write(saveAs ? "/pipelines" : "/pipeline", saveAs ? "POST" : "PUT", {
      filename, pipeline: state.pipeline, revision: state.revision,
    });
    state.filename = result.filename; state.revision = result.revision; state.pipeline = result.pipeline;
    setDirty(false); await refreshLists(); toast("方案已原子保存");
  } catch (error) { toast(error.status === 409 ? "保存冲突：请重新加载或另存" : error.message, true); }
}

async function validate() {
  if (!state.pipeline) return;
  const output = $("#validationOutput");
  const pipelineVersion = state.pipelineVersion;
  clearValidation("校验中…"); renderAll();
  try {
    const report = await write("/validate", "POST", { pipeline: state.pipeline }, true);
    if (pipelineVersion !== state.pipelineVersion) return false;
    output.replaceChildren();
    state.errorNodeIds = new Set();
    if (report.ok) output.innerHTML = `<div class="diagnostic ok">校验通过 · ${report.plan.topological_order.length} 个节点 · ${report.plan.layers.length} 个波前</div>`;
    for (const item of report.diagnostics || []) {
      if (item.node_id) state.errorNodeIds.add(item.node_id);
      const block = document.createElement("div"); block.className = "diagnostic";
      block.innerHTML = `<strong>${item.code}</strong><br><code>${item.path}</code><br>${item.message}`;
      block.addEventListener("click", () => { if (item.node_id) { state.selected = item.node_id; switchTab("properties"); renderAll(); } });
      output.append(block);
    }
    renderAll();
    return report.ok;
  } catch (error) {
    if (pipelineVersion === state.pipelineVersion) output.textContent = error.message;
    return false;
  }
}

async function runDraft() {
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
  if (modelDocument !== state.pipeline || !$("#modelType").options.length) {
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
  for (const definition of state.catalog.models || []) type.add(new Option(`${definition.model_type} · ${definition.capability}`, definition.model_type));
  if (model) type.value = model.model_type;
  renderModelFields(model);
}

function renderModelFields(model = null) {
  const definition = state.catalog.models?.find(item => item.model_type === $("#modelType").value);
  const backend = $("#modelBackend"); backend.replaceChildren();
  for (const item of compatibleBackends(state.catalog.backends || [], definition)) backend.add(new Option(item.backend_type, item.backend_type));
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

function readConfigFields(selector) {
  const config = {};
  for (const input of $(selector).querySelectorAll("[data-field]")) {
    if (input.value !== "" || input.required) config[input.dataset.field] = parseField(input);
  }
  return config;
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
  if (!state.pipeline) return;
  const version = state.pipelineVersion;
  $("#selectionReport").textContent = "正在计算资产散列并检查编译产物…";
  try {
    const report = await write("/selection", "POST", {pipeline: state.pipeline, variant: $("#buildVariant").value}, true);
    if (version !== state.pipelineVersion) return;
    const summary = [report.ok ? "配置、资产与构建检查通过。" : "选择检查未通过。", "业务效果：尚未验收。请使用 verify_selection.py evaluate 生成数据集验收记录。"];
    $("#selectionReport").textContent = summary.join("\n") + "\n" + JSON.stringify(report, null, 2);
  } catch (error) { if (version === state.pipelineVersion) $("#selectionReport").textContent = error.message; }
});
$("#modelSelect").addEventListener("change", event => loadModelEditor(event.target.value));
$("#newModel").addEventListener("click", () => loadModelEditor());
$("#modelType").addEventListener("change", () => renderModelFields());
$("#modelBackend").addEventListener("change", () => renderBackendFields());
$("#modelForm").addEventListener("submit", event => {
  event.preventDefault();
  if (!state.pipeline) return toast("请先新建或打开方案", true);
  try {
    const model = {
      model_id: $("#modelId").value.trim(), model_type: $("#modelType").value,
      backend: $("#modelBackend").value, model_path: $("#modelPath").value.trim(),
      model_config: readConfigFields("#modelConfigFields"), backend_config: readConfigFields("#backendConfigFields"),
    };
    state.pipeline.models ??= [];
    upsertModel(state.pipeline, state.catalog, editingModelId, model);
    markPipelineChanged(); renderAll(); loadModelEditor(model.model_id); toast("模型已应用，请校验方案");
  } catch (error) { toast(error.message, true); }
});
$("#deleteModel").addEventListener("click", () => {
  if (!state.pipeline || !editingModelId) return;
  try {
    removeModel(state.pipeline, state.catalog, editingModelId);
    markPipelineChanged(); renderAll(); loadModelEditor();
  } catch (error) { toast(error.message, true); }
});

function switchTab(name) {
  document.querySelectorAll(".tabs button").forEach(button => button.classList.toggle("active", button.dataset.tab === name));
  for (const tab of ["properties", "models", "json", "validation", "run"]) $(`#${tab}Tab`).hidden = tab !== name;
}

$("#openButton").addEventListener("click", () => openPipeline($("#pipelineSelect").value).catch(error => toast(error.message, true)));
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
  const node = state.pipeline.pipeline.find(item => item.id === state.selected);
  const newId = $("#nodeId").value.trim();
  if (!newId || state.pipeline.pipeline.some(item => item !== node && item.id === newId)) return toast("节点 ID 为空或重复", true);
  try {
    const config = readConfigFields("#configFields");
    for (const item of state.pipeline.pipeline) item.depends_on = item.depends_on.map(id => id === node.id ? newId : id);
    if (newId !== node.id && graph.positions[node.id]) {
      graph.positions[newId] = graph.positions[node.id];
      delete graph.positions[node.id];
      graph.callbacks.positionsChanged(graph.positions);
    }
    node.id = newId; node.config = config; state.selected = newId; markPipelineChanged(); renderAll();
  } catch (error) { toast(`配置 JSON 错误：${error.message}`, true); }
});

$("#deleteNode").addEventListener("click", () => {
  removeNode(state.pipeline, state.catalog, state.selected);
  delete graph.positions[state.selected];
  graph.callbacks.positionsChanged(graph.positions);
  state.selected = ""; markPipelineChanged(); renderAll();
});

$("#applyJson").addEventListener("click", async () => {
  let parsed;
  try {
    parsed = JSON.parse($("#rawJson").value);
    if (!parsed || !Array.isArray(parsed.pipeline)) throw new Error("pipeline 必须是数组");
  } catch (error) {
    toast(`JSON 错误：${error.message}`, true);
    return;
  }

  const bizChanged = parsed.biz_name !== state.pipeline?.biz_name;
  state.pipeline = parsed; state.selected = ""; markPipelineChanged();
  if (bizChanged) clearCatalogSelection();
  if (!bizChanged) { renderAll(); return; }

  $("#bizSelect").value = state.pipeline.biz_name;
  try {
    if (await loadCatalog(state.pipeline.biz_name)) renderAll();
  } catch (error) {
    clearCatalogSelection(); filterProfiles();
    toast(`Catalog 加载失败：${error.message}`, true);
  }
});

window.addEventListener("beforeunload", event => { if (state.dirty) { event.preventDefault(); event.returnValue = ""; } });

try {
  await refreshLists();
  if (initialPipeline) {
    $("#pipelineSelect").value = initialPipeline;
    await openPipeline(initialPipeline);
  }
} catch (error) { toast(error.message, true); }
