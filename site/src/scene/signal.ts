import { smoothstep, type Lattice } from "./geometry.ts";

function hash(n: number): number {
  const x = Math.sin(n * 127.1 + 311.7) * 43758.5453;
  return x - Math.floor(x);
}

function noise1(value: number): number {
  const i = Math.floor(value);
  const f = value - i;
  const t = f * f * (3 - 2 * f);
  return hash(i) * (1 - t) + hash(i + 1) * t;
}

function fbm(value: number): number {
  return (
    noise1(value) * 0.57 +
    noise1(value * 2.17 + 7.1) * 0.28 +
    noise1(value * 4.61 + 13.7) * 0.15
  );
}

function modulo(value: number, length: number): number {
  return ((value % length) + length) % length;
}

interface Branch {
  edge: number;
  anchor: number;
}

export interface SignalState {
  progress: number;
}

export class Signal {
  readonly nodeStrength: Float32Array;
  readonly edgeStrength: Float32Array;
  readonly nodeHeat: Float32Array;
  readonly edgeHeat: Float32Array;
  private readonly routeEdges: number[];
  private readonly routeEdgeSet: Set<number>;
  private readonly routeIndex: Map<number, number>;
  private readonly branches: Branch[];
  private readonly lattice: Lattice;
  private readonly shimmerSeed: Float32Array;
  private phase = 0;
  private lastTime = Number.NaN;
  private headIndex = 0;
  private headFraction = 0;
  private pulseEnergy = 0.7;

  constructor(lattice: Lattice) {
    this.lattice = lattice;
    this.nodeStrength = new Float32Array(lattice.nodes.length);
    this.edgeStrength = new Float32Array(lattice.edges.length);
    this.nodeHeat = new Float32Array(lattice.nodes.length);
    this.edgeHeat = new Float32Array(lattice.edges.length);
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
    this.routeEdgeSet = new Set(this.routeEdges);
    this.routeIndex = new Map();
    lattice.route.forEach((node, index) => this.routeIndex.set(node, index));
    this.branches = lattice.edges.flatMap((edge, edgeId) => {
      if (this.routeEdgeSet.has(edgeId)) return [];
      const a = this.routeIndex.get(edge.a);
      const b = this.routeIndex.get(edge.b);
      if (a === undefined) {
        return b === undefined ? [] : [{ edge: edgeId, anchor: b }];
      }
      if (b === undefined) return [{ edge: edgeId, anchor: a }];
      return [];
    });
    this.phase = this.routeEdges.length * 0.55;
  }

  get headNode(): number {
    return this.lattice.route[this.headIndex];
  }

  get headEdge(): number {
    return this.routeEdges[this.headIndex];
  }

  get nextNode(): number {
    return this.lattice.route[(this.headIndex + 1) % this.routeEdges.length];
  }

  get headPositionFraction(): number {
    return this.headFraction;
  }

  get energy(): number {
    return this.pulseEnergy;
  }

  captureState(): SignalState {
    return { progress: this.phase / this.routeEdges.length };
  }

  restore(state: SignalState, seconds: number) {
    const progress = Number.isFinite(state.progress) ? state.progress : 0;
    this.phase = modulo(progress, 1) * this.routeEdges.length;
    this.lastTime = Math.max(0, seconds);
    this.update(this.lastTime);
  }

