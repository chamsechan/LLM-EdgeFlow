const WIDTH = 220;
const HEIGHT = 78;
const SVG_NS = "http://www.w3.org/2000/svg";

function svg(tag, attrs = {}) {
  const element = document.createElementNS(SVG_NS, tag);
  for (const [key, value] of Object.entries(attrs)) element.setAttribute(key, value);
  return element;
}

export function layeredPositions(nodes) {
  const byId = new Map(nodes.map(node => [node.id, node]));
  const indegree = new Map(nodes.map(node => [node.id, 0]));
  const dependents = new Map(nodes.map(node => [node.id, []]));
  const depths = new Map(nodes.map(node => [node.id, 0]));

  for (const node of nodes) {
    for (const dependency of node.depends_on || []) {
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

  const rows = new Map();
  const positions = {};
  for (const node of nodes) {
    const depth = depths.get(node.id);
    const row = rows.get(depth) || 0;
    positions[node.id] = { x: 65 + depth * 300, y: 60 + row * 135 };
    rows.set(depth, row + 1);
  }
  return positions;
}

export class GraphView {
  constructor(root, callbacks) {
    this.root = root;
    this.viewport = root.querySelector("#viewport");
    this.edgeLayer = root.querySelector("#edges");
    this.nodeLayer = root.querySelector("#nodes");
    this.draftEdge = root.querySelector("#draftEdge");
    this.callbacks = callbacks;
    this.positions = {};
    this.scale = 1;
    this.offset = { x: 0, y: 0 };
    this.connecting = null;
    this.bindPanZoom();
  }

  bindPanZoom() {
    this.root.addEventListener("wheel", event => {
      event.preventDefault();
      this.scale = Math.max(.35, Math.min(2.2, this.scale * (event.deltaY > 0 ? .9 : 1.1)));
      this.transform();
    }, { passive: false });
    let pan = null;
    this.root.addEventListener("pointerdown", event => {
      if (event.target === this.root) {
        pan = { clientX: event.clientX, clientY: event.clientY, offsetX: this.offset.x, offsetY: this.offset.y };
      }
    });
    window.addEventListener("pointermove", event => {
      if (pan) {
        this.offset.x = pan.offsetX + event.clientX - pan.clientX;
        this.offset.y = pan.offsetY + event.clientY - pan.clientY;
        this.transform();
      }
      if (this.connecting) {
        const point = this.localPoint(event.clientX, event.clientY);
        const source = this.positions[this.connecting];
        this.draftEdge.setAttribute("d", this.curve(source.x + WIDTH, source.y + HEIGHT / 2, point.x, point.y));
      }
    });
    window.addEventListener("pointerup", event => {
      pan = null;
      if (this.connecting) {
        const target = document.elementFromPoint ? document.elementFromPoint(event.clientX, event.clientY) : null;
        const inputPort = target?.closest ? target.closest(".port.input") : null;
        if (inputPort && inputPort.dataset?.nodeId && inputPort.dataset.nodeId !== this.connecting) {
          this.callbacks.connect(this.connecting, inputPort.dataset.nodeId);
        }
        this.cancelConnection();
      }
    });
  }

  localPoint(clientX, clientY) {
    const point = this.root.createSVGPoint();
    point.x = clientX; point.y = clientY;
    return point.matrixTransform(this.viewport.getScreenCTM().inverse());
  }

  transform() { this.viewport.setAttribute("transform", `translate(${this.offset.x} ${this.offset.y}) scale(${this.scale})`); }

  layout(nodes, force = false) {
    const nodeIds = new Set(nodes.map(node => node.id));
    let positionsChanged = false;
    for (const id of Object.keys(this.positions)) {
      if (!nodeIds.has(id)) {
        delete this.positions[id];
        positionsChanged = true;
      }
    }
    const needsLayout = force || nodes.some(node => !this.positions[node.id]);
    if (needsLayout) {
      const proposed = layeredPositions(nodes);
      for (const node of nodes) {
        if (force || !this.positions[node.id]) this.positions[node.id] = proposed[node.id];
      }
      positionsChanged = true;
    }
    if (positionsChanged) this.callbacks.positionsChanged(this.positions);
  }

  curve(x1, y1, x2, y2) {
    const bend = Math.max(55, Math.abs(x2 - x1) * .45);
    return `M${x1},${y1} C${x1 + bend},${y1} ${x2 - bend},${y2} ${x2},${y2}`;
  }

  render(nodes, selectedId, errorIds = new Set(), modelIds = new Set()) {
    this.nodes = nodes;
    this.errorIds = errorIds;
    this.layout(nodes);
    this.nodeLayer.replaceChildren();
    this.renderEdges(nodes);
    nodes.forEach(node => this.nodeLayer.append(this.nodeElement(
      node, node.id === selectedId, errorIds.has(node.id), modelIds.has(node.id)
    )));
  }

  renderEdges(nodes) {
    this.edgeLayer.replaceChildren();
    const byId = new Map(nodes.map(node => [node.id, node]));
    for (const node of nodes) {
      for (const dependency of node.depends_on || []) {
        if (!byId.has(dependency)) continue;
        const from = this.positions[dependency], to = this.positions[node.id];
        const edge = svg("path", { d: this.curve(from.x + WIDTH, from.y + HEIGHT / 2, to.x, to.y + HEIGHT / 2) });
        edge.addEventListener("click", () => this.callbacks.deleteEdge(dependency, node.id));
        this.edgeLayer.append(edge);
      }
    }
  }

  nodeElement(node, selected, hasError = false, hasModel = false) {
    const position = this.positions[node.id];
    const group = svg("g", {
      class: `node${selected ? " selected" : ""}${hasModel ? " has-model" : ""}${hasError ? " has-error" : ""}`,
      transform: `translate(${position.x} ${position.y})`
    });
    group.append(svg("rect", { class: "body", width: WIDTH, height: HEIGHT }));
    const title = svg("text", { x: 16, y: 29 }); title.textContent = node.node_type;
    const subtitle = svg("text", { class: "subtitle", x: 16, y: 53 }); subtitle.textContent = node.id;
    const input = svg("circle", { class: "port input", cx: 0, cy: HEIGHT / 2, r: 7 });
    input.dataset.nodeId = node.id;
    const output = svg("circle", { class: "port output", cx: WIDTH, cy: HEIGHT / 2, r: 7 });
    output.dataset.nodeId = node.id;
    input.addEventListener("pointerup", event => {
      event.stopPropagation();
      if (this.connecting && this.connecting !== node.id) this.callbacks.connect(this.connecting, node.id);
      this.cancelConnection();
    });
    output.addEventListener("pointerdown", event => {
      event.stopPropagation(); this.connecting = node.id;
    });
    group.addEventListener("click", event => { if (!event.target.classList.contains("port")) this.callbacks.select(node.id); });
    let drag = null;
    group.addEventListener("pointerdown", event => {
      if (event.target.classList.contains("port")) return;
      const point = this.localPoint(event.clientX, event.clientY);
      drag = { dx: point.x - position.x, dy: point.y - position.y };
      group.setPointerCapture(event.pointerId);
    });
    group.addEventListener("pointermove", event => {
      if (!drag) return;
      const point = this.localPoint(event.clientX, event.clientY);
      position.x = point.x - drag.dx; position.y = point.y - drag.dy;
      group.setAttribute("transform", `translate(${position.x} ${position.y})`);
      this.callbacks.positionsChanged(this.positions);
      this.renderEdges(this.nodes || []);
    });
    group.addEventListener("pointerup", () => { drag = null; });
    group.append(title, subtitle, input, output);
    return group;
  }

  cancelConnection() { this.connecting = null; this.draftEdge.removeAttribute("d"); }
}
