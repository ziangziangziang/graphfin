import { writeFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";
import {
  createLattice,
  crossingScale,
  positionOnSpiral,
  visibility,
} from "../../src/scene/geometry.ts";

const HERE = dirname(fileURLToPath(import.meta.url));

const CAM_Z = 9.8;
const FOV = 40;
const PROJ = 1 / Math.tan((FOV * Math.PI) / 360);
const ROT = [-0.18, -0.26, -0.1];
const HEAD_POINTS = [
  [842, 362],
  [845, 311],
  [829, 252],
  [797, 191],
  [748, 143],
  [690, 112],
];
const ARC_POINTS = [
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
const TAIL_POINTS = [
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
const SKELETON = [...HEAD_POINTS, ...ARC_POINTS.slice(1), ...TAIL_POINTS.slice(1)];
const CX = 546;
const CY = 470;
const LOGO_SCALE = 2.42 / 388;

const sub = (a, b) => [a[0] - b[0], a[1] - b[1], a[2] - b[2]];
const add = (a, b) => [a[0] + b[0], a[1] + b[1], a[2] + b[2]];
const mul = (a, s) => [a[0] * s, a[1] * s, a[2] * s];
const dot = (a, b) => a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
const cross = (a, b) => [
  a[1] * b[2] - a[2] * b[1],
  a[2] * b[0] - a[0] * b[2],
  a[0] * b[1] - a[1] * b[0],
];
const norm = (a) => {
  const l = Math.hypot(a[0], a[1], a[2]) || 1;
  return [a[0] / l, a[1] / l, a[2] / l];
};
const rotateWith = (p, rot) => {
  const [rx, ry, rz] = rot;
  let [x, y, z] = p;
  let t = y * Math.cos(rx) - z * Math.sin(rx);
  z = y * Math.sin(rx) + z * Math.cos(rx);
  y = t;
  t = x * Math.cos(ry) + z * Math.sin(ry);
  z = -x * Math.sin(ry) + z * Math.cos(ry);
  x = t;
  t = x * Math.cos(rz) - y * Math.sin(rz);
  y = x * Math.sin(rz) + y * Math.cos(rz);
  x = t;
  return [x, y, z];
};
const project = (p, scale) => projectWith(p, scale, ROT);
const projectWith = (p, scale, rot) => {
  const v = rotateWith(p, rot);
  const depth = Math.max(0.6, CAM_Z - v[2]);
  return {
    x: (v[0] * PROJ * scale) / depth,
    y: (v[1] * PROJ * scale) / depth,
    depth,
  };
};
const chaikin = (points, iterations) => {
  let current = points.map(([x, y]) => [x, y]);
  for (let i = 0; i < iterations; i++) {
    const next = [current[0]];
    for (let j = 0; j < current.length - 1; j++) {
      const a = current[j];
      const b = current[j + 1];
      next.push([a[0] * 0.75 + b[0] * 0.25, a[1] * 0.75 + b[1] * 0.25]);
      next.push([a[0] * 0.25 + b[0] * 0.75, a[1] * 0.25 + b[1] * 0.75]);
    }
    next.push(current[current.length - 1]);
    current = next;
  }
  return current;
};
const toWorld = ([x, y]) => [(x - CX) * LOGO_SCALE, (CY - y) * LOGO_SCALE, 0];
const f = (n) => n.toFixed(3);

function closedCenters(control, samples) {
  const pts = chaikin(control, 3).map(toWorld);
  const centers = [];
  for (let i = 0; i < samples; i++) {
    const t = (i / samples) * pts.length;
    const k = Math.min(pts.length - 2, Math.floor(t));
    const f0 = t - k;
    centers.push([
      pts[k][0] + (pts[k + 1][0] - pts[k][0]) * f0,
      pts[k][1] + (pts[k + 1][1] - pts[k][1]) * f0,
      0,
    ]);
  }
  return centers;
}

function transportedFrame(centers, twist, halfWidth) {
  const tangents = centers.map((c, i) =>
    norm(
      sub(
        centers[(i + 1) % centers.length],
        centers[(i - 1 + centers.length) % centers.length],
      ),
    ),
  );
  const normals = [];
  const binormals = [];
  let n = norm(cross(tangents[0], [0, 0, 1]));
  let b = norm(cross(tangents[0], n));
  for (let i = 0; i < centers.length; i++) {
    if (i > 0) {
      const v1 = sub(centers[i], centers[i - 1]);
      const c1 = dot(v1, v1) || 1;
      const nL = sub(n, mul(v1, (2 / c1) * dot(v1, n)));
      const tL = sub(tangents[i - 1], mul(v1, (2 / c1) * dot(v1, tangents[i - 1])));
      b = norm(sub(b, mul(nL, 2 * dot(nL, b))));
      n = norm(sub(nL, mul(tL, 2 * dot(tL, nL))));
    }
    normals.push(n);
    binormals.push(b);
  }
  return centers.map((c, i) => {
    const phi = twist * 2 * Math.PI * (i / centers.length);
    return add(
      c,
      add(
        mul(normals[i], Math.cos(phi) * halfWidth),
        mul(binormals[i], Math.sin(phi) * halfWidth),
      ),
    );
  });
}

const items = () => ({ nodes: [], bonds: [] });
const putNode = (bag, p, r, kind) => {
  bag.nodes.push({ p, r, kind });
};
const putBond = (bag, a, b) => {
  bag.bonds.push([a, b]);
};

function currentGBag(scale) {
  const graph = createLattice(34, 3);
  const drift = 0.11;
  const bag = items();
  const info = graph.nodes.map((node) => {
    const u = (node.u + drift) % 1;
    return {
      u,
      p: positionOnSpiral(u, node.v),
      fade: visibility(u),
      r: 0.048 * crossingScale(u),
    };
  });
  for (const { a, b } of graph.edges) {
    if (Math.abs(info[a].u - info[b].u) > 0.5) continue;
    putBond(bag, project(info[a].p, scale), project(info[b].p, scale));
  }
  for (const n of info) {
    const kind =
      n.fade < 0.25
        ? "atomfaint"
        : n.u > 0.3 && n.u < 0.42 && n.fade > 0.2
          ? "hotatom"
          : "atom";
    const s = project(n.p, scale);
    putNode(bag, s, (n.r * PROJ * scale) / s.depth, kind);
  }
  return bag;
}

function proposedGBag(scale, twist = 1) {
  const bridge = [...TAIL_POINTS, [430, 528], [520, 486], [640, 452], [760, 420]];
  const control = [...SKELETON, ...bridge, HEAD_POINTS[0]];
  const centers = closedCenters(control, 132);
  const frame = transportedFrame(centers, twist, 0.17);
  const rows = [-1, 0, 1];
  const bag = items();
  const grid = centers.map((_, i) => {
    const tangent = norm(
      sub(centers[(i + 1) % centers.length], centers[(i - 1 + centers.length) % centers.length]),
    );
    const nrm = norm(cross(tangent, [0, 0, 1]));
    const bnm = norm(cross(tangent, nrm));
    return rows.map((r) =>
      add(frame[i], add(mul(nrm, 0.115 * r), mul(bnm, 0.115 * r))),
    );
  });
  for (let i = 0; i < centers.length; i++) {
    const j = (i + 1) % centers.length;
    for (let k = 0; k < rows.length; k++) {
      putBond(bag, project(grid[i][k], scale), project(grid[j][k], scale));
      if (k === 0) {
        putBond(bag, project(grid[i][k], scale), project(grid[j][1], scale));
        putBond(bag, project(grid[i][k], scale), project(grid[j][2], scale));
      } else {
        putBond(bag, project(grid[i][k], scale), project(grid[j][1], scale));
      }
    }
  }
  for (let i = 0; i < centers.length; i++) {
    const phase = i / centers.length;
    const hot = (phase > 0.14 && phase < 0.26) || (phase > 0.64 && phase < 0.76);
    for (let k = 0; k < rows.length; k++) {
      const s = project(grid[i][k], scale);
      putNode(bag, s, ((hot ? 0.05 : 0.046) * PROJ * scale) / s.depth, hot ? "hotatom" : "atom");
    }
  }
  return bag;
}

function parametricBag(fn, nu, nv, scale, nodeScale = 1, rot = ROT) {
  const bag = items();
  const grid = [];
  for (let i = 0; i < nu; i++) {
    const row = [];
    for (let j = 0; j < nv; j++)
      row.push(
        projectWith(
          fn(i / nu, nv === 1 ? 0 : (j / (nv - 1)) * 2 - 1),
          scale,
          rot,
        ),
      );
    grid.push(row);
  }
  for (let i = 0; i < nu; i++) {
    const j = (i + 1) % nu;
    for (let k = 0; k < nv; k++) {
      putBond(bag, grid[i][k], grid[j][k]);
      if (k > 0) putBond(bag, grid[i][k], grid[j][k - 1]);
    }
  }
  for (let i = 0; i < nu; i++)
    for (let k = 0; k < nv; k++) {
      const r = (0.05 * PROJ * scale * nodeScale) / grid[i][k].depth;
      putNode(bag, grid[i][k], r, "atom");
    }
  return bag;
}

function emitBag(bag, boxW, boxH, nodeMin = 0.012) {
  let minX = Infinity;
  let maxX = -Infinity;
  let minY = Infinity;
  let maxY = -Infinity;
  const all = [...bag.nodes.map((n) => n.p), ...bag.bonds.flat()];
  for (const p of all) {
    minX = Math.min(minX, p.x);
    maxX = Math.max(maxX, p.x);
    minY = Math.min(minY, p.y);
    maxY = Math.max(maxY, p.y);
  }
  const w = Math.max(1e-6, maxX - minX);
  const h = Math.max(1e-6, maxY - minY);
  const s = Math.min(boxW / w, boxH / h);
  const ox = -((minX + maxX) / 2) * s;
  const oy = -((minY + maxY) / 2) * s;
  const T = (p) => `${f(p.x * s + ox)},${f(p.y * s + oy)}`;
  const bonds = bag.bonds
    .slice()
    .sort((p, q) => p[0].depth - q[0].depth)
    .map(
      (b) => `\\draw[carbonbond] (${T(b[0])}) -- (${T(b[1])});`,
    );
  const nodes = bag.nodes
    .slice()
    .sort((p, q) => p.p.depth - q.p.depth)
    .map(
      (n) =>
        `\\fill[${n.kind}] (${T(n.p)}) circle[radius=${f(Math.max(nodeMin, n.r * s))}];`,
    );
  return `${bonds.join("\n")}\n${nodes.join("\n")}`;
}

function emitCurrentG() {
  return emitBag(currentGBag(4), 9.6, 7.2, 0.02);
}

function emitCandidates() {
  const torus = (u, v) => {
    const R = 1.15;
    const r = 0.42;
    const a = 2 * Math.PI * u;
    return [
      (R + r * Math.cos(v)) * Math.cos(a),
      (R + r * Math.cos(v)) * Math.sin(a),
      r * Math.sin(v),
    ];
  };
  const mobius = (u, v) => {
    const R = 1.15;
    const w = 0.42;
    const a = 2 * Math.PI * u;
    return [
      (R + w * v * Math.cos(a / 2)) * Math.cos(a),
      (R + w * v * Math.cos(a / 2)) * Math.sin(a),
      w * v * Math.sin(a / 2),
    ];
  };
  const helix = (u, v) => {
    const R = 0.78;
    const s = v >= 0 ? 1 : -1;
    const phase = 2 * Math.PI * 2.5 * u + s * 0.9;
    return [R * Math.cos(phase), R * Math.sin(phase), (u - 0.5) * 3.0];
  };
  const klein = (u, v) => {
    const R = 1.05;
    const a = 2 * Math.PI * u;
    const b = 2 * Math.PI * v;
    return [
      (R + Math.cos(a / 2) * Math.sin(b) - Math.sin(a / 2) * Math.sin(2 * b)) * Math.cos(a),
      (R + Math.cos(a / 2) * Math.sin(b) - Math.sin(a / 2) * Math.sin(2 * b)) * Math.sin(a),
      Math.sin(a / 2) * Math.sin(b) + Math.cos(a / 2) * Math.sin(2 * b),
    ];
  };
  const globe = (u, v) => {
    const a = 2 * Math.PI * u;
    const p = v * Math.PI * 0.46;
    return [Math.sin(p) * Math.cos(a), Math.cos(p) * 0.95, Math.sin(p) * Math.sin(a)];
  };
  const gyroid = (u, v) => {
    const x = -2.4 + 4.8 * u;
    const y = -1.4 + 2.8 * (v * 0.5 + 0.5);
    const cz = Math.max(-1, Math.min(1, 0.4 - Math.cos(x) - Math.cos(y)));
    return [x, y, Math.acos(cz)];
  };
  const panels = [
    ["(a) G ribbon, current", parametricBag((u, v) => positionOnSpiral(u, v * 3), 92, 4, 1)],
    ["(b) graphene torus", parametricBag(torus, 92, 4, 1)],
    ["(c) M\\\"obius band", parametricBag(mobius, 92, 4, 1)],
    ["(d) double helix", parametricBag(helix, 104, 2, 1)],
    ["(e) Klein bottle", parametricBag(klein, 96, 11, 1, 1, [-0.35, 1.15, 0.15])],
    ["(f) Schwarz P patch", parametricBag(gyroid, 40, 15, 1, 0.55)],
    ["(g) network globe", parametricBag(globe, 84, 9, 1)],
    ["(h) phase-locked G torus", proposedGBag(1, 1)],
  ];
  const cols = 4;
  const cellW = 3.7;
  const cellH = 2.9;
  const body = panels
    .map(([label, bag], i) => {
      const col = i % cols;
      const row = Math.floor(i / cols);
      const x = (col - (cols - 1) / 2) * cellW;
      const y = -row * (cellH + 0.5);
      return `\\node[font=\\scriptsize\\bfseries, text=graphfinink] at (${f(x)},${f(y - cellH / 2 - 0.22)}) {${label}};\n\\begin{scope}[shift={(${f(x)},${f(y)})}]\n${emitBag(bag, cellW - 0.35, cellH, 0.008)}\n\\end{scope}`;
    })
    .join("\n");
  return `\\begin{scope}\n${body}\n\\end{scope}`;
}

function emitStages() {
  const ctrl = SKELETON.map(([x, y]) => [(x - CX) * LOGO_SCALE, (CY - y) * LOGO_SCALE]);
  const smoothed = chaikin(SKELETON, 3).map(([x, y]) => [(x - CX) * LOGO_SCALE, (CY - y) * LOGO_SCALE]);
  let minX = Infinity;
  let maxX = -Infinity;
  let minY = Infinity;
  let maxY = -Infinity;
  for (const [x, y] of [...ctrl, ...smoothed]) {
    minX = Math.min(minX, x);
    maxX = Math.max(maxX, x);
    minY = Math.min(minY, y);
    maxY = Math.max(maxY, y);
  }
  const s = 2.0 / Math.max(maxX - minX, maxY - minY);
  const cx = (minX + maxX) / 2;
  const cy = (minY + maxY) / 2;
  const toC = ([x, y]) => `${f((x - cx) * s)},${f((y - cy) * s)}`;
  const ctrlPath = ctrl.map((p, i) => `${i ? " -- " : ""}(${toC(p)})`).join("");
  const smoothPath = smoothed.map((p, i) => `${i ? " -- " : ""}(${toC(p)})`).join("");
  const grid = [];
  for (let i = 0; i <= 24; i++) {
    const u = i / 24;
    const line = [-1, -0.5, 0, 0.5, 1]
      .map((v) => {
        const p = positionOnSpiral(u, v * 3);
        return `(${f(p[0] * s)},${f(p[1] * s)})`;
      })
      .join(" -- ");
    grid.push(`\\draw[carbonbond, thin] ${line};`);
  }
  const rungs = [];
  for (let i = 0; i <= 24; i++) {
    const u = i / 24;
    const a = positionOnSpiral(u, -3);
    const b = positionOnSpiral(u, 3);
    rungs.push(
      `\\draw[carbonbond, very thin] (${f(a[0] * s)},${f(a[1] * s)}) -- (${f(b[0] * s)},${f(b[1] * s)});`,
    );
  }
  const cellW = 3.72;
  const cell = (code, label, index) => {
    const x = (index - 1.5) * cellW;
    return `\\node[font=\\scriptsize\\bfseries, text=graphfinink] at (${f(x)},-1.95) {${label}};\n\\begin{scope}[shift={(${f(x)},0)}]\n${code}\n\\end{scope}`;
  };
  const stages =
    cell(
      `\\draw[guide] ${ctrlPath};\n${ctrl.map((p) => `\\fill[guidepoint] (${toC(p)}) circle (0.026);`).join("\n")}`,
      "(a) control polygon",
      0,
    ) +
    cell(`\\draw[guide] ${smoothPath};`, "(b) Chaikin, 3 passes", 1) +
    cell(`\\draw[guide] ${smoothPath};\n${grid.join("\n")}\n${rungs.join("\n")}`, "(c) sweep", 2) +
    cell(`\\begin{scope}\n${emitBag(currentGBag(4), 9.0, 6.6, 0.05)}\n\\end{scope}`, "(d) projection", 3);
  return `\\begin{scope}\n${stages}\n\\end{scope}`;
}

const styles = `
\\tikzset{
  carbonbond/.style={draw=graphfinslate, line width=0.18pt, line cap=round},
  atom/.style={fill=graphfincarbon, draw=none},
  atomfaint/.style={fill=graphfinfaint, draw=none},
  hotatom/.style={fill=graphfinhot, draw=graphfingold, line width=0.18pt},
  guide/.style={draw=graphfinline, line width=0.5pt},
  guidepoint/.style={fill=graphfinline, draw=none}
}
`;

const body = `${styles}
\\newcommand{\\CurrentG}{\n${emitCurrentG()}\n}
\\newcommand{\\Candidates}{\n${emitCandidates()}\n}
\\newcommand{\\SkeletonStages}{\n${emitStages()}\n}
`;

const target = join(HERE, "shapes.tex");
writeFileSync(target, body, "utf8");
process.stdout.write(`wrote ${target} (${body.length} bytes)\n`);
