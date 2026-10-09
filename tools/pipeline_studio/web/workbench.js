export function compatibleModels(models = [], modelDefinitions = [], target = null) {
  const modelType = typeof target === "string" ? target : target?.model_type || target?.model_dependencies?.[0]?.model_type;
  return modelType ? models.filter(model => model.type === modelType) : [...models];
}

export function modelBoundNodeIds(nodes = [], nodeDefinitions = []) {
  const definitionByType = new Map(nodeDefinitions.map(definition => [definition.node_type, definition]));
  const result = new Set();
  for (const node of nodes) {
    const definition = definitionByType.get(node.type);
    if ((definition?.model_dependencies || []).some(dependency => typeof node.params?.[dependency.config_field] === "string" && node.params[dependency.config_field].length > 0)) result.add(node.name);
  }
  return result;
}

export function createLatestRequestGate() {
  let generation = 0;
  return {
    invalidate() { generation += 1; },
    async run(load, commit) {
      const requestGeneration = ++generation;
      try {
        const value = await load();
        if (requestGeneration !== generation) return false;
        commit(value);
        return true;
      } catch (error) {
        if (requestGeneration !== generation) return false;
        throw error;
      }
    },
  };
}

export const INGRESS = "input";
export const EGRESS = "output";

export function pipelineIo(pipeline) {
  const pairs = direction => (pipeline?.io?.[direction] || []).map(({type, name}) => ({type, name}));
  return {input: pairs("input"), output: pairs("output")};
}

export function ioLabel(io) {
  const label = direction => (io?.[direction] || []).map(entry => `${entry.type}/${entry.name}`).join(", ") || "未选择";
  return `${label("input")} → ${label("output")}`;
}

export function graphDocument(pipeline, catalog) {
  const nodes = pipeline?.pipeline || [];
  const definitions = {};
  for (const node of nodes) definitions[node.name] = catalog.nodes?.find(definition => definition.node_type === node.type) || {};
  const ports = direction => {
    const selected = (pipeline?.io?.[direction] || []).flatMap(entry => catalog[`${direction}_converters`]?.find(converter => converter.type === entry.type && converter.name === entry.name)?.logical_ports || []);
    return [...new Map(selected.map(port => [port.key, port])).values()];
  };
  definitions[INGRESS] = {outputs: ports("input"), inputs: []};
  definitions[EGRESS] = {inputs: ports("output"), outputs: []};
  const edges = [];
  const addReference = (reference, target, targetPort) => {
    if (typeof reference !== "string") return;
    const parts = reference.split(".");
    if (parts.length === 2 && parts.every(Boolean)) edges.push({source: parts[0], sourcePort: parts[1], target, targetPort});
  };
  for (const node of nodes) {
    for (const [port, reference] of Object.entries(node.inputs || {})) addReference(reference, node.name, port);
    for (const source of node.depends_on || []) {
      if (!edges.some(edge => edge.source === source && edge.target === node.name)) edges.push({source, target: node.name, dependency: true});
    }
  }
  for (const entry of pipeline?.io?.output || []) {
    for (const [port, reference] of Object.entries(entry.inputs || {})) addReference(reference, EGRESS, port);
  }
  return {
    definitions, edges,
    nodes: pipeline ? [
      {id: INGRESS, node_type: "业务输入", depends_on: []},
      ...nodes.map(node => ({id: node.name, node_type: node.type, depends_on: [...new Set([...(node.depends_on || []), ...edges.filter(edge => edge.target === node.name).map(edge => edge.source)])]})),
      {id: EGRESS, node_type: "业务输出", depends_on: [...new Set(edges.filter(edge => edge.target === EGRESS).map(edge => edge.source))]},
    ] : [],
  };
}

export function compatibleBackends(backends = [], modelDefinition) {
  return backends.filter(backend => modelDefinition?.backends?.includes(backend.backend_type));
}

export function modelAvailability(backends, modelDefinition) {
  if (!modelDefinition) return {available: false, message: "当前构建未注册此模型类别"};
  const available = compatibleBackends(backends, modelDefinition).length > 0;
  return {available, message: available ? "" : "当前构建无兼容 Backend。可继续浏览；运行前请选择兼容模型或切换构建。"};
}

