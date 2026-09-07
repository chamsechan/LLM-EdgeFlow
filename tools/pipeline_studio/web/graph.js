const WIDTH = 280;
const PORT_TOP = 84;
const PORT_STEP = 30;
const SVG_NS = "http://www.w3.org/2000/svg";

function svg(tag, attrs = {}) {
  const element = document.createElementNS(SVG_NS, tag);
  for (const [key, value] of Object.entries(attrs)) element.setAttribute(key, value);
  return element;
}

export function nodeSize(definition = {}) {
  const rows = Math.max(definition.inputs?.length || 0, definition.outputs?.length || 0);
  return { width: WIDTH, height: rows ? PORT_TOP + (rows - 1) * PORT_STEP + 24 : 76 };
}

// Layout uses the displayed node dimensions; no business or validation semantics live here.
export function layeredPositions(nodes, sizes = {}) {
  const byId = new Map(nodes.map(node => [node.id, node]));
  const indegree = new Map(nodes.map(node => [node.id, 0]));
  const dependents = new Map(nodes.map(node => [node.id, []]));
  const depths = new Map(nodes.map(node => [node.id, 0]));
  for (const node of nodes) {
    for (const dependency of new Set(node.depends_on || [])) {
      if (!byId.has(dependency)) continue;
      indegree.set(node.id, indegree.get(node.id) + 1);
      dependents.get(dependency).push(node.id);
    }
  }
  const pending = nodes.filter(node => indegree.get(node.id) === 0).map(node => node.id);
  for (let index = 0; index < pending.length; ++index) {
    const id = pending[index];
    for (const dependent of dependents.get(id)) {
      depths.set(dependent, Math.max(depths.get(dependent), depths.get(id) + 1));
      indegree.set(dependent, indegree.get(dependent) - 1);
      if (indegree.get(dependent) === 0) pending.push(dependent);
    }
  }
  const layers = new Map();
  for (const node of nodes) {
    const depth = depths.get(node.id);
    if (!layers.has(depth)) layers.set(depth, []);
    layers.get(depth).push(node);
  }
  const height = node => (sizes[node.id] || nodeSize()).height;
  const totalHeight = layer => layer.reduce((sum, node) => sum + height(node), 0) + Math.max(0, layer.length - 1) * 48;
  const maxHeight = Math.max(0, ...[...layers.values()].map(totalHeight));
  const positions = {};
  for (const [depth, layer] of [...layers].sort(([a], [b]) => a - b)) {
    const parentCenter = node => {
      const parents = (node.depends_on || []).filter(id => positions[id]);
      return parents.length ? parents.reduce((sum, id) => sum + positions[id].y + height(byId.get(id)) / 2, 0) / parents.length : 0;
    };
    layer.sort((a, b) => parentCenter(a) - parentCenter(b));
    let y = 60 + (maxHeight - totalHeight(layer)) / 2;
    for (const node of layer) {
      positions[node.id] = { x: 65 + depth * (WIDTH + 76), y };
      y += height(node) + 48;
    }
  }
  return positions;
}

export function graphBounds(positions, sizes = {}, routes = []) {
  const points = [];
  for (const [id, position] of Object.entries(positions)) {
    const size = sizes[id] || nodeSize();
    points.push(position, { x: position.x + size.width, y: position.y + size.height });
  }
  for (const route of routes) points.push(...route);
  const valid = points.filter(point => Number.isFinite(point.x) && Number.isFinite(point.y));
  if (!valid.length) return { x: 0, y: 0, width: 0, height: 0 };
  const x = Math.min(...valid.map(point => point.x)), y = Math.min(...valid.map(point => point.y));
  return { x, y, width: Math.max(...valid.map(point => point.x)) - x, height: Math.max(...valid.map(point => point.y)) - y };
}

