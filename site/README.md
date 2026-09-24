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
mobile menu/keyboard behavior, breakpoint changes, reduced motion, pause/resume,
WebGL failure, context loss, and useful content without JavaScript. They use
software WebGL for reproducibility; this is not a real-device performance claim.

## Architecture

| File                                     | Responsibility                                                                             |
| ---------------------------------------- | ------------------------------------------------------------------------------------------ |
| `index.html`                             | Complete English page, semantic navigation, section scaffolding, fallback image, ownership |
| `src/main.ts`                            | Menu, motion control, lazy scene loading, section observer, page lifecycle                 |
| `src/i18n.ts`                            | Chinese translations, document language/title/metadata, shareable language state           |
| `src/style.css`, `src/styles/layout.css` | Brand styling, responsive layouts, focus and reduced-motion styles                         |
| `src/scene/geometry.ts`                  | Welded hexagonal strip, periodic seam, logo-accurate G mapping with anamorphic depth of field, end fade |
| `src/scene/signal.ts`                    | Continuous travelling pulse along a closed route of real lattice edges                     |
| `src/scene/lattice-mesh.ts`              | Instanced spheres/bonds, material fade, small procedural signal halos                      |
| `src/scene/camera.ts`                    | Composition fitting and restrained pointer parallax                                        |
| `src/scene/quality.ts`                   | Geometry, resolution, and frame-rate budgets                                               |
| `src/scene/scene.ts`                     | Renderer, IBL/tone mapping, desktop bloom, animation clock, visibility, resize, cleanup    |
| `vite.config.ts`                         | Pages base path and branch-aware documentation links                                       |

`SceneController` exposes `setSection`, `setPaused`, and `dispose`. Sections have
`data-scene-section` attributes and communicate through one observer. Future
entity labels, query highlights, failure/restart sequences, or multi-graph
transitions can extend this boundary without coupling geometry to page copy.
The first milestone only animates the introduction; the following sections are
intentionally short, with links to existing source documentation and examples.

### Rendering budgets and lifecycle

- Desktop: 272 nodes, 374 bonds, maximum DPR 1.75, 30 fps.
- Mobile (viewport at most 760px): 144 nodes, 192 bonds, simpler spheres and
  cylinders, maximum DPR 1.35, 24 fps, no pointer parallax.
- Two instanced meshes and one points layer. No image textures, physics, or
  continuously growing geometry. Three.js is the only runtime dependency.
- Lighting uses a procedural `RoomEnvironment` IBL, ACES filmic tone mapping,
  and a view-space fresnel rim for a ray-traced *look* (not path tracing).
  Desktop adds a single `UnrealBloomPass` tuned for the gold signal; mobile keeps
  direct rendering to protect its frame budget. IBL bake and bloom composer are
  created after the first frame so software GL cannot stall mount.
- The strip moves through a fixed parameter window. Its welded seam recycles
  indefinitely; edges crossing the visible window boundary are suppressed, and
  the ends disappear using opacity and fog. Buffers and instance counts remain fixed.
- Hidden documents and offscreen scenes stop the animation loop. Elapsed time
  does not jump on resume. Manual pause and reduced motion freeze the composition.
- Resize updates the camera and pixel ratio; crossing the mobile breakpoint
  disposes and rebuilds the small mesh set. Page exit/context loss frees meshes,
  materials, renderer, observers, listeners, and animation frames. Back/forward
  cache restoration mounts a fresh renderer.

### Assets and visual compromises

The source assets are `../assets/logo.svg`, `../assets/logo.png`, and
`../assets/hero.png` (there is no `asset/hero.png`). They are unmodified. The site
uses the SVG in the header/favicon and the existing hero PNG as the static
fallback. Vite fingerprints and rebases asset URLs. No external font/CDN is used.

This is a stylized carbon ribbon, not a chemically exact molecular model. The
strip follows the GraphFin logo G (defocused head in the right aperture, CCW
arc, inward tail). Perspective, an anamorphic depth ramp that lunges the ends
toward and away from the lens, progressively fading ends, and fog approximate
depth of field; a decorative breakout frame under the canvas lets the G paint
beyond the window, and a soft edge mask dissolves the hard clip. Materials use
a low-roughness metalness/IBL sheen; desktop bloom is a cheap threshold pass,
not a full deferred pipeline. The gold signal is a single route pulse whose
speed, tail length, and edge brightness wobble on layered sines plus a seeded
hash flicker (deterministic, still one connected trail). Static per-atom
offsets and tint drift keep the lattice from looking CAD-perfect. The gold
halo is a procedural soft point attached to active graph nodes, never a
separate waveform. Reduced motion uses the same static 3D composition when
supported; unavailable WebGL, context loss, or a failed renderer import leaves
the existing hero image visible.

The legacy PNG is about 1.1 MB. The deferred renderer bundle is about 142 KB
gzipped. A future optimized fallback derivative can reduce first-load transfer
without altering the original artwork. Vite reports the renderer's uncompressed
chunk above its default 500 KB advisory threshold; it is loaded separately from
the small page/navigation bundle.

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