// 只检查查看器使用的容器结构。名字、端口、字段、图合法性和模型兼容性
// 仍由 Catalog/Validator 负责。
export function assertBrowsablePipeline(pipeline) {
  const object = value => value !== null && typeof value === "object" && !Array.isArray(value);
  if (!object(pipeline) || !Array.isArray(pipeline.pipeline)) throw new Error("方案必须是包含 pipeline 数组的 JSON 对象");
  if (pipeline.pipeline.some(node => !object(node) || (node.depends_on != null && !Array.isArray(node.depends_on)))) throw new Error("pipeline 节点必须是对象，depends_on 必须是数组");
  if (pipeline.models != null && (!Array.isArray(pipeline.models) || pipeline.models.some(model => !object(model)))) throw new Error("models 必须是模型对象数组");
  if (pipeline.io != null && (!object(pipeline.io) || ["input", "output"].some(direction => pipeline.io[direction] != null && (!Array.isArray(pipeline.io[direction]) || pipeline.io[direction].some(entry => !object(entry)))))) throw new Error("io.input 与 io.output 必须是登记对象数组");
}

export async function readPipelineFile(file) {
  if (!file || !file.name.toLowerCase().endsWith(".json")) throw new Error("请选择一个 Pipeline JSON 文件");
  if (file.size > 4 * 1024 * 1024) throw new Error("方案文件超过 4 MiB");
  const pipeline = JSON.parse((await file.text()).replace(/^\uFEFF/, ""));
  assertBrowsablePipeline(pipeline);
  return { pipeline, filename: file.name, revision: "", imported: true };
}

export function schemaDefaults(fields = []) {
  return Object.fromEntries(fields.filter(field => field.default !== undefined).map(field => [field.name, structuredClone(field.default)]));
}

export function upsertModel(pipeline, catalog, previousName, model) {
  const definition = catalog.models.find(item => item.model_type === model.type && item.backends?.includes(model.backend?.type));
  if (!model.name?.trim() || !model.file?.trim() || !definition) throw new Error("请填写模型名、文件名并选择模型类别与 Backend");
  const availability = modelAvailability(catalog.backends, definition);
  if (!availability.available) throw new Error(availability.message);
  if (pipeline.models.some(item => item.name === model.name && item.name !== previousName)) throw new Error("模型名重复");
  for (const node of pipeline.pipeline) {
    const nodeDefinition = catalog.nodes.find(item => item.node_type === node.type);
    for (const dependency of nodeDefinition?.model_dependencies || []) {
      if (previousName && node.params?.[dependency.config_field] === previousName && dependency.model_type !== model.type) throw new Error("所选模型类别与引用节点不兼容");
    }
  }
  const index = pipeline.models.findIndex(item => item.name === previousName);
  if (index < 0) pipeline.models.push(model); else pipeline.models[index] = model;
  if (previousName && previousName !== model.name) {
    for (const node of pipeline.pipeline) {
      const definition = catalog.nodes.find(item => item.node_type === node.type);
      for (const dependency of definition?.model_dependencies || []) {
        if (node.params?.[dependency.config_field] === previousName) node.params[dependency.config_field] = model.name;
      }
    }
  }
}

export function removeModel(pipeline, catalog, name) {
  const used = pipeline.pipeline.some(node => {
    const definition = catalog.nodes.find(item => item.node_type === node.type);
    return (definition?.model_dependencies || []).some(dependency => node.params?.[dependency.config_field] === name);
  });
  if (used) throw new Error("模型仍被节点使用，请先更换绑定");
  pipeline.models = pipeline.models.filter(model => model.name !== name);
}

export function assetModel(asset) {
  const model = structuredClone(asset.model);
  for (const [pointer, path] of Object.entries(asset.paths)) {
    const parts = pointer.slice(1).split("/").map(part => part.replace(/~1/g, "/").replace(/~0/g, "~"));
    const field = parts.pop();
    let target = model;
    for (const part of parts) target = target[part];
    target[field] = path;
  }
  return model;
}
