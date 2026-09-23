import {
  ACESFilmicToneMapping,
  AmbientLight,
  DirectionalLight,
  FogExp2,
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
import { CameraRig } from "./camera";
import { LatticeMesh } from "./lattice-mesh";
import { qualityFor } from "./quality";

export type SceneSection = "hero" | "why" | "use-cases" | "reliability";
export interface SceneController {
  setPaused(paused: boolean): void;
  setSection(section: SceneSection): void;
  dispose(): void;
}

export function createScene(
  host: HTMLElement,
  onFailure: () => void,
): SceneController {
  const renderer = new WebGLRenderer({
    alpha: true,
    antialias: true,
    powerPreference: "low-power",
  });
  renderer.setClearColor(0x101315, 0);
  renderer.toneMapping = ACESFilmicToneMapping;
  renderer.toneMappingExposure = 1.0;
  renderer.debug.onShaderError = () => {
    throw new Error("The lattice shader is unavailable on this device.");
  };
  const scene = new Scene();
  scene.fog = new FogExp2(0x101315, 0.065);
  // IBL carries ambient fill; directionals only shape form and rim.
  // Bake the room asynchronously so createScene returns before SwiftShader stalls.
  const ambient = new AmbientLight(0xcbd5d1, 1.4);
  const key = new DirectionalLight(0xf0f3e6, 2.6);
  key.position.set(-3, 5, 6);
  const rim = new DirectionalLight(0x728d8a, 1.5);
  rim.position.set(4, -2, 3);
  scene.add(ambient, key, rim);
  const camera = new CameraRig();
  const reducedMotion = matchMedia("(prefers-reduced-motion: reduce)");
  const finePointer = matchMedia("(pointer: fine)");
  let quality = qualityFor(window.innerWidth, window.devicePixelRatio);
  let lattice = new LatticeMesh(quality);
  scene.add(lattice.group);
  host.append(renderer.domElement);
  host.dataset.quality = quality.mobile ? "mobile" : "desktop";
  let composer: EffectComposer | undefined;
  let bloomPass: UnrealBloomPass | undefined;
  let composerReady = false;
  let disposed = false;
  let paused = false;
  let visible = true;
  let section: SceneSection = "hero";
  let frame = 0;
  let lastFrame = 0;
  let seconds = 0;

  function bakeEnvironment() {
    if (disposed || scene.environment) return;
    try {
      const pmrem = new PMREMGenerator(renderer);
      const room = new RoomEnvironment();
      const env = pmrem.fromScene(room, 0.04).texture;
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
      ambient.intensity = 0.35;
    } catch {
      // Keep the directional-only fallback if the bake fails on this GPU.
    }
  }

  function syncComposer(width: number, height: number) {
    if (quality.mobile) {
      composer?.dispose();
      composer = undefined;
      bloomPass = undefined;
      composerReady = false;
      return;
    }
    if (!composer) {
      // Created on a later frame: MSAA/half-float targets can stall software GL
      // if they run inside the initial mount path.
      composerReady = false;
      return;
    }
    composer.setSize(width, height);
    bloomPass?.setSize(width, height);
  }

  function ensureComposer(width: number, height: number) {
    if (quality.mobile || composer || disposed) return;
    const size = renderer.getDrawingBufferSize(new Vector2());
    // No MSAA on the composer target: software GL cannot afford it and the
    // renderer's default framebuffer antialias still covers the direct path.
    const target = new WebGLRenderTarget(size.x, size.y, {
      type: HalfFloatType,
    });
    composer = new EffectComposer(renderer, target);
    composer.addPass(new RenderPass(scene, camera.camera));
    bloomPass = new UnrealBloomPass(
      new Vector2(width, height),
      0.32,
      0.5,
      0.78,
    );
    composer.addPass(bloomPass);
    composer.addPass(new OutputPass());
    composerReady = true;
  }

  function scheduleEnhancements() {
    requestAnimationFrame(() => {
      if (disposed) return;
      const { width, height } = host.getBoundingClientRect();
      if (!quality.mobile) ensureComposer(width, height);
      bakeEnvironment();
      if (visible && !document.hidden) draw();
    });
  }

  function draw(delta = 0) {
    const still = paused || reducedMotion.matches;
    camera.update(delta, still);
    if (composerReady && composer && !quality.mobile) {
      composer.render();
    } else {
      renderer.render(scene, camera.camera);
    }
  }

  function running() {
    return (
      !disposed &&
      !paused &&
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
    // One static frame when paused/reduced-motion/visible-but-idle;
    // skip GPU work when the host is offscreen or the tab is hidden.
    if (visible && !document.hidden) draw();
    if (running()) frame = requestAnimationFrame(animate);
  }

  function resize() {
    if (disposed) return;
    const next = qualityFor(window.innerWidth, window.devicePixelRatio);
    if (next.mobile !== quality.mobile) {
      scene.remove(lattice.group);
      lattice.dispose();
      lattice = new LatticeMesh(next);
      lattice.update(seconds);
      scene.add(lattice.group);
      composer?.dispose();
      composer = undefined;
      bloomPass = undefined;
      composerReady = false;
    }
    quality = next;
    host.dataset.quality = quality.mobile ? "mobile" : "desktop";
    renderer.setPixelRatio(quality.pixelRatio);
    const { width, height } = host.getBoundingClientRect();
    renderer.setSize(width, height, false);
    camera.resize(width, height, quality.mobile);
    syncComposer(width, height);
    if (!quality.mobile && !composer) scheduleEnhancements();
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
    composer?.dispose();
    composer = undefined;
    bloomPass = undefined;
    composerReady = false;
    scene.environment?.dispose();
    scene.environment = null;
    scene.clear();
    renderer.dispose();
    renderer.forceContextLoss();
    renderer.domElement.remove();
  }

  try {
    resize();
  } catch (error) {
    dispose();
    throw error;
  }
  // Bake IBL and build the desktop bloom composer only after mount returns,
  // so software GL cannot stall data-renderer=webgl or block input.
  scheduleEnhancements();
  return {
    setPaused(value) {
      paused = value;
      sync();
    },
    // A stable integration point for future scroll-driven scene chapters.
    setSection(value) {
      section = value;
      sync();
    },
    dispose,
  };
}