export function fitTransform(bounds, width, height, padding = 32) {
  if (width <= 0 || height <= 0 || !bounds.width || !bounds.height) return { scale: 1, offset: { x: 0, y: 0 } };
  const inset = Math.min(padding, width / 4, height / 4);
  const scale = Math.min(1, (width - inset * 2) / bounds.width, (height - inset * 2) / bounds.height);
  return { scale, offset: { x: (width - bounds.width * scale) / 2 - bounds.x * scale, y: (height - bounds.height * scale) / 2 - bounds.y * scale } };
}

export function zoomTransform(scale, offset, nextScale, anchor) {
  const ratio = nextScale / scale;
  return { scale: nextScale, offset: { x: anchor.x - (anchor.x - offset.x) * ratio, y: anchor.y - (anchor.y - offset.y) * ratio } };
}

function segmentBlocked(a, b, rectangles) {
  return rectangles.some(rect => a.x === b.x
    ? a.x > rect.left && a.x < rect.right && Math.max(a.y, b.y) > rect.top && Math.min(a.y, b.y) < rect.bottom
    : a.y > rect.top && a.y < rect.bottom && Math.max(a.x, b.x) > rect.left && Math.min(a.x, b.x) < rect.right);
}

function simplify(points) {
  const result = [];
  for (const point of points) {
    const previous = result.at(-1);
    if (previous && previous.x === point.x && previous.y === point.y) continue;
    const before = result.at(-2);
    if (before && ((before.x === previous.x && previous.x === point.x) || (before.y === previous.y && previous.y === point.y))) result.pop();
    result.push(point);
  }
  return result;
}

// A visibility grid routes horizontal/vertical segments around padded node rectangles.
// Direction is part of the search state so a shorter but zigzagging path loses to a clean one.
export function routeOrthogonal(source, target, obstacles, lane = 0) {
  const clearance = 12 + lane % 3 * 4;
  const start = { x: source.x + clearance + 10, y: source.y };
  const end = { x: target.x - clearance - 10, y: target.y };
  const rectangles = obstacles.map(rect => ({ left: rect.x - clearance, right: rect.x + rect.width + clearance, top: rect.y - clearance, bottom: rect.y + rect.height + clearance }));
  if (start.x <= end.x && source.y === target.y && !segmentBlocked(start, end, rectangles)) return [source, target];
  const xs = [...new Set([start.x, end.x, ...rectangles.flatMap(rect => [rect.left, rect.right])])].sort((a, b) => a - b);
  const ys = [...new Set([start.y, end.y, ...rectangles.flatMap(rect => [rect.top, rect.bottom])])].sort((a, b) => a - b);
  const nx = xs.length, ny = ys.length;
  const startIndex = ys.indexOf(start.y) * nx + xs.indexOf(start.x);
  const endIndex = ys.indexOf(end.y) * nx + xs.indexOf(end.x);
  const pointAt = index => ({ x: xs[index % nx], y: ys[Math.floor(index / nx)] });
  const startState = startIndex * 2; // horizontal start/end stubs
  const costs = new Map([[startState, 0]]), parents = new Map();
  const heap = [];
  const push = entry => {
    heap.push(entry);
    let index = heap.length - 1;
    while (index > 0) {
      const parent = (index - 1) >> 1;
      if (heap[parent].priority <= entry.priority) break;
      heap[index] = heap[parent]; index = parent;
    }
    heap[index] = entry;
  };
  const pop = () => {
    const first = heap[0], last = heap.pop();
    if (heap.length) {
      let index = 0;
      while (index * 2 + 1 < heap.length) {
        let child = index * 2 + 1;
        if (child + 1 < heap.length && heap[child + 1].priority < heap[child].priority) child++;
        if (heap[child].priority >= last.priority) break;
        heap[index] = heap[child]; index = child;
      }
      heap[index] = last;
    }
    return first;
  };
  push({ state: startState, cost: 0, priority: 0 });
  let finalState;
  while (heap.length) {
    const current = pop();
    if (current.cost !== costs.get(current.state)) continue;
    const index = Math.floor(current.state / 2), direction = current.state % 2;
    if (index === endIndex) { finalState = current.state; break; }
    const a = pointAt(index), x = index % nx, y = Math.floor(index / nx);
    const neighbors = [];
    if (x > 0) neighbors.push([index - 1, 0]);
    if (x + 1 < nx) neighbors.push([index + 1, 0]);
    if (y > 0) neighbors.push([index - nx, 1]);
    if (y + 1 < ny) neighbors.push([index + nx, 1]);
    for (const [next, nextDirection] of neighbors) {
      const b = pointAt(next);
      if (segmentBlocked(a, b, rectangles)) continue;
      const state = next * 2 + nextDirection;
      const cost = current.cost + Math.abs(b.x - a.x) + Math.abs(b.y - a.y) + (direction === nextDirection ? 0 : 24);
      if (cost >= (costs.get(state) ?? Infinity)) continue;
      costs.set(state, cost); parents.set(state, current.state);
      push({ state, cost, priority: cost + Math.abs(b.x - end.x) + Math.abs(b.y - end.y) });
    }
  }
  if (finalState === undefined) {
    // Manually overlapping cards can leave a port completely enclosed. Keep that binding visible.
    const y = Math.min(source.y, target.y, ...rectangles.map(rect => rect.top)) - 24;
    return simplify([source, start, { x: start.x, y }, { x: end.x, y }, end, target]);
  }
  const reversed = [];
  for (let state = finalState; state !== undefined; state = parents.get(state)) reversed.push(pointAt(Math.floor(state / 2)));
  return simplify([source, ...reversed.reverse(), target]);
}

