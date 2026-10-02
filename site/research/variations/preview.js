import { createLattice } from "../../src/scene/geometry.ts";
import {
  ACESFilmicToneMapping,
  AdditiveBlending,
  AmbientLight,
  BufferAttribute,
  BufferGeometry,
  Color,
  CylinderGeometry,
  DirectionalLight,
  DynamicDrawUsage,
  Group,
  InstancedBufferAttribute,
  InstancedMesh,
  MeshStandardMaterial,
  Object3D,
  PerspectiveCamera,
  PointLight,
  Points,
  Scene,
  ShaderMaterial,
  SphereGeometry,
  Vector3,
  WebGLRenderer,
} from "three";

const HEAD = [
  [842, 362], [845, 311], [829, 252], [797, 191], [748, 143], [690, 112],
];
const ARC = [
  [690, 112], [594, 100], [505, 126], [423, 171], [347, 234], [292, 314],
  [260, 408], [261, 510], [296, 612], [355, 700], [437, 770], [532, 817],
  [636, 834], [730, 811], [801, 756], [837, 676], [824, 594],
];
const TAIL = [
  [824, 594], [748, 551], [676, 545], [618, 575], [600, 605], [590, 623],
  [570, 615], [548, 595], [509, 588], [475, 566],
];
const BRIDGE = [[430, 528], [520, 486], [640, 452], [760, 420]];
const CX = 546;
const CY = 470;
const SCALE = 2.42 / 388;
const TWO_PI = Math.PI * 2;

const params = new URLSearchParams(location.search);
const stage = document.querySelector("#stage");
const label = document.querySelector("#label");
const sub = document.querySelector("#sub");

const sub3 = (a, b) => new Vector3(a.x - b.x, a.y - b.y, a.z - b.z);
const add3 = (a, b) => new Vector3(a.x + b.x, a.y + b.y, a.z + b.z);
const mul3 = (a, s) => new Vector3(a.x * s, a.y * s, a.z * s);
const dot3 = (a, b) => a.x * b.x + a.y * b.y + a.z * b.z;
const cross3 = (a, b) =>
  new Vector3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);

function chaikin(points, iterations) {
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
}

const toWorld = ([x, y]) => [(x - CX) * SCALE, (CY - y) * SCALE, 0];

const openPath = (() => {
  const pts = chaikin([...HEAD, ...ARC.slice(1), ...TAIL.slice(1)], 3).map(toWorld);
  const out = [];
  const per = 26;
  for (let i = 0; i < pts.length - 1; i++)
    for (let k = 0; k < per; k++) {
      const f = k / per;
      out.push(
        new Vector3(
          pts[i][0] + (pts[i + 1][0] - pts[i][0]) * f,
          pts[i][1] + (pts[i + 1][1] - pts[i][1]) * f,
          0,
        ),
      );
    }
  out.push(new Vector3(pts.at(-1)[0], pts.at(-1)[1], 0));
  return out;
})();

const closedPath = (() => {
  const pts = chaikin(
    [...HEAD, ...ARC.slice(1), ...TAIL.slice(1), ...BRIDGE, HEAD[0]],
    3,
  ).map(toWorld);
  const out = [];
  const per = 22;
  for (let i = 0; i < pts.length - 1; i++)
    for (let k = 0; k < per; k++) {
      const f = k / per;
      out.push(
        new Vector3(
          pts[i][0] + (pts[i + 1][0] - pts[i][0]) * f,
          pts[i][1] + (pts[i + 1][1] - pts[i][1]) * f,
          0,
        ),
      );
    }
  out.push(new Vector3(pts.at(-1)[0], pts.at(-1)[1], 0));
  return out;
})();

function weldedSheet(columns, rows, surface, joined) {
  const graph = createLattice(columns, rows);
  const bonds = graph.edges
    .filter(({ a, b }) => {
      const ua = graph.nodes[a].u;
      const ub = graph.nodes[b].u;
      if (Math.abs(ua - ub) > 0.5) return false;
      return joined ? joined(ua) === joined(ub) : true;
    })
    .map(({ a, b }) => [a, b]);
  return {
    points: graph.nodes.map((node) => surface(node.u, node.v)),
    bonds,
  };
}

