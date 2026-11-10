import assert from 'node:assert/strict';
import {readFile} from 'node:fs/promises';

const web = new URL('../../tools/pipeline_studio/web/', import.meta.url);
const load = async name => import(`data:text/javascript;base64,${Buffer.from(await readFile(new URL(name, web))).toString('base64')}`);
const graph = await load('graph.js');
const {graphDocument} = await load('workbench.js');
const port = key => ({key, type_id: 'text', cardinality: '1:1'});
const catalog = {
  nodes: [{node_type: 'transform', inputs: [port('text')], outputs: [port('answer')]}],
  input_converters: [{type: 'in', name: 'first', logical_ports: [port('text')]},
    {type: 'in', name: 'second', logical_ports: [port('text'), port('extra')]}],
  output_converters: [{type: 'out', name: 'result', logical_ports: [port('answer')]}],
};
const pipeline = {
  io: {input: [{type: 'in', name: 'first'}, {type: 'in', name: 'second'}],
    output: [{type: 'out', name: 'result', inputs: {answer: 'b.answer'}}]},
  pipeline: [
    {type: 'transform', name: 'a', inputs: {text: 'input.text'}},
    {type: 'transform', name: 'b', inputs: {text: 'a.answer'}, depends_on: ['a', 'order_only']},
    {type: 'not_available', name: 'order_only'},
  ],
};
const before = structuredClone(pipeline);
const display = graphDocument(pipeline, catalog);
assert.deepEqual(pipeline, before);
assert.deepEqual(display.nodes.map(node => node.id), ['input', 'a', 'b', 'order_only', 'output']);
assert.deepEqual(display.definitions.input.outputs.map(port => port.key), ['text', 'extra']);
assert.equal(display.edges.filter(edge => edge.source === 'a' && edge.target === 'b').length, 1);
assert.deepEqual(display.edges.find(edge => edge.source === 'order_only'), {
  source: 'order_only', target: 'b', dependency: true,
});
assert.deepEqual(display.nodes.find(node => node.id === 'b').depends_on, ['a', 'order_only']);
assert.deepEqual(display.definitions.order_only, {});
assert.equal(display.edges.at(-1).target, 'output');
assert.deepEqual(graphDocument(null, catalog).nodes, []);

assert.deepEqual(graph.nodeSize(), {width: 280, height: 76});
assert.deepEqual(graph.nodeSize({inputs: [port('a'), port('b'), port('c')]}), {width: 280, height: 168});
const sizes = Object.fromEntries(display.nodes.map(node => [node.id, graph.nodeSize(display.definitions[node.id])]));
const positions = graph.layeredPositions(display.nodes, sizes);
assert.ok(positions.a.x > positions.input.x);
assert.ok(positions.b.x > positions.a.x);
assert.ok(positions.output.x > positions.b.x);
for (const position of Object.values(positions)) {
  assert.ok(Number.isFinite(position.x));
  assert.ok(Number.isFinite(position.y));
}
const siblings = graph.layeredPositions([{id: 'tall'}, {id: 'short'}], {
  tall: {width: 280, height: 200}, short: {width: 280, height: 76},
});
assert.equal(siblings.short.y - siblings.tall.y, 248);
const duplicateOrder = graph.layeredPositions([{id: 'a'}, {id: 'b', depends_on: ['a', 'a', 'absent']}]);
assert.equal(duplicateOrder.b.x - duplicateOrder.a.x, 356);

// 折线保持端点与端口方向；反向和相邻端口也必须可见。
const source = {x: 0, y: 100};
for (const target of [{x: 500, y: 100}, {x: 500, y: 180}, {x: -100, y: 100}, {x: 0, y: 180}]) {
  const route = graph.routeOrthogonal(source, target);
  assert.deepEqual(route[0], source);
  assert.deepEqual(route.at(-1), target);
  assert.ok(route.length <= 6);
  assert.ok(route[1].x > source.x);
  assert.ok(route.at(-2).x < target.x);
  for (let index = 1; index < route.length; index++) {
    const a = route[index - 1], b = route[index];
    assert.ok(a.x === b.x || a.y === b.y, 'every route segment is orthogonal');
    assert.ok(Number.isFinite(b.x) && Number.isFinite(b.y));
  }
}
assert.deepEqual(graph.routeOrthogonal(source, {x: 500, y: 100}), [source, {x: 500, y: 100}]);
const bounds = graph.graphBounds({node: {x: 10, y: 20}, invalid: {x: NaN, y: Infinity}}, {}, [[{x: -50, y: -30}]]);
assert.deepEqual(bounds, {x: -50, y: -30, width: 340, height: 126});
assert.deepEqual(graph.graphBounds({}), {x: 0, y: 0, width: 0, height: 0});
const fit = graph.fitTransform(bounds, 800, 600);
assert.ok(fit.scale <= 1 && fit.scale > 0);
assert.ok(bounds.x * fit.scale + fit.offset.x >= 0);
assert.deepEqual(graph.fitTransform(bounds, 0, 600), {scale: 1, offset: {x: 0, y: 0}});
const anchor = {x: 200, y: 150};
const zoom = graph.zoomTransform(0.5, {x: 10, y: 20}, 2, anchor);
assert.equal((anchor.x - zoom.offset.x) / zoom.scale, (anchor.x - 10) / 0.5);
assert.equal((anchor.y - zoom.offset.y) / zoom.scale, (anchor.y - 20) / 0.5);

// Exercise real public view methods without reproducing SVG rendering.
const notifications = [];
const view = Object.create(graph.GraphView.prototype);
Object.assign(view, {
  root: {clientWidth: 800, clientHeight: 600},
  viewport: {setAttribute(name, value) { notifications.push([name, value]); }},
  callbacks: {positionsChanged(value) { notifications.push(['positions', structuredClone(value)]); }},
  positions: {a: {x: 900, y: 700}, removed: {x: 1, y: 1}, b: {x: NaN, y: 2}},
  sizes: {a: graph.nodeSize(), b: graph.nodeSize()}, routes: [], scale: 1, offset: {x: 0, y: 0},
});
view.layout([{id: 'a'}, {id: 'b', depends_on: ['a']}]);
assert.deepEqual(view.positions.a, {x: 900, y: 700});
assert.equal(view.positions.removed, undefined);
assert.ok(Number.isFinite(view.positions.b.x));
view.layout([{id: 'a'}, {id: 'b', depends_on: ['a']}], true);
assert.equal(view.positions.a.x, 65);
view.fit();
assert.equal(view.autoFit, true);
view.zoomBy(100);
assert.equal(view.scale, 3);
assert.equal(view.autoFit, false);
view.resetZoom();
assert.equal(view.scale, 1);
view.focusNode('a');
assert.equal((view.positions.a.x + 140) * view.scale + view.offset.x, 400);
assert.ok(notifications.some(([name]) => name === 'transform'));
// 用户节点名可以与对象原型成员同名；恢复布局时只保留已有的自有位置。
const namedNodes = ['constructor', '__proto__', 'toString'].map(id => ({id}));
const namedPositions = graph.layeredPositions(namedNodes);
view.positions = JSON.parse('{"constructor":{"x":900,"y":700}}');
view.sizes = {};
view.layout(namedNodes);
assert.deepEqual(view.positions.constructor, {x: 900, y: 700});
for (const {id} of namedNodes) {
  for (const positions of [namedPositions, view.positions]) {
    assert.equal(Object.hasOwn(positions, id), true);
    assert.ok(Number.isFinite(positions[id].x) && Number.isFinite(positions[id].y));
  }
}
console.log('PASS Studio graph document, geometry, routes and view transforms');
