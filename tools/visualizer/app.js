import { api, initialPipeline, write } from "./api.js";
import { GraphView } from "./graph.js";

const $ = selector => document.querySelector(selector);
const state = {
  pipeline: null,
  filename: "",
  revision: "",
  dirty: false,
  catalog: { nodes: [], businesses: [], profiles: [] },
  profiles: [],
  selected: "",
  jobId: "",
};

function toast(message, error = false) {
  const element = $("#toast");
  element.textContent = message;
  element.className = `show${error ? " error" : ""}`;
  clearTimeout(toast.timer);
  toast.timer = setTimeout(() => { element.className = ""; }, 2600);
}

function effectiveNodes() {
  if (!state.pipeline) return [];
  return state.pipeline.pipeline.map((node, index, all) => ({
    ...node,
    id: node.id || `node_${index}_${node.node_type}`,
    depends_on: node.depends_on ?? (index ? [all[index - 1].id || `node_${index - 1}_${all[index - 1].node_type}`] : []),
  }));
}

function isExplicit() {
  return state.pipeline?.pipeline?.every(node => typeof node.id === "string" && Array.isArray(node.depends_on));
}

async function ensureExplicit() {
  if (isExplicit()) return true;
  if (!confirm("这是旧式顺序配置。首次结构编辑需要由 C++ normalize 升级为显式 DAG，是否继续？")) return false;
  try {
    const result = await write("/normalize", "POST", { pipeline: state.pipeline });
    state.pipeline = result.pipeline;
    setDirty(true);
    renderAll();
    toast("已升级为 id + depends_on 显式 DAG");
    return true;
  } catch (error) { toast(error.message, true); return false; }
}

function setDirty(value) {
  state.dirty = value;
  const badge = $("#dirtyBadge");
  badge.textContent = !state.pipeline ? "未打开" : value ? "未保存" : "已保存";
  badge.classList.toggle("dirty", value);
  $("#documentTitle").textContent = state.filename || "未命名方案";
}

function positionsKey() { return `edgeflow.positions.${state.filename || state.pipeline?.business_name || "draft"}`; }
function restorePositions() {
  try { graph.positions = JSON.parse(localStorage.getItem(positionsKey()) || "{}"); }
  catch { graph.positions = {}; }
}

const graph = new GraphView($("#graph"), {
  select: id => { state.selected = id; renderAll(); },
  connect: async (source, target) => {
    if (!await ensureExplicit()) return;
    const node = state.pipeline.pipeline.find(item => item.id === target);
    if (!node || node.depends_on.includes(source)) return toast("重复连线已拒绝", true);
    if (source === target || reaches(source, target)) return toast("成环连线已拒绝", true);
    node.depends_on.push(source); setDirty(true); renderAll();
  },
  deleteEdge: async (source, target) => {
    if (!await ensureExplicit()) return;
    const node = state.pipeline.pipeline.find(item => item.id === target);
    node.depends_on = node.depends_on.filter(id => id !== source); setDirty(true); renderAll();
  },
  positionsChanged: positions => localStorage.setItem(positionsKey(), JSON.stringify(positions)),
});

function reaches(from, wanted) {
  const byId = new Map(state.pipeline.pipeline.map(node => [node.id, node]));
  const pending = [from], seen = new Set();
  while (pending.length) {
    const id = pending.pop();
    if (id === wanted) return true;
    if (seen.has(id)) continue;
    seen.add(id);
    for (const dep of byId.get(id)?.depends_on || []) pending.push(dep);
  }
  return false;
}

function renderAll() {
  const nodes = effectiveNodes();
  graph.render(nodes, state.selected);
  $("#rawJson").value = state.pipeline ? JSON.stringify(state.pipeline, null, 2) : "";
  renderInspector(nodes.find(node => node.id === state.selected));
  filterProfiles();
}

