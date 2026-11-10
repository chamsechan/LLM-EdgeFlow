import { api, write } from "./api.js";
import { GraphView } from "./graph.js";
import { createHistory, createDrafts, appendDiagnostic, appendConfigField, readConfigFields, readFormBuffer, restoreFormBuffer } from "./editor.js";
import { compatibleModels, modelBoundNodeIds, graphDocument, compatibleBackends, modelAvailability, assertBrowsablePipeline, readPipelineFile, upsertModel, removeModel } from "./workbench.js";

const $ = selector => document.querySelector(selector);
const state = {
  pipeline: null,
  authoringBusy: false,
  filename: "",
  sourceName: "",
  revision: "",
  dirty: false,
  catalog: { nodes: [], models: [], backends: [], input_converters: [], output_converters: [] },
  catalogReady: false,
  selected: "",
  errorNodeIds: new Set(),
  pipelineVersion: 0,
  documentVersion: 0,
  savedPipeline: "",
  selectedEdge: null,
  saving: false,
  loading: false,
};
const history = createHistory();
const drafts = createDrafts();
let documentRequest = 0;
let draftCommit = null;

function snapshot() { return { pipeline: state.pipeline, selected: state.selected }; }

function refreshDocumentView() {
  if (!drafts.has("json")) $("#rawJson").value = JSON.stringify(state.pipeline, null, 2);
  updateEditorStatus();
}

function updateEditorStatus() {
  const pending = drafts.pending;
  // 保存和检查可以等待正在提交的表单；其他编排操作仍需完成后再开始。
  const actionBusy = state.loading || (state.authoringBusy && !draftCommit);
  const badge = $("#dirtyBadge");
  badge.textContent = (state.loading || state.authoringBusy) ? "加载中…" : !state.pipeline ? "未打开" : (pending || state.dirty) ? "未保存" : state.sourceName ? "已导入" : "已保存";
  badge.classList.toggle("dirty", pending || state.dirty);
  $("#undoButton").disabled = (state.loading || state.authoringBusy) || pending || !history.canUndo;
  $("#redoButton").disabled = (state.loading || state.authoringBusy) || pending || !history.canRedo;
  $("#saveButton").disabled = actionBusy || state.saving || !state.pipeline;
  $("#saveAsButton").disabled = actionBusy || state.saving || !state.pipeline;
  $("#openButton").disabled = (state.loading || state.authoringBusy) || !$("#pipelineSelect").value;
  $("#browseButton").disabled = (state.loading || state.authoringBusy);
  updateCreateForm();
  $("#newEntryButton").disabled = (state.loading || state.authoringBusy);
  $("#welcomeCreate").disabled = (state.loading || state.authoringBusy);
  $("#welcomeBrowse").disabled = (state.loading || state.authoringBusy);
  $("#welcomePanel").hidden = Boolean(state.pipeline) || state.loading;
  $("#focusNodeButton").disabled = (state.loading || state.authoringBusy) || !state.selected || !graph.positions[state.selected];
  for (const button of $("#operatorList").querySelectorAll("button")) {
    button.disabled = !state.pipeline || !state.catalogReady || state.loading || state.authoringBusy;
  }
  $("#quickValidateButton").disabled = actionBusy || !state.pipeline || !state.catalogReady;
  $("#quickValidateButton").textContent = "检查";
  $("#validateButton").disabled = actionBusy || !state.pipeline || !state.catalogReady;
  $("#validateButton").textContent = "检查方案";
  $("#saveScope").textContent = state.pipeline ? state.filename
    ? `文件：${state.filename}`
    : "尚未保存为文件。" : "";
  $("#saveButton").title = $("#saveScope").textContent;
  $("#rawJson").readOnly = (state.loading || state.authoringBusy) || drafts.pendingExcept("json").length > 0;
  for (const [kind, form] of [["node", "#nodeForm"], ["model", "#modelForm"], ["io", "#ioForm"]]) {
    for (const control of $(form).querySelectorAll("input, select, textarea, button")) {
      control.disabled = (state.loading || state.authoringBusy) || !state.catalogReady || !state.pipeline || drafts.pendingExcept(kind).length > 0;
    }
  }
  const modelDefinition = categoryDefinition($("#modelType").value);
  if (!modelAvailability(state.catalog.backends || [], modelDefinition).available) $("#applyModel").disabled = true;
  $("#applyJson").disabled = (state.loading || state.authoringBusy) || drafts.pendingExcept("json").length > 0;
  $("#modelSelect").disabled = (state.loading || state.authoringBusy) || !state.catalogReady || drafts.pending;
  $("#newModel").disabled = (state.loading || state.authoringBusy) || !state.catalogReady || drafts.pending;
  for (const [kind, hint] of [["json", "rawJsonHint"], ["node", "nodeDraftHint"], ["model", "modelDraftHint"], ["io", "ioDraftHint"]]) {
    $(`#${hint}`).hidden = !drafts.has(kind);
  }
  for (const kind of ["node", "io", "json", "model"]) {
    $(`#discard${kind[0].toUpperCase() + kind.slice(1)}`).disabled = state.loading || state.authoringBusy || !drafts.has(kind);
  }
}

function requireApplied(except = "") {
  if (state.loading || state.authoringBusy) { toast("方案操作中，请稍候"); return false; }
  const pending = drafts.pendingExcept(except);
  if (!pending.length) return true;
  const labels = { json: "原始 JSON", node: "节点属性", model: "模型", io: "输入与输出" };
  toast(`请先完成或撤回${pending.map(kind => labels[kind]).join("、")}中的修改`);
  switchTab(["node", "io"].includes(pending[0]) ? "properties" : pending[0] === "model" ? "models" : "json");
  return false;
}

