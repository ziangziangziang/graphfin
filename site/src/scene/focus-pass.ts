import {
  AddEquation,
  CustomBlending,
  HalfFloatType,
  LinearMipmapLinearFilter,
  OneFactor,
  ShaderMaterial,
  Texture,
  Vector2,
  WebGLRenderer,
  WebGLRenderTarget,
} from "three";
import { FullScreenQuad, Pass } from "three/examples/jsm/postprocessing/Pass.js";

const vertexShader = `varying vec2 vUv;
  void main() { vUv = uv; gl_Position = vec4(position, 1.0); }`;
const LEVELS = 8;
const BASE_SIGMA = 0.0014;
export const LENS_APERTURE = 0.3;
export const FOCUS_FORWARD_OFFSET = 0.6;

/** Thin-lens image radius, in viewport heights (aperture is in scene units). */
export function confusionRadius(distance: number, focus: number, aperture = LENS_APERTURE) {
  return aperture / (2 * Math.tan(20 * Math.PI / 180)) *
    Math.abs(1 / Math.max(0.1, distance) - 1 / focus);
}

/**
 * Depth-dependent, energy-preserving scattering with a soft aperture profile.
 * Split radiance into overlapping CoC bands, convolve each at its own radius,
 * then sum. Filtering RGBA together spreads silhouettes as well as highlights;
 * there is no sharp centre tap left underneath the blur. Mip prefiltering keeps
 * subpixel wires stable when large blur bands run at a lower resolution.
 */
export class FocusPass extends Pass {
  readonly needsSwap = true;
  private width = 1;
  private height = 1;
  private readonly source = new WebGLRenderTarget(1, 1, {
    type: HalfFloatType,
    depthBuffer: false,
    generateMipmaps: true,
    minFilter: LinearMipmapLinearFilter,
  });
  private readonly layers = Array.from({ length: LEVELS - 1 }, () => ({
    horizontal: new WebGLRenderTarget(1, 1, { type: HalfFloatType, depthBuffer: false }),
    vertical: new WebGLRenderTarget(1, 1, { type: HalfFloatType, depthBuffer: false }),
  }));
  private readonly uniforms = {
    tDiffuse: { value: null as Texture | null },
    tDepth: { value: null as Texture | null },
    nearClip: { value: 0.1 },
    farClip: { value: 40 },
    focus: { value: 10.3 },
    aperture: { value: LENS_APERTURE },
    band: { value: 0 },
  };
  private readonly extract = new ShaderMaterial({
    uniforms: this.uniforms,
    vertexShader,
    fragmentShader: `uniform sampler2D tDiffuse;
      uniform sampler2D tDepth;
      uniform float nearClip, farClip, focus, aperture, band;
      varying vec2 vUv;
      void main() {
        float depth = texture2D(tDepth, vUv).r;
        float distance = (nearClip * farClip) /
          (farClip - (farClip - nearClip) * depth);
        // A soft aperture has sigma = half the thin-lens circle radius.
        float sigma = 0.5 * aperture / (2.0 * tan(radians(20.0))) *
          abs(1.0 / distance - 1.0 / focus);
        float level = sigma < ${BASE_SIGMA} ? sigma / ${BASE_SIGMA} :
          1.0 + log2(sigma / ${BASE_SIGMA});
        level = clamp(level, 0.0, ${LEVELS - 1}.0);
        float weight = max(0.0, 1.0 - abs(level - band));
        // Render targets already contain premultiplied radiance. Multiplying
        // by alpha again would darken thin wires and their defocused fringes.
        gl_FragColor = texture2D(tDiffuse, vUv) * weight;
      }`,
    depthTest: false,
    depthWrite: false,
  });
  private readonly blur = new ShaderMaterial({
    uniforms: {
      tDiffuse: { value: null as Texture | null },
      stepUv: { value: new Vector2() },
    },
    vertexShader,
    fragmentShader: `uniform sampler2D tDiffuse;
      uniform vec2 stepUv;
      varying vec2 vUv;
      void main() {
        vec4 color = vec4(0.0);
        float total = 0.0;
        for (int i = -6; i <= 6; i++) {
          float x = float(i) * 0.5;
          float weight = exp(-0.5 * x * x);
          vec2 uv = vUv + float(i) * stepUv;
          // Zero outside the sensor, never repeat a bright edge texel.
          if (all(greaterThanEqual(uv, vec2(0.0))) &&
              all(lessThanEqual(uv, vec2(1.0)))) {
            color += texture2D(tDiffuse, uv) * weight;
          }
          total += weight;
        }
        gl_FragColor = color / total;
      }`,
    depthTest: false,
    depthWrite: false,
  });
  private readonly add = new ShaderMaterial({
    uniforms: { tDiffuse: { value: null as Texture | null } },
    vertexShader,
    fragmentShader: `uniform sampler2D tDiffuse; varying vec2 vUv;
      void main() { gl_FragColor = texture2D(tDiffuse, vUv); }`,
    transparent: true,
    blending: CustomBlending,
    blendEquation: AddEquation,
    blendSrc: OneFactor,
    blendDst: OneFactor,
    depthTest: false,
    depthWrite: false,
  });
  private readonly quad = new FullScreenQuad(this.extract);

