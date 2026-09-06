export function compatibleModels(models = [], modelDefinitions = [], nodeDefinition = null) {
  const requiredCapability = nodeDefinition?.model_capability;
  if (!requiredCapability) return [...models];

  const capabilityByType = new Map(
    modelDefinitions.map(definition => [definition.model_type, definition.capability])
  );
  return models.filter(model => {
    const capability = capabilityByType.get(model.model_type) || model.capability;
    return capability === requiredCapability;
  });
}

export function modelBoundNodeIds(nodes = [], nodeDefinitions = []) {
  const definitionByType = new Map(
    nodeDefinitions.map(definition => [definition.node_type, definition])
  );
  const result = new Set();
  for (const node of nodes) {
    const field = definitionByType.get(node.node_type)?.model_config_field;
    const modelId = field ? node.config?.[field] : undefined;
    if (typeof modelId === "string" && modelId.length > 0) result.add(node.id);
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

export function graphDocument(pipeline, catalog) {
  const nodes = pipeline?.pipeline || [];
  const business = catalog.bizs?.find(biz => biz.biz_name === pipeline?.biz_name);
  const definitions = {};
  for (const node of nodes) definitions[node.id] = catalog.nodes?.find(def => def.node_type === node.node_type) || {};
  definitions[INGRESS] = { outputs: business?.ingress || [], inputs: [] };
  definitions[EGRESS] = { inputs: business?.egress || [], outputs: [] };
  const producers = new Map((business?.ingress || []).map(port => [port.key, { source: INGRESS, sourcePort: port.key }]));
  for (const node of nodes) for (const port of definitions[node.id].outputs || []) {
    producers.set(node.ports?.outputs?.[port.key] || port.key, { source: node.id, sourcePort: port.key });
  }
  const edges = [];
  for (const node of nodes) {
    for (const port of definitions[node.id].inputs || []) {
      const key = node.ports?.inputs?.[port.key] || (port.required ? port.key : null);
      if (key && producers.has(key)) edges.push({ ...producers.get(key), target: node.id, targetPort: port.key });
    }
    for (const source of node.depends_on || []) {
      if (!edges.some(edge => edge.source === source && edge.target === node.id)) edges.push({ source, target: node.id, dependency: true });
    }
  }
  for (const port of business?.egress || []) {
    if (producers.has(port.key)) edges.push({ ...producers.get(port.key), target: EGRESS, targetPort: port.key });
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

export function connectPorts(pipeline, catalog, source, sourcePort, target, targetPort) {
  const graph = graphDocument(pipeline, catalog);
  const output = graph.definitions[source]?.outputs?.find(port => port.key === sourcePort);
  const input = graph.definitions[target]?.inputs?.find(port => port.key === targetPort);
  if (!output || !input || output.type_id !== input.type_id) throw new Error("端口不存在或数据类型不兼容");
  const byId = new Map(pipeline.pipeline.map(node => [node.id, node]));
  const pending = [source], seen = new Set();
  while (pending.length) {
    const id = pending.pop();
    if (id === target) throw new Error("连线会形成环");
    if (seen.has(id)) continue;
    seen.add(id); pending.push(...(byId.get(id)?.depends_on || []));
  }
  const sourceNode = byId.get(source), targetNode = byId.get(target);
  let key = sourceNode?.ports?.outputs?.[sourcePort] || sourcePort;
  if (target === EGRESS) {
    if (!sourceNode) throw new Error("业务输出需要节点产出");
    const conflict = graph.edges.find(edge => edge.target === EGRESS && edge.targetPort === targetPort && (edge.source !== source || edge.sourcePort !== sourcePort));
    if (conflict) throw new Error("业务输出已有生产者，请先断开原连线");
    key = targetPort;
    sourceNode.ports ??= {}; sourceNode.ports.outputs ??= {};
    sourceNode.ports.outputs[sourcePort] = key;
    for (const edge of graph.edges.filter(edge => edge.source === source && edge.sourcePort === sourcePort && edge.target !== EGRESS)) {
      const consumer = byId.get(edge.target);
      consumer.ports ??= {}; consumer.ports.inputs ??= {};
      consumer.ports.inputs[edge.targetPort] = key;
    }
  } else {
    if (!targetNode) throw new Error("请选择节点输入端口");
    targetNode.ports ??= {}; targetNode.ports.inputs ??= {};
    targetNode.ports.inputs[targetPort] = key;
    targetNode.depends_on ??= [];
    if (sourceNode && !targetNode.depends_on.includes(source)) targetNode.depends_on.push(source);
  }
}

export function disconnectPorts(pipeline, catalog, edge) {
  const node = pipeline.pipeline.find(item => item.id === edge.target);
  if (edge.target === EGRESS) {
    const source = pipeline.pipeline.find(item => item.id === edge.source);
    if (!source) return;
    const previous = source.ports?.outputs?.[edge.sourcePort] || edge.sourcePort;
    source.ports ??= {}; source.ports.outputs ??= {};
    const key = `${source.id}__${edge.sourcePort}`;
    source.ports.outputs[edge.sourcePort] = key;
    for (const consumer of pipeline.pipeline) for (const [port, value] of Object.entries(consumer.ports?.inputs || {})) {
      if (value === previous) consumer.ports.inputs[port] = key;
    }
  } else if (node) {
    if (!edge.dependency) {
      const required = graphDocument(pipeline, catalog).definitions[node.id]?.inputs?.find(port => port.key === edge.targetPort)?.required;
      node.ports ??= {}; node.ports.inputs ??= {};
      if (required) node.ports.inputs[edge.targetPort] = `${node.id}__unconnected__${edge.targetPort}`;
      else delete node.ports.inputs[edge.targetPort];
    }
    const stillConnected = graphDocument(pipeline, catalog).edges.some(item => !item.dependency && item.source === edge.source && item.target === edge.target);
    if (!stillConnected) node.depends_on = (node.depends_on || []).filter(id => id !== edge.source);
  }
}

export function removeNode(pipeline, catalog, id) {
  const edges = graphDocument(pipeline, catalog).edges.filter(edge => edge.source === id && edge.target !== EGRESS);
  for (const edge of edges) disconnectPorts(pipeline, catalog, edge);
  pipeline.pipeline = pipeline.pipeline.filter(node => node.id !== id);
  for (const node of pipeline.pipeline) node.depends_on = (node.depends_on || []).filter(dep => dep !== id);
}

export function compatibleBackends(backends, modelDefinition) {
  return backends.filter(backend => backend.supported_protocols.includes(modelDefinition?.required_protocol));
}

export function schemaDefaults(fields = []) {
  return Object.fromEntries(fields.filter(field => field.default !== undefined).map(field => [field.name, structuredClone(field.default)]));
}

export function upsertModel(pipeline, catalog, previousId, model) {
  const definition = catalog.models.find(item => item.model_type === model.model_type);
  if (!model.model_id?.trim() || !model.model_path?.trim() || !definition) throw new Error("请填写模型 ID、资产路径并选择模型类型");
  if (!compatibleBackends(catalog.backends, definition).some(item => item.backend_type === model.backend)) throw new Error("Backend 与模型协议不兼容");
  if (pipeline.models.some(item => item.model_id === model.model_id && item.model_id !== previousId)) throw new Error("模型 ID 重复");
  for (const node of pipeline.pipeline) {
    const nodeDefinition = catalog.nodes.find(item => item.node_type === node.node_type);
    if (previousId && node.config?.[nodeDefinition?.model_config_field] === previousId && nodeDefinition.model_capability !== definition.capability) throw new Error("所选模型能力与引用节点不兼容");
  }
  model.capability = definition.capability;
  const index = pipeline.models.findIndex(item => item.model_id === previousId);
  if (index < 0) pipeline.models.push(model); else pipeline.models[index] = model;
  if (previousId && previousId !== model.model_id) for (const node of pipeline.pipeline) {
    const field = catalog.nodes.find(item => item.node_type === node.node_type)?.model_config_field;
    if (field && node.config?.[field] === previousId) node.config[field] = model.model_id;
  }
}

export function removeModel(pipeline, catalog, id) {
  const used = pipeline.pipeline.some(node => {
    const field = catalog.nodes.find(item => item.node_type === node.node_type)?.model_config_field;
    return field && node.config?.[field] === id;
  });
  if (used) throw new Error("模型仍被节点使用，请先更换绑定");
  pipeline.models = pipeline.models.filter(model => model.model_id !== id);
}