function operationFeedback(message, error = false) {
  const output = $("#operationFeedback");
  output.textContent = message; output.hidden = !message;
  output.classList.toggle("error", error);
  if (error) setInspectorOpen(true);
}

function showFailure(error) {
  let report = error.payload;
  if (Array.isArray(report?.diagnostics) && report.diagnostics.length) {
    showValidation(report); switchTab("validation");
    operationFeedback("方案校验未通过，请按诊断修复后重试。", true);
  } else operationFeedback(error.message, true);
}

function resetDocumentFeedback() {
  operationFeedback("");
  setInspectorOpen(false);
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
  state.errorNodeIds = new Set();
  $("#validationOutput").textContent = message;
  $("#validationBadge").textContent = state.pipeline ? "尚未检查" : "";
}

function markPipelineChanged(record = true) {
  state.pipelineVersion += 1;
  clearValidation("方案已修改，请重新校验");
  state.selectedEdge = null;
  if (record) history.record(snapshot());
  setDirty(JSON.stringify(state.pipeline) !== state.savedPipeline);
}

function positionsKey() { return `edgeflow.positions.${state.filename || state.sourceName || "draft"}`; }
function restorePositions() {
  try {
    const saved = JSON.parse(localStorage.getItem(positionsKey()) || "{}");
    graph.positions = Object.assign(Object.create(null), Object.fromEntries(Object.entries(saved).filter(([, point]) => Number.isFinite(point?.x) && Number.isFinite(point?.y))));
  }
  catch { graph.positions = Object.create(null); }
}

function savePositions(positions) {
  try { localStorage.setItem(positionsKey(), JSON.stringify(positions)); }
  catch { /* 浏览器存储已满或被禁用时，不得中断编辑。 */ }
}

async function selectNode(id) {
  if (state.loading) return;
  if (drafts.has("node") && state.selected !== id) {
    return ensureAppliedOrAction(() => selectNode(id));
  }
  state.selected = id; state.selectedEdge = null; renderAll();
  switchTab("properties");
  if (["input", "output"].includes(id)) {
    $("#ioSettings").open = true;
    $("#ioSettings").scrollIntoView({block: "nearest"});
  }
}

const graph = new GraphView($("#graph"), {
  select: selectNode,
  connect: (source, sourcePort, target, targetPort) => {
    applyAuthoring({ kind: "connect", source: { node: source, port: sourcePort }, target: { node: target, port: targetPort } });
  },
  selectEdge: edge => {
    if (state.loading) return;
    state.selectedEdge = edge; renderAll();
  },
  positionsChanged: savePositions,
  feedback: message => toast(message),
  viewChanged: ({ scale }) => {
    $("#zoomLabel").textContent = `${Math.round(scale * 100)}%`;
    $("#zoomHint").hidden = !state.pipeline || scale >= .65;
    $("#zoomHint").textContent = "当前为全图预览。选中节点后，点击“放大选中节点”查看文字和端口。";
  },
});

function renderAll() {
  graph.editable = !state.loading && !state.authoringBusy && state.catalogReady;
  const nodes = effectiveNodes();
  const modelNames = modelBoundNodeIds(nodes, state.catalog.nodes);
  const document = graphDocument(state.pipeline, state.catalog);
  graph.render(document.nodes, state.selected, state.errorNodeIds, modelNames, document.definitions, document.edges, state.selectedEdge);
  if (!drafts.has("json")) $("#rawJson").value = state.pipeline ? JSON.stringify(state.pipeline, null, 2) : "";
  $("#graphSummary").textContent = state.pipeline ? `${nodes.length} 个节点 · ${document.edges.length} 条连线` : "打开方案，查看算法流程";
  $("#edgeSelection").hidden = !state.selectedEdge;
  $("#edgeSelectionLabel").textContent = state.selectedEdge ? state.selectedEdge.dependency ? "已选中额外顺序" : `${state.selectedEdge.sourcePort} → ${state.selectedEdge.targetPort}` : "";
  $("#deleteEdgeButton").disabled = !state.selectedEdge;
  renderInspector(nodes.find(node => node.name === state.selected));
  renderIoForm();
  refreshModelList();
  updateEditorStatus();
}

const createIo = {input: [], output: []};
let ioDraft = null;

function rememberIo() {
  drafts.set("io", readFormBuffer($("#ioForm")));
  updateEditorStatus();
}

function readIoParameters() {
  for (const [direction, entries] of Object.entries(ioDraft || {})) {
    for (const [index, entry] of entries.entries()) {
      const container = $(`[data-io-entry="${direction}.${index}"] .io-parameters`);
      if (!container) continue;
      const definition = state.catalog[`${direction}_converters`]?.find(item => item.type === entry.type && item.name === entry.name);
      if (!definition) continue;
      const params = readConfigFields(container, entry.params || {});
      if (Object.keys(params).length) entry.params = params; else delete entry.params;
    }
  }
}

