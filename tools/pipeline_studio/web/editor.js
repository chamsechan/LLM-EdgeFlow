// Document history stores applied edits only. Form buffers stay separate until
// Apply, so selection, validation and repainting cannot discard user input.
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

// Config fields share the same editor for Node, Model and Backend parameters.
export function appendConfigField(container, field, values, modelChoices = null) {
  const label = document.createElement("label"); label.textContent = field.name;
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
  input.dataset.field = field.name; input.dataset.type = field.type;
  // Definition.required concerns field presence; a required string may be empty.
  input.required = Boolean(field.required) && field.type !== "string";
  const present = Object.hasOwn(values, field.name);
  const value = present ? values[field.name] : field.default;
  const text = typeof value === "object" ? JSON.stringify(value) : String(value ?? "");
  input.value = text;
  if (field.type === "string" && input.tagName === "TEXTAREA") {
    // Browsers normalize CR/CRLF in textarea.value. Preserve the original string
    // when applying an untouched field, including after a draft repaint.
    if (present || field.required) input.dataset.originalValue = text;
    input.dataset.displayValue = input.value;
    input.rows = Math.min(4, input.value.split("\n").length);
  }
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
  return input.value === input.dataset.displayValue ? input.dataset.originalValue : input.value;
}

export function readConfigFields(container) {
  const config = {};
  for (const input of container.querySelectorAll("[data-field]")) {
    if (input.value === input.dataset.displayValue && input.dataset.originalValue === undefined) continue;
    if (input.dataset.type === "string" || input.value !== "" || input.required) config[input.dataset.field] = parseField(input);
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
