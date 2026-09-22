import type { Lattice } from "./geometry.ts";

/** One travelling pulse with a longer fading tail. All active bonds are adjacent. */
export class Signal {
  readonly nodeStrength: Float32Array;
  readonly edgeStrength: Float32Array;
  private readonly routeEdges: number[];
  private readonly lattice: Lattice;

  constructor(lattice: Lattice) {
    this.lattice = lattice;
    this.nodeStrength = new Float32Array(lattice.nodes.length);
    this.edgeStrength = new Float32Array(lattice.edges.length);
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
    const head = (count * 0.55 + seconds * 1.65) % count;
    const tail = Math.max(7, count * 0.13);
    this.routeEdges.forEach((edgeId, i) => {
      const distance = (head - i + count) % count;
      if (distance > tail) return;
      const strength =
        Math.pow(1 - distance / tail, 1.1) * Math.min(1, distance * 2);
      this.edgeStrength[edgeId] = strength;
      const { a, b } = this.lattice.edges[edgeId];
      this.nodeStrength[a] = Math.max(this.nodeStrength[a], strength);
      this.nodeStrength[b] = Math.max(this.nodeStrength[b], strength);
    });
  }
}
