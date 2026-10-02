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
  PointLight,
  Points,
  ShaderMaterial,
  SphereGeometry,
  Vector3,
} from "three";
import {
  createLattice,
  crossingScale,
  heroFlaw,
  parameterForFlow,
  positionOnSpiral,
  visibility,
  wrap,
  type ShapeVariant,
} from "./geometry.ts";
import { Signal, type SignalState } from "./signal.ts";
import type { Quality } from "./quality";

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

function clamp(value: number, low = 0, high = 1): number {
  return Math.max(low, Math.min(high, value));
}

export interface LatticeMeshState {
  signal: SignalState;
  drift: number;
  rotation: [number, number, number];
  angularVelocity: [number, number, number];
}

function metalMaterial(mobile = false) {
  const material = new MeshStandardMaterial({
    color: 0xffffff,
    roughness: 0.2,
    metalness: 1,
    transparent: true,
    envMapIntensity: mobile ? 1.25 : 1.4,
  });
  const colorDetail = mobile
    ? "float surface = clamp(vSurface, 0.0, 1.0);\n" +
      "float grain = sin(surface * 83.0 + 1.7) * 0.5 + 0.5;\n" +
      "float wearPatch = smoothstep(0.48, 0.9, grain);\n" +
      "float scratch = smoothstep(0.76, 0.98, 1.0 - grain);\n" +
      "float dust = smoothstep(0.7, 0.98, sin(surface * 17.0 + 4.0) * 0.5 + 0.5);\n" +
      "diffuseColor.rgb *= 0.9 + surface * 0.1;\n" +
      "diffuseColor.rgb *= 1.0 - scratch * 0.035 - dust * 0.02;\n"
    : "float surface = clamp(vSurface, 0.0, 1.0);\n" +
      "float wearPatch = smoothstep(0.54, 0.9, sin(vMaterialPosition.x * 5.1 + vMaterialPosition.y * 3.7 + vMaterialPosition.z * 4.3 + surface * 1.7) * 0.5 + 0.5);\n" +
      "float scratch = smoothstep(0.76, 0.98, sin(vMaterialPosition.y * 220.0 + vMaterialPosition.z * 170.0 + surface * 13.0) * 0.5 + 0.5);\n" +
      "float dust = smoothstep(0.72, 0.98, sin(vMaterialPosition.x * 180.0 + vMaterialPosition.z * 150.0 + surface * 9.0) * 0.5 + 0.5);\n" +
      "diffuseColor.rgb *= 0.9 + surface * 0.1;\n" +
      "diffuseColor.rgb *= 1.0 - scratch * 0.04 - dust * 0.02;\n";
  material.onBeforeCompile = (shader) => {
    shader.vertexShader =
      "attribute float instanceFade;\n" +
      "attribute float instanceHeat;\n" +
      "attribute float instanceSurface;\n" +
      "varying float vFade;\n" +
      "varying float vHeat;\n" +
      "varying float vSurface;\n" +
      (mobile ? "" : "varying vec3 vMaterialPosition;\n") +
      shader.vertexShader;
    shader.vertexShader = shader.vertexShader.replace(
      "#include <begin_vertex>",
      "#include <begin_vertex>\nvFade = instanceFade;\nvHeat = instanceHeat;\nvSurface = instanceSurface;" +
        (mobile ? "" : "\nvMaterialPosition = transformed;"),
    );
    shader.fragmentShader =
      "varying float vFade;\n" +
      "varying float vHeat;\n" +
      "varying float vSurface;\n" +
      (mobile ? "" : "varying vec3 vMaterialPosition;\n") +
      shader.fragmentShader;
    shader.fragmentShader = shader.fragmentShader.replace(
      "#include <color_fragment>",
      "#include <color_fragment>\n" +
        colorDetail +
        "diffuseColor.a *= vFade;\n" +
        "if(diffuseColor.a < 0.003) discard;",
    );
    shader.fragmentShader = shader.fragmentShader.replace(
      "#include <roughnessmap_fragment>",
      "#include <roughnessmap_fragment>\n" +
        "roughnessFactor = clamp(roughnessFactor * (0.86 + surface * 0.28) + scratch * 0.035 + wearPatch * 0.025, 0.16, 0.3);",
    );
    shader.fragmentShader = shader.fragmentShader.replace(
      "#include <metalnessmap_fragment>",
      "#include <metalnessmap_fragment>\n" +
        "metalnessFactor = clamp(metalnessFactor - wearPatch * 0.025, 0.96, 1.0);",
    );
    if (!mobile) {
      shader.fragmentShader = shader.fragmentShader.replace(
        "#include <normal_fragment_maps>",
        "#include <normal_fragment_maps>\n" +
          "normal = normalize(normal + vec3(sin(vMaterialPosition.y * 41.0 + surface * 3.0), sin(vMaterialPosition.z * 53.0 + surface * 5.0), sin(vMaterialPosition.x * 37.0 + surface * 7.0)) * (0.004 + scratch * 0.006));",
      );
    }
    shader.fragmentShader = shader.fragmentShader.replace(
      "#include <emissivemap_fragment>",
      "#include <emissivemap_fragment>\n" +
        "float heat = clamp(vHeat, 0.0, 1.6);\n" +
        "float hot = smoothstep(0.38, 1.1, heat);\n" +
        "float white = smoothstep(0.92, 1.32, heat);\n" +
        "vec3 ember = vec3(0.52, 0.075, 0.012);\n" +
        "vec3 amber = vec3(1.0, 0.34, 0.035);\n" +
        "vec3 hotColor = mix(ember, amber, hot);\n" +
        "hotColor = mix(hotColor, vec3(1.0, 0.95, 0.78), white);\n" +
        "totalEmissiveRadiance += hotColor * pow(heat, 1.45) * (1.1 + white * 2.2);\n" +
        // The inactive metal reflects its studio environment; only signal heat
        // emits light. Painted-on emissive rims flatten the surface response.
        "totalEmissiveRadiance += vec3(0.3, 0.02, 0.0) * pow(heat, 3.0) * 0.7;",
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
  private readonly nodeHeat;
  private readonly bondHeat;
  private readonly nodeSurface;
  private readonly bondSurface;
  private readonly positions: Vector3[];
  private readonly parameters: Float32Array;
  private readonly transform = new Object3D();
  private readonly axis = new Vector3(0, 1, 0);
  private readonly direction = new Vector3();
  private readonly color = new Color();
  private readonly metal = new Color("#899398");
  private readonly gold = new Color("#f2bd57");
  private readonly ember = new Color("#a83b13");
  private readonly pulseLight: PointLight;
  private readonly pulsePosition = new Vector3();
  private readonly rotation = new Vector3(-0.04, -0.08, -0.025);
  private readonly angularVelocity = new Vector3();
  private readonly mobile: boolean;
  private readonly rows: number;
  private readonly variant: ShapeVariant;
  private drift = 0;
  private lastSeconds = 0;

  constructor(
    quality: Quality,
    variant: ShapeVariant = "ribbon",
  ) {
    this.variant = variant;
    this.graph = createLattice(quality.columns, quality.rows, variant === "fuller");
    this.rows = quality.rows;
    this.signal = new Signal(this.graph);
    this.mobile = quality.mobile;
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
    this.nodeHeat = new InstancedBufferAttribute(
      new Float32Array(this.graph.nodes.length),
      1,
    ).setUsage(DynamicDrawUsage);
    this.bondHeat = new InstancedBufferAttribute(
      new Float32Array(this.graph.edges.length),
      1,
    ).setUsage(DynamicDrawUsage);
    this.nodeSurface = new InstancedBufferAttribute(
      new Float32Array(this.graph.nodes.length),
      1,
    );
    this.bondSurface = new InstancedBufferAttribute(
      new Float32Array(this.graph.edges.length),
      1,
    );
    for (let i = 0; i < this.graph.nodes.length; i++) {
      this.nodeSurface.setX(i, hash(i * 5.37 + 1.2));
    }
    for (let i = 0; i < this.graph.edges.length; i++) {
      this.bondSurface.setX(i, hash(i * 4.91 + 8.7));
    }
    sphere.setAttribute("instanceFade", this.nodeFade);
    sphere.setAttribute("instanceHeat", this.nodeHeat);
    sphere.setAttribute("instanceSurface", this.nodeSurface);
    cylinder.setAttribute("instanceFade", this.bondFade);
    cylinder.setAttribute("instanceHeat", this.bondHeat);
    cylinder.setAttribute("instanceSurface", this.bondSurface);
    this.nodes = new InstancedMesh(
      sphere,
      metalMaterial(quality.mobile),
      this.graph.nodes.length,
    );
    this.bonds = new InstancedMesh(
      cylinder,
      metalMaterial(quality.mobile),
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
    glowGeometry.setAttribute(
      "heat",
      new BufferAttribute(
        new Float32Array(this.graph.nodes.length),
        1,
      ).setUsage(DynamicDrawUsage),
    );
    const glowMaterial = new ShaderMaterial({
      uniforms: { pixelRatio: { value: quality.pixelRatio } },
      vertexShader: `attribute float strength;
        attribute float heat;
        varying float vStrength;
        varying float vHeat;
        uniform float pixelRatio;
        void main() {
          vStrength = strength;
          vHeat = heat;
          vec4 view = modelViewMatrix * vec4(position, 1.0);
          gl_Position = projectionMatrix * view;
          float size = 76.0 + 96.0 * clamp(heat, 0.0, 1.4);
          gl_PointSize = min(112.0, size * pixelRatio / max(1.0, -view.z));
        }`,
      fragmentShader: `varying float vStrength;
        varying float vHeat;
        void main() {
          float d = length(gl_PointCoord - .5) * 2.0;
          float core = exp(-d * d * 10.0);
          float halo = exp(-d * d * 3.2) * (1.0 - smoothstep(.68, 1.0, d));
          float hot = smoothstep(.62, 1.2, vHeat);
          vec3 color = mix(vec3(1.0, .18, .018), vec3(1.0, .66, .16), smoothstep(.04, .72, vHeat));
          color = mix(color, vec3(1.0, .94, .76), hot);
          float alpha = (core * .78 + halo * .56) * vStrength * (.55 + hot * .8);
          gl_FragColor = vec4(color * (1.0 + hot * .75), alpha * .7);
        }`,
      transparent: true,
      blending: AdditiveBlending,
      depthWrite: false,
    });
    this.halo = new Points(glowGeometry, glowMaterial);
    this.halo.frustumCulled = false;
    this.halo.renderOrder = 2;
    this.pulseLight = new PointLight(0xffa34a, 0, 3.1, 2);
    this.group.add(this.bonds, this.nodes, this.halo, this.pulseLight);
    this.update(0);
  }

  update(seconds: number) {
    const time = Math.max(0, seconds);
    const delta = Math.min(0.08, Math.max(0, time - this.lastSeconds));
    this.lastSeconds = time;
    this.signal.update(time);
    // u=1 is the far central arm and u=0 is the forward upper-right arm.
    // Keep transporting the lattice toward u=0 even when the signal stalls.
    this.drift = wrap(
      this.drift - delta * (0.0045 + noise1(time * 0.08 + 3.1) * 0.002),
    );
    const targetX = -0.04 + (fbm(time * 0.037 + 4.3) - 0.5) * 0.035;
    const targetY = -0.08 + (fbm(time * 0.029 + 18.1) - 0.5) * 0.045;
    const targetZ = -0.025 + (fbm(time * 0.023 + 31.7) - 0.5) * 0.025;
    const decay = Math.exp(-3.2 * delta);
    this.angularVelocity.x =
      (this.angularVelocity.x + (targetX - this.rotation.x) * 7 * delta) * decay;
    this.angularVelocity.y =
      (this.angularVelocity.y + (targetY - this.rotation.y) * 7 * delta) * decay;
    this.angularVelocity.z =
      (this.angularVelocity.z + (targetZ - this.rotation.z) * 7 * delta) * decay;
    this.rotation.addScaledVector(this.angularVelocity, delta);
    this.group.rotation.set(this.rotation.x, this.rotation.y, this.rotation.z);

    const glowPositions = this.halo.geometry.getAttribute(
      "position",
    ) as BufferAttribute;
    const strengths = this.halo.geometry.getAttribute(
      "strength",
    ) as BufferAttribute;
    const heats = this.halo.geometry.getAttribute("heat") as BufferAttribute;
    const rotationEnergy = this.angularVelocity.length();
    this.graph.nodes.forEach((node, i) => {
      const u = parameterForFlow(wrap(node.u + this.drift));
      this.parameters[i] = u;
      const p = this.positions[i].set(
        ...positionOnSpiral(u, node.v, this.variant, this.rows),
      );
      const surface = this.nodeSurface.getX(i);
      const ox = (hash(i * 1.7) - 0.5) * 0.014 + (surface - 0.5) * 0.004;
      const oy = (hash(i * 2.3 + 5) - 0.5) * 0.014;
      const oz = (hash(i * 3.1 + 9) - 0.5) * 0.011;
      p.x += ox;
      p.y += oy;
      p.z += oz;
      const signal = clamp(this.signal.nodeStrength[i]);
      const heat = clamp(this.signal.nodeHeat[i], 0, 1.6);
      const flex = (fbm(time * 0.18 + i * 0.017) - 0.5) * (0.006 + signal * 0.008);
      const kick = (noise1(time * 0.73 + i * 0.11) - 0.5) * signal * 0.01;
      p.x += flex + kick;
      p.y += (noise1(time * 0.27 + i * 0.071) - 0.5) * (0.004 + signal * 0.006);
      p.z += signal * 0.008 + rotationEnergy * signal * 0.004;
      const flaw = heroFlaw(u);
      p.y += (hash(i * 6.7) - 0.5) * flaw * 0.018;
      p.z -= flaw * 0.006;
      const fade = visibility(u);
      this.transform.position.copy(p);
      const scale = Math.min(
        0.053,
        0.043 + surface * 0.004 + signal * 0.004 + heat * 0.0015,
      );
      this.transform.rotation.set(
        (hash(i * 2.11 + 1) - 0.5) * 0.7,
        (hash(i * 3.17 + 7) - 0.5) * 0.7,
        (hash(i * 4.23 + 13) - 0.5) * 0.7,
      );
      const localScale = crossingScale(u);
      this.transform.scale.set(
        scale * localScale * (0.9 + surface * 0.08),
        scale * localScale * (0.94 + surface * 0.05),
        scale * localScale * (0.88 + surface * 0.1),
      );
      this.transform.updateMatrix();
      this.nodes.setMatrixAt(i, this.transform.matrix);
      const tint = 0.78 + surface * 0.22 - flaw * 0.16;
      this.color
        .copy(this.metal)
        .multiplyScalar(tint)
        .lerp(this.gold, clamp(signal * 0.82 + heat * 0.18));
      this.color.lerp(this.ember, clamp((heat - signal) * 0.22, 0, 0.32));
      this.nodes.setColorAt(i, this.color);
      this.nodeHeat.setX(i, heat);
      this.nodeFade.setX(i, fade);
      glowPositions.setXYZ(i, p.x, p.y, p.z);
      strengths.setX(
        i,
        heat * fade * (0.65 + signal * 0.55) * (0.84 + localScale * 0.16),
      );
      heats.setX(i, heat);
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
      const surface = this.bondSurface.getX(i);
      const radius = 0.015 + surface * 0.0045 + this.signal.edgeHeat[i] * 0.0018;
      this.transform.scale.set(radius, wraps ? 0 : length, radius);
      this.transform.updateMatrix();
      this.bonds.setMatrixAt(i, this.transform.matrix);
      const edgeSignal = clamp(this.signal.edgeStrength[i]);
      const edgeHeat = clamp(this.signal.edgeHeat[i], 0, 1.6);
      const midpointU = (this.parameters[a] + this.parameters[b]) * 0.5;
      const flaw = heroFlaw(midpointU);
      const tint = 0.75 + surface * 0.25 - flaw * 0.14;
      this.color
        .copy(this.metal)
        .multiplyScalar(tint)
        .lerp(this.gold, clamp(edgeSignal * 0.78 + edgeHeat * 0.24));
      this.color.lerp(this.ember, clamp((edgeHeat - edgeSignal) * 0.24, 0, 0.34));
      this.bonds.setColorAt(i, this.color);
      this.bondHeat.setX(i, edgeHeat);
      this.bondFade.setX(i, fade);
    });
    this.nodes.instanceMatrix.needsUpdate =
      this.bonds.instanceMatrix.needsUpdate = true;
    this.nodes.instanceColor!.needsUpdate =
      this.bonds.instanceColor!.needsUpdate = true;
    this.nodeFade.needsUpdate = this.bondFade.needsUpdate = true;
    this.nodeHeat.needsUpdate = this.bondHeat.needsUpdate = true;
    glowPositions.needsUpdate = strengths.needsUpdate = heats.needsUpdate = true;
    this.pulsePosition
      .lerpVectors(
        this.positions[this.signal.headNode],
        this.positions[this.signal.nextNode],
        this.signal.headPositionFraction,
      )
      .z += 0.025;
    const headEdge = this.graph.edges[this.signal.headEdge];
    const headA = headEdge.a;
    const headB = headEdge.b;
    const headWrap = Math.abs(this.parameters[headA] - this.parameters[headB]) > 0.5;
    const headVisibility = headWrap
      ? 0
      : Math.min(this.nodeFade.getX(headA), this.nodeFade.getX(headB));
    this.pulseLight.position.copy(this.pulsePosition);
    this.pulseLight.distance = this.mobile ? 2.4 : 3.1;
    this.pulseLight.intensity =
      (this.mobile ? 1.15 : 2.35) *
      clamp(this.signal.energy, 0, 1.8) *
      headVisibility;
  }

  setPixelRatio(pixelRatio: number) {
    this.halo.material.uniforms.pixelRatio.value = pixelRatio;
  }

  captureState(): LatticeMeshState {
    return {
      signal: this.signal.captureState(),
      drift: this.drift,
      rotation: [this.rotation.x, this.rotation.y, this.rotation.z],
      angularVelocity: [
        this.angularVelocity.x,
        this.angularVelocity.y,
        this.angularVelocity.z,
      ],
    };
  }

  restoreState(state: LatticeMeshState, seconds: number) {
    this.drift = wrap(state.drift);
    this.rotation.set(...state.rotation);
    this.angularVelocity.set(...state.angularVelocity);
    this.lastSeconds = Math.max(0, seconds);
    this.signal.restore(state.signal, this.lastSeconds);
    this.update(this.lastSeconds);
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