const FRONT_START = 0.25;
const FRONT_SPAN = 0.5;
const RAMP_IN = 0.1;
const RAMP_OUT = 0.1;
const clamp01 = (x) => Math.max(0, Math.min(1, x));
const smooth = (a, b, x) => {
  const t = clamp01((x - a) / (b - a));
  return t * t * (3 - 2 * t);
};

/** Front-pass parameter of a loop position, mirrored across the return. */
function loopState(u, closed) {
  if (!closed) return { phi: ((u % 1) + 1) % 1, t: 0 };
  const w = ((u % 1) + 1) % 1;
  if (w >= FRONT_START && w < FRONT_START + FRONT_SPAN)
    return { phi: (w - FRONT_START) / FRONT_SPAN, t: 0 };
  const t = (((w - (FRONT_START + FRONT_SPAN)) % 1) + 1) % 1;
  return { phi: 1 - t, t };
}

function makeLoop(options) {
  const {
    closed = false,
    twist = 0,
    depthHalf = 2.5,
    returnDepth = 1.7,
    fold = 0.45,
    radial = 0,
    returnScale = 1,
  } = options;
  return {
    closed,
    state: (u) => loopState(u, closed),
    point(u, v) {
      const { phi, t } = loopState(u, closed);
      const before = openPath[Math.floor(clamp01(phi - 0.01) * (openPath.length - 1))];
      const after = openPath[Math.floor(clamp01(phi + 0.01) * (openPath.length - 1))];
      const point = openPath[Math.floor(clamp01(phi) * (openPath.length - 1))];
      const tx = after.x - before.x;
      const ty = after.y - before.y;
      const len = Math.hypot(tx, ty) || 1;
      const nx = -ty / len;
      const ny = tx / len;
      const along = (phi - 0.5) * 2;
      const depth =
        Math.sign(along) * Math.pow(Math.abs(along), 1.15) * depthHalf;
      const width = (v * 0.2) / 3.03;
      const phiTwist = twist * TWO_PI * phi;
      const lateral = width * Math.cos(phiTwist);
      const lift = v * 0.26;
      let scale = 1;
      let dz = 0;
      if (closed && t > 0) {
        scale = 1 + (returnScale - 1) * t;
        const ramp = smooth(0, RAMP_IN, t) * (1 - smooth(1 - RAMP_OUT, 1, t));
        const bulge = Math.sin(Math.PI * t) * fold * ramp;
        dz = -returnDepth * ramp;
        return point
          .clone()
          .multiplyScalar(scale)
          .add(new Vector3(nx, ny, 0).multiplyScalar(bulge + radial * ramp))
          .add(new Vector3(0, 0, depth + dz + lift));
      }
      return new Vector3(
        point.x * scale + nx * lateral,
        point.y * scale + ny * lateral,
        depth + dz + lift,
      );
    },
  };
}

function sheetFrom(loop, columns, rows) {
  return weldedSheet(
    columns,
    rows,
    (u, v) => loop.point(u, v),
    loop.closed ? (u) => (loop.state(u).t > 0 ? 1 : 0) : null,
  );
}

function stackSheets(path, layers, spread, samples) {
  const points = [];
  const bonds = [];
  const layersIndex = [];
  for (let l = 0; l < layers; l++) {
    const z = (l / (layers - 1) - 0.5) * spread;
    const scale = 1 - Math.abs(l / (layers - 1) - 0.5) * 0.1;
    const row = [];
    for (let i = 0; i < samples; i++) {
      const p = path[Math.floor((i / samples) * path.length)];
      row.push(points.length);
      points.push(new Vector3(p.x * scale, p.y * scale, z + Math.sin(i * 0.7) * 0.02));
    }
    for (let i = 0; i < samples; i++) bonds.push([row[i], row[(i + 1) % samples]]);
    layersIndex.push(row);
  }
  for (let l = 0; l + 1 < layers; l++)
    for (let i = 0; i < samples; i += 3)
      bonds.push([layersIndex[l][i], layersIndex[l + 1][i]]);
  return { points, bonds, layersIndex };
}