function renderIoList(container, entries, direction, creating) {
  container.replaceChildren();
  const converters = state.catalog[`${direction}_converters`] || [];
  const mutate = change => {
    if (!creating && !requireApplied("io")) return;
    try {
      if (!creating) readIoParameters();
      change();
      if (creating) { renderCreateIo(); updateCreateForm(); }
      else { drafts.set("io", {}); renderIoForm(); rememberIo(); applyIo(); }
    } catch (error) { operationFeedback(error.message, true); }
  };
  for (const [index, entry] of entries.entries()) {
    const row = document.createElement("fieldset"); row.dataset.ioEntry = `${direction}.${index}`;
    const legend = document.createElement("legend"); legend.textContent = `${direction === "input" ? "输入" : "输出"} ${index + 1}`; row.append(legend);
    const type = document.createElement("select"), name = document.createElement("select");
    type.id = `${creating ? "create" : "edit"}_${direction}_${index}_type`;
    name.id = `${creating ? "create" : "edit"}_${direction}_${index}_name`;
    for (const value of new Set(converters.map(item => item.type))) type.add(new Option(value, value));
    if (entry.type && ![...type.options].some(option => option.value === entry.type)) type.add(new Option(`${entry.type} · 未注册`, entry.type));
    type.value = entry.type || "";
    for (const converter of converters.filter(item => item.type === entry.type)) name.add(new Option(converter.name, converter.name));
    if (entry.name && ![...name.options].some(option => option.value === entry.name)) name.add(new Option(`${entry.name} · 未注册`, entry.name));
    name.value = entry.name || "";
    for (const [labelText, select] of [["数据类型", type], ["转换方式", name]]) {
      const label = document.createElement("label"); label.textContent = labelText; label.append(select); row.append(label);
    }
    type.addEventListener("change", () => mutate(() => {
      entries[index] = {type: type.value, name: converters.find(item => item.type === type.value)?.name || ""};
    }));
    name.addEventListener("change", () => mutate(() => { entries[index] = {type: entry.type, name: name.value}; }));
    if (!creating) {
      const params = document.createElement("div"); params.className = "io-parameters";
      const definition = converters.find(item => item.type === entry.type && item.name === entry.name);
      for (const field of definition?.config_fields || []) appendConfigField(params, field, entry.params || {});
      if (!definition) {
        const hint = document.createElement("p"); hint.className = "field-help";
        hint.textContent = "当前构建不支持此转换方式，已有参数将保留。可在 JSON 页查看，或选择其他转换方式。";
        params.append(hint);
      }
      row.append(params);
    }
    const remove = document.createElement("button"); remove.type = "button"; remove.textContent = "删除此项";
    remove.addEventListener("click", () => mutate(() => { entries.splice(index, 1); }));
    row.append(remove); container.append(row);
  }
  const add = document.createElement("button"); add.type = "button"; add.textContent = direction === "input" ? "添加输入" : "添加输出";
  add.disabled = !converters.length;
  add.addEventListener("click", () => mutate(() => { entries.push({type: converters[0].type, name: converters[0].name}); }));
  container.append(add);
}

function renderCreateIo() {
  for (const direction of ["input", "output"]) renderIoList($(`#create${direction === "input" ? "Inputs" : "Outputs"}`), createIo[direction], direction, true);
}

function renderIoForm() {
  $("#ioSettings").hidden = !state.pipeline;
  if (!state.pipeline) return;
  if (!drafts.has("io")) ioDraft = structuredClone(state.pipeline.io || {input: [], output: []});
  for (const direction of ["input", "output"]) {
    ioDraft[direction] ??= [];
    renderIoList($(`#io${direction === "input" ? "Inputs" : "Outputs"}`), ioDraft[direction], direction, false);
  }
  if (drafts.has("io")) restoreFormBuffer($("#ioForm"), drafts.get("io"));
}

function applyIo(redraw = true) {
  if (!$("#ioForm").checkValidity()) return false;
  if (!requireApplied("io")) return false;
  try {
    readIoParameters();
    state.pipeline.io = structuredClone(ioDraft);
    drafts.clear("io"); markPipelineChanged(); if (redraw) renderAll(); else refreshDocumentView();
    return true;
  } catch (error) { operationFeedback(error.message, true); return false; }
}

function renderInspector(node) {
  $("#emptyInspector").hidden = Boolean(node);
  $("#nodeForm").hidden = !node;
  const boundary = ["input", "output"].includes(state.selected);
  $("#emptyInspectorTitle").textContent = boundary ? state.selected === "input" ? "业务输入" : "业务输出" : "选择一个节点";
  $("#emptyInspectorDescription").textContent = boundary ? "在上方查看或设置方案的输入与输出。" : "点击画布中的节点，在这里查看和调整参数。";
  if (!node) return;
  $("#nodeName").value = node.name;
  const definition = state.catalog.nodes.find(item => item.node_type === node.type);
  $("#nodeType").textContent = node.type;
  $("#nodeType").title = definition?.description || "";
  const container = $("#configFields");
  container.replaceChildren();
  const extra = document.createElement("details"), summary = document.createElement("summary");
  summary.textContent = "更多参数"; extra.append(summary);
  for (const field of definition?.config_fields || []) {
    const dep = (definition?.model_dependencies || []).find(d => d.config_field === field.name);
    const choices = dep ? compatibleModels(state.pipeline.models, dep.model_type).map(model => model.name) : null;
    const essential = field.required || Object.hasOwn(node.params || {}, field.name);
    appendConfigField(essential ? container : extra, field, node.params || {}, choices);
  }
  if (extra.children.length > 1) container.append(extra);
  renderBindings(node);
  if (drafts.has("node")) restoreFormBuffer($("#nodeForm"), drafts.get("node"));
}