  setFocus(distance: number, nearClip: number, farClip: number) {
    this.uniforms.focus.value = distance;
    this.uniforms.nearClip.value = nearClip;
    this.uniforms.farClip.value = farClip;
  }

  render(renderer: WebGLRenderer, write: WebGLRenderTarget, read: WebGLRenderTarget) {
    this.uniforms.tDiffuse.value = read.texture;
    this.uniforms.tDepth.value = read.depthTexture;
    const output = this.renderToScreen ? null : write;
    const autoClear = renderer.autoClear;
    renderer.autoClear = false;
    renderer.setRenderTarget(output);
    renderer.clear();
    try {
      for (let band = 0; band < LEVELS; band++) {
        this.uniforms.band.value = band;
        this.quad.material = this.extract;
        renderer.setRenderTarget(this.source);
        this.quad.render(renderer);
        let texture = this.source.texture;
        if (band > 0) {
          const sigma = BASE_SIGMA * 2 ** (band - 1) * this.height;
          const { horizontal, vertical } = this.layers[band - 1];
          this.quad.material = this.blur;
          this.blur.uniforms.tDiffuse.value = texture;
          this.blur.uniforms.stepUv.value.set(sigma / (2 * this.width), 0);
          renderer.setRenderTarget(horizontal);
          this.quad.render(renderer);
          this.blur.uniforms.tDiffuse.value = horizontal.texture;
          this.blur.uniforms.stepUv.value.set(0, sigma / (2 * this.height));
          renderer.setRenderTarget(vertical);
          this.quad.render(renderer);
          texture = vertical.texture;
        }
        this.add.uniforms.tDiffuse.value = texture;
        this.quad.material = this.add;
        renderer.setRenderTarget(output);
        this.quad.render(renderer);
      }
    } finally {
      renderer.autoClear = autoClear;
    }
  }

  setSize(width: number, height: number) {
    this.width = width;
    this.height = height;
    this.source.setSize(width, height);
    this.layers.forEach(({ horizontal, vertical }, index) => {
      const sigma = BASE_SIGMA * 2 ** index * height;
      // Keep taps at most one destination texel apart. Wider spacing leaves
      // horizontal/vertical echoes on the thin far-arm bonds.
      const reduction = 2 ** Math.ceil(Math.log2(Math.max(1, sigma / 2)));
      const w = Math.max(1, Math.ceil(width / reduction));
      const h = Math.max(1, Math.ceil(height / reduction));
      horizontal.setSize(w, h);
      vertical.setSize(w, h);
    });
  }

  dispose() {
    this.source.dispose();
    for (const { horizontal, vertical } of this.layers) {
      horizontal.dispose();
      vertical.dispose();
    }
    for (const material of [this.extract, this.blur, this.add]) material.dispose();
    this.quad.dispose();
  }
}
