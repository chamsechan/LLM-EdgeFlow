export function compatibleModels(models = [], modelDefinitions = [], target = null) {
  let requiredCapability = null;
  if (typeof target === "string") {
    requiredCapability = target;
  } else if (target && typeof target === "object") {
    if (target.capability) {
      requiredCapability = target.capability;
    } else if (Array.isArray(target.model_dependencies) && target.model_dependencies.length > 0) {
      requiredCapability = target.model_dependencies[0].capability;
    }
  }
  if (!requiredCapability) return [...models];

  const capabilityByType = new Map(
    modelDefinitions.map(definition => [definition.model_type, definition.capability])
  );
  return models.filter(model => {
    const capability = capabilityByType.get(model.model_type);
    return capability === requiredCapability;
  });
}

export function modelBoundNodeIds(nodes = [], nodeDefinitions = []) {
  const definitionByType = new Map(
    nodeDefinitions.map(definition => [definition.node_type, definition])
  );
  const result = new Set();
  for (const node of nodes) {
    const def = definitionByType.get(node.node_type);
    if (!def) continue;
    const fields = (def.model_dependencies || []).map(d => d.config_field);
    for (const field of fields) {
      const modelId = node.config?.[field];
      if (typeof modelId === "string" && modelId.length > 0) {
        result.add(node.id);
        break;
      }
    }
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

export const INGRESS = "$ingress";
export const EGRESS = "$egress";

export function pipelineBinding(pipeline) {
  return pipeline?.deployment?.io?.io_binding || "";
}

export function graphDocument(pipeline, catalog) {
  const nodes = pipeline?.pipeline || [];
  const binding = catalog.io_bindings?.find(binding => binding.binding_id === pipelineBinding(pipeline));
  const biz = catalog.bizs?.find(biz => biz.biz_name === binding?.biz_name);
  const definitions = {};
  for (const node of nodes) definitions[node.id] = catalog.nodes?.find(def => def.node_type === node.node_type) || {};
  definitions[INGRESS] = { outputs: biz?.ingress || [], inputs: [] };
  definitions[EGRESS] = { inputs: biz?.egress || [], outputs: [] };
  const producerMap = new Map();
  const addProducer = (key, prod) => {
    if (!key) return;
    const list = producerMap.get(key) || [];
    list.push(prod);
    producerMap.set(key, list);
  };
  for (const port of biz?.ingress || []) {
    addProducer(port.key, { source: INGRESS, sourcePort: port.key });
  }
  for (const node of nodes) {
    for (const port of definitions[node.id]?.outputs || []) {
      const key = node.outputs?.[port.key] || port.key;
      addProducer(key, { source: node.id, sourcePort: port.key });
    }
  }
  const edges = [];
  for (const node of nodes) {
    for (const port of definitions[node.id]?.inputs || []) {
      const key = node.inputs?.[port.key];
      if (key && producerMap.has(key)) {
        const prods = producerMap.get(key);
        if (prods.length === 1) {
          edges.push({ ...prods[0], target: node.id, targetPort: port.key });
        } else {
          for (const prod of prods) {
            edges.push({ ...prod, target: node.id, targetPort: port.key, ambiguous: true });
          }
        }
      }
    }
    for (const source of node.depends_on || []) {
      if (!edges.some(edge => edge.source === source && edge.target === node.id)) {
        edges.push({ source, target: node.id, dependency: true });
      }
    }
  }
  for (const port of biz?.egress || []) {
    if (producerMap.has(port.key)) {
      const prods = producerMap.get(port.key);
      if (prods.length === 1) {
        edges.push({ ...prods[0], target: EGRESS, targetPort: port.key });
      } else {
        for (const prod of prods) {
          edges.push({ ...prod, target: EGRESS, targetPort: port.key, ambiguous: true });
        }
      }
    }
  }
  return {
    definitions, edges,
    nodes: pipeline ? [
      { id: INGRESS, node_type: "业务输入", depends_on: [] },
      ...nodes.map(node => ({ ...node, depends_on: [...new Set([...(node.depends_on || []), ...edges.filter(edge => edge.target === node.id).map(edge => edge.source)])] })),
      { id: EGRESS, node_type: "业务输出", depends_on: [...new Set(edges.filter(edge => edge.target === EGRESS).map(edge => edge.source))] },
    ] : [],
  };
}

export function compatibleBackends(backends, modelDefinition) {
  return backends.filter(backend => backend.supported_protocols.includes(modelDefinition?.required_protocol));
}

export function modelAvailability(backends, modelDefinition) {
  if (!modelDefinition) return { available: false, message: "当前构建未注册此模型类型" };
  const available = compatibleBackends(backends, modelDefinition).length > 0;
  return {
    available,
    message: available ? "" : `当前构建无兼容 Backend（需要 ${modelDefinition.required_protocol} 协议）。可继续浏览；运行前请选择兼容模型或切换构建。`,
  };
}

// Check only the container shapes consumed by the viewer. Catalog/Validator
// still owns IDs, ports, fields, graph legality and model compatibility.
export function assertBrowsablePipeline(pipeline) {
  const object = value => value !== null && typeof value === "object" && !Array.isArray(value);
  if (!object(pipeline) || !Array.isArray(pipeline.pipeline)) throw new Error("方案必须是包含 pipeline 数组的 JSON 对象");
  if (pipeline.pipeline.some(node => !object(node) || (node.depends_on != null && !Array.isArray(node.depends_on)))) throw new Error("pipeline 节点必须是对象，depends_on 必须是数组");
  if (pipeline.models != null && (!Array.isArray(pipeline.models) || pipeline.models.some(model => !object(model)))) throw new Error("models 必须是模型对象数组");
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

export function upsertModel(pipeline, catalog, previousId, model) {
  const definition = catalog.models.find(item => item.model_type === model.model_type);
  if (!model.model_id?.trim() || !model.model_path?.trim() || !definition) throw new Error("请填写模型 ID、资产路径并选择模型类型");
  const availability = modelAvailability(catalog.backends, definition);
  if (!availability.available) throw new Error(availability.message);
  if (!compatibleBackends(catalog.backends, definition).some(item => item.backend_type === model.backend)) throw new Error("Backend 与模型协议不兼容");
  if (pipeline.models.some(item => item.model_id === model.model_id && item.model_id !== previousId)) throw new Error("模型 ID 重复");
  for (const node of pipeline.pipeline) {
    const nodeDefinition = catalog.nodes.find(item => item.node_type === node.node_type);
    if (previousId && nodeDefinition) {
      const deps = nodeDefinition.model_dependencies || [];
      for (const dep of deps) {
        if (node.config?.[dep.config_field] === previousId && dep.capability !== definition.capability) {
          throw new Error("所选模型能力与引用节点不兼容");
        }
      }
    }
  }
  const index = pipeline.models.findIndex(item => item.model_id === previousId);
  if (index < 0) pipeline.models.push(model); else pipeline.models[index] = model;
  if (previousId && previousId !== model.model_id) {
    for (const node of pipeline.pipeline) {
      const nodeDefinition = catalog.nodes.find(item => item.node_type === node.node_type);
      if (nodeDefinition && node.config) {
        const deps = nodeDefinition.model_dependencies || [];
        for (const dep of deps) {
          if (node.config[dep.config_field] === previousId) {
            node.config[dep.config_field] = model.model_id;
          }
        }
      }
    }
  }
}

export function removeModel(pipeline, catalog, id) {
  const used = pipeline.pipeline.some(node => {
    const nodeDefinition = catalog.nodes.find(item => item.node_type === node.node_type);
    if (!nodeDefinition || !node.config) return false;
    const deps = nodeDefinition.model_dependencies || [];
    return deps.some(dep => node.config[dep.config_field] === id);
  });
  if (used) throw new Error("模型仍被节点使用，请先更换绑定");
  pipeline.models = pipeline.models.filter(model => model.model_id !== id);
}

export function assetModelPath(path, assetRoot = "models") {
  const root = assetRoot.replace(/\/+$/, "");
  return root && root !== "." ? `${root}/${path}` : path;
}

export function assetModel(asset, assetRoot = "models") {
  const model = structuredClone(asset.model);
  const modelPath = asset.paths["/model_path"];
  const directory = modelPath.includes("/") ? modelPath.slice(0, modelPath.lastIndexOf("/") + 1) : "";
  model.model_path = assetModelPath(modelPath, assetRoot);
  for (const [pointer, path] of Object.entries(asset.paths)) {
    if (pointer === "/model_path") continue;
    if (!path.startsWith(directory)) {
      throw new Error("资产附属文件不在模型目录内，请在模型表单中填写可用的绝对附属文件路径");
    }
    const parts = pointer.slice(1).split("/").map(part => part.replace(/~1/g, "/").replace(/~0/g, "~"));
    const field = parts.pop();
    let target = model;
    for (const part of parts) target = target[part];
    target[field] = path.slice(directory.length);
  }
  return model;
}
