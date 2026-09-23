import "./style.css";
import "./styles/layout.css";
import { setupLanguage } from "./i18n";
import type { SceneController, SceneSection } from "./scene/scene";

document.documentElement.classList.add("js");
document.querySelector("#year")!.textContent = String(new Date().getFullYear());
const menu = document.querySelector<HTMLButtonElement>("#menu")!;
const navigation = document.querySelector<HTMLElement>("#navigation")!;
const motion = document.querySelector<HTMLButtonElement>("#motion")!;
const visual = document.querySelector<HTMLElement>("#hero-visual")!;
const reducedMotion = matchMedia("(prefers-reduced-motion: reduce)");
let controller: SceneController | undefined;
let paused = false;

function updateMotionLabel() {
  motion.dataset.i18n = paused ? "resume" : "pause";
  motion.textContent = language.text(motion.dataset.i18n);
  motion.setAttribute("aria-pressed", String(paused));
  motion.hidden = !controller || reducedMotion.matches;
}
// setupLanguage applies synchronously, before the returned dictionary exists.
const language = setupLanguage(() => queueMicrotask(updateMotionLabel));

menu.hidden = false;
function closeMenu() {
  navigation.dataset.open = "false";
  menu.setAttribute("aria-expanded", "false");
}
menu.addEventListener("click", () => {
  const open = menu.getAttribute("aria-expanded") !== "true";
  navigation.dataset.open = String(open);
  menu.setAttribute("aria-expanded", String(open));
});
navigation.addEventListener("click", (event) => {
  if ((event.target as HTMLElement).closest("a")) closeMenu();
});
document.addEventListener("keydown", (event) => {
  if (event.key === "Escape" && menu.getAttribute("aria-expanded") === "true") {
    closeMenu();
    menu.focus();
  }
});
motion.addEventListener("click", () => {
  paused = !paused;
  controller?.setPaused(paused);
  updateMotionLabel();
});
reducedMotion.addEventListener("change", updateMotionLabel);

function fallback() {
  controller = undefined;
  visual.dataset.renderer = "static";
  updateMotionLabel();
}

let pageActive = true;
let initialization = 0;
async function mountScene() {
  const generation = ++initialization;
  try {
    const { createScene } = await import("./scene/scene");
    if (!pageActive || generation !== initialization) return;
    controller = createScene(document.querySelector("#scene-host")!, fallback);
    controller.setPaused(paused);
    visual.dataset.renderer = "webgl";
    updateMotionLabel();
  } catch {
    fallback();
  }
}
void mountScene();

// Future sections can drive the same scene through this small typed boundary.
const sections = new IntersectionObserver(
  (entries) => {
    for (const entry of entries) {
      if (entry.isIntersecting)
        controller?.setSection(
          (entry.target as HTMLElement).dataset.sceneSection as SceneSection,
        );
    }
  },
  { rootMargin: "-15% 0px -50% 0px" },
);
document
  .querySelectorAll("[data-scene-section]")
  .forEach((section) => sections.observe(section));

window.addEventListener("pagehide", () => {
  pageActive = false;
  controller?.dispose();
  controller = undefined;
  visual.dataset.renderer = "static";
});
window.addEventListener("pageshow", (event) => {
  if (event.persisted) {
    pageActive = true;
    void mountScene();
  }
});
if (import.meta.hot)
  import.meta.hot.dispose(() => {
    pageActive = false;
    controller?.dispose();
    sections.disconnect();
  });