function renderOperators() {
  const query = $("#operatorSearch").value.trim().toLowerCase();
  const list = $("#operatorList"); list.replaceChildren();
  for (const node of state.catalog.nodes.filter(item => `${item.node_type} ${item.description}`.toLowerCase().includes(query))) {
    const button = document.createElement("button"); button.className = "operator";
    const name = document.createElement("strong"), description = document.createElement("small");
    name.textContent = node.node_type; description.textContent = node.description;
    button.append(name, description);
    button.disabled = !state.pipeline || !state.catalogReady || state.loading || state.authoringBusy;
    button.addEventListener("click", () => addNode(node)); list.append(button);
  }
  if (!list.children.length) list.textContent = "没有找到匹配的节点。";
}

async function addNode(definition) {
  if (await applyAuthoring({ kind: "add_node", type: definition.node_type })) {
    switchTab("properties");
    requestAnimationFrame(() => graph.focusNode(state.selected));
  }
}

async function applyAuthoring(operation, options = {}) {
  if (!options.except && drafts.pending) return ensureAppliedOrAction(() => applyAuthoring(operation, options));
  if (!state.pipeline || !requireApplied(options.except || "")) return false;
  const source = structuredClone(state.pipeline);
  options.configure?.(source);
  const version = state.pipelineVersion, documentVersion = state.documentVersion;
  const current = () => !state.loading && version === state.pipelineVersion &&
    documentVersion === state.documentVersion;
  state.authoringBusy = true; updateEditorStatus(); graph.editable = false;
  try {
    const result = await write("/authoring/preview", "POST", {
      pipeline: source, operation,
    }, true);
    if (!current()) return false;
    if (!result.ok || !result.pipeline) {
      operationFeedback(result.diagnostics.map(item => item.message).join("\n") || "编排操作失败", true);
      return false;
    }
    const validation = result.validation;
    if (options.except) drafts.clear(options.except);
    state.pipeline = result.pipeline;
    if (options.selected !== undefined) state.selected = options.selected;
    else if (operation.kind === "add_node") state.selected = result.changes?.find(c => c.action === "add_node")?.node_name || "";
    options.onCommit?.();
    markPipelineChanged();
    showValidation(validation);
    operationFeedback((result.changes || []).map(c => c.description).join("\n") || "编排操作完成");
    return true;
  } catch (error) {
    if (current()) operationFeedback(error.message, true);
    return false;
  } finally { state.authoringBusy = false; renderAll(); }
}

function renderBindings(node) {
  const container = $("#nodeBindings");
  if (!container) return;
  container.replaceChildren();
  const label = document.createElement("p"); label.textContent = "额外执行顺序（数据依赖由框架推导）"; container.append(label);
  for (const id of node.depends_on || []) {
    const row = document.createElement("div"); row.className = "row";
    const text = document.createElement("span"); text.textContent = id;
    const remove = document.createElement("button"); remove.type = "button"; remove.textContent = "删除依赖";
    remove.disabled = state.authoringBusy;
    remove.addEventListener("click", () => applyAuthoring({ kind: "remove_dependency", node: node.name, depends_on: id }));
    row.append(text, remove); container.append(row);
  }
  const select = document.createElement("select"); select.id = "dependencySource";
  select.add(new Option("选择前置节点", ""));
  for (const other of effectiveNodes()) if (other.name !== node.name && !(node.depends_on || []).includes(other.name)) select.add(new Option(other.name, other.name));
  const add = document.createElement("button"); add.type = "button"; add.id = "addDependencyButton"; add.textContent = "添加依赖";
  add.disabled = state.authoringBusy;
  add.addEventListener("click", () => { if (select.value) applyAuthoring({ kind: "add_dependency", node: node.name, depends_on: select.value }); });
  container.append(select, add);
}

async function refreshLists() {
  const [catalog, pipelines] = await Promise.allSettled([api("/catalog"), api("/pipelines")]);
  if (pipelines.status === "fulfilled") {
    const schemes = $("#pipelineSelect"); schemes.replaceChildren(new Option("选择方案", ""));
    for (const item of pipelines.value.pipelines) schemes.add(new Option(item.filename, item.filename));
  }
  if (catalog.status === "rejected") throw catalog.reason;
  state.catalog = catalog.value;
  state.catalogReady = true;
  renderOperators(); renderCreateIo(); updateCreateForm();
  if (pipelines.status === "rejected") throw pipelines.reason;
}

function updateCreateForm() {
  const disabled = state.loading || state.authoringBusy || !state.catalogReady;
  $("#newButton").disabled = disabled || !createIo.input.length || !createIo.output.length;
  $("#createHint").textContent = !state.catalogReady ? "节点列表尚未就绪，请构建工具并刷新页面；仍可打开和保存 JSON。"
    : "选择输入与输出，创建后添加节点并连接端口。也可打开已有方案后另存为。";
  for (const element of $("#createIo").querySelectorAll("select, button")) element.disabled = disabled;
}

async function openPipeline(filename) {
  if (!filename) return;
  return openDocument(() => api(`/pipeline?filename=${encodeURIComponent(filename)}`));
}

