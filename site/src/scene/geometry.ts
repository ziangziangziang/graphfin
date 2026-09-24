export interface GraphNode {
  u: number;
  v: number;
}
export interface GraphEdge {
  a: number;
  b: number;
}
export interface Lattice {
  nodes: GraphNode[];
  edges: GraphEdge[];
  route: number[];
}

export const smoothstep = (low: number, high: number, value: number) => {
  const t = Math.max(0, Math.min(1, (value - low) / (high - low)));
  return t * t * (3 - 2 * t);
};
export const wrap = (value: number) => ((value % 1) + 1) % 1;

/** Shared corners are welded before curving the sheet, preserving true hexagons. */
export function createLattice(columns: number, rows: number): Lattice {
  const nodes: GraphNode[] = [];
  const edges: GraphEdge[] = [];
  const corners = new Map<string, number>();
  const bonds = new Set<string>();
  const height = Math.sqrt(3);
  const period = columns * 1.5;
  for (let q = 0; q < columns; q++) {
    for (let r = 0; r < rows; r++) {
      const cell: number[] = [];
      for (let k = 0; k < 6; k++) {
        const angle = (k * Math.PI) / 3;
        const x = (((q * 1.5 + Math.cos(angle)) % period) + period) % period;
        const y = height * (r + (q % 2) / 2) + Math.sin(angle);
        const key = `${x.toFixed(4)},${y.toFixed(4)}`;
        let id = corners.get(key);
        if (id === undefined) {
          id = nodes.length;
          corners.set(key, id);
          nodes.push({ u: x / period, v: y - (height * (rows - 0.5)) / 2 });
        }
        cell.push(id);
      }
      for (let k = 0; k < 6; k++) {
        const a = Math.min(cell[k], cell[(k + 1) % 6]);
        const b = Math.max(cell[k], cell[(k + 1) % 6]);
        const key = `${a}:${b}`;
        if (!bonds.has(key)) {
          bonds.add(key);
          edges.push({ a, b });
        }
      }
    }
  }

  const neighbors: number[][] = nodes.map(() => []);
  for (const { a, b } of edges) {
    neighbors[a].push(b);
    neighbors[b].push(a);
  }
  // BFS supplies a real edge-connected route, never a decorative overlaid curve.
  const start = nodes.reduce(
    (best, node, i) => (node.u < nodes[best].u ? i : best),
    0,
  );
  // Welded longitudinal seam: recycle the sheet forever without a moving gap.
  const end = neighbors[start].find((i) => nodes[i].u - nodes[start].u > 0.5)!;
  const parent = new Int32Array(nodes.length).fill(-1);
  const queue = [start];
  parent[start] = start;
  for (let i = 0; i < queue.length && parent[end] === -1; i++) {
    for (const next of neighbors[queue[i]]) {
      if (parent[next] !== -1) continue;
      if (Math.abs(nodes[next].u - nodes[queue[i]].u) > 0.5) continue;
      parent[next] = queue[i];
      queue.push(next);
    }
  }
  const route = [end];
  while (route.at(-1) !== start) route.push(parent[route.at(-1)!]);
  route.reverse();
  route.push(start);
  return { nodes, edges, route };
}

const HEAD_END = 0.16;
const TAIL_START = 0.82;
const RIBBON_HALF = 0.2;
const SLAB_Z = 0.26;
const V_SCALE = 3.03;
const TAN_EPS = 0.03;
const CHAIKIN_ITERS = 3;
/** Slightly oversized so the G crosses the decorative breakout frame. */
const logoScale = 2.42 / 388;
const logoCenterX = 546;
const logoCenterY = 470;

/** Chaikin-cut the skeleton so head/arc/tail junctions stay tangent-continuous. */
function chaikin(
  points: readonly (readonly [number, number])[],
  iterations: number,
): [number, number][] {
  let current = points.map(([x, y]): [number, number] => [x, y]);
  for (let iter = 0; iter < iterations; iter++) {
    const next: [number, number][] = [current[0]];
    for (let i = 0; i < current.length - 1; i++) {
      const a = current[i];
      const b = current[i + 1];
      next.push([a[0] * 0.75 + b[0] * 0.25, a[1] * 0.75 + b[1] * 0.25]);
      next.push([a[0] * 0.25 + b[0] * 0.75, a[1] * 0.25 + b[1] * 0.75]);
    }
    next.push(current[current.length - 1]);
    current = next;
  }
  return current;
}

