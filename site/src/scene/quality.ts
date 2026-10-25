import type { ShapeVariant } from "./geometry.ts";

export interface Quality {
  mobile: boolean;
  columns: number;
  rows: number;
  nodeScale: number;
  bondScale: number;
  sphereSegments: number;
  bondSegments: number;
  pixelRatio: number;
  fps: number;
}

export function qualityFor(
  width: number,
  pixelRatio: number,
  variant: ShapeVariant = "fuller",
): Quality {
  const mobile = width <= 760;
  return {
    mobile,
    columns: variant === "fuller" ? (mobile ? 44 : 64) : (mobile ? 32 : 48),
    rows: variant === "fuller" ? (mobile ? 5 : 7) : (mobile ? 2 : 3),
    // More cells in the same tube: keep open gaps and distinct metal joints.
    nodeScale: variant === "fuller" ? (mobile ? 0.82 : 0.7) : 1,
    bondScale: variant === "fuller" ? (mobile ? 0.9 : 0.82) : 1,
    sphereSegments: mobile ? 8 : 12,
    bondSegments: mobile ? 5 : 8,
    pixelRatio: Math.min(pixelRatio, mobile ? 1.35 : 1.75),
    fps: mobile ? 24 : 30,
  };
}