async function openDocument(load, { creating = false } = {}) {
  if ((state.dirty || drafts.pending) && !confirm(`当前修改尚未保存，确认丢弃并${creating ? "新建" : "打开其他方案"}？`)) return;
  const request = ++documentRequest;
  state.loading = true; graph.editable = false; updateEditorStatus();
  try {
    const result = await load();
    if (request !== documentRequest) return;
    assertBrowsablePipeline(result.pipeline);
    state.pipeline = result.pipeline;
    state.filename = creating || result.imported ? "" : result.filename;
    state.sourceName = !creating && result.imported ? result.filename : "";
    state.revision = creating ? "" : result.revision; state.selected = "";
    if (!creating) $("#pipelineSelect").value = state.filename;
    drafts.clear(); state.selectedEdge = null; state.documentVersion += 1;
    resetDocumentFeedback();
    state.savedPipeline = creating ? "" : JSON.stringify(state.pipeline); history.reset(snapshot());
    state.pipelineVersion += 1;
    restorePositions(); clearValidation(); setDirty(creating);
    if (creating) {
          $(".create-panel").open = false;
      switchTab("properties");
      operationFeedback("空白方案已创建。从节点库添加节点，再连接输入和输出。完成后检查并保存。");
    }
  } finally {
    if (request === documentRequest) { state.loading = false; renderAll(); }
  }
}

async function createPipeline() {
  if (!state.catalogReady || state.loading || state.authoringBusy) return;
  return openDocument(() => write("/init", "POST", structuredClone(createIo)), { creating: true });
}

// 受控方案文件名为 pipeline_[a-z0-9_]+.json；这里只做大小写、空白和前后缀的规范化。
function solutionName(value = "") {
  return value.trim().toLowerCase().replace(/\.json$/, "").replace(/^pipeline_/, "").replace(/[\s-]+/g, "_");
}

async function save(saveAs) {
  if (!state.pipeline) return toast("没有可保存的方案", true);
  if (state.saving || !requireApplied()) return;
  let filename = state.filename;
  if (saveAs || !filename) {
    const suggested = solutionName(filename || state.sourceName);
    const entered = prompt("保存方案：请输入名称（小写字母、数字或下划线），保存为 configs/pipeline_<名称>.json",
      /^[a-z0-9_]+$/.test(suggested) ? suggested : "my_solution");
    if (entered === null) return;
    const name = solutionName(entered);
    if (!/^[a-z0-9_]+$/.test(name)) return toast("名称只能包含小写字母、数字和下划线，例如 my_solution", true);
    filename = `pipeline_${name}.json`;
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
    const feedback = `已保存：${result.filename}${state.dirty || drafts.pending ? "；当前仍有新修改" : ""}`;
    operationFeedback(feedback); toast(feedback);
    const pipelines = await api("/pipelines");
    if (documentVersion !== state.documentVersion) return;
    const schemes = $("#pipelineSelect"); schemes.replaceChildren(new Option("选择方案", ""));
    for (const item of pipelines.pipelines) schemes.add(new Option(item.filename, item.filename));
    $("#pipelineSelect").value = state.filename;
  } catch (error) { if (documentVersion === state.documentVersion) showFailure(error); }
  finally { state.saving = false; updateEditorStatus(); }
}

function showValidation(report) {
  const output = $("#validationOutput");
  $("#validationBadge").textContent = report.ok ? "检查通过" : `${report.diagnostics?.length || 1} 处待完善`;
  output.replaceChildren();
  state.errorNodeIds = new Set();
  if (report.ok) {
    const block = document.createElement("div"); block.className = "diagnostic ok";
    block.textContent = "连接与配置检查通过。";
    output.append(block);
  }
  for (const item of report.diagnostics || []) {
    if (item.node_name) state.errorNodeIds.add(item.node_name);
    appendDiagnostic(
      output,
      item,
      id => { selectNode(id); switchTab("properties"); graph.focusNode(id); }
    );
  }
  renderAll();
}

async function validate() {
  if (!state.catalogReady) {
    toast("校验工具暂不可用，请检查服务后重试", true);
    return false;
  }
  if (!state.pipeline || !requireApplied()) return;
  switchTab("validation"); operationFeedback("");
  const output = $("#validationOutput");
  const pipelineVersion = state.pipelineVersion;
  clearValidation("校验中…"); renderAll();
  try {
    const report = await write("/validate", "POST", { pipeline: state.pipeline }, true);
    if (pipelineVersion !== state.pipelineVersion) return false;
    showValidation(report);
    return report.ok;
  } catch (error) {
    if (pipelineVersion === state.pipelineVersion) output.textContent = error.message;
    return false;
  }
}

async function ensureAppliedOrAction(action) {
  if (state.loading) return false;
  if (draftCommit && !await draftCommit) return false;
  if (state.authoringBusy) return false;
  const pending = drafts.pendingExcept("");
  for (const kind of pending) {
    const ok = await applyDraft(kind);
    if (!ok) return false;
  }
  return await action();
}

let editingModelName = "";
let modelDocument = null;

function refreshModelList() {
  const select = $("#modelSelect"), previous = select.value;
  select.replaceChildren(new Option("新增模型", ""));
  for (const model of state.pipeline?.models || []) select.add(new Option(model.name, model.name));
  select.value = previous;
  if (!drafts.has("model") && (modelDocument !== state.pipeline || !$("#modelType").options.length)) {
    modelDocument = state.pipeline;
    loadModelEditor(state.pipeline?.models?.[0]?.name || "");
  }
}

function loadModelEditor(id = "") {
  editingModelName = id;
  $("#modelSelect").value = id;
  const model = state.pipeline?.models?.find(item => item.name === id);
  $("#modelName").value = model?.name || "";
  $("#modelFile").value = model?.file || "";
  const type = $("#modelType"); type.replaceChildren();
  for (const category of new Set((state.catalog.models || []).map(item => item.model_type))) {
    const availability = modelAvailability(state.catalog.backends || [], categoryDefinition(category));
    const option = new Option(`${category}${availability.available ? "" : " · 当前构建无兼容 Backend"}`, category);
    option.title = availability.message;
    type.add(option);
  }
  if (model && !state.catalog.models?.some(item => item.model_type === model.type)) type.add(new Option(`${model.type} · 当前构建未注册`, model.type));
  if (model) type.value = model.type;
  renderModelFields(model);
  updateEditorStatus();
}

