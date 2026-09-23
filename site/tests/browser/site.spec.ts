import { expect, test } from "@playwright/test";
import { createHash } from "node:crypto";

const digest = (image: Buffer) =>
  createHash("sha256").update(image).digest("hex");

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
  await expect(page.locator("#menu")).toBeHidden();
  await expect(page.locator("h1")).toHaveText(
    "Relationshipsand observations,together.",
  );
  await expect(
    page.getByRole("link", { name: "Read the docs" }),
  ).toHaveAttribute("href", /graphfin\/blob\/[^/]+\/docs\/README.md/);
  await page.locator("#language").click();
  await expect(page.locator("html")).toHaveAttribute("lang", "zh-CN");
  await expect(page).toHaveURL(/lang=zh/);
  await expect(page.locator("h1")).toContainText("关系与观测");
  await page.reload();
  await expect(page.locator("html")).toHaveAttribute("lang", "zh-CN");
  await page.locator("#language").click();
  await page.getByRole("link", { name: "Explore GraphFin" }).click();
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
  await page.getByRole("link", { name: "应用场景", exact: true }).click();
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
  await page.emulateMedia({ reducedMotion: "reduce" });
  await page.goto("./");
  await expect(page.locator("#hero-visual")).toHaveAttribute(
    "data-renderer",
    "webgl",
  );
  await expect(page.locator("#motion")).toBeHidden();
  const canvas = page.locator("canvas");
  await page.waitForTimeout(500);
  const before = digest(await canvas.screenshot());
  await page.mouse.move(100, 200);
  await page.waitForTimeout(600);
  expect(digest(await canvas.screenshot())).toBe(before);
  await page.emulateMedia({ reducedMotion: "no-preference" });
  await expect(page.locator("#motion")).toBeVisible();
});

test("animation can be paused and resumed", async ({ page }) => {
  await page.goto("./");
  await expect(page.locator("#hero-visual")).toHaveAttribute(
    "data-renderer",
    "webgl",
  );
  await page.locator("#motion").click();
  await expect(page.locator("#motion")).toHaveAttribute("aria-pressed", "true");
  const canvas = page.locator("canvas");
  await page.waitForTimeout(800);
  const before = digest(await canvas.screenshot());
  await page.waitForTimeout(500);
  expect(digest(await canvas.screenshot())).toBe(before);
  await page.locator("#motion").click();
  await page.waitForTimeout(600);
  expect(digest(await canvas.screenshot())).not.toBe(before);
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
  await page.goto("./");
  await expect(page.locator("#hero-visual")).toHaveAttribute(
    "data-renderer",
    "static",
  );
  await expect(page.locator(".hero-fallback")).toBeVisible();
  expect(
    await page
      .locator(".hero-fallback")
      .evaluate((el) => (el as HTMLImageElement).naturalWidth),
  ).toBeGreaterThan(0);
  await expect(page.locator("h1")).toBeVisible();
  await expect(page.locator("#motion")).toBeHidden();
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

test("visual review captures", async ({ page }) => {
  await page.emulateMedia({ reducedMotion: "reduce" });
  await page.goto("./");
  await expect(page.locator("#hero-visual")).toHaveAttribute(
    "data-renderer",
    "webgl",
  );
  await page.screenshot({ path: "/tmp/graphfin-desktop.png", fullPage: true });
  await page.setViewportSize({ width: 390, height: 844 });
  await page.screenshot({ path: "/tmp/graphfin-mobile.png", fullPage: true });
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
  const count = () =>
    page.evaluate(
      () =>
        (window as unknown as { graphfinTestDraws: { count: number } })
          .graphfinTestDraws.count,
    );
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
  await page.waitForTimeout(300);
  const offscreen = await count();
  await page.waitForTimeout(300);
  expect(await count()).toBe(offscreen);
});
