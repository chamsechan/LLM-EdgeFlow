// 文档历史只保存完整的编辑。尚未完成的表单输入独立存放，
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
  if (item.node_name) appendText("div", `节点：${item.node_name}`);
  if (item.port) appendText("div", `端口：${item.port}`);
  if (item.related_nodes?.length) appendText("div", `相关节点：${item.related_nodes.join("、")}`);
  if (item.remediation) {
    const rem = item.remediation;
    if (rem.summary) appendText("div", `原因诊断：${rem.summary}`);
    if (Array.isArray(rem.fixes) && rem.fixes.length > 0) {
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
          btn.textContent = "查看修复方案";
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
  if (item.node_name) block.addEventListener("click", () => selectNode(item.node_name));
  container.append(block);
}

// Node、Model 和 Backend 参数共用同一个配置字段编辑器。
export function appendConfigField(container, field, values, modelChoices = null) {
  const label = document.createElement("label"); label.textContent = field.name;
  const hasDefault = field.default !== undefined && field.default !== null;
  let input;
  if (modelChoices !== null || (Array.isArray(field.enum) && field.enum.length && !["object", "array", "map", "json"].includes(field.type))) {
    input = document.createElement("select");
    for (const value of modelChoices ?? field.enum) input.add(new Option(value, value));
  } else if (field.type === "boolean") {
    input = document.createElement("select"); input.add(new Option("true", "true")); input.add(new Option("false", "false"));
  } else if (["string", "object", "array", "map", "json"].includes(field.type)) {
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
  const text = ["object", "array", "map", "json"].includes(field.type) ? (value === undefined ? "" : JSON.stringify(value)) : String(value ?? "");
  input.value = text;
  if (!present) {
    if (input.dataset.unsetOption === "true") input.value = "";
    else if (["number", "integer", "array", "object", "map", "json"].includes(field.type)) {
      input.value = "";
      input.placeholder = hasDefault ? (["array", "object", "map", "json"].includes(field.type) ? text : `默认 ${text}`) : "";
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
  if (field.file || field.semantic) {
    const help = document.createElement("small"); help.className = "field-help";
    help.textContent = field.file ? "相对方案所在目录的文件名" : field.semantic; label.append(help);
  }
  const collection = collectionEditor(field, input);
  if (collection) {
    container.append(collection);
    const details = document.createElement("details"), summary = document.createElement("summary");
    summary.textContent = `${field.name} · 编辑 JSON`;
    details.append(summary, label); container.append(details);
  } else container.append(label);
}

// 只为两种常见集合形状提供表单，形状来自 Definition 的 items 声明；
// 其他形状继续使用 JSON 编辑，不另建一套 schema 引擎。
function collectionEditor(field, raw) {
  const map = field.type === "map" && field.items?.type === "array" && field.items.items?.type === "string";
  const array = field.type === "array" && field.items?.type === "object" && field.items.fields?.length;
  if (!map && !array) return null;
  const box = document.createElement("fieldset"); box.dataset.collection = field.name;
  const legend = document.createElement("legend"); legend.textContent = field.name;
  const rows = document.createElement("div"), add = document.createElement("button");
  add.type = "button"; add.textContent = "＋ 添加一项";
  box.append(legend, rows, add);
  let emitting = false;
  const emit = (complete = true) => {
    emitting = true;
    raw.dispatchEvent(new Event("input", {bubbles: true}));
    if (complete) raw.dispatchEvent(new Event("change", {bubbles: true}));
    emitting = false;
  };
  const read = () => JSON.parse(raw.value || raw.placeholder || (map ? "{}" : "[]"));
  const render = () => {
    let value;
    try { value = read(); } catch { box.hidden = true; return; }
    if (map ? !value || Array.isArray(value) || typeof value !== "object" || Object.values(value).some(words => !Array.isArray(words) || words.some(word => typeof word !== "string" || /[\r\n]/.test(word))) : !Array.isArray(value) || value.some(item => !item || typeof item !== "object" || Array.isArray(item))) {
      box.hidden = true; return;
    }
    box.hidden = false; rows.replaceChildren();
    for (const [key, item] of Object.entries(value)) {
      const row = document.createElement("div"); row.className = "collection-row"; row.dataset.collectionRow = "true";
      const remove = document.createElement("button"); remove.type = "button"; remove.textContent = "删除此项";
      const commit = (complete = true) => { raw.value = JSON.stringify(value); emit(complete); };
      if (map) {
        let currentKey = key;
        const name = document.createElement("input"), words = document.createElement("textarea");
        name.value = key; name.setAttribute("aria-label", "键");
        name.dataset.buffer = `${field.name}.${rows.children.length}.name`;
        words.dataset.buffer = `${field.name}.${rows.children.length}.words`;
        words.value = item.join("\n"); words.rows = Math.min(5, Math.max(2, item.length)); words.setAttribute("aria-label", "值，每行一个");
        const hint = document.createElement("small"); hint.textContent = "值，每行一个";
        const rename = (complete = false) => {
          if (!name.value || name.value !== currentKey && Object.hasOwn(value, name.value)) {
            name.setCustomValidity("键不能为空或重复"); if (complete) name.reportValidity(); return;
          }
          name.setCustomValidity("");
          const entries = Object.entries(value).map(([k, v]) => [k === currentKey ? name.value : k, v]);
          value = Object.fromEntries(entries); currentKey = name.value; commit(complete);
        };
        name.addEventListener("input", () => rename());
        name.addEventListener("change", () => rename(true));
        name.validateEditor = () => { name.setCustomValidity(!name.value || name.value !== currentKey && Object.hasOwn(value, name.value) ? "键不能为空或重复" : ""); };
        words.addEventListener("change", () => { value[currentKey] = words.value === "" ? [] : words.value.split("\n"); commit(); });
        words.addEventListener("input", () => { value[currentKey] = words.value === "" ? [] : words.value.split("\n"); commit(false); });
        remove.addEventListener("click", () => { delete value[currentKey]; commit(); render(); });
        row.append(name, hint, words);
      } else {
        const known = new Set(field.items.fields.map(child => child.name));
        for (const child of field.items.fields) appendConfigField(row, child, item);
        for (const input of row.querySelectorAll("[data-field]")) {
          input.dataset.buffer = `${field.name}.${key}.${input.dataset.field}`;
          input.validateEditor = () => { try { input.setCustomValidity(""); readConfigFields(row); } catch {} };
        }
        const update = event => {
          if (event.target === remove) return;
          event.target.setCustomValidity?.("");
          try {
            if ([...row.querySelectorAll("input, select, textarea")].some(input => !input.checkValidity())) return;
            value[key] = {...Object.fromEntries(Object.entries(item).filter(([name]) => !known.has(name))), ...readConfigFields(row)};
            commit(event.type === "change");
          } catch { /* The field renderer retains and locates invalid input. */ }
        };
        row.addEventListener("input", update);
        row.addEventListener("change", update);
        remove.addEventListener("click", () => { value.splice(Number(key), 1); commit(); render(); });
      }
      row.append(remove); rows.append(row);
    }
  };
  add.addEventListener("click", () => {
    const value = read();
    if (map) { let n = 1; while (Object.hasOwn(value, `key_${n}`)) n++; value[`key_${n}`] = []; }
    else value.push(Object.fromEntries(field.items.fields.filter(child => child.required && child.type === "string").map(child => [child.name, ""])));
    raw.value = JSON.stringify(value); emit(); render();
  });
  raw.refreshEditor = render;
  raw.addEventListener("change", () => { if (!emitting) render(); });
  render();
  return box;
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
  if (["object", "array", "map", "json"].includes(input.dataset.type)) return JSON.parse(input.value);
  return input.value === input.dataset.displayValue ? input.dataset.originalValue : input.value;
}

export function readConfigFields(container, original = {}) {
  const fields = [...container.querySelectorAll("[data-field]")].filter(input => !input.closest?.("[data-collection-row]") || container.dataset?.collectionRow);
  const known = new Set(fields.map(input => input.dataset.field));
  const config = Object.fromEntries(Object.entries(original).filter(([name]) => !known.has(name)));
  for (const input of fields) {
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
  const scope = input.closest("[data-io-entry]")?.dataset.ioEntry || (input.closest("#backendConfigFields") ? "backend" : "config");
  return input.id || `${scope}.${input.dataset.buffer || input.dataset.field}`;
}

export function readFormBuffer(form) {
  return Object.fromEntries([...form.querySelectorAll("input, select, textarea")]
    .filter(input => input.dataset.buffer || (input.id || input.dataset.field) && !input.closest?.("[data-collection-row]"))
    .map(input => [bufferKey(input), input.value]));
}

export function restoreFormBuffer(form, buffer) {
  for (const input of form.querySelectorAll("input, select, textarea")) {
    if (input.dataset.buffer) continue;
    if (Object.hasOwn(buffer, bufferKey(input))) input.value = buffer[bufferKey(input)];
    input.refreshEditor?.();
  }
  for (const input of form.querySelectorAll("input, select, textarea")) {
    if (input.dataset.buffer && Object.hasOwn(buffer, bufferKey(input))) input.value = buffer[bufferKey(input)];
  }
  for (const input of form.querySelectorAll("input, select, textarea")) input.validateEditor?.();
}
