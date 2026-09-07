import assert from "node:assert/strict";
import { readFileSync } from "node:fs";

const source = readFileSync(new URL("../../tools/pipeline_studio/web/graph.js", import.meta.url), "utf8");
const { GraphView, nodeSize, layeredPositions, graphBounds, fitTransform, zoomTransform, routeOrthogonal } = await import(`data:text/javascript;base64,${Buffer.from(source).toString("base64")}`);

const near = (a, b) => assert.ok(Math.abs(a - b) < 1e-8, `${a} != ${b}`);
const nodes = [{ id: "sink", depends_on: ["root"] }, { id: "root" }, { id: "parallel" }];
const sizes = { root: nodeSize({ inputs: Array(7).fill({}) }), parallel: nodeSize({ outputs: [{}] }), sink: nodeSize() };
const positions = layeredPositions(nodes, sizes);
assert.equal(positions.root.x, 65);
assert.ok(positions.sink.x > positions.root.x);
assert.ok(positions.parallel.y >= positions.root.y + sizes.root.height + 40, "actual tall node bounds must not overlap the next row");
assert.ok(nodeSize({ inputs: [{}] }).height < 140, "single-port nodes should not retain fixed 310px rows");

const distantPositions = { first: { x: -1200, y: -800 }, last: { x: 4800, y: 500 } };
const route = [{ x: -1300, y: -1000 }, { x: 5100, y: -1000 }];
const bounds = graphBounds(distantPositions, {}, [route]);
const camera = fitTransform(bounds, 640, 400);
assert.ok(camera.scale < .35, "fit must accommodate graphs larger than the previous zoom floor");
for (const point of [{ x: bounds.x, y: bounds.y }, { x: bounds.x + bounds.width, y: bounds.y + bounds.height }]) {
  const x = point.x * camera.scale + camera.offset.x, y = point.y * camera.scale + camera.offset.y;
  assert.ok(x >= 32 - 1e-8 && x <= 608 + 1e-8);
  assert.ok(y >= 32 - 1e-8 && y <= 368 + 1e-8);
}
assert.deepEqual(fitTransform(graphBounds({}), 640, 400), { scale: 1, offset: { x: 0, y: 0 } });
const anchor = { x: 285, y: 173 }, zoomed = zoomTransform(camera.scale, camera.offset, .9, anchor);
near((anchor.x - camera.offset.x) / camera.scale, (anchor.x - zoomed.offset.x) / zoomed.scale);
near((anchor.y - camera.offset.y) / camera.scale, (anchor.y - zoomed.offset.y) / zoomed.scale);

function crossesCard(a, b, rect) {
  if (a.x === b.x) return a.x > rect.x && a.x < rect.x + rect.width && Math.max(a.y, b.y) > rect.y && Math.min(a.y, b.y) < rect.y + rect.height;
  assert.equal(a.y, b.y, "every route segment must be orthogonal");
  return a.y > rect.y && a.y < rect.y + rect.height && Math.max(a.x, b.x) > rect.x && Math.min(a.x, b.x) < rect.x + rect.width;
}
for (const [from, to, rectangles] of [
  [{ x: 280, y: 84 }, { x: 1100, y: 84 }, [{ x: 0, y: 0, width: 280, height: 140 }, { x: 390, y: -30, width: 280, height: 230 }, { x: 750, y: -150, width: 220, height: 270 }, { x: 1100, y: 0, width: 280, height: 140 }]],
  [{ x: 1080, y: 300 }, { x: 0, y: 84 }, [{ x: 800, y: 220, width: 280, height: 140 }, { x: 0, y: 0, width: 280, height: 140 }, { x: 400, y: -60, width: 280, height: 440 }]],
  [{ x: -420, y: -216 }, { x: 80, y: 90 }, [{ x: -700, y: -300, width: 280, height: 140 }, { x: -320, y: -250, width: 280, height: 300 }, { x: 80, y: 0, width: 280, height: 140 }]],
]) {
  const path = routeOrthogonal(from, to, rectangles);
  assert.deepEqual(path[0], from);
  assert.deepEqual(path.at(-1), to);
  assert.deepEqual(routeOrthogonal(from, to, rectangles), path, "routing must be stable across renders");
  for (let index = 1; index < path.length; index++) {
    for (const rect of rectangles) assert.ok(!crossesCard(path[index - 1], path[index], rect), `route crosses ${JSON.stringify(rect)}: ${JSON.stringify(path)}`);
  }
}

