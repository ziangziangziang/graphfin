import "./style.css";
import "./styles/layout.css";
import { setupLanguage } from "./i18n";
import { qualityFor } from "./scene/quality";
import type { SceneController, SceneSection } from "./scene/scene";

document.documentElement.classList.add("js");
document.querySelector("#year")!.textContent = String(new Date().getFullYear());
const menu = document.querySelector<HTMLButtonElement>("#menu")!;
const navigation = document.querySelector<HTMLElement>("#navigation")!;
const visual = document.querySelector<HTMLElement>("#hero-visual")!;
const sceneHost = document.querySelector<HTMLElement>("#scene-host")!;
let controller: SceneController | undefined;
sceneHost.dataset.shape = "fuller";
visual.dataset.renderer = "webgl";
visual.dataset.canvasReady = "false";
sceneHost.dataset.quality = qualityFor(
  window.innerWidth,
  window.devicePixelRatio,
).mobile
  ? "mobile"
  : "desktop";

setupLanguage(() => {});

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
function fallback() {
  controller = undefined;
  visual.dataset.renderer = "static";
  visual.dataset.canvasReady = "false";
}

let pageActive = true;
let initialization = 0;
async function mountScene() {
  const generation = ++initialization;
  try {
    const { createScene } = await import("./scene/scene");
    if (!pageActive || generation !== initialization) return;
    controller = createScene(sceneHost, fallback);
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
