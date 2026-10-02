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
export function createLattice(
  columns: number,
  rows: number,
  tubular = false,
): Lattice {
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
        const rawY = height * (r + (q % 2) / 2) + Math.sin(angle);
        const y = tubular ? wrap(rawY / (height * rows)) * height * rows : rawY;
        const key = `${x.toFixed(4)},${y.toFixed(4)}`;
        let id = corners.get(key);
        if (id === undefined) {
          id = nodes.length;
          corners.set(key, id);
          nodes.push({
            u: x / period,
            v: y - (height * (rows - (tubular ? 0 : 0.5))) / 2,
          });
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
  // Periodic topology lets lattice sites stream through the G forever. The
  // mesh suppresses whichever bonds currently cross the recycling boundary.
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

export type ShapeVariant = "fuller" | "ribbon";

export const shapeVariant = (value: string | null): ShapeVariant =>
  value === "ribbon" ? "ribbon" : "fuller";

const RADIUS = 2.24;
const ARC_END = 0.82;
const CAMERA_DISTANCE = 10.3;

function centerline(u: number): [number, number] {
  if (u <= ARC_END) {
    const angle = Math.PI / 6 + (u / ARC_END) * (Math.PI * 5 / 3);
    return [Math.cos(angle) * RADIUS, Math.sin(angle) * RADIUS];
  }
  const t = (u - ARC_END) / (1 - ARC_END);
  const s = 1 - t;
  // A broad cubic bend shares the circle's tangent before turning inward.
  const x = Math.cos(Math.PI / 6) * RADIUS;
  const y = -RADIUS / 2;
  return [
    s ** 3 * x + 3 * s * s * t * (x + 0.43) + 3 * s * t * t * 1.62 + t ** 3 * 0.04,
    s ** 3 * y + 3 * s * s * t * (y + 0.745) + 3 * s * t * t * 0.12 + t ** 3 * 0.12,
  ];
}

/** Positive z is toward the lens; decreasing u carries the lattice forward. */
function center(u: number): [number, number, number] {
  const point = centerline(u);
  const depth =
    2.62 * Math.max(0, 1 - u / 0.32) ** 2 -
    2.57 * smoothstep(0.77, 1, u) +
    0.36 * (0.5 - u);
  // Compensate the centerline's perspective stretch, while spheres, ribbon
  // width and overlaps retain real depth. The camera moves around this view.
  const scale = 1 - depth / CAMERA_DISTANCE;
  // Continue past the legible G, through the lens and beyond the rear clip
  // plane. Do not perspective-compensate these extensions: the approaching
  // arm grows naturally and leaves the sensor; the rear arm gets smaller.
  const nearExtension = 18 * Math.max(0, 1 - u / 0.11) ** 3;
  const farExtension = 60 * smoothstep(0.92, 1, u) ** 3;
  return [point[0] * scale, point[1] * scale, depth + nearExtension - farExtension];
}

// Parameter speed is not distance along the metal. Distribute cells by arc
// length through the visible volume, so the foreground extension doesn't pull
// a handful of hexagons into long rods. Outside that volume only a small phase
// budget is needed to recycle the hidden geometry.
const FLOW_SAMPLES = 2048;
const flowDistance = new Float64Array(FLOW_SAMPLES + 1);
const flowPoint = (u: number) => {
  const p = center(u);
  p[2] = Math.max(-6, Math.min(8.5, p[2]));
  return p;
};
let previousFlowPoint = flowPoint(0);
for (let i = 1; i <= FLOW_SAMPLES; i++) {
  const p = flowPoint(i / FLOW_SAMPLES);
  flowDistance[i] = flowDistance[i - 1] + Math.max(
    8 / FLOW_SAMPLES,
    Math.hypot(...p.map((value, axis) => value - previousFlowPoint[axis])),
  );
  previousFlowPoint = p;
}

/** Map periodic material travel to distance along the visible G. */
export function parameterForFlow(phase: number): number {
  const distance = Math.max(0, Math.min(1, phase)) * flowDistance[FLOW_SAMPLES];
  let low = 0;
  let high = FLOW_SAMPLES;
  while (high - low > 1) {
    const middle = (low + high) >>> 1;
    if (flowDistance[middle] < distance) low = middle;
    else high = middle;
  }
  const fraction = (distance - flowDistance[low]) / (flowDistance[high] - flowDistance[low]);
  return (low + fraction) / FLOW_SAMPLES;
}

/** Shared round G with either a welded tube or an open, lightly rolled sheet. */
export function positionOnSpiral(
  u: number,
  v: number,
  variant: ShapeVariant = "ribbon",
  rows = 3,
): [number, number, number] {
  const before = center(Math.max(0, u - 0.001));
  const after = center(Math.min(1, u + 0.001));
  const point = center(u);
  const tangent = after.map((value, axis) => value - before[axis]);
  const length = Math.hypot(...tangent) || 1;
  const [tx, ty, tz] = tangent.map((value) => value / length);
  const planar = Math.hypot(tx, ty) || 1;
  const nx = -ty / planar;
  const ny = tx / planar;
  // N lies in the image plane; B completes an orthonormal 3D sweep frame.
  const bx = -tz * ny;
  const by = tz * nx;
  const bz = tx * ny - ty * nx;
  let lateral: number;
  let lift: number;
  if (variant === "fuller") {
    const angle = (v / (rows * Math.sqrt(3))) * Math.PI * 2;
    lateral = Math.cos(angle) * 0.38;
    lift = Math.sin(angle) * 0.38;
  } else {
    const width = v * 0.145;
    const lean = 0.34 + Math.sin(u * Math.PI * 2) * 0.2;
    lateral = width * Math.cos(lean);
    lift = width * Math.sin(lean);
  }
  const sag = Math.sin(u * 27 + v * 1.7) * 0.004;
  return [
    point[0] + nx * lateral + bx * lift + sag,
    point[1] + ny * lateral + by * lift,
    point[2] + bz * lift + sag * 0.4,
  ];
}

export function crossingScale(u: number): number {
  return 1 - smoothstep(0.85, 1, u) * 0.08;
}

export function heroFlaw(u: number): number {
  const distance = (wrap(u) - 0.64) / 0.115;
  return Math.exp(-(distance * distance));
}

// Material never fades with progress. Recycling happens outside the camera;
// the only visible loss of detail comes from optical defocus and distance.
export function visibility(_u: number): number {
  return 1;
}