// Small DOM harness exercises event safety and camera lifecycle without a browser dependency.
class Element {
  constructor(tag = "g") {
    this.tag = tag; this.attrs = {}; this.dataset = {}; this.children = []; this.listeners = {};
    this.classList = {
      contains: name => (this.attrs.class || "").split(" ").includes(name),
      toggle: (name, on) => {
        const values = new Set((this.attrs.class || "").split(" ").filter(Boolean));
        if (on) values.add(name); else values.delete(name);
        this.attrs.class = [...values].join(" ");
      },
    };
  }
  setAttribute(key, value) { this.attrs[key] = value; }
  removeAttribute(key) { delete this.attrs[key]; }
  append(...children) { this.children.push(...children); }
  replaceChildren(...children) { this.children = children; }
  addEventListener(name, listener) { (this.listeners[name] ??= []).push(listener); }
  fire(name, event = {}) { for (const listener of this.listeners[name] || []) listener(event); }
}
const windowElement = new Element();
globalThis.window = windowElement;
globalThis.document = { createElementNS: (_, tag) => new Element(tag) };
let resized;
globalThis.ResizeObserver = class {
  constructor(callback) { resized = callback; }
  observe() {}
};
const root = new Element("svg");
root.clientWidth = 800; root.clientHeight = 500;
const layers = Object.fromEntries(["viewport", "edges", "nodes", "draftEdge"].map(id => [`#${id}`, new Element()]));
root.querySelector = selector => layers[selector];
root.getBoundingClientRect = () => ({ left: 20, top: 30 });
let selected, deleted = 0, connections = 0;
const graph = new GraphView(root, { selectEdge: edge => { selected = edge; }, deleteEdge: () => deleted++, connect: () => connections++ });
const renderedNodes = [{ id: "a", node_type: "Source" }, { id: "b", node_type: "Target", depends_on: ["a"] }];
const definitions = { a: { outputs: [{ key: "out" }] }, b: { inputs: [{ key: "in" }] } };
const binding = { source: "a", sourcePort: "out", target: "b", targetPort: "in" };
graph.render(renderedNodes, "", new Set(), new Set(), definitions, [binding]);
assert.equal(graph.editable, false);
const initialNode = layers["#nodes"].children[0];
graph.render(renderedNodes, "a", new Set(), new Set(), definitions, [binding]);
assert.equal(layers["#nodes"].children[0], initialNode, "selection must retain DOM targets for double-click and keyboard focus");
assert.ok(initialNode.classList.contains("selected"));
const edge = layers["#edges"].children[0];
edge.fire("click", { stopPropagation() {} });
assert.equal(selected, binding);
assert.equal(deleted, 0, "clicking an edge must never delete a binding");
assert.ok(edge.classList.contains("selected"));
assert.ok(edge.children.some(child => child.classList.contains("edge-hit")));
const output = layers["#nodes"].children[0].children.find(child => child.classList.contains("output"));
output.fire("pointerdown", { stopPropagation() {}, button: 0 });
assert.equal(graph.connecting, null, "browse mode must not begin a connection");
graph.editable = true;
output.fire("pointerdown", { stopPropagation() {}, button: 0 });
assert.ok(graph.connecting);
graph.editable = false;
graph.render(renderedNodes, "", new Set(), new Set(), definitions, [binding]);
assert.equal(graph.connecting, null, "returning to browse cancels a pending connection");
assert.equal(connections, 0);

graph.zoomBy(2);
const oldScale = graph.scale;
graph.positions = { a: { x: -3000, y: -1000 }, b: { x: 3000, y: 1000 } };
graph.render(renderedNodes, "", new Set(), new Set(), definitions, [binding]);
assert.ok(graph.scale < oldScale, "opening stored positions must fit even without relayout");
assert.ok(graph.autoFit);
root.clientWidth = 500;
resized();
const updatedBounds = graphBounds(graph.positions, graph.sizes, graph.routes);
near(graph.scale, fitTransform(updatedBounds, 500, 500).scale);
const localAnchor = { x: 100, y: 200 };
const originalWorld = { x: (localAnchor.x - graph.offset.x) / graph.scale, y: (localAnchor.y - graph.offset.y) / graph.scale };
root.fire("wheel", { preventDefault() {}, deltaY: -90, clientX: 120, clientY: 230 });
near((localAnchor.x - graph.offset.x) / graph.scale, originalWorld.x);
near((localAnchor.y - graph.offset.y) / graph.scale, originalWorld.y);
graph.resetZoom();
near(graph.scale, 1);
layers["#nodes"].children[1].fire("dblclick", { stopPropagation() {}, target: { classList: { contains: () => false } } });
near(graph.positions.b.x + graph.sizes.b.width / 2, (root.clientWidth / 2 - graph.offset.x) / graph.scale);
console.log("Studio graph geometry, routing and interaction checks passed");
