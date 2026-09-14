import { api, initialPipeline, write } from "./api.js";
import { GraphView } from "./graph.js";
import { createHistory, createDrafts, appendDiagnostic, appendConfigField, readConfigFields, readFormBuffer, restoreFormBuffer } from "./editor.js";
import { compatibleModels, createLatestRequestGate, modelBoundNodeIds, graphDocument, compatibleBackends, modelAvailability, assertBrowsablePipeline, readPipelineFile, upsertModel, removeModel } from "./workbench.js";

import { captureRun, runIsCurrent, runSummary, renderSamples } from "./workflow.js";

const $ = selector => document.querySelector(selector);
const state = {
  pipeline: null,
  authoringBusy: false,
  deployment: null,
  modelPathActions: {},
  preflight: null,
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
  run: null,
  saveTargets: [],
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
let pendingAction = null;
let pendingFixReview = null;
let preflightRequest = 0;

function snapshot() { return { pipeline: state.pipeline, selected: state.selected, modelPathActions: state.modelPathActions }; }

function updateEditorStatus() {
  const pending = drafts.pending;
  const badge = $("#dirtyBadge");
  badge.textContent = (state.loading || state.authoringBusy) ? "加载中…" : !state.pipeline ? "未打开" : pending ? "有未应用修改" : state.dirty ? "未保存" : state.sourceName ? "已导入" : "已保存";
  badge.classList.toggle("dirty", pending || state.dirty);
  $("#undoButton").disabled = (state.loading || state.authoringBusy) || pending || !history.canUndo;
  $("#redoButton").disabled = (state.loading || state.authoringBusy) || pending || !history.canRedo;
  $("#saveButton").disabled = (state.loading || state.authoringBusy) || state.saving || !state.pipeline;
  $("#saveAsButton").disabled = (state.loading || state.authoringBusy) || state.saving || !state.pipeline;
  $("#saveSolutionButton").disabled = (state.loading || state.authoringBusy) || state.saving || !state.pipeline;
  $("#openButton").disabled = (state.loading || state.authoringBusy);
  $("#browseButton").disabled = (state.loading || state.authoringBusy);
  $("#newButton").disabled = (state.loading || state.authoringBusy) || !state.catalogReady;
  $("#newEntryButton").disabled = (state.loading || state.authoringBusy);
  $("#quickValidateButton").disabled = (state.loading || state.authoringBusy) || !state.pipeline || !state.catalogReady;
  $("#quickValidateButton").textContent = pending ? "应用修改并校验" : "校验";
  $("#openRunButton").disabled = (state.loading || state.authoringBusy) || !state.pipeline;
  $("#openRunButton").textContent = pending ? "应用修改并运行" : "运行";
  $("#validateButton").disabled = (state.loading || state.authoringBusy) || !state.pipeline || !state.catalogReady;
  $("#validateButton").textContent = pending ? "应用修改并校验" : "校验方案";
  $("#runButton").textContent = pending ? "应用修改并运行" : "运行草稿";
  const preflightBtn = $("#preflightButton");
  if (preflightBtn) preflightBtn.disabled = (state.loading || state.authoringBusy) || !state.pipeline;
  const assocBtn = $("#associateDeploymentButton");
  if (assocBtn) assocBtn.disabled = (state.loading || state.authoringBusy) || !state.pipeline || !state.filename;
  $("#browseHint").hidden = state.editing;
  $("#saveScope").textContent = state.pipeline ? state.filename
    ? `保存目标：${state.saveTargets.join(" + ")}（configs/）`
    : "保存将另存到 configs/；导入源文件保持原样。" : "";
  $("#saveButton").title = $("#saveScope").textContent;
  renderRunContext();
  $("#rawJson").readOnly = !state.editing || (state.loading || state.authoringBusy) || drafts.pendingExcept("json").length > 0;
  for (const [kind, form] of [["node", "#nodeForm"], ["model", "#modelForm"]]) {
    for (const control of $(form).querySelectorAll("input, select, textarea, button")) {
      control.disabled = !state.editing || (state.loading || state.authoringBusy) || !state.catalogReady || !state.pipeline || drafts.pendingExcept(kind).length > 0;
    }
  }
  const modelDefinition = state.catalog.models?.find(item => item.model_type === $("#modelType").value);
  if (!modelAvailability(state.catalog.backends || [], modelDefinition).available) $("#applyModel").disabled = true;
  $("#applyJson").disabled = !state.editing || (state.loading || state.authoringBusy) || drafts.pendingExcept("json").length > 0;
  $("#modelSelect").disabled = (state.loading || state.authoringBusy) || !state.catalogReady || drafts.pending;
  $("#newModel").disabled = !state.editing || (state.loading || state.authoringBusy) || !state.catalogReady || drafts.pending;
  for (const [kind, hint, discard] of [["json", "rawJsonHint", "discardJson"], ["node", "nodeDraftHint", "discardNode"], ["model", "modelDraftHint", "discardModel"]]) {
    $(`#${hint}`).hidden = !drafts.has(kind);
    $(`#${discard}`).disabled = !drafts.has(kind);
  }
}

function requireApplied(except = "", continuation = null, action = "继续操作") {
  if (state.loading || state.authoringBusy) { toast("方案操作中，请稍候"); return false; }
  const pending = drafts.pendingExcept(except);
  if (!pending.length) return true;
  const labels = { json: "原始 JSON", node: "节点属性", model: "模型" };
  if (!continuation) toast(`请先应用或放弃${pending.map(kind => labels[kind]).join("、")}中的修改`);
  switchTab(pending[0] === "node" ? "properties" : pending[0] === "model" ? "models" : "json");
  pendingAction = continuation ? { continuation, documentVersion: state.documentVersion } : null;
  $("#draftActions").hidden = !pendingAction;
  $("#draftActionLabel").textContent = `有未应用修改，处理后${action}。`;
  return false;
}

function clearPendingAction() {
  pendingAction = null;
  $("#draftActions").hidden = true;
}

function operationFeedback(message, error = false) {
  const output = $("#operationFeedback");
  output.textContent = message; output.hidden = !message;
  output.classList.toggle("error", error);
  if (error) setInspectorOpen(true);
}

function showFailure(error) {
  let report = error.payload;
  if (report?.error?.code === "VALIDATION_FAILED") {
    try { report = typeof report.error.message === "string" ? JSON.parse(report.error.message) : report.error.message; } catch { /* Use original error below. */ }
  }
  if (Array.isArray(report?.diagnostics) && report.diagnostics.length) {
    showValidation(report); switchTab("validation");
    operationFeedback("方案校验未通过，请按诊断修复后重试。", true);
  } else operationFeedback(error.message, true);
}

function resetDocumentFeedback() {
  clearPendingAction(); operationFeedback("");
  for (const id of ["savedCommand", "savedConfig"]) $(`#${id}`).textContent = "";
  $("#savedDeployment").hidden = true;
  $("#runModelRoot").value = "models";
  setInspectorOpen(state.editing);
  renderRun();
}

function toast(message, error = false) {
  const element = $("#toast");
  clearTimeout(toast.timer);
  element.replaceChildren();
  const text = document.createElement("span"); text.textContent = message;
  element.append(text);
  element.className = `show${error ? " error" : ""}`;
  if (error) {
    const dismiss = document.createElement("button");
    dismiss.id = "toastDismiss"; dismiss.textContent = "关闭";
    dismiss.setAttribute("aria-label", "关闭错误提示");
    dismiss.addEventListener("click", () => { element.className = ""; });
    element.append(dismiss);
  } else toast.timer = setTimeout(() => { element.className = ""; }, 2600);
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
  state.validationReport = null;
  state.errorNodeIds = new Set();
  $("#validationOutput").textContent = message;
}

function markPipelineChanged(record = true) {
  state.pipelineVersion += 1;
  clearValidation("方案已修改，请重新校验");
  $("#selectionReport").textContent = "方案已修改，资产与效果验收需要重新检查。";
  state.selectedEdge = null;
  pendingFixReview = null;
  if ($("#fixReviewPanel")) $("#fixReviewPanel").hidden = true;
  renderPreflightFreshness();
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
    requireApplied("", () => selectNode(id), "切换节点"); return;
  }
  state.selected = id; state.selectedEdge = null; renderAll();
  switchTab("properties");
}