function torusPoints(radius, tube, nu, nv, tilt) {
  const points = [];
  const index = [];
  for (let i = 0; i < nu; i++) {
    index[i] = [];
    for (let j = 0; j < nv; j++) {
      const a = (i / nu) * TWO_PI;
      const b = (j / nv) * TWO_PI;
      const p = new Vector3(
        (radius + tube * Math.cos(b)) * Math.cos(a),
        (radius + tube * Math.cos(b)) * Math.sin(a),
        tube * Math.sin(b) + tilt,
      );
      index[i][j] = points.length;
      points.push(p);
    }
  }
  const bonds = [];
  for (let i = 0; i < nu; i++) {
    const k = (i + 1) % nu;
    for (let j = 0; j < nv; j++) {
      bonds.push([index[i][j], index[k][j]]);
      bonds.push([index[i][j], index[k][(j + 1) % nv]]);
    }
  }
  return { points, bonds };
}

function carbonMaterial() {
  const material = new MeshStandardMaterial({
    color: 0xffffff,
    roughness: 0.3,
    metalness: 0.72,
  });
  material.onBeforeCompile = (shader) => {
    shader.vertexShader =
      "attribute float instanceHeat;\nvarying float vHeat;\n" + shader.vertexShader;
    shader.vertexShader = shader.vertexShader.replace(
      "#include <begin_vertex>",
      "#include <begin_vertex>\nvHeat = instanceHeat;",
    );
    shader.fragmentShader = "varying float vHeat;\n" + shader.fragmentShader;
    shader.fragmentShader = shader.fragmentShader.replace(
      "#include <emissivemap_fragment>",
      "#include <emissivemap_fragment>\n" +
        "vec3 ember = vec3(0.52, 0.075, 0.012);\n" +
        "vec3 amber = vec3(1.0, 0.34, 0.035);\n" +
        "vec3 hotColor = mix(ember, amber, smoothstep(0.38, 1.1, vHeat));\n" +
        "hotColor = mix(hotColor, vec3(1.0, 0.95, 0.78), smoothstep(0.92, 1.32, vHeat));\n" +
        "totalEmissiveRadiance += hotColor * pow(vHeat, 1.6) * 1.15;\n" +
        "vec3 rimView = normalize(vViewPosition);\n" +
        "float rim = pow(1.0 - clamp(dot(normalize(normal), rimView), 0.0, 1.0), 2.8);\n" +
        "totalEmissiveRadiance += rim * vec3(0.12, 0.2, 0.18) * 0.5;",
    );
  };
  return material;
}

