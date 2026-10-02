import "./styles/comparison.css";
import { createScene, type SceneController } from "./scene/scene";
import type { ShapeVariant } from "./scene/geometry";

const controllers: SceneController[] = [];
const still = new URL(location.href).searchParams.get("still") === "1";
let active = true;

function mount() {
  if (!active) return;
  for (const variant of ["fuller", "ribbon"] as ShapeVariant[]) {
    const host = document.querySelector<HTMLElement>(`#${variant}-host`)!;
    const status = host.parentElement!.querySelector<HTMLElement>(".render-status")!;
    const fallback = () => {
      host.parentElement!.dataset.renderer = "static";
      status.textContent = "Static preview — 3D view unavailable";
    };
    try {
      controllers.push(createScene(host, fallback, { variant, still }));
      status.textContent = "";
    } catch {
      fallback();
    }
  }
}

function dispose() {
  for (const controller of controllers.splice(0)) controller.dispose();
}

mount();
window.addEventListener("pagehide", () => { active = false; dispose(); });
window.addEventListener("pageshow", (event) => {
  if (event.persisted) { active = true; mount(); }
});
if (import.meta.hot) import.meta.hot.dispose(dispose);
