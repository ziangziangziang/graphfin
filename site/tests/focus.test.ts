import assert from "node:assert/strict";
import { test } from "node:test";
import { confusionRadius, FOCUS_FORWARD_OFFSET } from "../src/scene/focus-pass.ts";

test("focus is sharp, near defocus grows faster than equally distant far defocus", () => {
  assert.equal(confusionRadius(10.3, 10.3), 0);
  assert.ok(confusionRadius(7.3, 10.3) > confusionRadius(13.3, 10.3));
  assert.ok(confusionRadius(4, 10.3) > confusionRadius(7.3, 10.3) * 3);
  assert.equal(confusionRadius(4, 10.3, 0), 0);
});

test("optical spread increases continuously rather than switching blur at a cutoff", () => {
  let previous = 0;
  for (let distance = 10.3; distance >= 1; distance -= 0.01) {
    const radius = confusionRadius(distance, 10.3);
    assert.ok(Number.isFinite(radius) && radius >= previous);
    previous = radius;
  }
  // Defocus at infinity converges; near the lens it continues to expand.
  assert.ok(confusionRadius(10000, 10.3) < 0.1);
  assert.ok(confusionRadius(0.1, 10.3) > 3);
});

test("the approaching arm retains detail before strong defocus near the lens", () => {
  const focus = 10.3 - FOCUS_FORWARD_OFFSET;
  const sigmaPixels = (distance: number) => confusionRadius(distance, focus) * 600 / 2;
  assert.ok(sigmaPixels(10.3) < 1, "middle arc should remain clear");
  assert.ok(sigmaPixels(8.8) < 1.5, "front arm should retain its lattice structure");
  assert.ok(sigmaPixels(7) < 5, "projecting arm should soften gradually");
  assert.ok(sigmaPixels(3) > 25, "geometry near the lens should still defocus");
});
