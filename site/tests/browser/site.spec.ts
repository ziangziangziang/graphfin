import { expect, test, type Page } from "@playwright/test";
import { mkdirSync, readdirSync } from "node:fs";
import { resolve } from "node:path";

/** Install a drawElementsInstanced counter used by freeze/offscreen tests. */
async function installDrawCounter(page: Page) {
  await page.addInitScript(() => {
    const draws = { count: 0 };
    Object.assign(window, { graphfinTestDraws: draws });
    const original = WebGL2RenderingContext.prototype.drawElementsInstanced;
    WebGL2RenderingContext.prototype.drawElementsInstanced = function (
      ...args
    ) {
      draws.count++;
      return original.apply(this, args);
    };
  });
}

const drawCount = (page: Page) =>
  page.evaluate(
    () =>
      (window as unknown as { graphfinTestDraws: { count: number } })
        .graphfinTestDraws.count,
  );

/** Wait until IBL/bloom have settled (one static enhancement frame). */
async function waitForEnhanced(page: Page) {
  await expect(page.locator("#scene-host")).toHaveAttribute(
    "data-enhanced",
    "true",
    { timeout: 15000 },
  );
}

test("production subpath, renderer, anchors, language and assets", async ({
  page,
}) => {
  const errors: string[] = [];
  page.on("pageerror", (error) => errors.push(error.message));
  page.on("console", (message) => {
    if (message.type() === "error") errors.push(message.text());
  });
  page.on("response", (response) => {
    if (response.status() >= 400)
      errors.push(`${response.status()} ${response.url()}`);
  });
  await page.goto("./");
  await expect(page.locator("#hero-visual")).toHaveAttribute(
    "data-renderer",
    "webgl",
  );
  await expect(page.locator("#scene-host")).toHaveAttribute(
    "data-quality",
    "desktop",
  );
  await expect(page.locator("#scene-host")).toHaveAttribute("data-shape", "fuller");
  await expect(page.locator(".breakout-shadow")).toHaveCount(0);
  expect(
    await page
      .locator("#scene-host")
      .evaluate((element) => getComputedStyle(element).maskImage),
  ).toBe("none");
  await expect(page.locator("#menu")).toBeHidden();
  await expect(page.locator("h1")).toHaveText(
    "Query connections.Track changes.In one database.",
  );
  await expect(
    page.getByRole("link", { name: "Read the docs" }),
  ).toHaveAttribute("href", /graphfin\/blob\/[^/]+\/docs\/README.md/);
  await page.locator("#language").click();
  await expect(page.locator("html")).toHaveAttribute("lang", "zh-CN");
  await expect(page).toHaveURL(/lang=zh/);
  await expect(page.locator("h1")).toHaveText("关系和变化，一库掌握");
  await page.reload();
  await expect(page.locator("html")).toHaveAttribute("lang", "zh-CN");
  await page.locator("#language").click();
  await page.getByRole("link", { name: "See how it works" }).click();
  await expect(page).toHaveURL(/#why$/);
  await expect(page.locator("#why-title")).toBeInViewport();
  expect(errors).toEqual([]);
});

test("mobile navigation, Chinese layout, and responsive quality", async ({
  page,
}) => {
  await page.setViewportSize({ width: 390, height: 844 });
  await page.goto("./");
  await expect(page.locator("#hero-visual")).toHaveAttribute(
    "data-renderer",
    "webgl",
  );
  await expect(page.locator("#scene-host")).toHaveAttribute(
    "data-quality",
    "mobile",
  );
  await expect(page.locator("nav")).toBeHidden();
  await page.locator("#menu").click();
  await expect(page.locator("nav")).toBeVisible();
  await page.keyboard.press("Escape");
  await expect(page.locator("nav")).toBeHidden();
  await expect(page.locator("#menu")).toBeFocused();
  await page.locator("#language").click();
  await page.locator("#menu").click();
  await page.getByRole("link", { name: "示例", exact: true }).click();
  await expect(page.locator("nav")).toBeHidden();
  await expect(page).toHaveURL(/#use-cases$/);
  expect(
    await page.evaluate(
      () => document.documentElement.scrollWidth <= innerWidth,
    ),
  ).toBeTruthy();
  await page.setViewportSize({ width: 1440, height: 1000 });
  await expect(page.locator("#scene-host")).toHaveAttribute(
    "data-quality",
    "desktop",
  );
});

test("reduced motion freezes the composition, including pointer movement", async ({
  page,
}) => {
  await installDrawCounter(page);
  await page.emulateMedia({ reducedMotion: "reduce" });
  await page.goto("./");
  await expect(page.locator("#hero-visual")).toHaveAttribute(
    "data-renderer",
    "webgl",
  );
  await expect(page.locator("#motion")).toHaveCount(0);
  await waitForEnhanced(page);
  await page.waitForTimeout(400);
  const before = await drawCount(page);
  await page.mouse.move(100, 200);
  await page.mouse.move(300, 400);
  await page.waitForTimeout(600);
  expect(await drawCount(page)).toBe(before);
  await page.emulateMedia({ reducedMotion: "no-preference" });
  await page.waitForTimeout(600);
  expect(await drawCount(page)).toBeGreaterThan(before);
});

for (const shape of ["fuller", "ribbon"]) {
  test(`legacy ${shape} URL uses Fuller through language and quality changes`, async ({ page }) => {
    await page.goto(`./?shape=${shape}`);
    await waitForEnhanced(page);
    await expect(page.locator("#scene-host")).toHaveAttribute("data-shape", "fuller");
    await expect(page.locator("#motion")).toHaveCount(0);
    await page.locator("#language").click();
    await expect(page).toHaveURL(new RegExp(`shape=${shape}.*lang=zh`));
    await page.setViewportSize({ width: 390, height: 844 });
    await expect(page.locator("#scene-host")).toHaveAttribute("data-quality", "mobile");
    await expect(page.locator("#scene-host")).toHaveAttribute("data-shape", "fuller");
    await page.setViewportSize({ width: 1440, height: 1000 });
    await waitForEnhanced(page);
    await expect(page.locator("#scene-host")).toHaveAttribute("data-quality", "desktop");
    await expect(page.locator("#hero-visual")).toHaveAttribute("data-canvas-ready", "true");
  });
}

test("Pages output contains only the selected site and Fuller fallback", () => {
  const files = readdirSync(resolve(import.meta.dirname, "../../dist"), { recursive: true }) as string[];
  expect(files.filter(file => file.endsWith(".html"))).toEqual(["index.html"]);
  expect(files.some(file => /fuller-fallback.*\.png$/.test(file))).toBeTruthy();
  expect(files.filter(file => /ribbon|comparison|research/.test(file))).toEqual([]);
});

test("WebGL failure keeps the static brand image and all content", async ({
  page,
}) => {
  await page.addInitScript(() => {
    const original = HTMLCanvasElement.prototype.getContext;
    HTMLCanvasElement.prototype.getContext = function (
      this: HTMLCanvasElement,
      type: string,
      ...args: unknown[]
    ) {
      if (type.startsWith("webgl")) return null;
      return original.apply(this, [type, ...args] as Parameters<
        typeof original
      >);
    } as typeof original;
  });
  for (const shape of ["fuller", "ribbon"]) {
    await page.goto(`./?shape=${shape}`);
    await expect(page.locator("#hero-visual")).toHaveAttribute("data-renderer", "static");
    await expect(page.locator(".hero-fallback")).toBeVisible();
    await expect(page.locator(".hero-fallback")).toHaveAttribute("src", /fuller-fallback/);
    expect(await page.locator(".hero-fallback").evaluate((el) => (el as HTMLImageElement).naturalWidth)).toBeGreaterThan(0);
  }
  await expect(page.locator("h1")).toBeVisible();
  await expect(page.locator("#motion")).toHaveCount(0);
});

test("context loss disposes the canvas and restores the image", async ({
  page,
}) => {
  await page.goto("./");
  await expect(page.locator("#hero-visual")).toHaveAttribute(
    "data-renderer",
    "webgl",
  );
  await page
    .locator("canvas")
    .evaluate((canvas) =>
      canvas.dispatchEvent(new Event("webglcontextlost", { cancelable: true })),
    );
  await expect(page.locator("#hero-visual")).toHaveAttribute(
    "data-renderer",
    "static",
  );
  await expect(page.locator("canvas")).toHaveCount(0);
  await expect(page.locator(".hero-fallback")).toBeVisible();
});

test("without JavaScript the message, image, docs and mobile navigation remain available", async ({
  browser,
}) => {
  const context = await browser.newContext({
    javaScriptEnabled: false,
    viewport: { width: 390, height: 844 },
  });
  const page = await context.newPage();
  await page.goto("http://127.0.0.1:4173/graphfin/");
  await expect(page.locator("h1")).toBeVisible();
  await expect(page.locator(".hero-fallback")).toBeVisible();
  await expect(page.locator("nav")).toBeVisible();
  await expect(page.getByRole("link", { name: "Read the docs" })).toBeVisible();
  await context.close();
});

test("visual review captures", async ({ page }, testInfo) => {
  test.setTimeout(60000);
  const captures = process.env.UPDATE_VISUALS === "1"
    ? resolve(import.meta.dirname, "../../research/current")
    : testInfo.outputPath("captures");
  mkdirSync(captures, { recursive: true });
  await page.emulateMedia({ reducedMotion: "reduce" });
  for (const language of ["en", "zh"]) {
    const prefix = language === "en" ? "fuller" : "fuller-zh";
    await page.setViewportSize({ width: 1440, height: 1000 });
    await page.goto(language === "en" ? "./" : "./?lang=zh");
    await expect(page.locator("#hero-visual")).toHaveAttribute("data-renderer", "webgl");
    await waitForEnhanced(page);
    await page.screenshot({ path: resolve(captures, `${prefix}-desktop.png`), fullPage: true });
    await page.screenshot({ path: resolve(captures, `${prefix}-hero.png`) });
    await page.locator("#use-cases").screenshot({ path: resolve(captures, `${prefix}-examples.png`) });
    await page.evaluate(() => window.scrollTo({ top: 0, behavior: "instant" }));
    await page.setViewportSize({ width: 390, height: 844 });
    await expect(page.locator("#scene-host")).toHaveAttribute("data-quality", "mobile");
    await waitForEnhanced(page);
    await page.screenshot({ path: resolve(captures, `${prefix}-mobile.png`), fullPage: true });
  }
  await page.setViewportSize({ width: 1440, height: 1000 });
  await page.goto("./");
  await waitForEnhanced(page);
  // Isolate the actual production renderer for a transparent, reusable snapshot.
  await page.addStyleTag({
    content: `html, body { background: transparent !important; }
      body { visibility: hidden !important; }
      #scene-host { visibility: visible !important; position: fixed !important;
        inset: 0 auto auto 0 !important; width: 649px !important; height: 601px !important;
        opacity: 1 !important; transition: none !important; }`,
  });
  await expect(page.locator("#scene-host canvas")).toHaveAttribute("width", "649");
  await expect(page.locator("#scene-host canvas")).toHaveAttribute("height", "601");
  await page.locator("#scene-host canvas").screenshot({ path: resolve(captures, "fuller-fallback.png"), omitBackground: true });
});

test("layouts fit small phones, tablets, and wide desktops in both languages", async ({
  page,
}) => {
  await page.emulateMedia({ reducedMotion: "reduce" });
  await page.goto("./");
  for (const width of [320, 768, 1024, 1920]) {
    await page.setViewportSize({ width, height: 900 });
    for (let language = 0; language < 2; language++) {
      expect(
        await page.evaluate(
          () => document.documentElement.scrollWidth <= innerWidth,
        ),
      ).toBeTruthy();
      await page.locator("#language").click();
    }
  }
});

test("hidden documents and offscreen heroes stop GPU draws", async ({
  page,
}) => {
  await installDrawCounter(page);
  const count = () => drawCount(page);
  await page.goto("./");
  await expect(page.locator("#hero-visual")).toHaveAttribute(
    "data-renderer",
    "webgl",
  );
  await page.waitForTimeout(800);
  expect(await count()).toBeGreaterThan(0);
  await page.evaluate(() => {
    Object.defineProperty(document, "hidden", {
      configurable: true,
      get: () => true,
    });
    document.dispatchEvent(new Event("visibilitychange"));
  });
  const hidden = await count();
  await page.waitForTimeout(300);
  expect(await count()).toBe(hidden);
  await page.evaluate(() => {
    Object.defineProperty(document, "hidden", {
      configurable: true,
      get: () => false,
    });
    document.dispatchEvent(new Event("visibilitychange"));
  });
  await page.waitForTimeout(200);
  expect(await count()).toBeGreaterThan(hidden);
  await page.locator("footer").scrollIntoViewIfNeeded();
  // IntersectionObserver + the in-flight rAF can emit a burst of frames after
  // the scroll; require sustained idle (not a single quiet sample) before asserting.
  let offscreen = await count();
  let stable = 0;
  for (let i = 0; i < 50; i++) {
    await page.waitForTimeout(100);
    const next = await count();
    if (next === offscreen) {
      stable += 1;
      if (stable >= 5) break;
    } else {
      stable = 0;
      offscreen = next;
    }
  }
  await page.waitForTimeout(500);
  expect(await count()).toBe(offscreen);
});
