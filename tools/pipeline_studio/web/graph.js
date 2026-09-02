const WIDTH = 220;
const HEIGHT = 78;
const SVG_NS = "http://www.w3.org/2000/svg";

function svg(tag, attrs = {}) {
  const element = document.createElementNS(SVG_NS, tag);
  for (const [key, value] of Object.entries(attrs)) element.setAttribute(key, value);
  return element;
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
    window.addEventListener("pointerup", () => { pan = null; this.cancelConnection(); });
  }

  localPoint(clientX, clientY) {
    const point = this.root.createSVGPoint();
    point.x = clientX; point.y = clientY;
    return point.matrixTransform(this.viewport.getScreenCTM().inverse());
  }

  transform() { this.viewport.setAttribute("transform", `translate(${this.offset.x} ${this.offset.y}) scale(${this.scale})`); }

  layout(nodes, force = false) {
    nodes.forEach((node, index) => {
      if (force || !this.positions[node.id]) this.positions[node.id] = { x: 65 + (index % 3) * 285, y: 60 + Math.floor(index / 3) * 145 };
    });
    this.callbacks.positionsChanged(this.positions);
  }

  curve(x1, y1, x2, y2) {
    const bend = Math.max(55, Math.abs(x2 - x1) * .45);
    return `M${x1},${y1} C${x1 + bend},${y1} ${x2 - bend},${y2} ${x2},${y2}`;
  }

  render(nodes, selectedId) {
    this.nodes = nodes;
    this.layout(nodes);
    this.nodeLayer.replaceChildren();
    this.renderEdges(nodes);
    nodes.forEach(node => this.nodeLayer.append(this.nodeElement(node, node.id === selectedId)));
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

  nodeElement(node, selected) {
    const position = this.positions[node.id];
    const group = svg("g", { class: `node${selected ? " selected" : ""}`, transform: `translate(${position.x} ${position.y})` });
    group.append(svg("rect", { class: "body", width: WIDTH, height: HEIGHT }));
    const title = svg("text", { x: 16, y: 29 }); title.textContent = node.node_type;
    const subtitle = svg("text", { class: "subtitle", x: 16, y: 53 }); subtitle.textContent = node.id;
    const input = svg("circle", { class: "port input", cx: 0, cy: HEIGHT / 2, r: 7 });
    const output = svg("circle", { class: "port output", cx: WIDTH, cy: HEIGHT / 2, r: 7 });
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