class Hero {
  constructor(geometry, options) {
    this.options = options;
    this.group = new Group();
    this.group.rotation.set(-0.18, -0.34, -0.1);
    const count = geometry.points.length;
    this.count = count;
    const sphere = new SphereGeometry(1, 16, 10);
    const cylinder = new CylinderGeometry(1, 1, 1, 8, 1);
    this.nodeHeat = new InstancedBufferAttribute(new Float32Array(count), 1);
    this.bondHeat = new InstancedBufferAttribute(
      new Float32Array(geometry.bonds.length),
      1,
    );
    this.nodeHeat.setUsage(DynamicDrawUsage);
    this.bondHeat.setUsage(DynamicDrawUsage);
    sphere.setAttribute("instanceHeat", this.nodeHeat);
    cylinder.setAttribute("instanceHeat", this.bondHeat);
    this.nodes = new InstancedMesh(sphere, carbonMaterial(), count);
    this.bonds = new InstancedMesh(cylinder, carbonMaterial(), geometry.bonds.length);
    this.nodes.frustumCulled = false;
    this.bonds.frustumCulled = false;
    this.bonds.instanceMatrix.setUsage(DynamicDrawUsage);
    const transform = new Object3D();
    const axis = new Vector3(0, 1, 0);
    const color = new Color();
    const carbon = new Color("#43514d");
    const gold = new Color("#f2bd57");
    geometry.points.forEach((p, i) => {
      const heat = options.heat ? options.heat(p, i) : 0;
      const scale = options.nodeScale(p) * (1 + heat * 0.06);
      transform.position.copy(p);
      transform.quaternion.identity();
      transform.scale.setScalar(scale);
      transform.updateMatrix();
      this.nodes.setMatrixAt(i, transform.matrix);
      color.copy(carbon).lerp(gold, Math.min(1, heat));
      this.nodes.setColorAt(i, color);
      this.nodeHeat.setX(i, Math.min(1, heat));
    });
    geometry.bonds.forEach(([a, b], i) => {
      const dir = sub3(geometry.points[b], geometry.points[a]);
      const len = dir.length() || 0.001;
      transform.position
        .copy(geometry.points[a])
        .add(geometry.points[b])
        .multiplyScalar(0.5);
      transform.quaternion.setFromUnitVectors(axis, dir.normalize());
      transform.scale.set(options.bondScale, len, options.bondScale);
      transform.updateMatrix();
      this.bonds.setMatrixAt(i, transform.matrix);
      const heat = options.heat
        ? Math.min(
            1,
            Math.max(
              options.heat(geometry.points[a]),
              options.heat(geometry.points[b]),
            ),
          )
        : 0;
      color.copy(carbon).lerp(gold, heat);
      this.bonds.setColorAt(i, color);
      this.bondHeat.setX(i, heat);
    });
    const glowGeometry = new BufferGeometry();
    glowGeometry.setAttribute(
      "position",
      new BufferAttribute(new Float32Array(count * 3), 3).setUsage(DynamicDrawUsage),
    );
    glowGeometry.setAttribute(
      "strength",
      new BufferAttribute(new Float32Array(count), 1).setUsage(DynamicDrawUsage),
    );
    const glow = new ShaderMaterial({
      uniforms: { pixelRatio: { value: 1 } },
      vertexShader: `attribute float strength;
        varying float vStrength;
        void main() {
          vStrength = strength;
          vec4 view = modelViewMatrix * vec4(position, 1.0);
          gl_Position = projectionMatrix * view;
          gl_PointSize = min(150.0, 120.0 / max(1.0, -view.z));
        }`,
      fragmentShader: `varying float vStrength;
        void main() {
          float d = length(gl_PointCoord - 0.5) * 2.0;
          float core = exp(-d * d * 10.0);
          float halo = exp(-d * d * 3.2) * (1.0 - smoothstep(0.68, 1.0, d));
          gl_FragColor = vec4(1.0, 0.72, 0.28, (core * 0.8 + halo * 0.6) * vStrength * 0.7);
        }`,
      transparent: true,
      blending: AdditiveBlending,
      depthWrite: false,
    });
    this.halo = new Points(glowGeometry, glow);
    this.halo.frustumCulled = false;
    this.halo.renderOrder = 2;
    const positions = glowGeometry.getAttribute("position");
    const strengths = glowGeometry.getAttribute("strength");
    geometry.points.forEach((p, i) => {
      positions.setXYZ(i, p.x, p.y, p.z);
      const heat = options.heat ? options.heat(p, i) : 0;
      strengths.setX(i, heat);
    });
    positions.needsUpdate = strengths.needsUpdate = true;
    this.pulse = new PointLight(0xffa34a, options.pulseIntensity, 3.2, 2);
    if (options.pulseAt) this.pulse.position.copy(options.pulseAt(geometry.points));
    this.group.add(this.bonds, this.nodes, this.halo, this.pulse);
  }
}

const arcLengthPulse = (points, closed, head, tail) => {
  const n = points.length;
  const last = closed ? n : n - 1;
  return (p) => {
    let best = 0;
    let bestDist = Infinity;
    for (let i = 0; i < n; i++) {
      const d = p.distanceToSquared(points[i]);
      if (d < bestDist) {
        bestDist = d;
        best = i;
      }
    }
    const phase = (best / last) * TWO_PI;
    let d = (phase - head) % TWO_PI;
    if (d < 0) d += TWO_PI;
    if (d > tail) return 0;
    return Math.pow(1 - d / tail, 1.2) * Math.min(1, d * 2.2);
  };
};

