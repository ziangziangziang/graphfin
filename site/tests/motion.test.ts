import assert from "node:assert/strict";
import { test } from "node:test";
import { InstancedMesh, Matrix4 } from "three";
import { LatticeMesh } from "../src/scene/lattice-mesh.ts";
import { qualityFor } from "../src/scene/quality.ts";
import { wrap, type ShapeVariant } from "../src/scene/geometry.ts";

for (const variant of ["fuller", "ribbon"] as ShapeVariant[]) {
  for (const width of [390, 1440]) {
    test(`${variant} ${width}px: lattice flows forward through recycling, stalls, and resize`, () => {
      const lattice = new LatticeMesh(qualityFor(width, 1, variant), variant);
      let resized: LatticeMesh | undefined;
      try {
        // Cross the phase boundary in one ordinary frame. It must transport
        // the same instances forward rather than reset or grow the mesh.
        lattice.restoreState({ ...lattice.captureState(), drift: 0.0001 }, 80);
        const nodes = lattice.group.children.find(
          child => child instanceof InstancedMesh && child.count === lattice.graph.nodes.length,
        ) as InstancedMesh;
        assert.ok(nodes);
        const index = lattice.graph.nodes.reduce(
          (best, node, i) => Math.abs(node.u - 0.09) < Math.abs(lattice.graph.nodes[best].u - 0.09) ? i : best,
          0,
        );
        const before = new Matrix4();
        const after = new Matrix4();
        nodes.getMatrixAt(index, before);
        lattice.update(80.05);
        nodes.getMatrixAt(index, after);
        assert.ok(lattice.captureState().drift > 0.99);
        assert.ok(after.elements[14] > before.elements[14], "lattice must move toward the viewer");

        let previous = lattice.captureState();
        let advancedDuringStall = false;
        for (let seconds = 80.1; seconds <= 130; seconds += 0.05) {
          lattice.update(seconds);
          const current = lattice.captureState();
          const forward = wrap(previous.drift - current.drift);
          assert.ok(forward > 0.00022 && forward < 0.00033);
          if (Math.abs(current.signal.progress - previous.signal.progress) < 1e-8)
            advancedDuringStall = true;
          previous = current;
        }
        assert.ok(advancedDuringStall, "a signal hesitation must not stop the lattice");
        assert.equal(nodes.count, lattice.graph.nodes.length);
        assert.equal(lattice.group.children.length, 4);

        resized = new LatticeMesh(qualityFor(width === 390 ? 1440 : 390, 1, variant), variant);
        resized.restoreState(previous, 130);
        assert.ok(Math.abs(resized.captureState().drift - previous.drift) < 1e-10);
        resized.update(130.05);
        assert.ok(wrap(previous.drift - resized.captureState().drift) > 0);
      } finally {
        lattice.dispose();
        resized?.dispose();
      }
    });
  }
}