function edgeKey(binding) {
  return binding ? JSON.stringify([binding.source, binding.sourcePort, binding.target, binding.targetPort, Boolean(binding.dependency)]) : "";
}

function textLabel(value, maxWidth, fontSize) {
  // A conservative character estimate also works before SVG is attached or fonts finish loading.
  let width = 0, label = "";
  for (const char of String(value || "")) {
    width += /[^\x00-\x7f]/.test(char) ? fontSize : fontSize * .59;
    if (width > maxWidth - fontSize) return `${label}…`;
    label += char;
  }
  return label;
}

export class GraphView {
  constructor(root, callbacks = {}) {
    this.root = root;
    this.viewport = root.querySelector("#viewport");
    this.edgeLayer = root.querySelector("#edges");
    this.nodeLayer = root.querySelector("#nodes");
    this.draftEdge = root.querySelector("#draftEdge");
    this.callbacks = callbacks;
    this.positions = {};
    this.sizes = {};
    this.routes = [];
    this.scale = 1;
    this.offset = { x: 0, y: 0 };
    this.connecting = null;
    this.editable = false;
    this.autoFit = true;
    this.dimensions = { width: root.clientWidth, height: root.clientHeight };
    this.bindPanZoom();
    if (typeof ResizeObserver !== "undefined") {
      this.resizeObserver = new ResizeObserver(() => {
        const width = root.clientWidth, height = root.clientHeight;
        if (this.autoFit) this.fit();
        else {
          this.offset.x += (width - this.dimensions.width) / 2;
          this.offset.y += (height - this.dimensions.height) / 2;
          this.transform();
        }
        this.dimensions = { width, height };
      });
      this.resizeObserver.observe(root);
    }
  }