function renderInspector(node) {
  $("#emptyInspector").hidden = Boolean(node);
  $("#nodeForm").hidden = !node;
  if (!node) return;
  $("#nodeId").value = node.id;
  $("#nodeType").textContent = node.node_type;
  $("#configJson").value = JSON.stringify(node.config || {}, null, 2);
  const definition = state.catalog.nodes.find(item => item.node_type === node.node_type);
  const container = $("#configFields");
  container.replaceChildren();
  for (const field of definition?.config_fields || []) {
    const label = document.createElement("label"); label.textContent = field.name;
    let input;
    if (field.semantic === "model_ref") {
      input = document.createElement("select");
      for (const model of state.pipeline.models || []) {
        const option = new Option(model.model_id, model.model_id); input.add(option);
      }
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
    const value = (node.config || {})[field.name] ?? field.default;
    input.value = typeof value === "object" ? JSON.stringify(value) : value ?? "";
    label.append(input); container.append(label);
  }
}

function parseField(input) {
  if (input.dataset.type === "integer") return Number.parseInt(input.value, 10);
  if (input.dataset.type === "number") return Number(input.value);
  if (input.dataset.type === "boolean") return input.value === "true";
  if (input.dataset.type === "object" || input.dataset.type === "array") return JSON.parse(input.value);
  return input.value;
}

async function loadCatalog(business = "") {
  state.catalog = await api(`/catalog${business ? `?business=${encodeURIComponent(business)}` : ""}`);
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

async function addNode(definition) {
  if (!state.pipeline) return toast("请先打开或新建方案", true);
  if (!await ensureExplicit()) return;
  const base = definition.node_type.replace(/Node$/, "").replace(/([A-Z])/g, "_$1").toLowerCase().replace(/^_/, "");
  let index = 1, id = base;
  const ids = new Set(state.pipeline.pipeline.map(node => node.id));
  while (ids.has(id)) id = `${base}_${index++}`;
  const config = {};
  for (const field of definition.config_fields || []) if (field.default !== undefined) config[field.name] = field.default;
  state.pipeline.pipeline.push({ id, node_type: definition.node_type, depends_on: [], config });
  state.selected = id; setDirty(true); renderAll();
}

async function refreshLists() {
  const [allCatalog, profiles, pipelines] = await Promise.all([api("/catalog"), api("/profiles"), api("/pipelines")]);
  state.catalog = state.pipeline ? await api(`/catalog?business=${encodeURIComponent(state.pipeline.business_name)}`) : allCatalog;
  state.profiles = profiles.profiles;
  const business = $("#businessSelect"); business.replaceChildren();
  for (const item of allCatalog.businesses) business.add(new Option(`${item.display_name} · ${item.business_name}`, item.business_name));
  if (state.pipeline) business.value = state.pipeline.business_name;
  const schemes = $("#pipelineSelect"); schemes.replaceChildren(new Option("选择方案", ""));
  for (const item of pipelines.pipelines) schemes.add(new Option(`${item.filename} · ${item.business_name}`, item.filename));
  renderOperators(); filterProfiles();
}

function filterProfiles() {
  const businessName = state.pipeline?.business_name || $("#businessSelect").value;
  const matching = state.catalog.profiles?.length ? state.catalog.profiles : [];
  for (const selector of [$("#cloneProfile"), $("#runProfile")]) {
    const previous = selector.value; selector.replaceChildren();
    if (selector.id === "cloneProfile") selector.add(new Option("空图", ""));
    for (const profile of matching.filter(item => item.pipeline_business === businessName)) selector.add(new Option(`${profile.name} · ${profile.suite}`, profile.name));
    if ([...selector.options].some(item => item.value === previous)) selector.value = previous;
  }
}

async function openPipeline(filename) {
  if (!filename) return;
  if (state.dirty && !confirm("当前草稿尚未保存，确认丢弃并打开其他方案？")) return;
  const result = await api(`/pipeline?filename=${encodeURIComponent(filename)}`);
  state.pipeline = result.pipeline; state.filename = result.filename; state.revision = result.revision; state.selected = "";
  await loadCatalog(state.pipeline.business_name); restorePositions(); setDirty(false); renderAll();
  $("#businessSelect").value = state.pipeline.business_name;
}

async function createPipeline() {
  if (state.dirty && !confirm("当前草稿尚未保存，确认新建？")) return;
  const business = $("#businessSelect").value, profile = $("#cloneProfile").value;
  const result = await write("/init", "POST", { business, profile, empty: !profile });
  state.pipeline = result.pipeline; state.filename = ""; state.revision = ""; state.selected = "";
  await loadCatalog(business); restorePositions(); setDirty(true); renderAll();
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
  const output = $("#validationOutput"); output.innerHTML = "校验中…";
  try {
    const report = await write("/validate", "POST", { pipeline: state.pipeline }, true);
    output.replaceChildren();
    if (report.ok) output.innerHTML = `<div class="diagnostic ok">校验通过 · ${report.plan.topological_order.length} 个节点 · ${report.plan.layers.length} 个波前</div>`;
    for (const item of report.diagnostics || []) {
      const block = document.createElement("div"); block.className = "diagnostic";
      block.innerHTML = `<strong>${item.code}</strong><br><code>${item.path}</code><br>${item.message}`;
      block.addEventListener("click", () => { if (item.node_id) { state.selected = item.node_id; switchTab("properties"); renderAll(); } });
      output.append(block);
    }
    return report.ok;
  } catch (error) { output.textContent = error.message; return false; }
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

function switchTab(name) {
  document.querySelectorAll(".tabs button").forEach(button => button.classList.toggle("active", button.dataset.tab === name));
  for (const tab of ["properties", "json", "validation", "run"]) $(`#${tab}Tab`).hidden = tab !== name;
}

$("#openButton").addEventListener("click", () => openPipeline($("#pipelineSelect").value).catch(error => toast(error.message, true)));
$("#newButton").addEventListener("click", () => createPipeline().catch(error => toast(error.message, true)));
$("#saveButton").addEventListener("click", () => save(false));
$("#saveAsButton").addEventListener("click", () => save(true));
$("#layoutButton").addEventListener("click", () => { graph.layout(effectiveNodes(), true); graph.render(effectiveNodes(), state.selected); });
$("#operatorSearch").addEventListener("input", renderOperators);
$("#businessSelect").addEventListener("change", filterProfiles);
$("#validateButton").addEventListener("click", validate);
$("#runButton").addEventListener("click", runDraft);
$("#cancelButton").addEventListener("click", async () => { if (state.jobId) await write(`/runs/${state.jobId}`, "DELETE", {}); });
document.querySelectorAll(".tabs button").forEach(button => button.addEventListener("click", () => switchTab(button.dataset.tab)));

$("#nodeForm").addEventListener("submit", async event => {
  event.preventDefault(); if (!await ensureExplicit()) return;
  const node = state.pipeline.pipeline.find(item => item.id === state.selected);
  const newId = $("#nodeId").value.trim();
  if (!newId || state.pipeline.pipeline.some(item => item !== node && item.id === newId)) return toast("节点 ID 为空或重复", true);
  try {
    const config = JSON.parse($("#configJson").value || "{}");
    for (const input of $("#configFields").querySelectorAll("[data-field]")) if (input.value !== "") config[input.dataset.field] = parseField(input);
    for (const item of state.pipeline.pipeline) item.depends_on = item.depends_on.map(id => id === node.id ? newId : id);
    node.id = newId; node.config = config; state.selected = newId; setDirty(true); renderAll();
  } catch (error) { toast(`配置 JSON 错误：${error.message}`, true); }
});

$("#deleteNode").addEventListener("click", async () => {
  if (!await ensureExplicit()) return;
  state.pipeline.pipeline = state.pipeline.pipeline.filter(node => node.id !== state.selected);
  for (const node of state.pipeline.pipeline) node.depends_on = node.depends_on.filter(id => id !== state.selected);
  delete graph.positions[state.selected]; state.selected = ""; setDirty(true); renderAll();
});

$("#applyJson").addEventListener("click", () => {
  try {
    const parsed = JSON.parse($("#rawJson").value);
    if (!parsed || !Array.isArray(parsed.pipeline)) throw new Error("pipeline 必须是数组");
    state.pipeline = parsed; state.selected = ""; setDirty(true); renderAll();
  } catch (error) { toast(`JSON 错误：${error.message}`, true); }
});

window.addEventListener("beforeunload", event => { if (state.dirty) { event.preventDefault(); event.returnValue = ""; } });

try {
  await refreshLists();
  if (initialPipeline) {
    $("#pipelineSelect").value = initialPipeline;
    await openPipeline(initialPipeline);
  }
} catch (error) { toast(error.message, true); }