function categoryDefinition(category) {
  const definitions = (state.catalog.models || []).filter(item => item.model_type === category);
  return definitions.length ? {...definitions[0], backends: [...new Set(definitions.flatMap(item => item.backends || []))]} : null;
}

function renderModelFields(model = null) {
  const definition = categoryDefinition($("#modelType").value);
  const backend = $("#modelBackend"); backend.replaceChildren();
  const availability = modelAvailability(state.catalog.backends || [], definition);
  const compatible = compatibleBackends(state.catalog.backends || [], definition);
  for (const item of compatible) backend.add(new Option(item.backend_type, item.backend_type));
  const hint = $("#modelAvailability"); hint.textContent = availability.message; hint.hidden = availability.available;
  if (model && !compatible.some(item => item.backend_type === model.backend?.type)) {
    const option = new Option(`${model.backend?.type} · 当前构建不可用或不兼容`, model.backend?.type);
    option.disabled = true; backend.add(option);
    if (availability.available) { hint.hidden = false; hint.textContent = "当前 Backend 不可用或不兼容，请选择列表中的兼容 Backend。"; }
  } else if (!compatible.length) {
    const option = new Option("当前构建无兼容 Backend", ""); option.disabled = true; backend.add(option); option.selected = true;
  }
  if (model) backend.value = model.backend?.type || "";
  renderModelParameters(model?.params || {});
  renderBackendFields(model?.backend?.params || {});
}

function renderModelParameters(values = {}) {
  const definition = state.catalog.models?.find(item => item.model_type === $("#modelType").value && item.backends?.includes($("#modelBackend").value));
  $("#modelImplementation").textContent = definition ? `实现：${definition.impl_name}` : "当前组合未注册实现";
  const container = $("#modelConfigFields"); container.replaceChildren();
  for (const field of definition?.config_fields || []) appendConfigField(container, field, values);
}

function renderBackendFields(values = {}) {
  const definition = state.catalog.backends?.find(item => item.backend_type === $("#modelBackend").value);
  const container = $("#backendConfigFields"); container.replaceChildren();
  for (const field of definition?.config_fields || []) appendConfigField(container, field, values);
  $("#backendAdvanced").open = Object.keys(values).length > 0;
}

function updateBackendAvailability() {
  const definition = categoryDefinition($("#modelType").value);
  const availability = modelAvailability(state.catalog.backends || [], definition);
  const selectedIsCompatible = compatibleBackends(state.catalog.backends || [], definition).some(item => item.backend_type === $("#modelBackend").value);
  const hint = $("#modelAvailability");
  hint.hidden = selectedIsCompatible;
  hint.textContent = availability.available ? "当前 Backend 不可用或不兼容，请选择列表中的兼容 Backend。" : availability.message;
}

$("#modelSelect").addEventListener("change", event => {
  if (!requireApplied()) { event.target.value = editingModelName; return; }
  loadModelEditor(event.target.value);
});
$("#newModel").addEventListener("click", () => { if (requireApplied()) loadModelEditor(); });
$("#modelType").addEventListener("change", () => renderModelFields());
$("#modelBackend").addEventListener("change", () => {
  try { renderModelParameters(readConfigFields($("#modelConfigFields"))); renderBackendFields(); updateBackendAvailability(); }
  catch (error) { operationFeedback(error.message, true); }
});
function applyModel() {
  if (!$("#modelForm").checkValidity()) return false;
  if (!state.pipeline) return toast("请先新建或打开方案", true);
  if (!requireApplied("model")) return;
  try {
    const previous = state.pipeline.models?.find(model => model.name === editingModelName && model.type === $("#modelType").value);
    const previousBackend = previous?.backend?.type === $("#modelBackend").value ? previous.backend : {};
    const model = {
      ...previous,
      name: $("#modelName").value.trim(), type: $("#modelType").value, file: $("#modelFile").value.trim(),
      backend: {...previousBackend, type: $("#modelBackend").value},
    };
    delete model.params; delete model.backend.params;
    const params = readConfigFields($("#modelConfigFields"), previous?.params || {}), backendParams = readConfigFields($("#backendConfigFields"), previousBackend.params || {});
    if (Object.keys(params).length) model.params = params;
    if (Object.keys(backendParams).length) model.backend.params = backendParams;
    state.pipeline.models ??= [];
    upsertModel(state.pipeline, state.catalog, editingModelName, model);
    drafts.clear("model");
    markPipelineChanged(); renderAll(); loadModelEditor(model.name); toast("模型已应用，请校验方案");
    return true;
  } catch (error) { operationFeedback(error.message, true); return false; }
}
$("#deleteModel").addEventListener("click", () => {
  if (!state.pipeline || !editingModelName || !requireApplied()) return;
  try {
    removeModel(state.pipeline, state.catalog, editingModelName);
    markPipelineChanged(); renderAll(); loadModelEditor();
  } catch (error) { toast(error.message, true); }
});

function switchTab(name) {
  setInspectorOpen(true);
  document.querySelectorAll(".tabs button").forEach(button => {
    button.classList.toggle("active", button.dataset.tab === name);
    button.setAttribute("aria-selected", String(button.dataset.tab === name));
  });
  for (const tab of ["properties", "models", "json", "validation"]) $(`#${tab}Tab`).hidden = tab !== name;
}