  bindPanZoom() {
    this.root.addEventListener("wheel", event => {
      event.preventDefault();
      const bounds = this.root.getBoundingClientRect();
      this.zoomBy(Math.exp(-Math.max(-100, Math.min(100, event.deltaY)) * .002), { x: event.clientX - bounds.left, y: event.clientY - bounds.top });
    }, { passive: false });
    let pan = null;
    this.root.addEventListener("pointerdown", event => {
      if (event.button !== 0 || event.target.closest(".node, .edge")) return;
      this.root.focus({ preventScroll: true });
      pan = { clientX: event.clientX, clientY: event.clientY, offsetX: this.offset.x, offsetY: this.offset.y };
      this.root.setPointerCapture(event.pointerId);
    });
    window.addEventListener("pointermove", event => {
      if (pan) {
        this.autoFit = false;
        this.offset.x = pan.offsetX + event.clientX - pan.clientX;
        this.offset.y = pan.offsetY + event.clientY - pan.clientY;
        this.transform();
      }
      if (this.connecting) {
        const point = this.localPoint(event.clientX, event.clientY), source = this.positions[this.connecting.nodeId];
        this.draftEdge.setAttribute("d", this.curve(source.x + WIDTH, source.y + this.connecting.y, point.x, point.y));
      }
    });
    window.addEventListener("pointerup", event => {
      pan = null;
      if (this.connecting) {
        const target = document.elementFromPoint?.(event.clientX, event.clientY)?.closest?.(".port.input");
        if (this.editable && target?.dataset.nodeId && target.dataset.nodeId !== this.connecting.nodeId) {
          this.callbacks.connect?.(this.connecting.nodeId, this.connecting.port, target.dataset.nodeId, target.dataset.port);
        }
        this.cancelConnection();
      }
    });
    window.addEventListener("pointercancel", () => { pan = null; this.cancelConnection(); });
    this.root.addEventListener("keydown", event => {
      if (event.key === "Escape") this.cancelConnection();
    });
  }

  localPoint(clientX, clientY) {
    const point = this.root.createSVGPoint();
    point.x = clientX; point.y = clientY;
    return point.matrixTransform(this.viewport.getScreenCTM().inverse());
  }

  transform() {
    this.viewport.setAttribute("transform", `translate(${this.offset.x} ${this.offset.y}) scale(${this.scale})`);
    this.callbacks.viewChanged?.({ scale: this.scale });
  }

  zoomBy(factor, anchor = { x: this.root.clientWidth / 2, y: this.root.clientHeight / 2 }) {
    const nextScale = Math.max(.000001, Math.min(3, this.scale * factor));
    Object.assign(this, zoomTransform(this.scale, this.offset, nextScale, anchor));
    this.autoFit = false;
    this.transform();
  }

  resetZoom() { this.zoomBy(1 / this.scale); }

  focusNode(id) {
    const position = this.positions[id], size = this.sizes[id];
    if (!position || !size) return;
    this.scale = Math.max(1, this.scale);
    this.offset = { x: this.root.clientWidth / 2 - (position.x + size.width / 2) * this.scale, y: this.root.clientHeight / 2 - (position.y + size.height / 2) * this.scale };
    this.autoFit = false;
    this.transform();
  }

  layout(nodes, force = false) {
    const nodeIds = new Set(nodes.map(node => node.id));
    let positionsChanged = false;
    for (const id of Object.keys(this.positions)) {
      if (!nodeIds.has(id) || !Number.isFinite(this.positions[id]?.x) || !Number.isFinite(this.positions[id]?.y)) {
        delete this.positions[id]; positionsChanged = true;
      }
    }
    if (force || nodes.some(node => !this.positions[node.id])) {
      const proposed = layeredPositions(nodes, this.sizes || {});
      for (const node of nodes) if (force || !this.positions[node.id]) this.positions[node.id] = proposed[node.id];
      positionsChanged = true;
    }
    if (positionsChanged) {
      this.callbacks.positionsChanged?.(this.positions);
      this.needsFit = true;
    }
  }

  fit() {
    Object.assign(this, fitTransform(graphBounds(this.positions, this.sizes, this.routes), this.root.clientWidth, this.root.clientHeight));
    this.autoFit = true;
    this.transform();
  }

  curve(x1, y1, x2, y2) {
    const middle = (x1 + x2) / 2;
    return `M${x1},${y1} H${middle} V${y2} H${x2}`;
  }

