# Phase 0 reproducible environment

Phase 0 builds and benchmarks TuGraph inside pinned Linux containers. The
authoritative record of those containers is [`../images.lock`](../images.lock).
This file explains provenance and how another developer gets the identical
environment.

## Why containers

TuGraph does not build on macOS and it does not vendor its C++ dependencies.
`deps/build_deps.sh` only builds the web frontend with npm — the real
dependencies (brpc, braft, rocksdb, cpprestsdk, antlr4, Arrow, vsag, FAISS,
protobuf, Boost, gtest, ...) are installed into `/usr/local` and `/usr/lib64` by
the Dockerfiles under `ci/images/`. `deps/install/` is referenced by the top
level `CMakeLists.txt` but does not exist in this tree. Running the build
outside the provided image therefore requires undocumented manual provisioning,
which Phase 0 explicitly forbids.

## The two images

| Purpose | Image | Platform |
|---|---|---|
| Compile + run tests/benchmarks | `tugraph-compile-arm64:local` | linux/arm64 |
| Packaged runtime (used to validate install layout) | `tugraph-runtime-arm64:local` | linux/arm64 |

Both are CentOS 7.9 (AltArch) with GCC 8.4.0 and CMake 3.25.2, matching the
upstream centos7 CI toolchain.

## Obtaining the environment on another machine

The images are pinned by ID, not by a from-source rebuild, because of a
provenance gap (below). Transfer them directly:

```bash
# On a machine that already has the pinned images:
docker save tugraph-compile-arm64:local tugraph-runtime-arm64:local \
    | gzip > tugraph-phase0-arm64-images.tar.gz

# On the target machine (must be arm64 Linux, or use --platform linux/arm64):
gunzip -c tugraph-phase0-arm64-images.tar.gz | docker load

# Verify you have the exact pinned content:
docker image inspect tugraph-compile-arm64:local --format '{{.Id}}'
# must print the image_id recorded in dev/phase0/images.lock
```

`dev/phase0/container.sh` verifies the compile image ID against `images.lock` on
every build and benchmark run and aborts on mismatch. This is deliberate:
silently swapping the image would invalidate every previously recorded number.

## Provenance gap (known residual risk)

`docker history tugraph-compile-arm64:local` shows layers that
`COPY vendor/vsag_deps_patch.py` and `COPY vendor/vsag-deps`, but
`ci/images/vendor/` **does not exist** at commit `672e4b199`. The image was
therefore built from a different, newer revision of
`ci/images/tugraph-compile-arm64v8-centos7-Dockerfile` than the one in this
checkout.

Consequences:

- The Dockerfile in this checkout is **not** a verified reproduction of the
  pinned image, so Phase 0 does not claim `docker build` reproducibility for
  the toolchain.
- Reproducibility for Phase 0 means: same pinned image ID + same build flags +
  same resource limits. That is sufficient for another developer to reproduce
  the recorded baseline numbers.
- Rebuilding the toolchain image from source and diffing it against the pinned
  ID is tracked as a residual risk in
  [`../../../docs/architecture/07-scalability-risks.md`](../../../docs/architecture/07-scalability-risks.md).

The upstream definition remains the source of truth for *what* the environment
should contain:

```bash
# Not run during Phase 0 (needs network access to aliyun mirrors and to the
# tugraph OSS tarball host; Docker Hub was unreachable from the baseline host).
docker build -f ci/images/tugraph-compile-arm64v8-centos7-Dockerfile \
    -t tugraph-compile-arm64:rebuilt .
```

## Usage

```bash
dev/phase0/build.sh              # clean build into build/output
dev/phase0/run_bench.sh --help   # benchmark harness entry point
```

Or open the repo in the devcontainer (`.devcontainer/devcontainer.json`), which
uses the same pinned image.

## Reproducible-from-source path (x86_64/amd64)

The arm64 image cannot be rebuilt from HEAD (the provenance gap above). The
x86_64 path used by CI and the test host **can**:

```bash
dev/phase0/build_image.sh          # builds ci/images/tugraph-compile-centos7-Dockerfile
                                  # (self-contained: no external COPY/ADD)
PHASE0_COMPILE_IMAGE=tugraph-compile-amd64:from-source dev/phase0/build.sh
```

`build_image.sh` prints the resulting `image_id` and the source commit. Record
that id alongside any result produced from it so the toolchain is anchored to a
revision that is reproducible from this checkout. Prefer this path over the
arm64 pinned image whenever certification from source is required.

Every test run also records `run_id`, `git_commit` and a `timestamp_utc` in
`phase0-results/summary.json` (see `dev/phase0/run_tests_inner.sh`), and JSON
benchmark results carry the image id, so provenance is no longer only in prose.