function setInspectorOpen(open) {
  document.body.classList.toggle("inspector-open", open);
  $("#inspectorToggle").setAttribute("aria-pressed", String(open));
}

function restoreHistory(direction) {
  if (!requireApplied()) return;
  const restored = history[direction]();
  if (!restored) return;
  state.pipeline = restored.pipeline; state.selected = restored.selected;
  markPipelineChanged(false);
  renderAll();
}

function deleteSelectedEdge() {
  const edge = state.selectedEdge;
  if (!edge || !requireApplied()) return;
  return applyAuthoring(edge.dependency
    ? { kind: "remove_dependency", node: edge.target, depends_on: edge.source }
    : { kind: "disconnect", source: { node: edge.source, port: edge.sourcePort }, target: { node: edge.target, port: edge.targetPort } });
}

$("#inspectorToggle").addEventListener("click", () => setInspectorOpen(!document.body.classList.contains("inspector-open")));
const resizeHandle = $("#inspectorResize");
const resizeInspector = width => {
  width = Math.max(280, Math.min(600, innerWidth * .45, width));
  $(".inspector").style.width = `${width}px`;
  $(".inspector").style.flexBasis = `${width}px`;
  resizeHandle.setAttribute("aria-valuenow", String(Math.round(width)));
};
let resizing = false;
resizeHandle.addEventListener("pointerdown", event => {
  if (event.button !== 0) return;
  resizing = true; resizeHandle.setPointerCapture(event.pointerId); event.preventDefault();
});
resizeHandle.addEventListener("pointermove", event => { if (resizing) resizeInspector(innerWidth - event.clientX); });
for (const type of ["pointerup", "pointercancel"]) resizeHandle.addEventListener(type, () => { resizing = false; });
resizeHandle.addEventListener("keydown", event => {
  if (["ArrowLeft", "ArrowRight"].includes(event.key)) {
    event.preventDefault(); resizeInspector($(".inspector").getBoundingClientRect().width + (event.key === "ArrowLeft" ? 20 : -20));
  }
});
for (const [id, name] of [["operatorsToggle", "operators-hidden"]]) {
  $("#" + id).addEventListener("click", () => {
    const hidden = document.body.classList.toggle(name);
    $("#" + id).setAttribute("aria-pressed", String(!hidden));
  });
}
$("#fitButton").addEventListener("click", () => graph.fit());
$("#focusNodeButton").addEventListener("click", () => graph.focusNode(state.selected));
$("#zoomInButton").addEventListener("click", () => graph.zoomBy(1.2));
$("#zoomOutButton").addEventListener("click", () => graph.zoomBy(1 / 1.2));
$("#actualSizeButton").addEventListener("click", () => graph.resetZoom());
$("#undoButton").addEventListener("click", () => restoreHistory("undo"));
$("#redoButton").addEventListener("click", () => restoreHistory("redo"));
$("#deleteEdgeButton").addEventListener("click", deleteSelectedEdge);

$("#rawJson").addEventListener("input", event => {
  operationFeedback("");
  if (event.target.value === JSON.stringify(state.pipeline, null, 2)) drafts.clear("json");
  else drafts.set("json", event.target.value);
  updateEditorStatus();
});
for (const [kind, selector] of [["node", "#nodeForm"], ["model", "#modelForm"], ["io", "#ioForm"]]) {
  const remember = event => {
    // 图控件提交的是编辑操作，而非 Node 参数草稿。
    if (event.target.closest?.("#nodeBindings")) return;
    if (event.target.closest?.("[data-collection]")) {
      if (event.type === "input") { drafts.set(kind, readFormBuffer($(selector))); updateEditorStatus(); }
      return;
    }
    event.target.setCustomValidity?.("");
    operationFeedback("");
    drafts.set(kind, readFormBuffer($(selector))); updateEditorStatus();
    if (event.type === "change" && kind !== "model") applyDraft(kind, false);
  };
  $(selector).addEventListener("input", remember);
  $(selector).addEventListener("change", remember);
}
for (const kind of ["node", "io", "json", "model"]) {
  const button = $(`#discard${kind[0].toUpperCase() + kind.slice(1)}`);
  // 点击撤回不先让输入框失焦提交，否则会撤回刚刚已经生效的编辑。
  button.addEventListener("pointerdown", event => { if (event.button === 0) event.preventDefault(); });
  button.addEventListener("click", () => {
    if (state.loading || state.authoringBusy) return;
    drafts.clear(kind); operationFeedback("");
    if (kind === "model") loadModelEditor(editingModelName);
    renderAll();
  });
}

