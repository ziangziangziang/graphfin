import {
  AdditiveBlending,
  BufferAttribute,
  BufferGeometry,
  Color,
  CylinderGeometry,
  DynamicDrawUsage,
  Group,
  InstancedBufferAttribute,
  InstancedMesh,
  MeshStandardMaterial,
  Object3D,
  Points,
  ShaderMaterial,
  SphereGeometry,
  Vector3,
} from "three";
import {
  createLattice,
  positionOnSpiral,
  visibility,
  wrap,
} from "./geometry.ts";
import { Signal } from "./signal";
import type { Quality } from "./quality";

/** Cheap deterministic noise for organic, non-repeating wobble. */
function wob(seconds: number, rate: number, phase: number): number {
  return (
    Math.sin(seconds * rate + phase) * 0.62 +
    Math.sin(seconds * rate * 1.73 + phase * 2.1) * 0.38
  );
}

function hash(n: number): number {
  const x = Math.sin(n * 127.1 + 311.7) * 43758.5453;
  return x - Math.floor(x);
}

function carbonMaterial() {
  const material = new MeshStandardMaterial({
    color: 0xffffff,
    // Lower roughness + higher env → wet graphite sheen under the IBL.
    roughness: 0.2,
    metalness: 0.78,
    transparent: true,
    envMapIntensity: 1.55,
  });
  // Per-instance fade hides the wrap boundary. No per-node materials/draw calls.
  material.onBeforeCompile = (shader) => {
    shader.vertexShader =
      "attribute float instanceFade; varying float vFade;\n" +
      shader.vertexShader;
    shader.vertexShader = shader.vertexShader.replace(
      "#include <begin_vertex>",
      "#include <begin_vertex>\nvFade = instanceFade;",
    );
    shader.fragmentShader = "varying float vFade;\n" + shader.fragmentShader;
    shader.fragmentShader = shader.fragmentShader.replace(
      "#include <color_fragment>",
      "#include <color_fragment>\ndiffuseColor.a *= vFade;\nif(diffuseColor.a < 0.003) discard;",
    );
    shader.fragmentShader = shader.fragmentShader.replace(
      "#include <emissivemap_fragment>",
      "#include <emissivemap_fragment>\n" +
        // Gold signal blooms harder; cool base keeps a faint inner glow.
        "totalEmissiveRadiance += vColor.rgb * 0.3 + max(vColor.r - vColor.b, 0.0) * vec3(2.1, 1.25, 0.32);\n" +
        "vec3 rimView = normalize(vViewPosition);\n" +
        "float rim = pow(1.0 - clamp(dot(normalize(normal), rimView), 0.0, 1.0), 2.8);\n" +
        "totalEmissiveRadiance += rim * vec3(0.2, 0.27, 0.25) * (0.35 + 0.65 * vFade);",
    );
  };
  return material;
}

export class LatticeMesh {
  readonly group = new Group();
  readonly graph;
  private readonly signal;
  private readonly nodes;
  private readonly bonds;
  private readonly halo: Points<BufferGeometry, ShaderMaterial>;
  private readonly nodeFade;
  private readonly bondFade;
  private readonly positions: Vector3[];
  private readonly parameters: Float32Array;
  private readonly transform = new Object3D();
  private readonly axis = new Vector3(0, 1, 0);
  private readonly direction = new Vector3();
  private readonly color = new Color();
  private readonly carbon = new Color("#4b5b57");
  private readonly gold = new Color("#ffd16b");

  constructor(quality: Quality) {
    this.graph = createLattice(quality.columns, quality.rows);
    this.signal = new Signal(this.graph);
    const sphere = new SphereGeometry(
      1,
      quality.sphereSegments,
      quality.mobile ? 6 : 8,
    );
    const cylinder = new CylinderGeometry(1, 1, 1, quality.bondSegments, 1);
    this.nodeFade = new InstancedBufferAttribute(
      new Float32Array(this.graph.nodes.length),
      1,
    ).setUsage(DynamicDrawUsage);
    this.bondFade = new InstancedBufferAttribute(
      new Float32Array(this.graph.edges.length),
      1,
    ).setUsage(DynamicDrawUsage);
    sphere.setAttribute("instanceFade", this.nodeFade);
    cylinder.setAttribute("instanceFade", this.bondFade);
    this.nodes = new InstancedMesh(
      sphere,
      carbonMaterial(),
      this.graph.nodes.length,
    );
    this.bonds = new InstancedMesh(
      cylinder,
      carbonMaterial(),
      this.graph.edges.length,
    );
    this.nodes.instanceMatrix.setUsage(DynamicDrawUsage);
    this.bonds.instanceMatrix.setUsage(DynamicDrawUsage);
    this.nodes.frustumCulled = this.bonds.frustumCulled = false;
    this.positions = this.graph.nodes.map(() => new Vector3());
    this.parameters = new Float32Array(this.graph.nodes.length);

    const glowGeometry = new BufferGeometry();
    glowGeometry.setAttribute(
      "position",
      new BufferAttribute(
        new Float32Array(this.graph.nodes.length * 3),
        3,
      ).setUsage(DynamicDrawUsage),
    );
    glowGeometry.setAttribute(
      "strength",
      new BufferAttribute(
        new Float32Array(this.graph.nodes.length),
        1,
      ).setUsage(DynamicDrawUsage),
    );
    const glowMaterial = new ShaderMaterial({
      uniforms: { pixelRatio: { value: quality.pixelRatio } },
      vertexShader: `attribute float strength;
        varying float vStrength; uniform float pixelRatio;
        void main() {
          vStrength = strength;
          vec4 view = modelViewMatrix * vec4(position, 1.0);
          gl_Position = projectionMatrix * view;
          gl_PointSize = min(96.0, 165.0 * pixelRatio / -view.z);
        }`,
      fragmentShader: `varying float vStrength;
        void main() {
          float d = length(gl_PointCoord - .5) * 2.0;
          float core = exp(-d * d * 9.0);
          float halo = exp(-d * d * 3.5) * (1.0 - smoothstep(.7, 1.0, d));
          float alpha = (core * 0.55 + halo * 0.7) * vStrength;
          gl_FragColor = vec4(1.0, 0.72, 0.28, alpha * 0.62);
        }`,
      transparent: true,
      blending: AdditiveBlending,
      depthWrite: false,
    });
    this.halo = new Points(glowGeometry, glowMaterial);
    this.halo.frustumCulled = false;
    this.halo.renderOrder = 2;
    this.group.add(this.bonds, this.nodes, this.halo);
    this.update(0);
  }

