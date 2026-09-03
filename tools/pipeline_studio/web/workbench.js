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