const build = {
  closed: () => {
    const geometry = sheetFrom(makeLoop({ closed: true }), 68, 3);
    const heat = arcLengthPulse(geometry.points, true, 0.7, 0.62);
    const heat2 = arcLengthPulse(geometry.points, true, 0.7 + Math.PI, 0.62);
    return {
      geometry,
      options: {
        heat: (p) => Math.max(heat(p), heat2(p)),
        nodeScale: () => 0.043,
        bondScale: 0.015,
        pulseIntensity: 1.5,
        pulseAt: (points) => points[Math.round((0.7 / TWO_PI) * points.length)],
      },
    };
  },
  locked: () => {
    const geometry = sheetFrom(makeLoop({ closed: true, twist: 1 }), 68, 3);
    const heat = arcLengthPulse(geometry.points, true, 0.7, 0.5);
    const heat2 = arcLengthPulse(geometry.points, true, 0.7 + Math.PI, 0.5);
    return {
      geometry,
      options: {
        heat: (p) => Math.max(heat(p), heat2(p)),
        nodeScale: () => 0.043,
        bondScale: 0.015,
        pulseIntensity: 1.7,
        pulseAt: (points) => points[Math.round((0.7 / TWO_PI) * points.length)],
      },
    };
  },
  stack: () => {
    const geometry = stackSheets(closedPath, 5, 2.9, 84);
    const hotLayer = 3;
    const heat = (p) => {
      const t = p.z;
      const layer = Math.round(((t + 1.45) / 2.9) * 4);
      if (layer !== hotLayer) return t > 0.9 ? 0.06 : 0;
      return 0.35 + 0.65 * Math.pow(1 - Math.abs(p.y + 0.5) / 2.2, 1.4);
    };
    return {
      geometry,
      options: {
        heat,
        nodeScale: () => 0.036,
        bondScale: 0.012,
        pulseIntensity: 1.3,
        pulseAt: () => new Vector3(-0.6, 0.1, 0.6),
      },
    };
  },
  fold: () => {
    const geometry = sheetFrom(
      makeLoop({ closed: true, returnDepth: 1.7, fold: 0.45 }),
      68,
      3,
    );
    const heat = arcLengthPulse(geometry.points, true, 0.7, 0.62);
    const heat2 = arcLengthPulse(geometry.points, true, 0.7 + Math.PI, 0.62);
    return {
      geometry,
      options: {
        heat: (p) => Math.max(heat(p), heat2(p)),
        nodeScale: () => 0.043,
        bondScale: 0.015,
        pulseIntensity: 1.5,
        pulseAt: (points) => points[Math.round((0.7 / TWO_PI) * points.length)],
      },
    };
  },
  echo: () => {
    const geometry = sheetFrom(
      makeLoop({ closed: true, returnDepth: 0.9, fold: 0.55, radial: 0.5 }),
      68,
      3,
    );
    const heat = arcLengthPulse(geometry.points, true, 0.7, 0.62);
    const heat2 = arcLengthPulse(geometry.points, true, 0.7 + Math.PI, 0.62);
    return {
      geometry,
      options: {
        heat: (p) => Math.max(heat(p), heat2(p)),
        nodeScale: () => 0.043,
        bondScale: 0.015,
        pulseIntensity: 1.5,
        pulseAt: (points) => points[Math.round((0.7 / TWO_PI) * points.length)],
      },
    };
  },
  composite: () => {
    const shell = sheetFrom(makeLoop({ closed: true, twist: 1 }), 68, 3);
    const inner = torusPoints(0.38, 0.085, 52, 3, 0);
    const points = [...shell.points, ...inner.points];
    const bonds = [...shell.bonds];
    for (const [a, b] of inner.bonds)
      bonds.push([a + shell.points.length, b + shell.points.length]);
    const heat = arcLengthPulse(shell.points, true, 1.9, 0.62);
    const heat2 = arcLengthPulse(shell.points, true, 1.9 + Math.PI, 0.62);
    const innerHeat = (p) => (p.length() < 0.62 ? 0.72 : 0);
    return {
      geometry: { points, bonds },
      options: {
        heat: (p, i) =>
          i < shell.points.length
            ? Math.max(heat(p), heat2(p))
            : innerHeat(p),
        nodeScale: (p) => (p.length() < 0.62 ? 0.03 : 0.042),
        bondScale: 0.014,
        pulseIntensity: 1.6,
        pulseAt: () => new Vector3(0.38, 0, 0),
      },
    };
  },
  core: () => {
    const shell = sheetFrom(makeLoop({ closed: true }), 68, 3);
    const inner = torusPoints(0.38, 0.085, 52, 3, 0);
    const points = [...shell.points, ...inner.points];
    const bonds = [...shell.bonds];
    for (const [a, b] of inner.bonds) bonds.push([a + shell.points.length, b + shell.points.length]);
    const heat = arcLengthPulse(shell.points, true, 1.2, 0.8);
    const innerHeat = (p) => (p.length() < 0.62 ? 0.8 : 0);
    return {
      geometry: { points, bonds },
      options: {
        heat: (p, i) => (i < shell.points.length ? heat(p) * 0.8 : innerHeat(p)),
        nodeScale: (p) => (p.length() < 0.62 ? 0.03 : 0.04),
        bondScale: 0.014,
        pulseIntensity: 1.6,
        pulseAt: () => new Vector3(0.38, 0, 0),
      },
    };
  },
};

