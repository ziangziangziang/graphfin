import assert from "node:assert/strict";
import { test } from "node:test";
import {
  createLattice,
  crossingScale,
  parameterForFlow,
  positionOnSpiral,
  visibility,
  wrap,
  type ShapeVariant,
} from "../src/scene/geometry.ts";
import { Signal } from "../src/scene/signal.ts";
import { qualityFor } from "../src/scene/quality.ts";
import { CameraRig } from "../src/scene/camera.ts";
import { Vector3 } from "three";

/** Maximum rendered sphere radius: base scale 0.046 + full signal bump 0.007. */
const NODE_R_MAX = 0.053;
const renderedRadius = (u: number) => NODE_R_MAX * crossingScale(u);

function distance(
  a: readonly [number, number, number],
  b: readonly [number, number, number],
): number {
  return Math.hypot(a[0] - b[0], a[1] - b[1], a[2] - b[2]);
}

for (const variant of ["ribbon", "fuller"] as ShapeVariant[]) {
  for (const width of [390, 1440]) {
    const quality = qualityFor(width, 3, variant);
    const graph = createLattice(quality.columns, quality.rows, variant === "fuller");
    test(`${variant} ${width}px: welded carbon lattice and a closed signal route`, () => {
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
    test(`${variant} ${width}px: active signal stays connected through route recycling`, () => {
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
    test(`${variant} ${width}px: bounded geometry through endless recycling`, () => {
      for (const drift of [0, 0.02, 0.23, 0.5, 0.999]) {
        for (const node of graph.nodes) {
          const u = parameterForFlow(wrap(node.u + drift));
          assert.ok(
            positionOnSpiral(u, node.v, variant, quality.rows).every(
              (value, axis) =>
                Number.isFinite(value) &&
                Math.abs(value) < (axis === 2 ? 64 : 4),
            ),
          );
          assert.equal(visibility(u), 1, "transport must never dissolve the material");
        }
      }
      assert.equal(visibility(0), 1);
      assert.equal(visibility(1), 1);
      assert.equal(visibility(0.5), 1);
    });
  }
}

test("both recycling ends stay outside the camera throughout its orbit", () => {
  for (const variant of ["ribbon", "fuller"] as ShapeVariant[]) {
    const quality = qualityFor(1440, 1, variant);
    const camera = new CameraRig();
    camera.resize(780, 700, false);
    for (let seconds = 0; seconds <= 120; seconds += 2) {
      camera.point(Math.sin(seconds), Math.cos(seconds));
      camera.update(seconds, 0.08, false);
      camera.camera.updateMatrixWorld();
      for (const v of [-2.5, 0, 2.5]) {
        const head = new Vector3(...positionOnSpiral(0, v, variant, quality.rows)).applyMatrix4(camera.camera.matrixWorldInverse);
        const arc = new Vector3(...positionOnSpiral(0.45, v, variant, quality.rows)).applyMatrix4(camera.camera.matrixWorldInverse);
        const tail = new Vector3(...positionOnSpiral(0.995, v, variant, quality.rows)).applyMatrix4(camera.camera.matrixWorldInverse);
        assert.ok(head.z > arc.z + 1.5, `${variant}: head must remain in front`);
        assert.ok(arc.z > tail.z + 1.5, `${variant}: inner tip must remain behind`);
        assert.ok(-head.z < camera.camera.near, "forward end has passed the camera");
        assert.ok(-tail.z > camera.camera.far, "rear end is beyond the far clip plane");
      }
    }
  }
});

test("the focused outer loop projects to a round silhouette", () => {
  const camera = new CameraRig();
  camera.resize(700, 700, false);
  camera.update(0, 0, true);
  camera.camera.updateMatrixWorld();
  const points = Array.from({ length: 160 }, (_, i) =>
    new Vector3(...positionOnSpiral(0.11 + i / 159 * 0.71, 0)).project(camera.camera));
  const width = Math.max(...points.map(p => p.x)) - Math.min(...points.map(p => p.x));
  const height = Math.max(...points.map(p => p.y)) - Math.min(...points.map(p => p.y));
  assert.ok(width / height > 0.9 && width / height < 1.1);
});

test("the centerline advances in depth from the central arm to the upper-right arm", () => {
  for (let u = 0.002; u <= 1; u += 0.002) {
    const closer = positionOnSpiral(u - 0.002, 0);
    const farther = positionOnSpiral(u, 0);
    assert.ok(closer[2] > farther[2]);
  }
});

test("material spacing stays even as the upper arm approaches the lens", () => {
  assert.equal(parameterForFlow(0), 0);
  assert.equal(parameterForFlow(1), 1);
  const distances: number[] = [];
  let previous = parameterForFlow(0);
  for (let phase = 0.002; phase < 1; phase += 0.002) {
    const u = parameterForFlow(phase);
    assert.ok(u > previous, "material must never reverse or stall at a resampling boundary");
    const a = positionOnSpiral(previous, 0);
    const b = positionOnSpiral(u, 0);
    if (u < 0.8 && b[2] > 0 && a[2] < 8) distances.push(distance(a, b));
    previous = u;
  }
  assert.ok(distances.length > 100);
  assert.ok(Math.max(...distances) / Math.min(...distances) < 1.15,
    "forward hexagons must not stretch into rods");
});

test("signal state transfers between quality tiers and includes a true stall", () => {
  const desktop = new Signal(createLattice(34, 3));
  for (let time = 0; time <= 80; time += 0.37) desktop.update(time);
  const state = desktop.captureState();
  const mobile = new Signal(createLattice(24, 2));
  mobile.restore(state, 80);
  assert.ok(Math.abs(mobile.captureState().progress - state.progress) < 1e-6);
  let stalled = false;
  let previous = mobile.captureState().progress;
  for (let time = 80.05; time <= 130; time += 0.05) {
    mobile.update(time);
    const current = mobile.captureState().progress;
    if (Math.abs(current - previous) < 1e-8) stalled = true;
    previous = current;
  }
  assert.ok(stalled);
});

for (const variant of ["ribbon", "fuller"] as ShapeVariant[]) {
  test(`${variant}: mobile reduces geometry, pixel ratio, and render rate`, () => {
    const mobile = qualityFor(390, 3, variant);
    const desktop = qualityFor(1440, 3, variant);
    const mobileGraph = createLattice(mobile.columns, mobile.rows, variant === "fuller");
    const desktopGraph = createLattice(desktop.columns, desktop.rows, variant === "fuller");
    assert.ok(mobileGraph.nodes.length < desktopGraph.nodes.length * 0.65);
    assert.ok(mobileGraph.edges.length < desktopGraph.edges.length * 0.65);
    assert.ok(mobile.pixelRatio < desktop.pixelRatio);
    assert.ok(mobile.sphereSegments < desktop.sphereSegments);
    assert.ok(mobile.fps < desktop.fps);
  });
}

for (const variant of ["ribbon", "fuller"] as ShapeVariant[]) {
  for (const width of [390, 1440]) {
    const quality = qualityFor(width, 3, variant);
    const graph = createLattice(quality.columns, quality.rows, variant === "fuller");

    test(`${variant} ${width}px: visible nodes keep world-space clearance`, () => {
      // Require a gap beyond touching so full signal scale-up stays clear.
      for (const drift of [0, 0.02, 0.125, 0.25, 0.5, 0.75, 0.999]) {
        const placed = graph.nodes.map(({ u, v }) => ({
          p: positionOnSpiral(parameterForFlow(wrap(u + drift)), v, variant, quality.rows),
          radius: renderedRadius(parameterForFlow(wrap(u + drift))),
          fade: visibility(parameterForFlow(wrap(u + drift))),
        }));
        for (let i = 0; i < placed.length; i++) {
          for (let j = i + 1; j < placed.length; j++) {
            if (placed[i].fade < 0.25 || placed[j].fade < 0.25) continue;
            const d = distance(placed[i].p, placed[j].p);
            const minWorld = 1.55 * (placed[i].radius + placed[j].radius);
            assert.ok(
              d >= minWorld,
              `nodes ${i},${j} d=${d.toFixed(4)} < ${minWorld.toFixed(4)}`,
            );
          }
        }
      }
    });

    test(`${variant} ${width}px: solid nodes do not collide in screen space`, () => {
      // True visual collision = screen-projected disks overlap AND the spheres
      // also overlap in depth (|dz| < diameter). Depth-layered pairs that only
      // occlude each other along the view axis are fine 3D, not collisions.
      const minScreenRatio = 0.9;
      const halfFov = (40 * Math.PI) / 360;
      const camZ = 10.3;
      for (const drift of [0, 0.02, 0.125, 0.25, 0.5, 0.75, 0.999]) {
        const placed = graph.nodes.map(({ u, v }) => {
          const parameter = parameterForFlow(wrap(u + drift));
          const p = positionOnSpiral(parameter, v, variant, quality.rows);
          return {
            p,
            z: camZ - p[2],
            radius: renderedRadius(parameter),
            fade: visibility(parameter),
          };
        });
        for (let i = 0; i < placed.length; i++) {
          for (let j = i + 1; j < placed.length; j++) {
            if (placed[i].fade < 0.25 || placed[j].fade < 0.25) continue;
            // Fully separated in depth is normal 3D occlusion.
            if (
              Math.abs(placed[i].p[2] - placed[j].p[2]) >=
              placed[i].radius + placed[j].radius
            )
              continue;
            const zi = Math.max(placed[i].z, 0.5);
            const zj = Math.max(placed[j].z, 0.5);
            const ri = placed[i].radius / (zi * Math.tan(halfFov));
            const rj = placed[j].radius / (zj * Math.tan(halfFov));
            const screen =
              Math.hypot(
                placed[i].p[0] / zi - placed[j].p[0] / zj,
                placed[i].p[1] / zi - placed[j].p[1] / zj,
              ) / Math.tan(halfFov);
            const ratio = screen / (ri + rj);
            assert.ok(
              ratio >= minScreenRatio,
              `nodes ${i},${j} screenRatio=${ratio.toFixed(3)} < ${minScreenRatio}`,
            );
          }
        }
      }
    });
  }
}
