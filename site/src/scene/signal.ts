import type { Lattice } from "./geometry.ts";

/** Deterministic hash in [0,1) — same seed always yields the same shimmer. */
function hash(n: number): number {
  const x = Math.sin(n * 127.1 + 311.7) * 43758.5453;
  return x - Math.floor(x);
}

/**
 * One travelling pulse with a breathing tail. Speed and brightness wobble on
 * layered sines plus a stepped hash flicker so the flow feels alive without
 * tearing the route into disconnected islands.
 */
export class Signal {
  readonly nodeStrength: Float32Array;
  readonly edgeStrength: Float32Array;
  private readonly routeEdges: number[];
  private readonly lattice: Lattice;
  private readonly shimmerSeed: Float32Array;

  constructor(lattice: Lattice) {
    this.lattice = lattice;
    this.nodeStrength = new Float32Array(lattice.nodes.length);
    this.edgeStrength = new Float32Array(lattice.edges.length);
    this.shimmerSeed = Float32Array.from(
      { length: lattice.edges.length },
      (_, i) => hash(i * 3.17 + 11.3),
    );
    const lookup = new Map(
      lattice.edges.map((edge, i) => [`${edge.a}:${edge.b}`, i]),
    );
    this.routeEdges = lattice.route.slice(1).map((b, i) => {
      const a = lattice.route[i];
      return lookup.get(`${Math.min(a, b)}:${Math.max(a, b)}`)!;
    });
  }

  update(seconds: number) {
    this.nodeStrength.fill(0);
    this.edgeStrength.fill(0);
    const count = this.routeEdges.length;
    // Base cruise ~2.15 laps-factor with slow/fast surges and a tiny lead-lag.
    const head =
      (count * 0.55 +
        seconds * 2.15 +
        Math.sin(seconds * 0.63) * 1.1 +
        Math.sin(seconds * 1.57) * 0.4) %
      count;
    const tail = Math.max(7, count * (0.125 + 0.028 * Math.sin(seconds * 0.41)));
    // ~7 Hz stepped flicker: organic shimmer without per-frame strobing.
    const tick = Math.floor(seconds * 7);
    this.routeEdges.forEach((edgeId, i) => {
      const distance = (head - i + count) % count;
      if (distance > tail) return;
      const envelope =
        Math.pow(1 - distance / tail, 1.1) * Math.min(1, distance * 2);
      // Multiplicative only — never zeros a mid-tail edge (keeps connectivity).
      const glint = 0.86 + 0.14 * hash(this.shimmerSeed[edgeId] * 97 + tick);
      const strength = envelope * glint;
      this.edgeStrength[edgeId] = strength;
      const { a, b } = this.lattice.edges[edgeId];
      this.nodeStrength[a] = Math.max(this.nodeStrength[a], strength);
      this.nodeStrength[b] = Math.max(this.nodeStrength[b], strength);
    });
  }
}
