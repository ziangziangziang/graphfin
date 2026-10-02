# GraphFin website

The official GraphFin landing page: semantic HTML, plain TypeScript, Vite, and
Three.js. This project is independent of the C++/CMake build and the inherited
TuGraph manuals in `docs/docusaurus/`. English is the default; `?lang=zh` opens
the Chinese version. The language button updates the URL without reloading.

## Local preview

Use Node.js 22.12 or newer (CI uses Node 22).

```sh
cd site
npm ci
npm run dev
```

Open the printed address with `/graphfin/` appended, normally
`http://127.0.0.1:5173/graphfin/`.

To preview the actual production output:

```sh
npm run build
npm run preview
```

Open `http://127.0.0.1:4173/graphfin/`. Output goes to `site/dist/`.
No API server, database, C++ build, or secret is needed.

### Compare the two G forms

Open `/graphfin/research/g-comparison.html` for the live side-by-side study.
Its links open the complete site with `?shape=fuller` (rounded tubular lattice)
or `?shape=ribbon` (thin open sheet). Both variants retain the same round
centerline, forward upper-right tip, recessed central tip, perspective camera,
lighting, and evolving signal. Fuller Lattice is the selected default; the
ribbon remains in research for comparison. `?lang=zh` can be combined with either variant.

The approved brief, implementation details, concept images, and browser
captures are documented in [research/g-implementation-study.md](research/g-implementation-study.md).

## Verification

```sh
npm test
npm run build
npx playwright install chromium
npm run test:browser
```

On Linux, use `npx playwright install --with-deps chromium`.
To use an existing local Chrome, set `PLAYWRIGHT_CHROMIUM_EXECUTABLE` to its
executable path. Tests start the production preview server automatically.

Unit checks cover carbon-node degree, welded seam topology, continuous signal
connectivity over several cycles, bounded geometry, node collision clearance in
world and screen space, and mobile quality settings.
Browser checks cover production subpaths and assets, English/Chinese switching,
mobile menu/keyboard behavior, both shape variants and their comparison page,
breakpoint changes, reduced motion, WebGL failure, context loss, and useful
content without JavaScript. They use
software WebGL for reproducibility; this is not a real-device performance claim.

## Architecture

| File                                     | Responsibility                                                                             |
| ---------------------------------------- | ------------------------------------------------------------------------------------------ |
| `index.html`                             | Complete English page, semantic navigation, section scaffolding, fallback image, ownership |
| `src/main.ts`                            | Menu, shape selection and fallback, lazy scene loading, section observer, page lifecycle    |
| `src/i18n.ts`                            | Chinese translations, document language/title/metadata, shareable language state           |
| `src/style.css`, `src/styles/layout.css` | Brand styling, responsive layouts, focus and reduced-motion styles                         |
| `src/scene/geometry.ts`                  | Welded hexagonal lattice, circular G and smooth inward bend, sheet/tube sweep, real depth    |
| `src/scene/signal.ts`                    | Stateful value-noise pulse with connected route, thermal afterimage, echo, and fork         |
| `src/scene/lattice-mesh.ts`              | Instanced spheres/bonds, procedural material wear, damped follow-through, local light spill |
| `src/scene/camera.ts`                    | Composition fitting, gentle orbit/dolly, and restrained pointer parallax                     |
| `src/scene/quality.ts`                   | Geometry, resolution, and frame-rate budgets                                               |
| `src/scene/focus-pass.ts`                | Thin-lens blur radius, soft aperture filtering of colour and coverage, reusable blur targets |
| `src/scene/scene.ts`                     | Renderer, IBL/tone mapping, depth focus and desktop bloom, animation clock, visibility, resize, cleanup |
| `research/g-comparison.html`, `src/research-comparison.ts` | Two live variants and links to their full site views                       |
| `vite.config.ts`                         | Both build entries, Pages base path, and branch-aware documentation links                    |

`SceneController` exposes `setSection` and `dispose`. Sections have
`data-scene-section` attributes and communicate through one observer. Future
entity labels, query highlights, failure/restart sequences, or multi-graph
transitions can extend this boundary without coupling geometry to page copy.
The first milestone only animates the introduction; the following sections are
intentionally short, with links to existing source documentation and examples.

### Rendering budgets and lifecycle

- Desktop ribbon: 384 nodes, 528 bonds. Fuller: 480 nodes, 720 bonds.
  Both use maximum DPR 1.75 and 30 fps.
- Mobile (viewport at most 760px) ribbon: 192 nodes, 256 bonds. Fuller:
  256 nodes, 384 bonds. Both use simpler spheres and cylinders, maximum
  DPR 1.35, 24 fps, reduced automatic camera travel, and no pointer parallax.
- Two instanced meshes and one points layer. No image textures, physics, or
  continuously growing geometry. Three.js is the only runtime dependency.
- Lighting uses a procedural `RoomEnvironment` IBL, ACES filmic tone mapping,
  a warm key and cool rim reflected by polished metal. Only the signal emits
  light; there are no painted emissive rims on inactive metal.
  Both tiers use a depth-texture focus pass biased slightly toward the viewer.
  Eight overlapping blur-radius bands spread linear colour and coverage with
  a soft aperture profile. Desktop adds `UnrealBloomPass` for the gold signal;
  mobile omits bloom. IBL bake and the post chain are created after the first frame
  so software GL cannot stall mount.