  render(nodes, selectedId, errorIds = new Set(), modelIds = new Set(), definitions = {}, edges = [], selectedEdge = null) {
    const reopened = this.lastPositions !== this.positions;
    if (!this.editable) this.cancelConnection();
    const renderKey = JSON.stringify([nodes, definitions, edges, [...errorIds], [...modelIds], this.editable]);
    // Keep DOM targets stable during selection so double-click and keyboard focus survive.
    if (!reopened && !this.needsFit && this.renderKey === renderKey) {
      for (const group of this.nodeLayer.children) group.classList.toggle("selected", group.dataset.nodeId === selectedId);
      this.setSelectedEdge(selectedEdge);
      return;
    }
    this.definitions = definitions;
    this.edges = edges;
    this.nodes = nodes;
    this.selectedEdge = selectedEdge;
    this.sizes = Object.fromEntries(nodes.map(node => [node.id, nodeSize(definitions[node.id])]));
    this.layout(nodes);
    this.nodeLayer.replaceChildren();
    this.renderEdges();
    nodes.forEach(node => this.nodeLayer.append(this.nodeElement(node, node.id === selectedId, errorIds.has(node.id), modelIds.has(node.id))));
    if (reopened || this.needsFit || this.autoFit) this.fit();
    this.lastPositions = this.positions;
    this.renderKey = renderKey;
    this.needsFit = false;
  }

  setSelectedEdge(binding) {
    this.selectedEdge = binding;
    for (const group of this.edgeLayer.children) group.classList.toggle("selected", group.dataset.edgeKey === edgeKey(binding));
  }

  renderEdges() {
    this.edgeLayer.replaceChildren();
    this.routes = [];
    const obstacles = Object.entries(this.positions).map(([id, position]) => ({ ...position, ...(this.sizes[id] || nodeSize()) }));
    for (const [index, binding] of (this.edges || []).entries()) {
      const from = this.positions[binding.source], to = this.positions[binding.target];
      if (!from || !to) continue;
      const y1 = binding.dependency ? 34 : this.portY(binding.source, "outputs", binding.sourcePort);
      const y2 = binding.dependency ? 34 : this.portY(binding.target, "inputs", binding.targetPort);
      const route = routeOrthogonal({ x: from.x + WIDTH, y: from.y + y1 }, { x: to.x, y: to.y + y2 }, obstacles, index);
      this.routes.push(route);
      const d = route.map((point, i) => `${i ? "L" : "M"}${point.x},${point.y}`).join(" ");
      const group = svg("g", { class: `edge${binding.dependency ? " dependency" : ""}${edgeKey(binding) === edgeKey(this.selectedEdge) ? " selected" : ""}`, tabindex: 0, role: "button", "aria-label": `${binding.source} ${binding.sourcePort || ""} → ${binding.target} ${binding.targetPort || "执行依赖"}` });
      group.dataset.edgeKey = edgeKey(binding);
      const edge = svg("path", { class: "edge-line", d });
      const hit = svg("path", { class: "edge-hit", d, "aria-hidden": "true" });
      const title = svg("title");
      title.textContent = `${binding.source}${binding.sourcePort ? `.${binding.sourcePort}` : ""} → ${binding.target}${binding.targetPort ? `.${binding.targetPort}` : ""}${binding.dependency ? " · 执行依赖" : ""}`;
      group.append(title, edge, hit);
      const select = () => { this.setSelectedEdge(binding); this.callbacks.selectEdge?.(binding); };
      group.addEventListener("click", event => { event.stopPropagation(); select(); });
      group.addEventListener("keydown", event => {
        if (event.key === "Enter" || event.key === " ") { event.preventDefault(); select(); }
      });
      this.edgeLayer.append(group);
    }
  }

  portY(id, direction, name) {
    return PORT_TOP + Math.max(0, (this.definitions[id]?.[direction] || []).findIndex(port => port.key === name)) * PORT_STEP;
  }