// Logo skeleton in SVG space (y-down): defocused head, outer spiral, G tail.
const headPoints: readonly (readonly [number, number])[] = [
  [842, 362],
  [845, 311],
  [829, 252],
  [797, 191],
  [748, 143],
  [690, 112],
];
const arcPoints: readonly (readonly [number, number])[] = [
  [690, 112],
  [594, 100],
  [505, 126],
  [423, 171],
  [347, 234],
  [292, 314],
  [260, 408],
  [261, 510],
  [296, 612],
  [355, 700],
  [437, 770],
  [532, 817],
  [636, 834],
  [730, 811],
  [801, 756],
  [837, 676],
  [824, 594],
];
const tailPoints: readonly (readonly [number, number])[] = [
  [824, 594],
  [748, 551],
  [676, 545],
  [618, 575],
  [600, 605],
  [590, 623],
  [570, 615],
  [548, 595],
  [509, 588],
  [475, 566],
];
const skeletonSvg: readonly (readonly [number, number])[] = [
  ...headPoints,
  ...arcPoints.slice(1),
  ...tailPoints.slice(1),
];

interface LogoPath {
  world: [number, number][];
  cumulative: number[];
  length: number;
  /** Arclength of the original head→arc junction after smoothing. */
  headJoin: number;
  /** Arclength of the original arc→tail junction after smoothing. */
  tailJoin: number;
}

function logoPath(
  points: readonly (readonly [number, number])[],
  joinA: readonly [number, number],
  joinB: readonly [number, number],
): LogoPath {
  const smoothed = chaikin(points, CHAIKIN_ITERS).map(([x, y]): [number, number] => [
    (x - logoCenterX) * logoScale,
    (logoCenterY - y) * logoScale,
  ]);
  const cumulative = [0];
  for (let i = 1; i < smoothed.length; i++) {
    cumulative.push(
      cumulative[i - 1] +
        Math.hypot(
          smoothed[i][0] - smoothed[i - 1][0],
          smoothed[i][1] - smoothed[i - 1][1],
        ),
    );
  }
  const targetJoin = (svg: readonly [number, number]): number => {
    const wx = (svg[0] - logoCenterX) * logoScale;
    const wy = (logoCenterY - svg[1]) * logoScale;
    let best = 0;
    let bestDist = Infinity;
    for (let i = 0; i < smoothed.length; i++) {
      const dist = Math.hypot(smoothed[i][0] - wx, smoothed[i][1] - wy);
      if (dist < bestDist) {
        bestDist = dist;
        best = cumulative[i];
      }
    }
    return best;
  };
  return {
    world: smoothed,
    cumulative,
    length: cumulative[cumulative.length - 1],
    headJoin: targetJoin(joinA),
    tailJoin: targetJoin(joinB),
  };
}

const logo = logoPath(
  skeletonSvg,
  [690, 112],
  [824, 594],
);

function alongPath(path: LogoPath, distance: number): [number, number] {
  const target = Math.min(Math.max(distance, 0), path.length);
  let lo = 0;
  let hi = path.cumulative.length - 1;
  while (lo + 1 < hi) {
    const mid = (lo + hi) >> 1;
    if (path.cumulative[mid] <= target) lo = mid;
    else hi = mid;
  }
  const span = path.cumulative[hi] - path.cumulative[lo];
  const f = span > 0 ? (target - path.cumulative[lo]) / span : 0;
  const a = path.world[lo];
  const b = path.world[hi];
  return [a[0] + (b[0] - a[0]) * f, a[1] + (b[1] - a[1]) * f];
}

function centerline(u: number): [number, number] {
  let distance: number;
  if (u < HEAD_END) {
    distance = (u / HEAD_END) * logo.headJoin;
  } else if (u < TAIL_START) {
    distance =
      logo.headJoin +
      ((u - HEAD_END) / (TAIL_START - HEAD_END)) *
        (logo.tailJoin - logo.headJoin);
  } else {
    distance =
      logo.tailJoin +
      ((u - TAIL_START) / (1 - TAIL_START)) * (logo.length - logo.tailJoin);
  }
  return alongPath(logo, distance);
}

/** GraphFin logo G punching through the hero frame: DOF head, CCW arc, near tail. */
export function positionOnSpiral(
  u: number,
  v: number,
): [number, number, number] {
  const before = centerline(Math.max(0, u - TAN_EPS));
  const after = centerline(Math.min(1, u + TAN_EPS));
  const point = centerline(u);
  const tx = after[0] - before[0];
  const ty = after[1] - before[1];
  const tangent = Math.hypot(tx, ty) || 1;
  const offset = (v * RIBBON_HALF) / V_SCALE;
    // Ease the depth ramp so the visible mid-band stays sharp and the ends
    // accelerate toward/away from the camera for naked-eye parallax.
    const along = (u - 0.5) * 2;
    const depth = Math.sign(along) * Math.pow(Math.abs(along), 1.15) * 3.15;
  return [
    point[0] - (ty / tangent) * offset,
    point[1] + (tx / tangent) * offset,
    depth + v * SLAB_Z,
  ];
}

export function visibility(u: number): number {
  return smoothstep(0.015, 0.16, u) * (1 - smoothstep(0.78, 0.95, u));
}
