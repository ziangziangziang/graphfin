import { chromium } from "@playwright/test";
import { mkdirSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const HERE = dirname(fileURLToPath(import.meta.url));
const OUT = join(HERE, "renders");
const BASE = process.env.PREVIEW_BASE || "http://127.0.0.1:5173";
const VARIANTS = [
  ["closed", "v1-closed"],
  ["locked", "v2-phase-locked"],
  ["composite", "v3-composite"],
  ["core", "v4-concentric-core"],
  ["fold", "a1-back-return-fold"],
  ["echo", "b1-echo-return"],
];
const SIZES = [
  ["desktop", { width: 1280, height: 900 }],
  ["tiny", { width: 300, height: 520 }],
];

mkdirSync(OUT, { recursive: true });

const browser = await chromium.launch({
  executablePath: process.env.PLAYWRIGHT_CHROMIUM_EXECUTABLE,
  args: ["--use-angle=swiftshader", "--enable-unsafe-swiftshader"],
});
let page = await browser.newPage({ viewport: { width: 1280, height: 900 } });
page.on("pageerror", (error) => console.error("pageerror", error.message));
page.on("console", (message) => {
  if (message.type() === "error") console.error("console", message.text());
});

for (const [size, viewport] of SIZES) {
  for (const [variant, file] of VARIANTS) {
    if (size === "tiny" && !["fold", "echo", "closed"].includes(variant)) continue;
    await page.setViewportSize(viewport);
    await page.goto(`${BASE}/research/variations/preview.html?v=${variant}`, {
      waitUntil: "load",
    });
    await page.waitForFunction(() => window.__ready === true, null, {
      timeout: 60000,
    });
    await page.waitForTimeout(1200);
    await page.evaluate(() => {
      for (const id of ["#label", "#sub"])
        document.querySelector(id).style.visibility = "hidden";
    });
    const out = join(OUT, size === "tiny" ? `${file}-300px.png` : `${file}.png`);
    await page.locator("#stage canvas").screenshot({ path: out });
    console.log("captured", out.split("/").pop());
  }
}

await browser.close();
