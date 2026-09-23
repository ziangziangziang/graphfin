import assert from "node:assert/strict";
import { test } from "node:test";
import {
  createLattice,
  positionOnSpiral,
  visibility,
  wrap,
} from "../src/scene/geometry.ts";
import { Signal } from "../src/scene/signal.ts";
import { qualityFor } from "../src/scene/quality.ts";

for (const width of [390, 1440]) {
  const quality = qualityFor(width, 3);
  const graph = createLattice(quality.columns, quality.rows);
  test(`${width}px: welded carbon lattice and a closed signal route`, () => {
    const degree = new Uint8Array(graph.nodes.length);
    const edgeKeys = new Set<string>();
    for (const { a, b } of graph.edges) {
      degree[a]++;
      degree[b]++;
      assert.notEqual(a, b);
      edgeKeys.add(`${Math.min(a, b)}:${Math.max(a, b)}`);
    }
    assert.equal(edgeKeys.size, graph.edges.length);
    assert.ok([...degree].every((d) => d >= 2 && d <= 3));
    assert.equal(graph.route[0], graph.route.at(-1));
    assert.ok(graph.route.length > quality.columns);
    graph.route.slice(1).forEach((b, i) => {
      const a = graph.route[i];
      assert.ok(edgeKeys.has(`${Math.min(a, b)}:${Math.max(a, b)}`));
    });
  });
  test(`${width}px: active signal stays connected through route recycling`, () => {
    const signal = new Signal(graph);
    for (let time = 0; time < 240; time += 0.53) {
      signal.update(time);
      const active = graph.edges.filter((_, i) => signal.edgeStrength[i] > 0);
      assert.ok(active.length > 0 && active.length < graph.edges.length * 0.1);
      const visited = new Set([active[0].a]);
      for (let i = 0; i < active.length; i++) {
        for (const { a, b } of active)
          if (visited.has(a) || visited.has(b)) {
            visited.add(a);
            visited.add(b);
          }
      }
      assert.ok(active.every(({ a, b }) => visited.has(a) && visited.has(b)));
    }
  });
  test(`${width}px: bounded finite geometry and invisible recycling ends`, () => {
    for (const time of [0, 120, 1000, 100000]) {
      for (const node of graph.nodes) {
        const u = wrap(node.u + time * 0.0035);
        assert.ok(
          positionOnSpiral(u, node.v).every(
            (value) => Number.isFinite(value) && Math.abs(value) < 4,
          ),
        );
        assert.ok(visibility(u) >= 0 && visibility(u) <= 1);
      }
    }
    assert.equal(visibility(0), 0);
    assert.equal(visibility(1), 0);
  });
}
test("mobile reduces geometry, pixel ratio, and render rate", () => {
  const mobile = qualityFor(390, 3);
  const desktop = qualityFor(1440, 3);
  assert.ok(
    mobile.columns * mobile.rows < desktop.columns * desktop.rows * 0.5,
  );
  assert.ok(mobile.pixelRatio < desktop.pixelRatio);
  assert.ok(mobile.sphereSegments < desktop.sphereSegments);
  assert.ok(mobile.fps < desktop.fps);
});