  nodeElement(node, selected, hasError = false, hasModel = false) {
    const position = this.positions[node.id];
    const group = svg("g", {
      class: `node${selected ? " selected" : ""}${hasModel ? " has-model" : ""}${hasError ? " has-error" : ""}`,
      transform: `translate(${position.x} ${position.y})`, tabindex: 0, role: "button", "aria-label": `${node.node_type} · ${node.id}`,
    });
    group.dataset.nodeId = node.id;
    const definition = this.definitions[node.id] || {}, size = this.sizes[node.id];
    const nodeTitle = svg("title"); nodeTitle.textContent = `${node.node_type}\n${node.id}\n双击聚焦节点`;
    group.append(nodeTitle);
    group.append(svg("rect", { class: "body", width: size.width, height: size.height }));
    for (const [value, className, y, fontSize] of [[node.node_type, "node-title", 29, 17], [node.id, "subtitle", 53, 12.5]]) {
      const text = svg("text", { class: className, x: 16, y }); text.textContent = textLabel(value, WIDTH - 32, fontSize);
      const title = svg("title"); title.textContent = value; text.append(title); group.append(text);
    }
    for (const [direction, ports] of [["input", definition.inputs || []], ["output", definition.outputs || []]]) {
      ports.forEach((port, index) => {
        const y = PORT_TOP + index * PORT_STEP;
        const circle = svg("circle", { class: `port ${direction}${this.editable ? "" : " disabled"}`, cx: direction === "input" ? 0 : WIDTH, cy: y, r: 7 });
        circle.dataset.nodeId = node.id; circle.dataset.port = port.key;
        const description = `${port.key}: ${port.type_id} · ${port.cardinality || ""}${port.required && direction === "input" ? " · 必需" : ""}`;
        const tooltip = svg("title"); tooltip.textContent = description; circle.append(tooltip);
        const label = svg("text", { class: "port-label", x: direction === "input" ? 13 : WIDTH - 13, y: y + 4, "text-anchor": direction === "input" ? "start" : "end" });
        const paired = direction === "input" ? definition.outputs?.[index] : definition.inputs?.[index];
        label.textContent = textLabel(`${port.key}${port.required && direction === "input" ? " *" : ""}`, paired ? WIDTH / 2 - 25 : WIDTH - 30, 13);
        const labelTitle = svg("title"); labelTitle.textContent = description; label.append(labelTitle);
        if (direction === "output") circle.addEventListener("pointerdown", event => {
          event.stopPropagation();
          if (this.editable && event.button === 0) this.connecting = { nodeId: node.id, port: port.key, y };
        });
        group.append(circle, label);
      });
    }
    let drag = null, moved = false;
    group.addEventListener("click", event => {
      if (!event.target.classList.contains("port") && !moved) this.callbacks.select?.(node.id);
    });
    group.addEventListener("dblclick", event => {
      event.stopPropagation();
      if (!event.target.classList.contains("port")) this.focusNode(node.id);
    });
    group.addEventListener("keydown", event => {
      if (event.key === "Enter" || event.key === " ") { event.preventDefault(); this.callbacks.select?.(node.id); }
    });
    group.addEventListener("pointerdown", event => {
      if (event.button !== 0 || event.target.classList.contains("port")) return;
      const point = this.localPoint(event.clientX, event.clientY);
      drag = { dx: point.x - position.x, dy: point.y - position.y, clientX: event.clientX, clientY: event.clientY };
      moved = false;
      group.setPointerCapture(event.pointerId);
    });
    group.addEventListener("pointermove", event => {
      if (!drag) return;
      if (Math.hypot(event.clientX - drag.clientX, event.clientY - drag.clientY) < 3 && !moved) return;
      moved = true;
      this.autoFit = false;
      const point = this.localPoint(event.clientX, event.clientY);
      position.x = point.x - drag.dx; position.y = point.y - drag.dy;
      group.setAttribute("transform", `translate(${position.x} ${position.y})`);
      this.renderEdges();
    });
    const finishDrag = () => {
      if (drag && moved) this.callbacks.positionsChanged?.(this.positions);
      drag = null;
    };
    group.addEventListener("pointerup", finishDrag);
    group.addEventListener("pointercancel", finishDrag);
    return group;
  }

  cancelConnection() { this.connecting = null; this.draftEdge.removeAttribute("d"); }
}