const titles = {
  closed: ["V1 · closed G torus", "one loop = one period"],
  locked: ["V2 · phase-locked G torus", "twist k = period as geometry"],
  stack: ["V3 · temporal stack", "layers = graph snapshots over time"],
  core: ["V4 · concentric core", "outer = relations, core = observations"],
  composite: ["V3 · composite", "twisted shell + concentric core"],
  fold: ["A · back-return with side fold", "option 1: hidden return"],
  echo: ["B · visible echo return", "option 1b: return as outer echo"],
};

const key = params.get("v") || "closed";
const built = build[key]();
const badIdx = built.geometry.points.findIndex(
  (p) => !Number.isFinite(p.x + p.y + p.z),
);
{
  const pts = built.geometry.points;
  const n = pts.length;
  const half = 0.2;
  const need = 2 * half + 0.09;
  let minNode = Infinity, minSep = Infinity, maxCoord = 0;
  for (let i = 0; i < n; i++)
    for (let j = i + 1; j < n; j++) {
      const d = pts[i].distanceTo(pts[j]);
      if (d < minNode) minNode = d;
    }
  for (let i = 0; i < n; i++) {
    maxCoord = Math.max(maxCoord, Math.abs(pts[i].x), Math.abs(pts[i].y));
    for (let j = i + 1; j < n; j++) {
      const a = pts[i], b = pts[j];
      const planar = Math.hypot(a.x - b.x, a.y - b.y);
      if (planar < 0.35) {
        const sep = Math.abs(a.z - b.z);
        if (sep < minSep) minSep = sep;
      }
    }
  }
  window.__probe = {
    nodes: n,
    bonds: built.geometry.bonds.length,
    minNodeDistance: +minNode.toFixed(4),
    minStackSeparation: minSep === Infinity ? null : +minSep.toFixed(4),
    required: +need.toFixed(4),
    maxCoord: +maxCoord.toFixed(3),
  };
}
if (badIdx >= 0) {
  const n = built.geometry.points.length;
  window.__nan = {
    badIdx,
    n,
    sample: [0, 0.25, 0.3, 0.5, 0.7, 0.75].map((f) => {
      const p = built.geometry.points[Math.floor(f * n)];
      return [p.x, p.y, p.z];
    }),
  };
}
label.textContent = titles[key][0];
sub.textContent = titles[key][1];

const renderer = new WebGLRenderer({ antialias: true, powerPreference: "low-power" });
renderer.setClearColor(0x101315, 0);
renderer.toneMapping = ACESFilmicToneMapping;
renderer.toneMappingExposure = 0.95;
const camera = new PerspectiveCamera(40, 1, 0.1, 60);
const zoom = Number(params.get("z") || (key === "stack" ? 8.1 : 8.2));
camera.position.set(0, 0, zoom);
camera.lookAt(0, 0, 0);
const scene = new Scene();
scene.add(new AmbientLight(0x9eaaa5, 0.86));
const key1 = new DirectionalLight(0xffdfb2, 2.6);
key1.position.set(-3, 5, 6);
const rim = new DirectionalLight(0x466d70, 1.9);
rim.position.set(4, -2, 3);
scene.add(key1, rim);
const hero = new Hero(built.geometry, built.options);
scene.add(hero.group);
stage.append(renderer.domElement);

function resize() {
  const w = window.innerWidth;
  const h = window.innerHeight;
  renderer.setPixelRatio(Math.min(window.devicePixelRatio, 2));
  renderer.setSize(w, h, false);
  camera.aspect = w / Math.max(h, 1);
  camera.updateProjectionMatrix();
}
window.addEventListener("resize", resize);
resize();
renderer.render(scene, camera);
document.title = titles[key][0];
window.__ready = true;
window.__three = true;