- The lattice is an infinite parameter stream. Its sites continuously advance
  toward the viewer from the recessed central arm, pass the forward upper-right
  arm, and recycle through the far cutoff. The periodic return bond is hidden
  because it would span the open G aperture. Geometry continues through the
  lens and beyond the rear clip plane; material opacity never fades. Optical
  defocus grows before these hidden recycling boundaries. Buffers and instance
  counts remain fixed, and blur targets are reused between frames.
- Material phase is resampled by arc length through the visible depth range,
  preventing the foreground cells from stretching into rods as they approach.
  Additional longitudinal cells cover the extended arm at both quality tiers.
- Hidden documents and offscreen scenes stop the animation loop. Elapsed time
  does not jump on resume. The system's reduced-motion preference freezes the
  composition. There is no manual pause button; the always-on flow only stops
  when the page is hidden/offscreen or reduced motion is requested.
- Resize updates the camera and pixel ratio; crossing the mobile breakpoint
  disposes and rebuilds the small mesh set. Page exit/context loss frees meshes,
  materials, renderer, observers, listeners, and animation frames. Back/forward
  cache restoration mounts a fresh renderer.

### Assets and visual compromises

The original brand assets in `../assets/` are unmodified. The header/favicon
use `logo.svg`; `research/current/*-fallback.png` contain snapshots of the
implemented G forms. JavaScript selects the matching fallback; without
JavaScript the default fuller snapshot is visible. The imagegen concept
references stay in `research/concepts/` and are not used as live-render evidence.
Vite fingerprints and rebases asset URLs. No external font/CDN is used.

These are stylized lattices with a reflective metal finish, inspired by the
original carbon structure rather than chemically exact molecular models.
A circular 300-degree outer arc and broad inward bend keep the G round and
legible. The upper-right head sits in front of the middle plane and the inner
tip sits behind it. Centerline perspective compensation preserves the round
outline at the baseline view; cross-section width, node scale, occlusion, and
camera travel retain real 3D depth. A fixed 40-degree perspective field of view
avoids changing the lens while the camera gently orbits and moves closer/farther.
The focus pass keeps the middle arc legible while foreground silhouettes expand
and the receding center shrinks into defocus. Its image-space approximation uses
a soft aperture, not a ray-traced lens. A decorative breakout frame establishes
the screen plane. Materials use a silver-grey base, near-unit metalness,
roughness around 0.2, and subtle surface variation. Studio reflections change
with the moving surface and camera;
focus and bloom use post-processing. The signal integrates value-noise pacing with stalls, surges, a
short thermal afterimage, occasional forked heat, and a soft echo while its
active gold trail remains one connected route. A motivated amber point light
follows the head and spills onto neighboring bonds. Static offsets, a localized
dull bend, thickness variation, and damped follow-through keep the lattice from
looking CAD-perfect. The gold halo is a procedural soft point attached to active
and residual-heat graph nodes, never a separate waveform. Reduced motion uses
the same static 3D composition when supported; unavailable WebGL, context loss,
or a failed renderer import leaves the matching static snapshot visible.

The deferred renderer bundle is about 146 KB gzipped. Vite reports its
uncompressed chunk above the default 500 KB advisory threshold; it is loaded
separately from the small page/navigation bundle.

## GitHub Pages

`.github/workflows/documentation.yml` builds this site and deploys it to the
`gh-pages` branch (same rule as the inherited Docusaurus workflow). It is the
only Pages workflow; unrelated CI and C++ workflows are untouched.

1. In repository **Settings → Pages → Build and deployment**, select
   **Deploy from a branch** with branch `gh-pages` (root). The workflow
   force-updates that branch on each deploy.
2. Merge these changes into `main`. Changes to `site/`, `assets/`, or this
   workflow trigger deployment. Pull requests build and test but never deploy.
3. The workflow installs locked dependencies, runs unit checks, type-checks and
   builds, runs browser tests, then publishes `site/dist/` to `gh-pages` via
   `JamesIves/github-pages-deploy-action` (`clean: true` so stale Docusaurus
   files are removed). `contents: write` is limited to the deploy job.
4. The expected URL is `https://ziangziangziang.github.io/graphfin/`.
   A manual **GraphFin website** workflow run on `main` can also deploy.

The first successful run after the workflow is on `main` replaces the legacy
site at that URL; old generated Docusaurus URLs are not maintained by this
milestone.

### Base path and documentation source

`/graphfin/` is the default base. CI derives it from the repository name. Override
it for a different Pages subpath or a custom-domain root:

```sh
SITE_BASE_PATH=/ SITE_SOURCE_REF=main npm run build
```

`SITE_SOURCE_REF` controls GitHub documentation/example/license links. Locally
it defaults to the checked-out branch, or the commit SHA in a detached checkout.
CI supplies the built branch. This keeps the website's claims linked to the code
being built, rather than assuming the development changes already exist on
`main`. A source archive without Git metadata must set `SITE_SOURCE_REF`.

The Chinese UI currently links to the same authoritative GraphFin source docs;
the inherited translated TuGraph manuals are not presented as GraphFin docs.
Dedicated Chinese documentation and stable hosted docs are later milestones.

## Ownership

Ziang Zhang — <ziang.zhang@idefinity.com>  
深圳市想法无限科技有限公司

The website follows the repository's Apache 2.0 license. Existing upstream
license and attribution files are preserved.