window.addEventListener("keydown", event => {
  const typing = event.target.closest?.("input, textarea, select, [contenteditable=true]");
  if ((event.ctrlKey || event.metaKey) && event.key.toLowerCase() === "s") {
    event.preventDefault(); ensureAppliedOrAction(() => save(false)); return;
  }
  if (typing) return;
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
$("#pipelineSelect").addEventListener("change", updateEditorStatus);
$("#browseButton").addEventListener("click", () => $("#pipelineFile").click());
$("#welcomeBrowse").addEventListener("click", () => $("#browseButton").click());
$("#pipelineFile").addEventListener("change", async event => {
  const file = event.target.files?.[0];
  event.target.value = "";
  if (!file) return;
  try { await openDocument(() => readPipelineFile(file)); }
  catch (error) { toast(`打开文件失败：${error.message}`, true); }
});
$("#newEntryButton").addEventListener("click", () => {
  document.body.classList.remove("operators-hidden");
  $("#operatorsToggle").setAttribute("aria-pressed", "true");
  $(".create-panel").open = true; $("#createIo select, #createIo button")?.focus();
  if (!state.pipeline) setInspectorOpen(false);
});
$("#welcomeCreate").addEventListener("click", () => $("#newEntryButton").click());
$("#quickValidateButton").addEventListener("click", () => ensureAppliedOrAction(validate));
$("#newButton").addEventListener("click", () => createPipeline().catch(error => toast(error.message, true)));
$("#saveButton").addEventListener("click", () => ensureAppliedOrAction(() => save(false)));
$("#saveAsButton").addEventListener("click", () => ensureAppliedOrAction(() => save(true)));
$("#layoutButton").addEventListener("click", () => {
  graph.layout(graphDocument(state.pipeline, state.catalog).nodes, true);
  renderAll();
});
$("#operatorSearch").addEventListener("input", renderOperators);
$("#validateButton").addEventListener("click", () => ensureAppliedOrAction(validate));
document.querySelectorAll(".tabs button").forEach(button => button.addEventListener("click", () => switchTab(button.dataset.tab)));

async function applyNode(redraw = true) {
  if (!$("#nodeForm").checkValidity()) return false;
  if (!requireApplied("node")) return;
  if (!state.catalog.nodes.some(definition => definition.node_type === effectiveNodes().find(node => node.name === state.selected)?.type)) return toast("节点定义不可用，请重新打开方案或修正 JSON", true);
  const node = state.pipeline.pipeline.find(item => item.name === state.selected);
  const newId = $("#nodeName").value.trim();
  if (!newId || state.pipeline.pipeline.some(item => item !== node && item.name === newId)) {
    $("#nodeName").setCustomValidity("节点名为空或重复"); $("#nodeName").reportValidity();
    operationFeedback("节点名为空或重复", true); return false;
  }
  try {
    const config = readConfigFields($("#configFields"), node.params || {});
    const oldId = node.name;
    if (newId !== oldId) {
      return await applyAuthoring({ kind: "rename_node", node: oldId, new_name: newId }, {
        except: "node", selected: newId,
        configure: candidate => { candidate.pipeline.find(n => n.name === oldId).params = config; },
        onCommit: () => {
          if (graph.positions[oldId]) { graph.positions[newId] = graph.positions[oldId]; delete graph.positions[oldId]; savePositions(graph.positions); }
        },
      });
    }
    node.params = config;
    drafts.clear("node"); markPipelineChanged(); if (redraw) renderAll(); else refreshDocumentView();
    return true;
  } catch (error) { operationFeedback(`配置错误：${error.message}`, true); return false; }
}

$("#deleteNode").addEventListener("click", () => {
  const id = state.selected;
  applyAuthoring({ kind: "remove_node", node: id }, { selected: "", onCommit: () => {
    delete graph.positions[id]; savePositions(graph.positions);
  } });
});

async function applyJson() {
  if (!requireApplied("json")) return false;
  let parsed;
  try {
    parsed = JSON.parse($("#rawJson").value);
    assertBrowsablePipeline(parsed);
  } catch (error) {
    operationFeedback(`JSON 错误：${error.message}`, true);
    $("#rawJson").focus(); return false;
  }

  drafts.clear("json");
  state.pipeline = parsed; state.selected = ""; markPipelineChanged();
  renderAll();
  return true;
}

function showDraftError(kind) {
  switchTab(kind === "node" || kind === "io" ? "properties" : kind === "model" ? "models" : "json");
  const input = kind === "json" ? $("#rawJson") : $(`#${kind}Form`).querySelector("input:invalid, select:invalid, textarea:invalid");
  if (input) {
    for (let parent = input.parentElement; parent; parent = parent.parentElement) {
      if (parent.tagName === "DETAILS") parent.open = true;
    }
    input.focus(); input.reportValidity();
  }
  if (!$("#operationFeedback").textContent) operationFeedback("请修正标出的输入，或点击“撤回输入”。", true);
}

function applyDraft(kind, redraw = true) {
  if (draftCommit) return draftCommit;
  // 失焦提交与随后点击的保存共用同一个 Promise，不丢弃后续操作，也不重复重命名。
  draftCommit = Promise.resolve().then(async () => {
    operationFeedback("");
    const ok = await (kind === "node" ? applyNode(redraw) : kind === "model" ? applyModel() : kind === "io" ? applyIo(redraw) : applyJson());
    if (!ok) showDraftError(kind);
    return ok;
  }).finally(() => { draftCommit = null; updateEditorStatus(); });
  return draftCommit;
}
// 表单的 Enter 隐式提交只应用当前表单，不触发页面提交。
for (const kind of ["io", "node", "model"]) {
  $(`#${kind}Form`).addEventListener("submit", event => { event.preventDefault(); applyDraft(kind); });
}
$("#applyJson").addEventListener("click", () => applyDraft("json"));

window.addEventListener("beforeunload", event => { if (state.dirty || drafts.pending) { event.preventDefault(); event.returnValue = ""; } });

try {
  await refreshLists();
} catch (error) { toast(`工具列表暂不可用，仍可浏览文件：${error.message}`, true); }
updateEditorStatus();

try {
  const initial = await api("/initial");
  if (initial.document && documentRequest === 0) await openDocument(async () => initial.document);
} catch (error) { toast(error.message, true); }
