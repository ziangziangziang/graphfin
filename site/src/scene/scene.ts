import {
  AmbientLight,
  DirectionalLight,
  FogExp2,
  Scene,
  WebGLRenderer,
} from "three";
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
    antialias: false,
    powerPreference: "low-power",
  });
  renderer.setClearColor(0x101315, 0);
  renderer.debug.onShaderError = () => {
    throw new Error("The lattice shader is unavailable on this device.");
  };
  const scene = new Scene();
  scene.fog = new FogExp2(0x101315, 0.065);
  const ambient = new AmbientLight(0xcbd5d1, 2.2);
  const key = new DirectionalLight(0xf0f3e6, 3.4);
  key.position.set(-3, 5, 6);
  const rim = new DirectionalLight(0x728d8a, 2.3);
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
  let disposed = false;
  let paused = false;
  let visible = true;
  let section: SceneSection = "hero";
  let frame = 0;
  let lastFrame = 0;
  let seconds = 0;

  function draw(delta = 0) {
    const still = paused || reducedMotion.matches;
    camera.update(delta, still);
    renderer.render(scene, camera.camera);
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
    draw();
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
    }
    quality = next;
    host.dataset.quality = quality.mobile ? "mobile" : "desktop";
    renderer.setPixelRatio(quality.pixelRatio);
    const { width, height } = host.getBoundingClientRect();
    renderer.setSize(width, height, false);
    camera.resize(width, height, quality.mobile);
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