  update(seconds: number) {
    const count = this.routeEdges.length;
    if (!Number.isFinite(this.lastTime)) {
      this.lastTime = seconds;
    } else {
      const delta = Math.min(0.5, Math.max(0, seconds - this.lastTime));
      this.lastTime = seconds;
      const macro = fbm(seconds * 0.17 + 2.7);
      const surge = fbm(seconds * 0.41 + 11.2);
      const hesitation = smoothstep(0.5, 0.82, macro);
      const stall = smoothstep(0.78, 0.95, noise1(seconds * 0.13 + 51.3));
      const rush = smoothstep(0.64, 0.9, surge);
      const speed = Math.max(
        0,
        Math.min(
          3.2,
          0.25 + macro * 0.78 + rush * 1.4 - hesitation * 0.7 - stall * 1.1,
        ),
      );
      // Cruise, surge and hesitate on a changing envelope, integrated so the
      // pulse never jumps when its pace evolves. Short stalls remain possible.
      const pace = 1.55 + 0.45 * Math.sin(seconds * 0.31);
      this.phase = (this.phase + delta * speed * pace) % count;
    }
    this.headIndex = Math.floor(this.phase) % count;
    this.headFraction = this.phase - Math.floor(this.phase);

    this.nodeStrength.fill(0);
    this.edgeStrength.fill(0);
    this.nodeHeat.fill(0);
    this.edgeHeat.fill(0);

    const macro = fbm(seconds * 0.29 + 8.8);
    const tail = Math.max(
      6,
      count * (0.095 + macro * 0.038 + smoothstep(0.7, 0.92, macro) * 0.018),
    );
    const echoGate =
      smoothstep(0.68, 0.88, noise1(seconds * 0.29 + 31.7)) *
      (0.35 + noise1(seconds * 0.71 + 17.4) * 0.65);
    const echoDistance = 4 + noise1(seconds * 0.37 + 4.8) * 9;
    const echoWidth = 1.1 + noise1(seconds * 0.53 + 29.2) * 1.7;
    this.pulseEnergy = Math.max(
      0.35,
      Math.min(
        1.8,
        0.48 +
          noise1(seconds * 0.83 + 9.1) * 0.34 +
          smoothstep(0.68, 0.94, macro) * 0.55 +
          echoGate * 0.24,
      ),
    );

    this.routeEdges.forEach((edgeId, i) => {
      const distance = modulo(this.phase - i, count);
      const lead = Math.exp(-(distance * distance) / 0.9);
      const body =
        distance <= tail
          ? Math.pow(1 - distance / tail, 1.28) *
            Math.min(1, distance * 1.9)
          : 0;
      const glint = 0.9 + noise1(seconds * 0.9 + this.shimmerSeed[edgeId] * 37) * 0.1;
      const strength = Math.min(1, (lead + body * 0.86) * glint);
      if (distance <= tail) this.edgeStrength[edgeId] = strength;
      const residual = Math.exp(-distance / (tail * 0.72)) * 0.3;
      const heat = Math.max(strength, residual);
      this.edgeHeat[edgeId] = heat;
      const { a, b } = this.lattice.edges[edgeId];
      if (strength > 0) {
        this.nodeStrength[a] = Math.max(this.nodeStrength[a], strength);
        this.nodeStrength[b] = Math.max(this.nodeStrength[b], strength);
      }
      this.nodeHeat[a] = Math.max(this.nodeHeat[a], heat);
      this.nodeHeat[b] = Math.max(this.nodeHeat[b], heat);
    });

    if (echoGate > 0.001) {
      const echoHead = modulo(this.phase - echoDistance, count);
      this.routeEdges.forEach((edgeId, i) => {
        const distance = modulo(echoHead - i, count);
        const echo = echoGate * Math.exp(-(distance * distance) / (echoWidth * echoWidth));
        if (echo <= 0.001) return;
        this.edgeHeat[edgeId] = Math.max(this.edgeHeat[edgeId], echo * 0.95);
        const { a, b } = this.lattice.edges[edgeId];
        this.nodeHeat[a] = Math.max(this.nodeHeat[a], echo);
        this.nodeHeat[b] = Math.max(this.nodeHeat[b], echo);
      });
    }

    const branchGate =
      smoothstep(0.7, 0.9, noise1(seconds * 0.19 + 73.2)) *
      (0.3 + noise1(seconds * 0.47 + 44.1) * 0.7);
    if (branchGate > 0.001) {
      for (const branch of this.branches) {
        const distance = modulo(this.phase - branch.anchor, count);
        if (distance > 8) continue;
        const fork =
          branchGate *
          Math.exp(-distance * 0.48) *
          (0.35 + noise1(seconds * 0.43 + branch.edge * 1.7) * 0.65);
        if (fork <= 0.001) continue;
        this.edgeHeat[branch.edge] = Math.max(this.edgeHeat[branch.edge], fork * 0.78);
        const { a, b } = this.lattice.edges[branch.edge];
        this.nodeHeat[a] = Math.max(this.nodeHeat[a], fork * 0.6);
        this.nodeHeat[b] = Math.max(this.nodeHeat[b], fork * 0.6);
      }
    }
  }
}
