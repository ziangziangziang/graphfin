export interface Quality {
  mobile: boolean;
  columns: number;
  rows: number;
  sphereSegments: number;
  bondSegments: number;
  pixelRatio: number;
  fps: number;
}

export function qualityFor(width: number, pixelRatio: number): Quality {
  const mobile = width <= 760;
  return {
    mobile,
    columns: mobile ? 24 : 34,
    rows: mobile ? 2 : 3,
    sphereSegments: mobile ? 8 : 12,
    bondSegments: mobile ? 5 : 8,
    pixelRatio: Math.min(pixelRatio, mobile ? 1.35 : 1.75),
    fps: mobile ? 24 : 30,
  };
}
