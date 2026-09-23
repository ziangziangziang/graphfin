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

/** A shallow helix seen almost along its axis; the inward curl suggests the G. */
export function positionOnSpiral(
  u: number,
  v: number,
): [number, number, number] {
  // Slow the angular sweep as the ribbon turns inward, leaving the G's aperture
  // open on the right. The two ends retreat along the depth axis into the fade.
  const angle = 0.15 + u * 7.45 - 3.8 * smoothstep(0.68, 1, u) * (u - 0.68);
  const radius = 2.15 - 1.7 * smoothstep(0.72, 1, u) + v * 0.19;
  return [
    Math.cos(angle) * radius,
    Math.sin(angle) * radius + (u - 0.5) * 0.85,
    (u - 0.5) * 3.0 + v * 0.14 + Math.sin(angle) * 0.22,
  ];
}

export function visibility(u: number): number {
  return smoothstep(0.015, 0.16, u) * (1 - smoothstep(0.78, 0.95, u));
}
