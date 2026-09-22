import { execFileSync } from "node:child_process";
import { defineConfig } from "vite";

// Docs follow the source being built, including before this branch is merged.
function sourceRef() {
  if (process.env.SITE_SOURCE_REF) return process.env.SITE_SOURCE_REF;
  try {
    return execFileSync("git", ["rev-parse", "--abbrev-ref", "HEAD"], {
      encoding: "utf8",
    }).trim() === "HEAD"
      ? execFileSync("git", ["rev-parse", "HEAD"], { encoding: "utf8" }).trim()
      : execFileSync("git", ["branch", "--show-current"], {
          encoding: "utf8",
        }).trim();
  } catch {
    throw new Error(
      "Set SITE_SOURCE_REF when building outside a Git checkout.",
    );
  }
}

export default defineConfig({
  base: process.env.SITE_BASE_PATH || "/graphfin/",
  plugins: [
    {
      name: "graphfin-source-links",
      transformIndexHtml: (html) =>
        html.replaceAll(
          "__SOURCE_BASE__",
          `https://github.com/ziangziangziang/graphfin/blob/${encodeURIComponent(sourceRef())}`,
        ),
    },
  ],
  build: { target: "es2022" },
});
