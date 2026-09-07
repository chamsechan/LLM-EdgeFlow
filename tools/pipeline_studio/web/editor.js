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
