// 文档历史只保存已应用的编辑。表单缓冲在 Apply 之前独立存放，
// 因此选择、校验和重绘都不会丢弃用户输入。
export function createHistory(limit = 50) {
  let entries = [], cursor = -1;
  const copy = value => structuredClone(value);
  return {
    reset(value) { entries = [copy(value)]; cursor = 0; },
    record(value) {
      if (cursor >= 0 && JSON.stringify(entries[cursor]) === JSON.stringify(value)) return;
      entries = entries.slice(0, cursor + 1);
      entries.push(copy(value));
      if (entries.length > limit) entries.shift();
      cursor = entries.length - 1;
    },
    undo() { return cursor > 0 ? copy(entries[--cursor]) : null; },
    redo() { return cursor + 1 < entries.length ? copy(entries[++cursor]) : null; },
    get canUndo() { return cursor > 0; },
    get canRedo() { return cursor + 1 < entries.length; },
  };
}

export function createDrafts() {
  const values = new Map();
  return {
    set(kind, value) { values.set(kind, structuredClone(value)); },
    get(kind) { return values.get(kind); },
    has(kind) { return values.has(kind); },
    clear(kind) { if (kind) values.delete(kind); else values.clear(); },
    pendingExcept(kind) { return [...values.keys()].filter(key => key !== kind); },
    get pending() { return values.size > 0; },
  };
}

// Validator 文本可能包含用户提供的标识符和值。按纯文本保留，
// 同时保留诊断卡片提供的 Node 导航。
export function appendDiagnostic(container, item, selectNode, onPreviewFix) {
  const block = document.createElement("div"); block.className = "diagnostic";
  const appendText = (tag, value) => {
    const element = document.createElement(tag); element.textContent = value;
    block.append(element);
  };
  appendText("strong", item.code);
  appendText("div", item.path);
  appendText("p", item.message);
  if (item.node_id) appendText("div", `节点：${item.node_id}`);
  if (item.port) appendText("div", `端口：${item.port}`);
  if (item.related_nodes?.length) appendText("div", `相关节点：${item.related_nodes.join("、")}`);
  if (item.remediation) {
    const rem = item.remediation;
    if (rem.summary) appendText("div", `原因诊断：${rem.summary}`);
    if (rem.schema_version === 1 && Array.isArray(rem.fixes) && rem.fixes.length > 0) {
      const fixesTitle = document.createElement("div");
      fixesTitle.className = "fixes-title";
      fixesTitle.textContent = "可选修复操作：";
      block.append(fixesTitle);
      for (const fix of rem.fixes) {
        const fixItem = document.createElement("div");
        fixItem.className = "fix-candidate";
        const fixDesc = document.createElement("span");
        const badge = fix.verification === "pipeline_valid" ? "【通过验证】" : "【已解决该错误】";
        fixDesc.textContent = `${badge} ${fix.title}：${fix.effect}`;
        fixItem.append(fixDesc);
        if (onPreviewFix) {
          const btn = document.createElement("button");
          btn.type = "button";
          btn.className = "fix-apply-btn";
          btn.textContent = "预览并应用";
          btn.style.marginLeft = "8px";
          btn.addEventListener("click", (e) => {
            e.stopPropagation();
            onPreviewFix(fix);
          });
          fixItem.append(btn);
        }
        block.append(fixItem);
      }
    }
  }
  if (item.suggestions?.length) {
    appendText("div", "修复建议：");
    const list = document.createElement("ul");
    for (const suggestion of item.suggestions) {
      const entry = document.createElement("li"); entry.textContent = suggestion; list.append(entry);
    }
    block.append(list);
  }
  if (item.node_id) block.addEventListener("click", () => selectNode(item.node_id));
  container.append(block);
}

