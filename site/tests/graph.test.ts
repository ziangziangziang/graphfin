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

/** Maximum rendered sphere radius: base scale 0.046 + full signal bump 0.007. */
const NODE_R_MAX = 0.053;
const NODE_DIAMETER = 2 * NODE_R_MAX;

function distance(
  a: readonly [number, number, number],
  b: readonly [number, number, number],
): number {
  return Math.hypot(a[0] - b[0], a[1] - b[1], a[2] - b[2]);
}

/** Conservative NDC project matching CameraRig (fov 37°, lookAt origin). */
function projectedRadius(zDistance: number): number {
  const halfFov = (37 * Math.PI) / 360;
  return NODE_R_MAX / (zDistance * Math.tan(halfFov));
}

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

for (const width of [390, 1440]) {
  const quality = qualityFor(width, 3);
  const graph = createLattice(quality.columns, quality.rows);

  test(`${width}px: visible nodes keep world-space clearance`, () => {
    // Fade >= 0.15 keeps only clearly drawn spheres; require a real gap beyond
    // touching (1.55× diameter) so signal scale-up still cannot interpenetrate.
    const minWorld = 1.55 * NODE_DIAMETER;
    const fadeMin = 0.15;
    for (const time of [0, 40, 80, 120, 160, 200, 240, 280]) {
      const placed = graph.nodes.map((node) => {
        const u = wrap(node.u + time * 0.0035);
        return { p: positionOnSpiral(u, node.v), fade: visibility(u) };
      });
      for (let i = 0; i < placed.length; i++) {
        for (let j = i + 1; j < placed.length; j++) {
          if (placed[i].fade < fadeMin || placed[j].fade < fadeMin) continue;
          const d = distance(placed[i].p, placed[j].p);
          assert.ok(
            d >= minWorld,
            `t=${time} nodes ${i},${j} d=${d.toFixed(4)} < ${minWorld.toFixed(4)}`,
          );
        }
      }
    }
  });

  test(`${width}px: solid nodes do not collide in screen space`, () => {
    // True visual collision = screen-projected disks overlap AND the spheres
    // also overlap in depth (|dz| < diameter). Depth-layered pairs that only
    // occlude each other along the view axis are fine 3D, not collisions.
    const minScreenRatio = 0.9;
    const fadeMin = 0.3;
    const halfFov = (37 * Math.PI) / 360;
    const camZ = 10.3;
    for (const time of [0, 35, 70, 105, 140, 175, 210, 245, 280]) {
      const placed = graph.nodes.map((node) => {
        const u = wrap(node.u + time * 0.0035);
        const p = positionOnSpiral(u, node.v);
        return { p, fade: visibility(u), z: camZ - p[2] };
      });
      for (let i = 0; i < placed.length; i++) {
        for (let j = i + 1; j < placed.length; j++) {
          if (placed[i].fade < fadeMin || placed[j].fade < fadeMin) continue;
          // Fully separated in depth: one sphere is entirely in front of the
          // other — normal occlusion, not an interpenetration.
          if (Math.abs(placed[i].p[2] - placed[j].p[2]) >= NODE_DIAMETER)
            continue;
          const zi = Math.max(placed[i].z, 0.5);
          const zj = Math.max(placed[j].z, 0.5);
          const ri = NODE_R_MAX / (zi * Math.tan(halfFov));
          const rj = NODE_R_MAX / (zj * Math.tan(halfFov));
          const zAvg = (zi + zj) / 2;
          const screen =
            Math.hypot(
              (placed[i].p[0] - placed[j].p[0]) / zAvg,
              (placed[i].p[1] - placed[j].p[1]) / zAvg,
            ) / Math.tan(halfFov);
          const ratio = screen / (ri + rj);
          assert.ok(
            ratio >= minScreenRatio,
            `t=${time} nodes ${i},${j} screenRatio=${ratio.toFixed(3)} < ${minScreenRatio}`,
          );
        }
      }
    }
  });
}