  update(seconds: number) {
    this.signal.update(seconds);
    const glowPositions = this.halo.geometry.getAttribute(
      "position",
    ) as BufferAttribute;
    const strengths = this.halo.geometry.getAttribute(
      "strength",
    ) as BufferAttribute;
    // Global conveyor with a slow surge — same phase for every node, so the
    // welded sheet slides as one body (per-node speeds would tear bonds).
    const drift = seconds * 0.0042 + Math.sin(seconds * 0.11) * 0.0035;
    this.graph.nodes.forEach((node, i) => {
      // Bounded conveyor: IDs and buffers persist while positions wrap in the fog.
      const u = wrap(node.u + drift);
      this.parameters[i] = u;
      const p = this.positions[i].set(...positionOnSpiral(u, node.v));
      // Static micro-offset: each atom sits a hair off the ideal lattice —
      // manufacturing imperfection, not animation noise.
      const ox = (hash(i * 1.7) - 0.5) * 0.012;
      const oy = (hash(i * 2.3 + 5) - 0.5) * 0.012;
      const oz = (hash(i * 3.1 + 9) - 0.5) * 0.01;
      p.x += ox;
      p.y += oy;
      p.z += oz;
      const fade = visibility(u);
      const signal = this.signal.nodeStrength[i];
      this.transform.position.copy(p);
      this.transform.quaternion.identity();
      this.transform.scale.setScalar(0.046 + signal * 0.007);
      this.transform.updateMatrix();
      this.nodes.setMatrixAt(i, this.transform.matrix);
      // Slight per-atom albedo drift so the ring is not a flat plastic toy.
      const tint = 0.9 + 0.1 * hash(i * 5.9 + 3);
      this.color
        .copy(this.carbon)
        .multiplyScalar(tint)
        .lerp(this.gold, Math.min(1, signal * 1.05));
      this.nodes.setColorAt(i, this.color);
      this.nodeFade.setX(i, fade);
      glowPositions.setXYZ(i, p.x, p.y, p.z);
      strengths.setX(i, signal * fade);
    });
    this.graph.edges.forEach(({ a, b }, i) => {
      this.direction.subVectors(this.positions[b], this.positions[a]);
      const length = this.direction.length();
      const wraps = Math.abs(this.parameters[a] - this.parameters[b]) > 0.5;
      const fade = wraps
        ? 0
        : Math.min(this.nodeFade.getX(a), this.nodeFade.getX(b));
      this.transform.position
        .copy(this.positions[a])
        .add(this.positions[b])
        .multiplyScalar(0.5);
      this.transform.quaternion.setFromUnitVectors(
        this.axis,
        this.direction.normalize(),
      );
      this.transform.scale.set(0.014, wraps ? 0 : length, 0.014);
      this.transform.updateMatrix();
      this.bonds.setMatrixAt(i, this.transform.matrix);
      const edgeSignal = this.signal.edgeStrength[i];
      const tint = 0.92 + 0.08 * hash(i * 4.3 + 1);
      this.color
        .copy(this.carbon)
        .multiplyScalar(tint)
        .lerp(this.gold, Math.min(1, edgeSignal * 1.1));
      this.bonds.setColorAt(i, this.color);
      this.bondFade.setX(i, fade);
    });
    this.nodes.instanceMatrix.needsUpdate =
      this.bonds.instanceMatrix.needsUpdate = true;
    this.nodes.instanceColor!.needsUpdate =
      this.bonds.instanceColor!.needsUpdate = true;
    this.nodeFade.needsUpdate = this.bondFade.needsUpdate = true;
    glowPositions.needsUpdate = strengths.needsUpdate = true;
    // Layered incommensurate rates → quasi-random tumble that never exactly repeats.
    this.group.rotation.set(
      -0.18 + wob(seconds, 0.035, 0.4) * 0.035,
      -0.26 + wob(seconds, 0.028, 1.7) * 0.055,
      -0.1 + Math.sin(seconds * 0.021 + 2.2) * 0.014,
    );
  }

  dispose() {
    for (const mesh of [this.nodes, this.bonds, this.halo]) {
      mesh.geometry.dispose();
      mesh.material.dispose();
    }
    this.nodes.dispose();
    this.bonds.dispose();
    this.group.clear();
  }
}