// Node、Model 和 Backend 参数共用同一个配置字段编辑器。
export function appendConfigField(container, field, values, modelChoices = null) {
  const label = document.createElement("label"); label.textContent = field.name;
  const hasDefault = field.default !== undefined && field.default !== null;
  let input;
  if (modelChoices !== null || (Array.isArray(field.enum) && field.enum.length)) {
    input = document.createElement("select");
    for (const value of modelChoices ?? field.enum) input.add(new Option(value, value));
  } else if (field.type === "boolean") {
    input = document.createElement("select"); input.add(new Option("true", "true")); input.add(new Option("false", "false"));
  } else if (["string", "object", "array"].includes(field.type)) {
    input = document.createElement("textarea"); input.rows = 3;
  } else {
    input = document.createElement("input"); input.type = "number";
    if (field.minimum !== undefined) input.min = field.minimum;
    if (field.maximum !== undefined) input.max = field.maximum;
    input.step = field.type === "integer" ? "1" : "any";
  }
  if (input.tagName === "SELECT" && !field.required && modelChoices === null) {
    const option = new Option(hasDefault ? `默认（${field.default}）` : "未设置", "");
    input.add(option, 0);
    input.dataset.unsetOption = "true";
  }
  input.dataset.field = field.name; input.dataset.type = field.type;
  // Definition.required 关注字段是否存在；必填字符串可以为空。
  input.required = Boolean(field.required) && field.type !== "string";
  const present = Object.hasOwn(values, field.name);
  const value = present ? values[field.name] : field.default;
  const text = typeof value === "object" ? JSON.stringify(value) : String(value ?? "");
  input.value = text;
  if (!present) {
    if (input.dataset.unsetOption === "true") input.value = "";
    else if (["number", "integer", "array", "object"].includes(field.type)) {
      input.value = "";
      input.placeholder = hasDefault ? (["array", "object"].includes(field.type) ? text : `默认 ${text}`) : "";
    }
  }
  if (field.type === "string" && input.tagName === "TEXTAREA") {
    // 浏览器会规范化 textarea.value 中的 CR/CRLF。应用未改动的字段时
    // 保留原始字符串，草稿重绘后也一样。
    if (present || field.required) input.dataset.originalValue = text;
    input.dataset.displayValue = input.value;
    input.rows = Math.min(4, input.value.split("\n").length);
  }
  label.append(input);
  if (field.semantic && field.semantic !== "model_ref") {
    const help = document.createElement("small"); help.className = "field-help";
    help.textContent = field.semantic; label.append(help);
  }
  container.append(label);
}

function parseField(input) {
  if (["integer", "number"].includes(input.dataset.type) && input.value.trim() === "") throw new Error("请输入数值");
  if (input.dataset.type === "integer") {
    const value = Number(input.value);
    if (!Number.isSafeInteger(value)) throw new Error("请输入有效整数");
    return value;
  }
  if (input.dataset.type === "number") return Number(input.value);
  if (input.dataset.type === "boolean") return input.value === "true";
  if (input.dataset.type === "object" || input.dataset.type === "array") return JSON.parse(input.value);
  return input.value === input.dataset.displayValue ? input.dataset.originalValue : input.value;
}

export function readConfigFields(container) {
  const config = {};
  for (const input of container.querySelectorAll("[data-field]")) {
    if (input.dataset.unsetOption === "true" && input.value === "") continue;
    if (input.value === input.dataset.displayValue && input.dataset.originalValue === undefined) continue;
    try {
      if (input.dataset.type === "string" || input.value !== "" || input.required) config[input.dataset.field] = parseField(input);
    } catch (error) {
      const message = `${input.dataset.field}：${error.message}`;
      input.setCustomValidity?.(message); input.reportValidity?.(); input.focus?.();
      throw new Error(message);
    }
  }
  return config;
}

function bufferKey(input) {
  const scope = input.closest("#backendConfigFields") ? "backend" : "config";
  return input.id || `${scope}.${input.dataset.field}`;
}

export function readFormBuffer(form) {
  return Object.fromEntries([...form.querySelectorAll("input, select, textarea")]
    .filter(input => input.id || input.dataset.field)
    .map(input => [bufferKey(input), input.value]));
}

export function restoreFormBuffer(form, buffer) {
  for (const input of form.querySelectorAll("input, select, textarea")) {
    if (Object.hasOwn(buffer, bufferKey(input))) input.value = buffer[bufferKey(input)];
  }
}
