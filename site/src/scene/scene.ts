import {
  ACESFilmicToneMapping,
  AmbientLight,
  DepthTexture,
  DirectionalLight,
  HalfFloatType,
  PMREMGenerator,
  Scene,
  Vector2,
  WebGLRenderer,
  WebGLRenderTarget,
} from "three";
import { RoomEnvironment } from "three/examples/jsm/environments/RoomEnvironment.js";
import { EffectComposer } from "three/examples/jsm/postprocessing/EffectComposer.js";
import { OutputPass } from "three/examples/jsm/postprocessing/OutputPass.js";
import { RenderPass } from "three/examples/jsm/postprocessing/RenderPass.js";
import { UnrealBloomPass } from "three/examples/jsm/postprocessing/UnrealBloomPass.js";
import { FocusPass, FOCUS_FORWARD_OFFSET } from "./focus-pass";
import { CameraRig } from "./camera";
import { LatticeMesh } from "./lattice-mesh";
import { qualityFor } from "./quality";
import type { ShapeVariant } from "./geometry";

export type SceneSection = "hero" | "why" | "use-cases" | "reliability";
export interface SceneController {
  setSection(section: SceneSection): void;
  dispose(): void;
}

export function createScene(
  host: HTMLElement,
  onFailure: () => void,
  options: { variant?: ShapeVariant; still?: boolean } = {},
): SceneController {
  const variant = options.variant ?? "fuller";
  host.dataset.shape = variant;
  let quality = qualityFor(window.innerWidth, window.devicePixelRatio, variant);
  host.dataset.quality = quality.mobile ? "mobile" : "desktop";
  const renderer = new WebGLRenderer({
    alpha: true,
    antialias: true,
    powerPreference: "low-power",
  });
  renderer.setClearColor(0x000000, 0);
  renderer.toneMapping = ACESFilmicToneMapping;
  renderer.toneMappingExposure = 0.95;
  renderer.debug.onShaderError = () => {
    throw new Error("The lattice shader is unavailable on this device.");
  };
  const scene = new Scene();
  // IBL carries ambient fill; directionals only shape form and rim.
  // Bake the room asynchronously so createScene returns before SwiftShader stalls.
  const ambient = new AmbientLight(0x9eaaa5, 0.72);
  const key = new DirectionalLight(0xffedda, 2.8);
  key.position.set(-3, 5, 6);
  const rim = new DirectionalLight(0xb5d5ed, 2.3);
  rim.position.set(4, -2, 3);
  scene.add(ambient, key, rim);
  const camera = new CameraRig();
  const reducedMotion = matchMedia("(prefers-reduced-motion: reduce)");
  const finePointer = matchMedia("(pointer: fine)");
  let lattice = new LatticeMesh(quality, variant);
  scene.add(lattice.group);
  host.append(renderer.domElement);
  host.dataset.canvasReady = "false";
  host.parentElement?.setAttribute("data-canvas-ready", "false");
  host.parentElement?.setAttribute("data-renderer", "webgl");
  let composer: EffectComposer | undefined;
  let bloomPass: UnrealBloomPass | undefined;
  let focusPass: FocusPass | undefined;
  let outputPass: OutputPass | undefined;
  let composerReady = false;
  let disposed = false;
  let visible = true;
  let section: SceneSection = "hero";
  let frame = 0;
  let lastFrame = 0;
  let seconds = 0;
  let ready = false;
  let enhancementsScheduled = false;

  function bakeEnvironment() {
    if (disposed || scene.environment) return;
    try {
      const pmrem = new PMREMGenerator(renderer);
      const room = new RoomEnvironment();
      // A sharper studio map gives the polished metal distinct light strips.
      const env = pmrem.fromScene(room, 0.015).texture;
      room.traverse((object) => {
        const mesh = object as {
          geometry?: { dispose(): void };
          material?: { dispose(): void };
        };
        mesh.geometry?.dispose();
        mesh.material?.dispose();
      });
      pmrem.dispose();
      if (disposed) {
        env.dispose();
        return;
      }
      scene.environment = env;
      ambient.intensity = 0.2;
    } catch {
      // Keep the directional-only fallback if the bake fails on this GPU.
    }
  }

  function disposeComposer() {
    focusPass?.dispose();
    focusPass = undefined;
    bloomPass?.dispose();
    bloomPass = undefined;
    outputPass?.dispose();
    outputPass = undefined;
    composer?.dispose();
    composer = undefined;
    composerReady = false;
    host.dataset.enhanced = "false";
    host.dataset.focus = "pending";
  }

  function syncComposer(width: number, height: number) {
    if (!composer) {
      // Created on a later frame: MSAA/half-float targets can stall software GL
      // if they run inside the initial mount path.
      composerReady = false;
      return;
    }
    composer.setPixelRatio(quality.pixelRatio);
    composer.setSize(width, height);
  }

  function ensureComposer(width: number, height: number) {
    if (composer || disposed) return;
    const size = renderer.getDrawingBufferSize(new Vector2());
    // No MSAA on the composer target: software GL cannot afford it and the
    // renderer's default framebuffer antialias still covers the direct path.
    const target = new WebGLRenderTarget(size.x, size.y, {
      type: HalfFloatType,
      depthTexture: new DepthTexture(size.x, size.y),
    });
    composer = new EffectComposer(renderer, target);
    composer.addPass(new RenderPass(scene, camera.camera));
    focusPass = new FocusPass();
    composer.addPass(focusPass);
    if (!quality.mobile) {
      bloomPass = new UnrealBloomPass(
        new Vector2(width, height),
        // Emissive heat already pushes past this threshold; a tighter bloom
        // keeps the signal glow from washing over nearer, darker nodes.
        0.2,
        0.35,
        0.85,
      );
      composer.addPass(bloomPass);
    }
    outputPass = new OutputPass();
    composer.addPass(outputPass);
    composer.setPixelRatio(quality.pixelRatio);
    composer.setSize(width, height);
    composerReady = true;
    host.dataset.focus = quality.mobile ? "mobile" : "desktop";
  }

  function scheduleEnhancements() {
    if (enhancementsScheduled) return;
    enhancementsScheduled = true;
    requestAnimationFrame(() => {
      enhancementsScheduled = false;
      if (disposed || !ready) return;
      const { width, height } = host.getBoundingClientRect();
      ensureComposer(width, height);
      bakeEnvironment();
      // One static frame picks up IBL/focus; the animate loop stays gated by running().
      if (ready && visible && !document.hidden) draw();
      host.dataset.canvasReady = "true";
      host.parentElement?.setAttribute("data-canvas-ready", "true");
      // Tests wait for this before comparing frozen canvas digests.
      host.dataset.enhanced = "true";
      if (running() && !frame) frame = requestAnimationFrame(animate);
    });
  }

  function draw(delta = 0) {
    const still = options.still || reducedMotion.matches;
    camera.update(seconds, delta, Boolean(still));
    if (composerReady && composer && focusPass) {
      // Bias focus slightly toward the viewer. Combined with a smaller
      // aperture, the approaching metal stays legible beyond the screen frame
      // before spreading into foreground defocus near the lens.
      focusPass.setFocus(
        camera.camera.position.length() - FOCUS_FORWARD_OFFSET,
        camera.camera.near,
        camera.camera.far,
      );
      composer.render();
    } else {
      renderer.render(scene, camera.camera);
    }
  }

  function running() {
    return (
      ready &&
      !disposed &&
      !options.still &&
      !reducedMotion.matches &&
      !document.hidden &&
      visible &&
      section === "hero"
    );
  }

  function animate(now: number) {
    if (!running()) {
      frame = 0;
      return;
    }
    frame = requestAnimationFrame(animate);
    if (now - lastFrame < 1000 / quality.fps) return;
    const delta = lastFrame ? Math.min((now - lastFrame) / 1000, 0.08) : 0;
    lastFrame = now;
    seconds += delta;
    lattice.update(seconds);
    draw(delta);
  }

  function sync() {
    cancelAnimationFrame(frame);
    frame = 0;
    lastFrame = 0;
    if (disposed) return;
    // One static frame for reduced motion or visible-but-idle;
    // skip GPU work when the host is offscreen or the tab is hidden.
    if (ready && visible && !document.hidden) draw();
    if (running()) frame = requestAnimationFrame(animate);
  }

  function resize() {
    if (disposed) return;
    const next = qualityFor(window.innerWidth, window.devicePixelRatio, variant);
    if (next.mobile !== quality.mobile) {
      const state = lattice.captureState();
      scene.remove(lattice.group);
      lattice.dispose();
      lattice = new LatticeMesh(next, variant);
      lattice.restoreState(state, seconds);
      scene.add(lattice.group);
      disposeComposer();
    }
    quality = next;
    host.dataset.quality = quality.mobile ? "mobile" : "desktop";
    renderer.setPixelRatio(quality.pixelRatio);
    lattice.setPixelRatio(quality.pixelRatio);
    const { width, height } = host.getBoundingClientRect();
    renderer.setSize(width, height, false);
    camera.resize(width, height, quality.mobile);
    syncComposer(width, height);
    if (!composer) scheduleEnhancements();
    sync();
  }

  function pointer(event: PointerEvent) {
    if (!quality.mobile && finePointer.matches && !reducedMotion.matches) {
      camera.point(
        (event.clientX / window.innerWidth) * 2 - 1,
        -((event.clientY / window.innerHeight) * 2 - 1),
      );
    }
  }
  function resetPointer() {
    camera.point(0, 0);
  }
  function contextLost(event: Event) {
    event.preventDefault();
    dispose();
    onFailure();
  }

  const resizeObserver = new ResizeObserver(resize);
  resizeObserver.observe(host);
  const intersection = new IntersectionObserver(
    (entries) => {
      visible = entries[0].isIntersecting;
      sync();
    },
    { threshold: 0 },
  );
  intersection.observe(host);
  window.addEventListener("pointermove", pointer, { passive: true });
  document.addEventListener("pointerleave", resetPointer);
  window.addEventListener("resize", resize, { passive: true });
  document.addEventListener("visibilitychange", sync);
  reducedMotion.addEventListener("change", sync);
  renderer.domElement.addEventListener("webglcontextlost", contextLost);

  function dispose() {
    if (disposed) return;
    disposed = true;
    cancelAnimationFrame(frame);
    resizeObserver.disconnect();
    intersection.disconnect();
    window.removeEventListener("pointermove", pointer);
    document.removeEventListener("pointerleave", resetPointer);
    window.removeEventListener("resize", resize);
    document.removeEventListener("visibilitychange", sync);
    reducedMotion.removeEventListener("change", sync);
    renderer.domElement.removeEventListener("webglcontextlost", contextLost);
    lattice.dispose();
    disposeComposer();
    host.dataset.canvasReady = "false";
    host.parentElement?.setAttribute("data-canvas-ready", "false");
    scene.environment?.dispose();
    scene.environment = null;
    scene.clear();
    renderer.dispose();
    renderer.forceContextLoss();
    renderer.domElement.remove();
  }

  try {
    resize();
    ready = true;
  } catch (error) {
    dispose();
    throw error;
  }
  // Bake IBL and build the desktop bloom composer only after mount returns,
  // so software GL cannot stall data-renderer=webgl or block input.
  scheduleEnhancements();
  return {
    // A stable integration point for future scroll-driven scene chapters.
    setSection(value) {
      section = value;
      sync();
    },
    dispose,
  };
}