const graph = new GraphView($("#graph"), {
  select: selectNode,
  connect: (source, sourcePort, target, targetPort) => {
    applyAuthoring({ kind: "connect", source: { node_id: source, port: sourcePort }, target: { node_id: target, port: targetPort } });
  },
  selectEdge: edge => {
    if (state.loading) return;
    state.selectedEdge = edge; renderAll();
  },
  positionsChanged: savePositions,
  viewChanged: ({ scale }) => { $("#zoomLabel").textContent = `${Math.round(scale * 100)}%`; },
});

function renderAll() {
  graph.editable = state.editing && !state.loading && !state.authoringBusy && state.catalogReady;
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
  renderPathIntent();
  renderPreflightFreshness();
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
    const dep = (definition?.model_dependencies || []).find(d => d.config_field === field.name);
    const modelRef = Boolean(dep) || field.semantic === "model_ref";
    const requiredCap = dep ? dep.capability : null;
    const choices = modelRef ? compatibleModels(state.pipeline.models, state.catalog.models, requiredCap).map(model => model.model_id) : null;
    appendConfigField(container, field, node.config || {}, choices);
  }
  renderBindings(node, definition);
  if (drafts.has("node")) restoreFormBuffer($("#nodeForm"), drafts.get("node"));
}

async function loadCatalog(biz = "") {
  return catalogRequests.run(
    () => api(`/catalog${biz ? `?biz=${encodeURIComponent(biz)}` : ""}`),
    catalog => {
      if (catalog.schema_version !== 3) {
        state.catalogReady = false;
        const msg = `不支持的 Catalog 版本 (v${catalog.schema_version})，Pipeline Studio 要求 Catalog v3。请升级或重新构建后端工具。`;
        toast(msg, true);
        clearValidation(msg);
        renderOperators();
        renderAll();
        return;
      }
      state.catalog = catalog; state.catalogReady = true; renderOperators(); renderAll();
    }
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
  return applyAuthoring({ kind: "add_node", node_type: definition.node_type });
}

async function applyAuthoring(operation, options = {}) {
  if (!state.editing || !state.pipeline || !requireApplied(options.except || "")) return false;
  const source = structuredClone(state.pipeline);
  const version = state.pipelineVersion, documentVersion = state.documentVersion;
  const buffer = JSON.stringify(drafts.get(options.except || ""));
  const current = () => !state.loading && state.editing && version === state.pipelineVersion &&
    documentVersion === state.documentVersion && JSON.stringify(state.pipeline) === JSON.stringify(source) &&
    !drafts.pendingExcept(options.except || "").length && buffer === JSON.stringify(drafts.get(options.except || ""));
  state.authoringBusy = true; updateEditorStatus(); graph.editable = false;
  try {
    const report = await write("/validate", "POST", { pipeline: source, explain: true }, true);
    if (!current()) return false;
    const result = await write("/authoring/preview", "POST", {
      pipeline: source, operation, revision: report.revision, tool_fingerprint: report.tool_fingerprint,
    }, true);
    if (!current()) return false;
    if (!result.ok || !result.pipeline) {
      operationFeedback((result.diagnostics || []).map(d => d.message).join("\n") || "编排操作失败", true);
      return false;
    }
    let validation = result.validation;
    if (options.configure) {
      options.configure(result.pipeline);
      validation = await write("/validate", "POST", { pipeline: result.pipeline, explain: true }, true);
      if (!current()) return false;
      if (validation.tool_fingerprint !== result.tool_fingerprint) throw new Error("工具已更新，请重试操作");
    }
    if (options.except) drafts.clear(options.except);
    state.pipeline = result.pipeline;
    if (options.selected !== undefined) state.selected = options.selected;
    else if (operation.kind === "add_node") state.selected = result.changes?.find(c => c.action === "add_node")?.node_id || "";
    options.onCommit?.();
    markPipelineChanged();
    state.validationReport = { ...validation, revision: result.revision, tool_fingerprint: result.tool_fingerprint };
    if (options.configure) state.validationReport = validation;
    showValidation(state.validationReport);
    operationFeedback((result.changes || []).map(c => c.description).join("\n") || "编排操作完成");
    return true;
  } catch (error) {
    if (current()) operationFeedback(error.message, true);
    return false;
  } finally { state.authoringBusy = false; renderAll(); }
}

function renderBindings(node, definition) {
  const container = $("#nodeBindings");
  if (!container) return;
  container.replaceChildren();
  for (const port of definition?.inputs || []) {
    const row = document.createElement("div"); row.className = "row";
    const label = document.createElement("span"); label.textContent = port.key;
    const button = document.createElement("button"); button.type = "button";
    button.textContent = "恢复默认绑定"; button.disabled = !state.editing || state.authoringBusy;
    button.addEventListener("click", () => applyAuthoring({ kind: "reset_input_binding", target: { node_id: node.id, port: port.key } }));
    row.append(label, button); container.append(row);
  }
  const label = document.createElement("p"); label.textContent = "执行依赖（断开数据线后保留，可单独删除）"; container.append(label);
  for (const id of node.depends_on || []) {
    const row = document.createElement("div"); row.className = "row";
    const text = document.createElement("span"); text.textContent = id;
    const remove = document.createElement("button"); remove.type = "button"; remove.textContent = "删除依赖";
    remove.disabled = !state.editing || state.authoringBusy;
    remove.addEventListener("click", () => applyAuthoring({ kind: "remove_dependency", node_id: node.id, depends_on_id: id }));
    row.append(text, remove); container.append(row);
  }
  const select = document.createElement("select"); select.id = "dependencySource";
  select.add(new Option("选择前置节点", ""));
  for (const other of effectiveNodes()) if (other.id !== node.id && !(node.depends_on || []).includes(other.id)) select.add(new Option(other.id, other.id));
  const add = document.createElement("button"); add.type = "button"; add.id = "addDependencyButton"; add.textContent = "添加依赖";
  add.disabled = !state.editing || state.authoringBusy;
  add.addEventListener("click", () => { if (select.value) applyAuthoring({ kind: "add_dependency", node_id: node.id, depends_on_id: select.value }); });
  container.append(select, add);
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
  const confSelect = $("#associateConfSelect");
  if (confSelect) {
    const prevConf = confSelect.value;
    confSelect.replaceChildren(new Option("选择部署配置", ""));
    for (const conf of pipelines.deployments || []) confSelect.add(new Option(conf, conf));
    if (prevConf && [...confSelect.options].some(o => o.value === prevConf)) confSelect.value = prevConf;
  }
  if (state.pipeline) {
    await loadCatalog(state.pipeline.biz_name);
  } else {
    catalogRequests.invalidate();
    if (allCatalog.schema_version !== 3) {
      state.catalogReady = false;
      const msg = `不支持的 Catalog 版本 (v${allCatalog.schema_version})，Pipeline Studio 要求 Catalog v3。请升级或重新构建后端工具。`;
      toast(msg, true);
      clearValidation(msg);
      renderOperators();
      renderAll();
      return;
    }
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
    state.deployment = result.deployment || null; state.modelPathActions = {};
    if (state.deployment) $("#runModelRoot").value = state.deployment.model_root;
    state.revision = result.revision; state.saveTargets = result.save_targets || (state.filename ? [state.filename] : []); state.selected = "";
    $("#pipelineSelect").value = state.filename;
    drafts.clear(); state.selectedEdge = null; state.documentVersion += 1;
    resetDocumentFeedback();
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
    state.deployment = null; state.modelPathActions = {};
    state.pipeline = result.pipeline; state.filename = ""; state.sourceName = ""; state.revision = ""; state.saveTargets = []; state.selected = "";
    drafts.clear(); state.selectedEdge = null; state.documentVersion += 1;
    resetDocumentFeedback();
    state.savedPipeline = ""; history.reset(snapshot());
    state.pipelineVersion += 1;
    restorePositions(); clearValidation(); setDirty(true); clearCatalogSelection();
    await loadCatalog(biz);
  } finally {
    if (request === documentRequest) { state.loading = false; graph.editable = state.editing; renderAll(); }
  }
}

async function save(saveAs, runnable = false) {
  if (!state.pipeline) return toast("没有可保存的方案", true);
  if (state.saving || !requireApplied("", () => save(saveAs, runnable), "保存方案")) return;
  if (runnable && !$("#runProfile").value) return toast("请先选择匹配业务的运行 Profile", true);
  let filename = state.filename;
  if (saveAs || !filename) {
    const suggested = /^pipeline_[a-z0-9_]+\.json$/.test(state.sourceName) ? state.sourceName : "pipeline_new_solution.json";
    filename = prompt(`${runnable ? "创建 JSON 与配套 .conf" : "创建 JSON 文件；如需配套 .conf，请使用运行页的另存为可运行方案"}。\n另存到 configs：文件名（pipeline_[a-z0-9_]+.json）`, filename || suggested);
    if (!filename) return;
    saveAs = true;
  }
  const documentVersion = state.documentVersion;
  const pipeline = structuredClone(state.pipeline);
  state.saving = true; updateEditorStatus();
  try {
    const result = await write(runnable ? "/solutions" : saveAs ? "/pipelines" : "/pipeline", saveAs ? "POST" : "PUT", {
      filename, pipeline, revision: state.revision,
      profile: $("#runProfile").value, model_root: $("#runModelRoot").value, model_path_actions: modelPathActions(),
    });
    if (documentVersion !== state.documentVersion) return;
    state.filename = result.filename; state.sourceName = ""; state.revision = result.revision;
    state.saveTargets = result.save_targets || [result.filename];
    state.deployment = result.deployment || null;
    if (JSON.stringify(state.pipeline) === JSON.stringify(pipeline)) state.modelPathActions = {};
    state.savedPipeline = JSON.stringify(pipeline);
    savePositions(graph.positions);
    setDirty(JSON.stringify(state.pipeline) !== state.savedPipeline);
    const pipelines = await api("/pipelines");
    if (documentVersion !== state.documentVersion) return;
    const schemes = $("#pipelineSelect"); schemes.replaceChildren(new Option("选择方案", ""));
    for (const item of pipelines.pipelines) schemes.add(new Option(`${item.filename} · ${item.biz_name}`, item.filename));
    $("#pipelineSelect").value = state.filename;
    if (result.conf_filename) {
      $("#savedCommand").textContent = `已保存 ${result.filename} 和 ${result.conf_filename}。以下命令运行已保存版本：\n\n${result.command}`;
      $("#savedConfig").textContent = JSON.stringify(result.configuration, null, 2);
      $("#savedDeployment").hidden = false;
    } else {
      $("#savedDeployment").hidden = true;
      $("#savedCommand").textContent = ""; $("#savedConfig").textContent = "";
    }
    const feedback = `已保存：${state.saveTargets.map(name => `configs/${name}`).join("、")}${state.dirty || drafts.pending ? "；当前仍有新修改" : ""}`;
    operationFeedback(feedback); toast(feedback);
  } catch (error) { if (documentVersion === state.documentVersion) showFailure(error); }
  finally { state.saving = false; updateEditorStatus(); }
}

function showValidation(report) {
  const output = $("#validationOutput");
  output.replaceChildren();
  state.errorNodeIds = new Set();
  if (report.ok) {
    const block = document.createElement("div"); block.className = "diagnostic ok";
    block.textContent = `校验通过 · ${report.plan.topological_order.length} 个节点 · ${report.plan.layers.length} 个波前`;
    output.append(block);
  }
  for (const item of report.diagnostics || []) {
    if (item.node_id) state.errorNodeIds.add(item.node_id);
    appendDiagnostic(
      output,
      item,
      id => { selectNode(id); switchTab("properties"); graph.focusNode?.(id); },
      fix => handleApplyFix(fix)
    );
  }
  renderAll();
}

async function handleApplyFix(fix) {
  if (!state.pipeline || !state.validationReport || !requireApplied()) return;
  const pipelineVersion = state.pipelineVersion;
  const documentVersion = state.documentVersion;
  const source = structuredClone(state.pipeline);
  const report = state.validationReport;
  const isCurrent = () => pipelineVersion === state.pipelineVersion &&
    documentVersion === state.documentVersion && !state.loading && !drafts.pending &&
    report === state.validationReport;
  try {
    operationFeedback(`正在预览修复：${fix.title}…`);
    const res = await write("/fixes/preview", "POST", {
      pipeline: source,
      patch: fix.patch,
      revision: state.validationReport.revision,
      tool_fingerprint: state.validationReport.tool_fingerprint,
    }, true);
    if (!isCurrent()) {
      operationFeedback("草稿已变更，请重新校验后选择修复。");
      return;
    }
    if (!res.ok) {
      operationFeedback(`修复预览失败：${res.error || "未知错误"}`);
      return;
    }
    const changes = [];
    const describe = (before, after, path = "") => {
      if (JSON.stringify(before) === JSON.stringify(after)) return;
      if (before && after && typeof before === "object" && typeof after === "object" &&
          !Array.isArray(before) && !Array.isArray(after)) {
        for (const key of new Set([...Object.keys(before), ...Object.keys(after)])) {
          describe(before[key], after[key], `${path}/${key.replaceAll("~", "~0").replaceAll("/", "~1")}`);
        }
      } else {
        changes.push(`${path || "/"}\n  修改前：${before === undefined ? "（不存在）" : JSON.stringify(before)}\n  修改后：${after === undefined ? "（删除）" : JSON.stringify(after)}`);
      }
    };
    describe(source, res.patched);
    const remaining = (res.report?.diagnostics || []).map(item => `${item.code} ${item.path}: ${item.message}`);
    const reviewPanel = $("#fixReviewPanel");
    reviewPanel.replaceChildren(); reviewPanel.hidden = false;
    const title = document.createElement("div"); title.className = "fix-review-title";
    title.textContent = `修复审阅：${fix.title}`;
    const effect = document.createElement("p"); effect.textContent = fix.effect;
    const status = document.createElement("p");
    status.textContent = res.report?.ok ? "候选方案校验通过；尚未运行" : `候选仍有 ${remaining.length} 项诊断，请审阅影响`;
    const details = document.createElement("details");
    const summary = document.createElement("summary"); summary.textContent = `变更与诊断详情（${changes.length} 处）`;
    const pre = document.createElement("pre"); pre.className = "code"; pre.textContent = [...changes, ...remaining].join("\n\n");
    details.append(summary, pre);
    const apply = document.createElement("button"); apply.id = "applyReviewedFix"; apply.textContent = "应用此修复";
    apply.addEventListener("click", handleApplyReviewedFix);
    const cancel = document.createElement("button"); cancel.id = "cancelReviewedFix"; cancel.textContent = "取消";
    cancel.addEventListener("click", () => { pendingFixReview = null; reviewPanel.hidden = true; });
    reviewPanel.append(title, effect, status, details, apply, cancel);
    pendingFixReview = { fix, source, report, res, isCurrent };
    switchTab("validation");

  } catch (error) {
    operationFeedback(`修复应用失败: ${error.message}`);
  }
}

async function handleApplyReviewedFix() {
  const review = pendingFixReview;
  if (!review || !review.isCurrent() || !requireApplied()) {
    pendingFixReview = null;
    $("#fixReviewPanel").hidden = true;
    operationFeedback("修复候选已过期，请重新校验和预览。", true);
    return false;
  }
  const button = $("#applyReviewedFix"); if (button) button.disabled = true;
  try {
    const checked = await write("/fixes/preview", "POST", {
      pipeline: review.source, patch: review.fix.patch,
      revision: review.report.revision, tool_fingerprint: review.report.tool_fingerprint,
    }, true);
    if (pendingFixReview !== review || !review.isCurrent()) return false;
    if (!checked.ok || JSON.stringify(checked.patched) !== JSON.stringify(review.res.patched)) throw new Error("修复候选已改变，请重新预览");
    state.pipeline = checked.patched;
    markPipelineChanged();
    operationFeedback(`已应用修复：${review.fix.title}（可撤销）`);
    await validate(); return true;
  } catch (error) { operationFeedback(error.message, true); return false; }
  finally { if (button) button.disabled = false; }
}

async function validate() {
  if (!state.catalogReady) {
    toast("Catalog 暂不可用或版本不兼容，无法校验方案", true);
    return false;
  }
  if (!state.pipeline || !requireApplied("", validate, "校验方案")) return;
  switchTab("validation"); operationFeedback("");
  const output = $("#validationOutput");
  const pipelineVersion = state.pipelineVersion;
  clearValidation("校验中…"); renderAll();
  try {
    const report = await write("/validate", "POST", { pipeline: state.pipeline, explain: true }, true);
    if (pipelineVersion !== state.pipelineVersion) return false;
    state.validationReport = report;
    showValidation(report);
    return report.ok;
  } catch (error) {
    if (pipelineVersion === state.pipelineVersion) output.textContent = error.message;
    return false;
  }
}

function runBusy() {
  return state.run && ["starting", "queued", "running"].includes(state.run.job.status);
}

function renderRunContext() {
  renderPreflightFreshness();
  const run = state.run;
  $("#modelRootLabel").textContent = $("#runModelRoot").value || "未设置";
  $("#runButton").disabled = state.loading || !state.pipeline || Boolean(runBusy()) || !$("#runProfile").value;
  $("#cancelButton").disabled = !runBusy() || !run?.id;
  const currentDocument = run?.documentVersion === state.documentVersion;
  $("#runContext").textContent = !run ? "尚未运行" : !currentDocument
    ? `其他方案：${run.filename} · ${runBusy() ? "运行中，可取消后运行当前方案" : "已结束；当前方案尚未运行"}`
    : `${run.filename} · ${new Date(run.startedAt).toLocaleString("zh-CN", { hour12: false })}\nProfile：${run.profile} · 模型目录：${run.modelRoot}`;
  const stale = currentDocument && ((run.deploymentSnapshot && run.deploymentSnapshot !== deploymentSnapshot()) || !runIsCurrent(run, {
    documentVersion: state.documentVersion, pipeline: state.pipeline, pending: drafts.pending,
    profile: $("#runProfile").value, modelRoot: $("#runModelRoot").value,
  }));
  $("#runFreshness").hidden = !stale;
  $("#runFreshness").textContent = "方案或运行设置已修改。以下结果属于提交时的版本，请重新运行验证当前修改。";
}

function renderRun() {
  renderRunContext();
  const run = state.run;
  const job = run?.documentVersion === state.documentVersion ? run.job : null;
  $("#runSummary").textContent = job ? runSummary(job) + (run.pollError ? `\n获取状态失败，正在重试：${run.pollError}` : "") : "";
  $("#runLog").textContent = job ? `${job.status}\n${job.logs || ""}${job.error ? `\n${job.error.message}` : ""}` : "尚未运行";
  $("#runResult").textContent = job?.result ? JSON.stringify(job.result, null, 2) : "";
  $("#resolvedConfig").textContent = job?.configuration ? JSON.stringify(job.configuration, null, 2) : "运行后显示模型路径与生效参数";
  renderSamples($("#runSamples"), job?.result);
}

async function runDraft() {
  if (!state.pipeline || runBusy() || !requireApplied("", runDraft, "运行方案")) return;
  switchTab("run"); operationFeedback("");
  const run = captureRun({ documentVersion: state.documentVersion, pipeline: state.pipeline,
    filename: state.filename || state.sourceName || "未命名方案", profile: $("#runProfile").value, modelRoot: $("#runModelRoot").value });
  run.deploymentSnapshot = deploymentSnapshot();
  state.run = run; renderRun();
  try {
    const result = await write("/runs", "POST", { pipeline: JSON.parse(run.pipeline), profile: run.profile, model_root: run.modelRoot, filename: state.filename, model_path_actions: modelPathActions() });
    if (state.run !== run) return;
    run.id = result.job_id; run.job = { status: result.status || "queued" }; renderRun(); pollRun(run);
  } catch (error) {
    if (state.run !== run) return;
    run.job = { status: "failed", error: { message: error.payload?.error?.code === "VALIDATION_FAILED" ? "方案校验未通过，详情见校验页。" : error.message } }; renderRun();
    if (run.documentVersion === state.documentVersion) showFailure(error);
  }
}

async function pollRun(run) {
  if (state.run !== run || !run.id) return;
  try {
    const { job } = await api(`/runs/${run.id}`);
    if (state.run !== run) return;
    run.job = job; run.pollError = ""; renderRun();
    if (["completed", "failed", "cancelled"].includes(job.status)) return;
    setTimeout(() => pollRun(run), 700);
  } catch (error) {
    if (state.run !== run) return;
    // A failed status request does not mean the process stopped. Retain cancel
    // and retry until the server returns its terminal status.
    run.pollError = error.message; renderRun();
    setTimeout(() => pollRun(run), 2000);
  }
}

async function ensureAppliedOrAction(action) {
  const pending = drafts.pendingExcept("");
  for (const kind of pending) {
    const ok = await applyDraft(kind);
    if (!ok) return;
  }
  clearPendingAction();
  return await action();
}

function modelPathActions() {
  return Object.fromEntries(Object.entries(state.modelPathActions || {}).filter(([id, action]) =>
    state.pipeline?.models?.some(m => m.model_id === id && m.model_path === action.path)));
}

function deploymentSnapshot() {
  return JSON.stringify({ documentVersion: state.documentVersion, pipeline: state.pipeline, pending: drafts.pending,
    filename: state.filename, deployment: state.deployment, profile: $("#runProfile")?.value,
    modelRoot: $("#runModelRoot")?.value, actions: modelPathActions() });
}

function renderPreflightFreshness() {
  const summary = $("#preflightSummary");
  if (!summary || !state.preflight || state.preflight.snapshot === deploymentSnapshot()) return;
  summary.hidden = false; summary.className = "preflight-summary stale";
  summary.textContent = "预检已过期：方案、部署配置或运行设置已改变，请重新检查运行条件。";
}

function renderPathIntent() {
  const container = $("#modelPathIntent");
  if (!container) return;
  container.replaceChildren(); container.hidden = !state.deployment;
  if (!state.deployment) return;
  const saved = JSON.parse(state.savedPipeline || "{}");
  for (const model of state.pipeline?.models || []) {
    const old = saved.models?.find(m => m.model_id === model.model_id);
    if (!old || old.model_path === model.model_path) continue;
    const label = document.createElement("label"); label.textContent = `${model.model_id}：${old.model_path} → ${model.model_path}`;
    const select = document.createElement("select"); select.dataset.modelIntent = model.model_id;
    select.add(new Option("存在部署覆盖时，请明确路径意图", ""));
    select.add(new Option("将新路径作为所选模型目录下的资产", "select_asset"));
    select.add(new Option("保留当前部署覆盖", "preserve_override"));
    select.value = modelPathActions()[model.model_id]?.action || "";
    select.addEventListener("change", () => {
      state.modelPathActions ||= {};
      if (select.value) state.modelPathActions[model.model_id] = { path: model.model_path, action: select.value };
      else delete state.modelPathActions[model.model_id];
      history.record(snapshot()); setDirty(true); renderPreflightFreshness();
    });
    label.append(select); container.append(label);
  }
}

async function runPreflight() {
  if (!state.pipeline || !requireApplied("", runPreflight, "运行预检")) return;
  const summaryEl = $("#preflightSummary");
  if (!summaryEl) return;
  const request = ++preflightRequest;
  const snapshot = deploymentSnapshot();
  state.preflight = { request, snapshot };
  summaryEl.hidden = false;
  summaryEl.className = "preflight-summary loading";
  summaryEl.textContent = "正在执行独立部署预检（无 Demo 执行）…";
  try {
    const res = await write("/preflight", "POST", {
      pipeline: state.pipeline, filename: state.filename, model_path_actions: modelPathActions(),
      profile: $("#runProfile")?.value || "",
      model_root: $("#runModelRoot")?.value || "models",
    }, true);
    if (request !== preflightRequest || snapshot !== deploymentSnapshot()) { renderPreflightFreshness(); return; }
    summaryEl.className = "preflight-summary";
    const summary = res.summary || {};
    summaryEl.replaceChildren();
    const heading = document.createElement("div");
    heading.className = `preflight-status ${summary.status || "unknown"}`;
    heading.textContent = summary.status === "ready" ? "✓ 部署预检通过（具备运行条件）"
      : summary.status === "attention_required" ? "⚠ 预检需关注：部分工具或模型资产未就绪"
      : summary.status === "validation_failed" ? "✗ Pipeline 校验未通过"
      : `预检状态：${summary.status || "未知"}`;
    summaryEl.append(heading);

    const desc = document.createElement("p");
    desc.textContent = `业务：${summary.biz_name || "未知"} · 项目根：${summary.project_root || "本地"}`;
    summaryEl.append(desc);

    if (summary.tools) {
      const toolsP = document.createElement("div");
      toolsP.textContent = `工具就绪：alg_pipeline_tool=${summary.tools.alg_pipeline_tool ? "✓" : "✗"} · alg_demo=${summary.tools.alg_demo ? "✓" : "✗"}`;
      summaryEl.append(toolsP);
    }
    if (Array.isArray(summary.assets)) {
      const assetsList = document.createElement("ul");
      for (const asset of summary.assets) {
        const li = document.createElement("li");
        li.textContent = `${asset.model_id}: ${asset.resolved_path} [${asset.status}]`;
        assetsList.append(li);
      }
      summaryEl.append(assetsList);
    }
    if (summary.next_step) {
      const nextP = document.createElement("p");
      nextP.className = "preflight-next-step";
      nextP.textContent = `后续指引：${summary.next_step}`;
      summaryEl.append(nextP);
    }
  } catch (error) {
    if (request !== preflightRequest || snapshot !== deploymentSnapshot()) { renderPreflightFreshness(); return; }
    summaryEl.className = "preflight-summary error";
    summaryEl.textContent = `预检执行失败：${error.message}`;
  }
}

async function handleAssociateDeployment() {
  if (!state.pipeline || !state.filename) {
    toast("请先保存当前 Pipeline 到 configs/ 再关联部署配置", true);
    return;
  }
  if (!requireApplied()) return;
  const documentVersion = state.documentVersion;
  const filename = state.filename;
  const confName = $("#associateConfSelect")?.value;
  if (!confName) {
    toast("请选择要关联的部署配置", true);
    return;
  }
  try {
    operationFeedback(`正在关联部署配置：${confName}…`);
    const res = await write("/deployment/associate", "POST", {
      filename, profile: $("#runProfile")?.value || "",
      conf_name: confName,
      model_root: $("#runModelRoot")?.value || "models",
    });
    if (documentVersion !== state.documentVersion || filename !== state.filename) return;
    if (res.ok) {
      state.deployment = res.deployment;
      renderPreflightFreshness();
      state.saveTargets = res.save_targets || [state.filename, confName];
      operationFeedback(`已成功关联部署配置：${confName}`);
      toast(`已关联部署配置：${confName}`);
      renderAll();
    }
  } catch (error) {
    operationFeedback(`关联部署配置失败：${error.message}`, true);
    toast(error.message, true);
  }
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
function applyModel() {
  if (!state.editing || !$("#modelForm").reportValidity()) return false;
  if (!state.pipeline) return toast("请先新建或打开方案", true);
  if (!requireApplied("model")) return;
  try {
    const model = {
      model_id: $("#modelId").value.trim(), model_type: $("#modelType").value,
      backend: $("#modelBackend").value, model_path: $("#modelPath").value.trim(),
      model_config: readConfigFields($("#modelConfigFields")), backend_config: readConfigFields($("#backendConfigFields")),
    };
    state.pipeline.models ??= [];
    const before = state.pipeline.models.find(m => m.model_id === editingModelId);
    const changedPath = !before || before.model_path !== model.model_path || editingModelId !== model.model_id;
    upsertModel(state.pipeline, state.catalog, editingModelId, model);
    if (changedPath) {
      state.modelPathActions ||= {};
      state.modelPathActions[model.model_id] = { path: model.model_path, action: "select_asset" };
    }
    drafts.clear("model");
    markPipelineChanged(); renderAll(); loadModelEditor(model.model_id); toast("模型已应用，请校验方案");
    return true;
  } catch (error) { operationFeedback(error.message, true); return false; }
}
$("#deleteModel").addEventListener("click", () => {
  if (!state.pipeline || !editingModelId || !requireApplied()) return;
  try {
    removeModel(state.pipeline, state.catalog, editingModelId);
    markPipelineChanged(); renderAll(); loadModelEditor();
  } catch (error) { toast(error.message, true); }
});

function switchTab(name) {
  setInspectorOpen(true);
  document.querySelectorAll(".tabs button").forEach(button => {
    button.classList.toggle("active", button.dataset.tab === name);
    button.setAttribute("aria-selected", String(button.dataset.tab === name));
  });
  for (const tab of ["properties", "models", "json", "validation", "run"]) $(`#${tab}Tab`).hidden = tab !== name;
}

function setInspectorOpen(open) {
  document.body.classList.toggle("inspector-open", open);
  $("#inspectorToggle").setAttribute("aria-pressed", String(open));
}

function setEditing(editing) {
  if (!editing && !requireApplied("", () => setEditing(false), "返回浏览")) return;
  setInspectorOpen(editing);
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
  state.modelPathActions = restored.modelPathActions || {};
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
  const edge = state.selectedEdge;
  if (!state.editing || !edge || !requireApplied()) return;
  return applyAuthoring(edge.dependency
    ? { kind: "remove_dependency", node_id: edge.target, depends_on_id: edge.source }
    : { kind: "disconnect", source: { node_id: edge.source, port: edge.sourcePort }, target: { node_id: edge.target, port: edge.targetPort } });
}

$("#editModeButton").addEventListener("click", () => setEditing(!state.editing));
$("#inspectorToggle").addEventListener("click", () => setInspectorOpen(!document.body.classList.contains("inspector-open")));
for (const [id, name] of [["operatorsToggle", "operators-hidden"]]) {
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
  if (!state.editing) return;
  operationFeedback("");
  if (event.target.value === JSON.stringify(state.pipeline, null, 2)) drafts.clear("json");
  else drafts.set("json", event.target.value);
  updateEditorStatus();
});
for (const [kind, selector] of [["node", "#nodeForm"], ["model", "#modelForm"]]) {
  const remember = event => {
    // Graph controls submit authoring operations, not node-parameter drafts.
    if (event.target.closest?.("#nodeBindings")) return;
    event.target.setCustomValidity?.("");
    if (!state.editing) return;
    operationFeedback("");
    drafts.set(kind, readFormBuffer($(selector))); updateEditorStatus();
  };
  $(selector).addEventListener("input", remember);
  $(selector).addEventListener("change", remember);
}
$("#discardJson").addEventListener("click", () => { drafts.clear("json"); clearPendingAction(); renderAll(); });
$("#discardNode").addEventListener("click", () => { drafts.clear("node"); clearPendingAction(); renderAll(); });
$("#discardModel").addEventListener("click", () => { drafts.clear("model"); clearPendingAction(); loadModelEditor(editingModelId); updateEditorStatus(); });

window.addEventListener("keydown", event => {
  const typing = event.target.closest?.("input, textarea, select, [contenteditable=true]");
  if ((event.ctrlKey || event.metaKey) && event.key.toLowerCase() === "s") {
    event.preventDefault(); if (state.editing) ensureAppliedOrAction(() => save(false)); return;
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
$("#newEntryButton").addEventListener("click", () => {
  setEditing(true);
  document.body.classList.remove("operators-hidden");
  $("#operatorsToggle").setAttribute("aria-pressed", "true");
  $(".create-panel").open = true; $("#bizSelect").focus();
});
$("#quickValidateButton").addEventListener("click", () => ensureAppliedOrAction(validate));
$("#openRunButton").addEventListener("click", () => ensureAppliedOrAction(() => switchTab("run")));
for (const id of ["runProfile", "runModelRoot"]) $("#" + id).addEventListener("input", renderRunContext);
$("#newButton").addEventListener("click", () => createPipeline().catch(error => toast(error.message, true)));
$("#saveButton").addEventListener("click", () => ensureAppliedOrAction(() => save(false)));
$("#saveAsButton").addEventListener("click", () => ensureAppliedOrAction(() => save(true)));
$("#saveSolutionButton").addEventListener("click", () => ensureAppliedOrAction(() => save(true, true)));
$("#layoutButton").addEventListener("click", () => {
  graph.layout(graphDocument(state.pipeline, state.catalog).nodes, true);
  renderAll();
});
$("#operatorSearch").addEventListener("input", renderOperators);
$("#bizSelect").addEventListener("change", filterProfiles);
$("#validateButton").addEventListener("click", () => ensureAppliedOrAction(validate));
$("#runButton").addEventListener("click", () => ensureAppliedOrAction(runDraft));
$("#preflightButton")?.addEventListener("click", () => ensureAppliedOrAction(runPreflight));
$("#associateDeploymentButton")?.addEventListener("click", handleAssociateDeployment);
$("#cancelButton").addEventListener("click", async () => {
  const run = state.run;
  if (!run?.id || !runBusy()) return;
  try { await write(`/runs/${run.id}`, "DELETE", {}); }
  catch (error) { if (state.run === run) operationFeedback(error.message, true); }
});
document.querySelectorAll(".tabs button").forEach(button => button.addEventListener("click", () => switchTab(button.dataset.tab)));

async function applyNode() {
  if (!state.editing || !$("#nodeForm").reportValidity()) return false;
  if (!requireApplied("node")) return;
  if (!state.catalog.nodes.some(definition => definition.node_type === effectiveNodes().find(node => node.id === state.selected)?.node_type)) return toast("节点定义不可用，请重新打开方案或修正 JSON", true);
  const node = state.pipeline.pipeline.find(item => item.id === state.selected);
  const newId = $("#nodeId").value.trim();
  if (!newId || state.pipeline.pipeline.some(item => item !== node && item.id === newId)) {
    $("#nodeId").setCustomValidity("节点 ID 为空或重复"); $("#nodeId").reportValidity();
    operationFeedback("节点 ID 为空或重复", true); return false;
  }
  try {
    const config = readConfigFields($("#configFields"));
    const oldId = node.id;
    if (newId !== oldId) {
      return await applyAuthoring({ kind: "rename_node", node_id: oldId, new_id: newId }, {
        except: "node", selected: newId,
        configure: candidate => { candidate.pipeline.find(n => n.id === newId).config = config; },
        onCommit: () => {
          if (graph.positions[oldId]) { graph.positions[newId] = graph.positions[oldId]; delete graph.positions[oldId]; savePositions(graph.positions); }
        },
      });
    }
    node.config = config;
    drafts.clear("node"); markPipelineChanged(); renderAll();
    return true;
  } catch (error) { operationFeedback(`配置错误：${error.message}`, true); return false; }
}

$("#deleteNode").addEventListener("click", () => {
  const id = state.selected;
  applyAuthoring({ kind: "remove_node", node_id: id }, { selected: "", onCommit: () => {
    delete graph.positions[id]; savePositions(graph.positions);
  } });
});

async function applyJson() {
  if (!state.editing || !requireApplied("json")) return false;
  let parsed;
  try {
    parsed = JSON.parse($("#rawJson").value);
    assertBrowsablePipeline(parsed);
  } catch (error) {
    operationFeedback(`JSON 错误：${error.message}`, true);
    $("#rawJson").focus(); return false;
  }

  const bizChanged = parsed.biz_name !== state.pipeline?.biz_name || !state.catalogReady;
  drafts.clear("json");
  state.pipeline = parsed; state.selected = ""; markPipelineChanged();
  if (bizChanged) clearCatalogSelection();
  if (!bizChanged) { renderAll(); return true; }

  state.loading = true; graph.editable = false;
  $("#bizSelect").value = state.pipeline.biz_name;
  renderAll();
  try {
    if (await loadCatalog(state.pipeline.biz_name)) renderAll();
  } catch (error) {
    clearCatalogSelection(); renderAll();
    toast(`Catalog 加载失败：${error.message}`, true);
  } finally { state.loading = false; graph.editable = state.editing; renderAll(); }
  return true;
}

const applyDraft = async kind => {
  operationFeedback("");
  return kind === "node" ? applyNode() : kind === "model" ? applyModel() : applyJson();
};
async function applyManually(kind) {
  if (await applyDraft(kind)) clearPendingAction();
}
$("#nodeForm").addEventListener("submit", event => { event.preventDefault(); applyManually("node"); });
$("#modelForm").addEventListener("submit", event => { event.preventDefault(); applyManually("model"); });
$("#applyJson").addEventListener("click", () => applyManually("json"));
$("#stayDraft").addEventListener("click", clearPendingAction);
async function continueDraft(discard) {
  const action = pendingAction;
  if (!action || action.documentVersion !== state.documentVersion) return;
  $("#applyContinue").disabled = true; $("#discardContinue").disabled = true;
  try {
    const kind = drafts.pendingExcept("")[0];
    if (kind) {
      if (discard) {
        drafts.clear(kind);
        if (kind === "model") loadModelEditor(editingModelId);
        renderAll();
      } else if (!await applyDraft(kind)) return;
    }
    if (action.documentVersion !== state.documentVersion || pendingAction !== action) return;
    clearPendingAction();
    await action.continuation();
  } finally { $("#applyContinue").disabled = false; $("#discardContinue").disabled = false; }
}
$("#applyContinue").addEventListener("click", () => continueDraft(false));
$("#discardContinue").addEventListener("click", () => continueDraft(true));

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
